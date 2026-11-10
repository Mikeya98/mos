/**
 * @file    exception.c
 * @brief   MOS RTOS 异常处理 — Data Abort / Prefetch Abort (v0.6 阶段3/6)
 *
 * 权限隔离之后, 用户态任务访问内核区会触发 Permission Fault。
 * 如果内核直接死循环, 整个系统就停了; 正确的做法是:
 *   1. 打印故障信息 (任务名 / 故障地址 / FSR 状态)
 *   2. 杀掉出错的用户任务
 *   3. 调度到下一个任务, 系统继续运行
 *
 * 阶段6 之后, 用户私有区的 Data Abort 多了一种正常结局: 缺页。
 * 这类故障由 vm_fault 处理, 补好页表后直接返回用户态重试指令,
 * 不算错误 (也就不会打 ABORT 现场)。只有"补不了"的地址 ——
 * 越界、踩保护页、页框耗尽 —— 才走杀任务那条路。
 *
 * 内核态自己触发的 Abort 另当别论: 那说明内核有 bug (空指针、
 * 栈溢出、野指针), 继续跑只会反复异常, 所以打印后直接停机。
 */

#include "kernel/task.h"
#include "lib/printf.h"
#include "mm/mmu.h"
#include "mm/vm.h"

/* =========================== CP15 故障寄存器 =========================== */

static inline u32 cp15_read_dfar(void)
{
    u32 val;
    __asm__ volatile ("MRC p15, 0, %0, c6, c0, 0" : "=r"(val));  /* DFAR */
    return val;
}

static inline u32 cp15_read_dfsr(void)
{
    u32 val;
    __asm__ volatile ("MRC p15, 0, %0, c5, c0, 0" : "=r"(val));  /* DFSR */
    return val;
}

static inline u32 cp15_read_ifar(void)
{
    u32 val;
    __asm__ volatile ("MRC p15, 0, %0, c6, c0, 2" : "=r"(val));  /* IFAR */
    return val;
}

static inline u32 cp15_read_ifsr(void)
{
    u32 val;
    __asm__ volatile ("MRC p15, 0, %0, c5, c0, 1" : "=r"(val));  /* IFSR */
    return val;
}

/* =========================== FSR 状态解码 =========================== */

/**
 * @brief  把 FSR[3:0] 的状态码翻译成可读字符串
 *
 * 常见值:
 *   0b00110  转换错误 (section) — 地址根本没映射
 *   0b01101  权限错误 (section) — 映射了但 AP 不允许 (我们的隔离就是这种)
 *   0b00001  对齐错误
 */
static const char *fsr_status_str(u32 fsr)
{
    switch (fsr & 0xFu) {
    case 0x1:  return "alignment fault";
    case 0x2:  return "debug event";
    case 0x3:  return "access flag fault (section)";
    case 0x5:  return "translation fault (section)";
    case 0x6:  return "translation fault (section)";
    case 0x7:  return "translation fault (page)";
    case 0x9:  return "domain fault (section)";
    case 0xB:  return "domain fault (page)";
    case 0xD:  return "permission fault (section)";
    case 0xF:  return "permission fault (page)";
    case 0x8:  return "synchronous external abort";
    case 0xE:  return "synchronous external abort (page)";
    default:   return "unknown";
    }
}

/* =========================== 公共处理 =========================== */

/**
 * @brief  打印故障现场的公共部分
 * @return 1 = 故障发生在用户态, 0 = 发生在内核态
 */
static int report_fault(const char *kind, u32 pc, u32 spsr,
                        u32 far, u32 fsr)
{
    task_t *cur = get_current();
    u32 mode = spsr & 0x1Fu;

    printf("\n========================================\n");
    printf("  [ABORT] %s\n", kind);
    printf("  task   : %s (pid=%u)\n",
           cur ? cur->name : "?", cur ? cur->pid : 0);
    printf("  mode   : %s (0x%X)\n",
           (mode == 0x10) ? "USR" : "SVC/kernel", mode);
    printf("  pc     : %X\n", pc);
    printf("  addr   : %X\n", far);
    printf("  fsr    : %X  (%s%s)\n", fsr,
           (fsr & (1u << 10)) ? "permission, " : "",
           fsr_status_str(fsr));
    printf("========================================\n");

    return (mode == 0x10) ? 1 : 0;
}

/* 故障状态码: 这些是"页没映射", 可以尝试按需补页 */
static int is_page_fault(u32 fsr)
{
    switch (fsr & 0xFu) {
    case 0x3:   /* access flag fault (section) */
    case 0x5:   /* translation fault (section): 1MB 没映射 */
    case 0x6:   /* translation fault (section) */
    case 0x7:   /* translation fault (page): 4KB 页没映射 ← 主要路径 */
        return 1;
    default:
        return 0;
    }
}

/**
 * @brief  写权限错误 (v0.6 阶段8 的写时复制路径)
 *
 * fork 之后父子共享的页是 AP=10 (用户只读), 任何一方写都会触发
 * permission fault (状态码 0xF 页 / 0xD section), 同时 WnR 位
 * (FSR bit11) = 1。这一类不是错误, 而是"该复制这一页了"的信号。
 */
