/**
 * @file    exception.c
 * @brief   MOS RTOS 异常处理 — Data Abort / Prefetch Abort (v0.6 阶段3)
 *
 * 权限隔离之后, 用户态任务访问内核区会触发 Permission Fault。
 * 如果内核直接死循环, 整个系统就停了; 正确的做法是:
 *   1. 打印故障信息 (任务名 / 故障地址 / FSR 状态)
 *   2. 杀掉出错的用户任务
 *   3. 调度到下一个任务, 系统继续运行
 *
 * 内核态自己触发的 Abort 另当别论: 那说明内核有 bug (空指针、
 * 栈溢出、野指针), 继续跑只会反复异常, 所以打印后直接停机。
 */

#include "kernel/task.h"
#include "lib/printf.h"
#include "mm/mmu.h"

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

/**
 * @brief  Data Abort 处理 (startup.S 的 data_handler 调用)
 *
 * @param  fault_pc  触发故障的指令地址 (lr_abt - 8)
 * @param  spsr_abt  故障发生时的 CPSR (低 5 位 = 模式)
 *
 * 无论哪种情况都不返回。
 */
void data_abort_handler(u32 fault_pc, u32 spsr_abt)
{
    u32 dfsr = cp15_read_dfsr();
    u32 dfar = cp15_read_dfar();
    int from_user = report_fault("Data Abort", fault_pc, spsr_abt, dfar, dfsr);

    if (!from_user) {
        printf("[ABORT] kernel fault — halting\n");
        while (1) {
            __asm__ volatile ("WFI");
        }
    }

    printf("[ABORT] killing user task, scheduler continues\n\n");

    task_kill(get_current());
    schedule();

    /* 不应该到这里: 被杀任务已经不在就绪队列里 */
    while (1) {
        __asm__ volatile ("WFI");
    }
}

/**
 * @brief  Prefetch Abort 处理 (取指失败: 跳到无权执行/未映射的地址)
 */
void prefetch_abort_handler(u32 fault_pc, u32 spsr_abt)
{
    u32 ifsr = cp15_read_ifsr();
    u32 ifar = cp15_read_ifar();
    int from_user = report_fault("Prefetch Abort", fault_pc, spsr_abt, ifar, ifsr);

    if (!from_user) {
        printf("[ABORT] kernel fault — halting\n");
        while (1) {
            __asm__ volatile ("WFI");
        }
    }

    printf("[ABORT] killing user task, scheduler continues\n\n");

    task_kill(get_current());
    schedule();

    while (1) {
        __asm__ volatile ("WFI");
    }
}