static int is_write_perm_fault(u32 fsr)
{
    u32 status = fsr & 0xFu;

    if (status != 0xFu && status != 0xDu) {
        return 0;
    }
    return (fsr & (1u << 11)) ? 1 : 0;
}

/**
 * @brief  Data Abort 处理 (startup.S 的 data_handler 调用)
 *
 * @param  fault_pc  触发故障的指令地址 (lr_abt - 8)
 * @param  spsr_abt  故障发生时的 CPSR (低 5 位 = 模式)
 * @return 只有"缺页已补齐"这一种情况会返回, 返回后汇编代码用 RFE
 *         回到故障指令重新执行。其余情况杀任务/停机, 永不返回。
 */
int data_abort_handler(u32 fault_pc, u32 spsr_abt)
{
    u32 dfsr = cp15_read_dfsr();
    u32 dfar = cp15_read_dfar();
    u32 mode = spsr_abt & 0x1Fu;

    /* ------------------- 内核态故障: 内核自己有 bug, 停机 ------------------- */
    if (mode != 0x10) {
        report_fault("Data Abort", fault_pc, spsr_abt, dfar, dfsr);
        printf("[ABORT] kernel fault — halting\n");
        while (1) {
            __asm__ volatile ("WFI");
        }
    }

    /* ------------------- 用户态: 缺页 / 写时复制 ------------------- */
    {
        int page_fault = is_page_fault(dfsr);
        int cow_fault  = (!page_fault && is_write_perm_fault(dfsr));
        int wr         = (dfsr & (1u << 11)) ? 1 : 0;

        if (page_fault || cow_fault) {
            /*
             * cow_fault 一定是写操作; 普通缺页按 FSR 的 WnR 位区分读写。
             */
            int rc = vm_fault(dfar, (u32)(cow_fault ? 1 : wr));

            if (rc == VM_OK) {
                /*
                 * 页表已经补好/复制好, TLB 里这一项也刷过了。
                 * 返回 1 给汇编, RFE 之后 CPU 重新执行同一条指令。
                 */
                return 1;
            }

            /*
             * 补不了: 打出现场, 再补一句失败原因。
             * 这几类错误在调试时完全不同方向, 必须分开写:
             *   保护页 → 栈溢出, 去查递归/大局部变量;
             *   页框耗尽 → 内存不够, 去查泄漏或者调大池子;
             *   非法写 → 往代码区写, 或者地址越界。
             */
            report_fault("Data Abort", fault_pc, spsr_abt, dfar, dfsr);
            if (rc == VM_GUARD) {
                printf("[ABORT] stack overflow: %X is the guard page (%X)\n",
                       dfar, USER_GUARD_VADDR);
            } else if (rc == VM_NOMEM) {
                printf("[ABORT] out of page frames while faulting %X\n", dfar);
            } else {
                printf("[ABORT] %X is not a demand-paging address\n", dfar);
            }
        } else {
            report_fault("Data Abort", fault_pc, spsr_abt, dfar, dfsr);
        }
    }

    printf("[ABORT] terminating user task, scheduler continues\n\n");

    /* 用 SIGSEGV 风格的退出码, 有父进程的话它 wait 时能拿到 -11 */
    task_exit(-11);
}

/**
 * @brief  Prefetch Abort 处理 (取指失败: 跳到无权执行/未映射的地址)
 *
 * 阶段12 之后这里多了一种正常结局: 用户程序窗口 (0x09000000) 的
 * 代码页还没从文件装载。第一次执行到一个新页时, 取指就会 fault,
 * 这类 IFSR 状态同样是 translation fault, 交给 vm_fault 补页,
 * 返回 1 让汇编代码 RFE 回去重新取这条指令。
 *
 * 其余情况 (往栈上跳、PC 越界、内核自己跑飞) 仍然杀任务/停机。
 * 返回 0 不会发生 —— 失败路径调用 task_exit 永不返回, 汇编那边
 * 对返回值的处理只是形式上的统一。
 */
int prefetch_abort_handler(u32 fault_pc, u32 spsr_abt)
{
    u32 ifsr = cp15_read_ifsr();
    u32 ifar = cp15_read_ifar();
    u32 mode = spsr_abt & 0x1Fu;

    if (mode != 0x10) {
        report_fault("Prefetch Abort", fault_pc, spsr_abt, ifar, ifsr);
        printf("[ABORT] kernel fault — halting\n");
        while (1) {
            __asm__ volatile ("WFI");
        }
    }

    if (is_page_fault(ifsr)) {
        int rc = vm_fault(ifar, 0);

        if (rc == VM_OK) {
            return 1;               /* 代码页已装载, 重试取指 */
        }

        report_fault("Prefetch Abort", fault_pc, spsr_abt, ifar, ifsr);
        if (rc == VM_NOMEM) {
            printf("[ABORT] out of page frames while loading %X\n", ifar);
        } else {
            printf("[ABORT] %X is not a loadable program page\n", ifar);
        }
    } else {
        report_fault("Prefetch Abort", fault_pc, spsr_abt, ifar, ifsr);
    }

    printf("[ABORT] terminating user task, scheduler continues\n\n");

    task_exit(-4);      /* SIGILL 风格的退出码 */
    return 0;
}
