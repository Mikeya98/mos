/**
 * @file    usermain.c
 * @brief   用户态 demo 任务 (v0.6 阶段4) — 独立地址空间 / 系统调用 / 异常恢复
 *
 * 这里的函数全部放在 .user_text 段 (链接到 0x08000000), 在 USR 模式下
 * 执行, 只能通过 SVC 进内核。不能调用 printf / memcpy 等内核函数,
 * 也不能直接读内核里的字符串 —— 内核区在页表里是 AP=01, 用户态访问
 * 会触发 Permission Fault (Data Abort)。
 *
 * 每个任务有自己的页表, 相同的虚拟地址指向不同的物理页:
 *   0x08000000  用户代码 (共享, 用户只读)
 *   0x08100000  私有数据页 (1 页, 首次访问时分配)
 *   0x08101000  栈保护页 (永远 fault)
 *   0x08102000  栈底, 栈从 0x08200000 向下长, 最多 1016KB
 *
 * 演示八个场景:
 *   u_waiter   — 调 sem_wait 阻塞, 被 u_worker 唤醒
 *   u_worker   — 正常系统调用: getpid / write / sem_post
 *   u_fault    — 直接读内核地址 → Data Abort → 任务被杀, 系统继续跑
 *   u_badptr   — 把内核指针/未映射地址传给 sys_write → copy_from_user 返回 EFAULT
 *   u_iso_a    — 独立地址空间: 写自己的 0x08100000, 等 u_iso_b 跑完再读回
 *   u_iso_b    — 独立地址空间: 写同一虚拟地址, 两边互不影响
 *   u_fork     — fork + 写时复制: 父子共享页, 谁写谁复制
 *   u_exec     — exec: 清空私有地址空间, 从新入口重新开始
 *   u_elf      — exec_elf: 从 RAM 盘读 ELF, 装进 0x09000000 执行;
 *                第二次执行同一文件时只读代码页命中文件页缓存
 *   u_wait     — wait/僵尸: 子进程退出保留退出码, 父进程回收
 *   u_demand   — 按需分页: 数据页/栈页都是第一次访问才分配
 *   u_overflow — 写栈保护页 → 内核判定栈溢出, 杀掉任务
 *   u_swap     — 页框池耗尽 → 把页换出到 swap 区, 再读回来校验
 *   u_signal   — 信号: handler 投递 / sigreturn / 默认终止 /
 *                屏蔽字 / 信号唤醒阻塞在 sem_wait 上的任务 (-EINTR)
 */

#include "lib/types.h"
#include "lib/printf.h"
#include "kernel/task.h"
#include "kernel/syscall.h"
#include "user/user.h"
#include "user/user_syscall.h"
#include "mm/mmu.h"
#include "mm/addr_space.h"
#include "mm/vm.h"
#include "mm/frame.h"
#include "mm/swap.h"
#include "mm/page_cache.h"

/* =========================== 段属性 =========================== */
#define USER_TEXT   __attribute__((section(".user_text")))
#define USER_RODATA __attribute__((section(".user_rodata")))

/* =========================== 用户态字符串 =========================== */
/*
 * 必须显式放进 .user_rodata。
 * 普通字符串字面量会进内核 .rodata (AP=01), 用户态读它会 Data Abort。
 */
static const char msg_wait[]   USER_RODATA = "[user] u_waiter: sem_wait(0) — should block here\n";
static const char msg_woken[]  USER_RODATA = "[user] u_waiter: woken by u_worker, exiting\n";

static const char msg_work1[]  USER_RODATA = "[user] u_worker: getpid() > 0, stack top at 0x08200000 (on-demand pages)\n";
static const char msg_work2[]  USER_RODATA = "[user] u_worker: sem_post(0) done, exiting\n";
static const char msg_stackbad[] USER_RODATA = "[user] u_worker: private stack check FAILED\n";

static const char msg_fault1[] USER_RODATA = "[user] u_fault : now reading kernel address 0x00100000 ...\n";
static const char msg_fault2[] USER_RODATA = "[user] u_fault : still alive — isolation FAILED!\n";

static const char msg_bad1[]   USER_RODATA = "[user] u_badptr: passing kernel pointer 0x00100000 to sys_write\n";
static const char msg_bad2[]   USER_RODATA = "[user] u_badptr: rejected with negative errno — pointer check OK\n";
static const char msg_bad3[]   USER_RODATA = "[user] u_badptr: accepted — copy_from_user FAILED!\n";
static const char msg_bad4[]   USER_RODATA = "[user] u_badptr: passing unmapped 0x08200000 to sys_write\n";
static const char msg_bad5[]   USER_RODATA = "[user] u_badptr: unmapped page rejected by page-table walk\n";
static const char msg_bad6[]   USER_RODATA = "[user] u_badptr: unmapped page accepted — walk FAILED!\n";

static const char msg_nl[]     USER_RODATA = "\n";
static const char msg_isa_ttbr[] USER_RODATA = "[user] u_iso_a : my TTBR0|ASID = ";
static const char msg_isb_ttbr[] USER_RODATA = "[user] u_iso_b : my TTBR0|ASID = ";

static const char msg_isa1[]   USER_RODATA = "[user] u_iso_a : wrote A to my 0x08100000, waiting for B\n";
static const char msg_isa2[]   USER_RODATA = "[user] u_iso_a : read back A — address spaces are separate\n";
static const char msg_isa3[]   USER_RODATA = "[user] u_iso_a : read back B — SHARED MEMORY, isolation FAILED!\n";

static const char msg_isb1[]   USER_RODATA = "[user] u_iso_b : wrote B to the same virtual address\n";
static const char msg_isb2[]   USER_RODATA = "[user] u_iso_b : read back B — my own page\n";
static const char msg_isb3[]   USER_RODATA = "[user] u_iso_b : read back A — SHARED MEMORY, isolation FAILED!\n";

static const char msg_dm1[]    USER_RODATA = "[user] u_demand : first write to data page 0x08100000 ...\n";
static const char msg_dm2[]    USER_RODATA = "[user] u_demand : data page OK, now recursing 8 levels (touching stack pages)\n";
static const char msg_dm3[]    USER_RODATA = "[user] u_demand : asking kernel for vm stats (buffer on an untouched page)\n";
static const char msg_dm_f[]   USER_RODATA = "[user] u_demand : faults        = ";
static const char msg_dm_p[]   USER_RODATA = "[user] u_demand : mapped pages  = ";
static const char msg_dm_ff[]  USER_RODATA = "[user] u_demand : frames free   = ";
static const char msg_dm_ft[]  USER_RODATA = "[user] u_demand : frames total  = ";
static const char msg_dm_er[]  USER_RODATA = "[user] u_demand : vm_stats syscall FAILED\n";

static const char msg_of1[]    USER_RODATA = "[user] u_overflow: writing the guard page 0x08101000 ...\n";
static const char msg_of2[]    USER_RODATA = "[user] u_overflow: still alive — guard page FAILED!\n";

static const char msg_sw1[]    USER_RODATA = "[user] u_swap  : free frames + 8 = pages to touch: ";
static const char msg_sw2[]    USER_RODATA = "[user] u_swap  : wrote all pages, now reading back (swapped pages fault in) ...\n";
static const char msg_sw3[]    USER_RODATA = "[user] u_swap  : read back OK — every page kept its pattern\n";
static const char msg_sw4[]    USER_RODATA = "[user] u_swap  : read back MISMATCH — swap corrupted a page!\n";
static const char msg_sw5[]    USER_RODATA = "[user] u_swap  : faults          = ";
static const char msg_sw6[]    USER_RODATA = "[user] u_swap  : mapped pages    = ";
static const char msg_sw7[]    USER_RODATA = "[user] u_swap  : frames free     = ";
static const char msg_sw8[]    USER_RODATA = "[user] u_swap  : swap out / in   = ";
static const char msg_sw9[]    USER_RODATA = "[user] u_swap  : swap used/total = ";
static const char msg_sw_err[] USER_RODATA = "[user] u_swap  : vm_stats syscall FAILED\n";
static const char msg_empty[]  USER_RODATA = "";

#define SAY(s)  usys_write((s), (u32)(sizeof(s) - 1))

/**
 * 用户态自己拼字符串: 输出 "prefix 0xXXXXXXXX suffix"。
 *
 * 用户代码没有 libc, printf 也在内核区, 所以十六进制转换得自己写。
 * 缓冲区放在用户栈上 —— 顺便又用了一次私有的栈页。
 */
static void USER_TEXT say_hex(const char *prefix, u32 val, const char *suffix)
{
    char buf[64];
    u32 n = 0;
    u32 i;

    for (i = 0; prefix[i] && n < 48; i++) {
        buf[n++] = prefix[i];
    }
    buf[n++] = '0';
    buf[n++] = 'x';
    for (i = 0; i < 8; i++) {
        u32 nib = (val >> (28u - i * 4u)) & 0xFu;
        buf[n++] = (char)((nib < 10u) ? ('0' + nib) : ('A' + nib - 10u));
    }
    for (i = 0; suffix[i] && n < 63; i++) {
        buf[n++] = suffix[i];
    }

    usys_write(buf, n);
}

/* =========================== 用户任务 =========================== */

/**
 * 等待信号量: 先阻塞, 被 u_worker post 之后才继续。
 * 用来验证"用户任务在内核里阻塞/被唤醒"这条路径。
 */
static void USER_TEXT u_waiter(void)
{
    SAY(msg_wait);
    usys_sem_wait(0);       /* 阻塞: 内核把它从就绪队列摘掉 */
    SAY(msg_woken);
    usys_exit(0);
}

/**
 * 正常系统调用: getpid + write + sem_post。
 * 顺便验证私有栈: 局部变量地址必须落在本任务的栈页里。
 */
static void USER_TEXT u_worker(void)
{
    volatile u32 stack_probe[4];
    u32 sp_addr;
    s32 pid;
    int ok;

    sp_addr = (u32)(uintptr_t)&stack_probe[0];
    stack_probe[0] = 0x5A5A1234u;
    stack_probe[1] = stack_probe[0] ^ 0xFFFFFFFFu;

    ok = (sp_addr >= USER_STACK_VADDR) && (sp_addr < USER_STACK_TOP) &&
         (stack_probe[0] == 0x5A5A1234u) &&
         (stack_probe[1] == 0xA5A5EDCBu);

    pid = usys_getpid();

    if (pid > 0 && ok) {
        SAY(msg_work1);
    } else {
        SAY(msg_stackbad);
    }

    usys_sem_post(0);       /* 唤醒 u_waiter */
    SAY(msg_work2);
    usys_exit(0);
}

/**
 * 越权访问: 读内核向量表所在的 0x00100000。
 * 内核区 AP=01, 用户态读 → Permission Fault → Data Abort,
 * 异常处理打印现场后杀掉本任务, 其他任务继续运行。
 */
static void USER_TEXT u_fault(void)
{
    volatile u32 *kernel_addr = (volatile u32 *)0x00100000u;
    u32 value;

    SAY(msg_fault1);

    value = *kernel_addr;   /* ← 这里触发 Data Abort */
    (void)value;

    SAY(msg_fault2);
    usys_exit(0);
}

/**
 * 两种非法指针:
 *   1. 内核地址 0x00100000 —— 范围检查就该拒掉
 *   2. 用户范围但没映射的 0x08200000 —— 只有走页表才能发现
 */
static void USER_TEXT u_badptr(void)
{
    s32 ret;

    SAY(msg_bad1);
    ret = usys_write((const char *)0x00100000u, 16);
    if (ret < 0) {
        SAY(msg_bad2);
    } else {
        SAY(msg_bad3);
    }

    SAY(msg_bad4);
    ret = usys_write((const char *)0x08200000u, 16);
    if (ret < 0) {
        SAY(msg_bad5);
    } else {
        SAY(msg_bad6);
    }

    usys_exit(0);
}

/**
 * 地址空间隔离验证 (A 侧)
 *
 * 往自己虚拟地址 0x08100000 写一个标记, 通知 B 也写同一个虚拟地址,
 * 等 B 跑完再读回来。如果两个任务共享物理页, 这里读到的是 B 的值。
 */
static void USER_TEXT u_iso_a(void)
{
    volatile u32 *slot = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;

    SAY(msg_isa1);
    say_hex(msg_isa_ttbr, usys_ttbr0(), msg_nl);
    *slot = 0xA1A1A1A1u;

    usys_sem_post(1);       /* 通知 B: 可以写了 */
    usys_sem_wait(2);       /* 等 B 写完 */

    if (*slot == 0xA1A1A1A1u) {
        SAY(msg_isa2);
    } else {
        SAY(msg_isa3);
    }

    usys_exit(0);
}

/**
 * 地址空间隔离验证 (B 侧)
 */
static void USER_TEXT u_iso_b(void)
{
    volatile u32 *slot = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;

    usys_sem_wait(1);       /* 等 A 先写 */
    SAY(msg_isb1);
    say_hex(msg_isb_ttbr, usys_ttbr0(), msg_nl);
    *slot = 0xB2B2B2B2u;
    usys_sem_post(2);       /* 告诉 A: 写完了 */

    if (*slot == 0xB2B2B2B2u) {
        SAY(msg_isb2);
    } else {
        SAY(msg_isb3);
    }

    usys_exit(0);
}

/**
 * 递归函数, 每层在栈上占约 2KB。
 *
 * 用来演示"栈页是按需分配的": 调 4 层只碰几页, 内核就只给这几页
 * 分配页框, 剩下的 1000 多 KB 虚拟栈一个物理页都不占。
 */
static u32 USER_TEXT touch_stack(u32 depth)
{
    volatile u8 page[1900];
    u32 i;

    page[0] = (u8)depth;
    for (i = 0; i < sizeof(page); i += 64) {
        page[i] = (u8)(depth + i);
    }

    if (depth > 0) {
        return touch_stack(depth - 1) + page[0];
    }
    return page[sizeof(page) - 1];
}

/**
 * 按需分页演示。
 *
 * 三条路径:
 *   1. 用户自己写未映射的数据页 → Data Abort → vm_fault → 重试指令
 *   2. 递归压栈 → 每跨一页触发一次缺页
 *   3. 把统计缓冲区放在一页从没碰过的栈上, 由内核 copy_to_user
 *      代为补页 (vm_touch), 这条路径不经过 Data Abort
 */
static void USER_TEXT u_demand(void)
{
    volatile u32 *slot = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;
    vm_stats_t *out = (vm_stats_t *)(uintptr_t)0x08180000u;   /* 高处栈页, 从没碰过 */
    u32 sum;
    s32 ret;

    SAY(msg_dm1);
    *slot = 0x1234ABCDu;        /* ← 第一次访问, 缺页 */
    sum = *slot;

    SAY(msg_dm2);
    sum += touch_stack(8);      /* ← 每跨一页缺一次 (8 层约 16KB) */
    if (sum == 0xFFFFFFFFu) {   /* 防止优化掉上面的读 */
        SAY(msg_dm_er);
    }

    SAY(msg_dm3);
    ret = usys_vm_stats(out);   /* 内核往未映射页写 → vm_touch 补页 */
    if (ret == 0) {
        say_hex(msg_dm_f,  out->faults,       msg_nl);
        say_hex(msg_dm_p,  out->pages,        msg_nl);
        say_hex(msg_dm_ff, out->frames_free,  msg_nl);
        say_hex(msg_dm_ft, out->frames_total, msg_nl);
    } else {
        SAY(msg_dm_er);
    }

    usys_exit(0);
}

/**
 * 栈溢出: 直接写保护页。
 *
 * 保护页 (L2[1]) 永远不映射, vm_fault 会明确拒绝, 内核打印
 * "stack overflow" 后杀掉本任务 —— 数据页不会被栈悄悄覆盖。
 */
static void USER_TEXT u_overflow(void)
{
    volatile u32 *guard = (volatile u32 *)(uintptr_t)USER_GUARD_VADDR;

    SAY(msg_of1);
    *guard = 0xDEADBEEFu;       /* ← Data Abort, 保护页拒绝映射 */
    SAY(msg_of2);
    usys_exit(0);
}

/**
 * 页面换出演示 (v0.6 阶段7)。
 *
 * 先问内核还有多少空闲页框, 然后故意多碰 8 页:
 *   - 前 N 页直接拿页框;
 *   - 页框用完之后, 每碰一页就先把本任务的一页换出到 swap 区,
 *     腾出页框, 再把新页映射进来;
 *   - 第二遍按顺序读回来, 被换出的页会缺页 → 从 swap 读回,
 *     同时可能又把别的页换出去。
 *
 * 最后读一遍校验和, 确认换出/换入没有丢数据。
 */
static void USER_TEXT u_swap(void)
{
    volatile u32 *base = (volatile u32 *)(uintptr_t)0x081F0000u;
    vm_stats_t st;
    u32 target;
    u32 i;
    u32 expect;
    u32 ok = 1;

    if (usys_vm_stats(&st) != 0) {
        SAY(msg_sw_err);
        usys_exit(0);
    }

    /*
     * 空闲页框 + 8: 保证至少触发 8 次换出, 又不超过 swap 区容量。
     * 上限 72 页, 留在栈区虚拟地址范围内 (0x08102000 以上)。
     */
    target = st.frames_free + 8u;
    if (target > 72u) {
        target = 72u;
    }
    if (target < 32u) {
        target = 32u;
    }

    SAY(msg_sw1);
    say_hex(msg_empty, target, msg_nl);

    /* 第一遍: 每页写一个带页号的标记, 页间相隔 4KB */
    for (i = 0; i < target; i++) {
        base[-(i * 1024u)] = 0xA5000000u | (i & 0xFFFFu);
    }

    SAY(msg_sw2);

    /* 第二遍: 读回来。换出的页在这里触发 swap-in */
    for (i = 0; i < target; i++) {
        expect = 0xA5000000u | (i & 0xFFFFu);
        if (base[-(i * 1024u)] != expect) {
            ok = 0;
        }
    }

    if (ok) {
        SAY(msg_sw3);
    } else {
        SAY(msg_sw4);
    }

    if (usys_vm_stats(&st) == 0) {
        say_hex(msg_sw5, st.faults,     msg_nl);
        say_hex(msg_sw6, st.pages,      msg_nl);
        say_hex(msg_sw7, st.frames_free, msg_nl);
        say_hex(msg_sw8, (st.swap_out << 16) | (st.swap_in & 0xFFFFu), msg_nl);
        say_hex(msg_sw9, (st.swap_used << 16) | (st.swap_total & 0xFFFFu), msg_nl);
    } else {
        SAY(msg_sw_err);
    }

    usys_exit(0);
}

/* =========================== fork + COW 演示 (v0.6 阶段8) =========================== */

static const char msg_fk1[] USER_RODATA = "[user] u_fork  : before fork, data page = ";
static const char msg_fk2[] USER_RODATA = "[user] u_fork  : fork() returned child pid = ";
static const char msg_fk3[] USER_RODATA = "[user] u_fork  : after child wrote, parent reads ";
static const char msg_fk4[] USER_RODATA = "[user] u_fork  : parent value CHANGED — COW FAILED!\n";
static const char msg_fk5[] USER_RODATA = "[user] u_fork_c: inherited value = ";
static const char msg_fk6[] USER_RODATA = "[user] u_fork_c: inherited value WRONG — fork FAILED!\n";
static const char msg_fk7[] USER_RODATA = "[user] u_fork_c: wrote 0x22222222 to my own copy\n";
static const char msg_fk8[] USER_RODATA = "[user] u_fork_c: pages copied since fork = ";
static const char msg_fk9[] USER_RODATA = "[user] u_fork  : pages copied since fork = ";
static const char msg_fk10[] USER_RODATA = "[user] u_fork  : fork() FAILED\n";

/**
 * fork + 写时复制演示。
 *
 *   父进程: 往数据页写 0x11111111 → fork → 等子进程退出 → 读回
 *   子进程: 读到 0x11111111 → 改成 0x22222222 → 退出
 *
 * 父子页表在 fork 后指向同一个物理帧, 且都是用户只读。子进程写
 * 数据页和栈页时触发 permission fault, vm_fault 复制一份私有副本。
 * 父进程最后仍应读到 0x11111111。
 */
static void USER_TEXT u_fork(void)
{
    volatile u32 *slot = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;
    vm_stats_t st;
    const u32 magic = 0x11111111u;
    s32 child;

    *slot = magic;
    SAY(msg_fk1);
    say_hex(msg_empty, magic, msg_nl);

    child = usys_fork();

    if (child == 0) {
        /* ===== 子进程 ===== */
        SAY(msg_fk5);
        say_hex(msg_empty, *slot, msg_nl);
        if (*slot != magic) {
            SAY(msg_fk6);
        }

        *slot = 0x22222222u;        /* ← 第一次写: COW 复制 */
        SAY(msg_fk7);

        if (usys_vm_stats(&st) == 0) {
            say_hex(msg_fk8, st.cow, msg_nl);
        }

        usys_sem_post(3);           /* 告诉父进程: 我改完退出了 */
        usys_exit(0);
    }

    if (child < 0) {
        SAY(msg_fk10);
        usys_exit(0);
    }

    /* ===== 父进程 ===== */
    SAY(msg_fk2);
    say_hex(msg_empty, (u32)child, msg_nl);

    usys_sem_wait(3);               /* 等子进程写完并退出 */

    SAY(msg_fk3);
    say_hex(msg_empty, *slot, msg_nl);
    if (*slot != magic) {
        SAY(msg_fk4);
    }

    if (usys_vm_stats(&st) == 0) {
        say_hex(msg_fk9, st.cow, msg_nl);
    }

    usys_exit(0);
}

/* =========================== exec 演示 (v0.6 阶段9) =========================== */

static const char msg_ex1[] USER_RODATA = "[user] u_exec  : before exec, data page = ";
static const char msg_ex2[] USER_RODATA = "[user] u_exec  : exec -> u_exec_new (private pages reset)\n";
static const char msg_ex3[] USER_RODATA = "[user] u_exec2 : after exec, data page = ";
static const char msg_ex4[] USER_RODATA = "[user] u_exec2 : value is 0, old address space is gone\n";
static const char msg_ex5[] USER_RODATA = "[user] u_exec2 : old value survived — exec FAILED\n";
static const char msg_ex6[] USER_RODATA = "[user] u_exec2 : writing 0x33333333 to the new data page\n";
static const char msg_ex7[] USER_RODATA = "[user] u_exec  : exec returned error ";

/**
 * exec 之后的新入口。
 *
 * 这个函数和 u_exec 在同一段用户代码里, 但它被调用时地址空间已经
 * 重置过: 数据页是新的（全 0）, 用户栈也是新的。所以这里读到的
 * 数据页必须是 0, 不是 u_exec 写进去的 0xAAAAAAAA。
 */
static void USER_TEXT u_exec_new(void)
{
    volatile u32 *slot = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;

    SAY(msg_ex3);
    say_hex(msg_empty, *slot, msg_nl);

    if (*slot == 0u) {
        SAY(msg_ex4);
    } else {
        SAY(msg_ex5);
    }

    SAY(msg_ex6);
    *slot = 0x33333333u;

    usys_exit(0);
}

static void USER_TEXT u_exec(void)
{
    volatile u32 *slot = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;
    s32 rc;

    *slot = 0xAAAAAAAAu;
    SAY(msg_ex1);
    say_hex(msg_empty, *slot, msg_nl);

    SAY(msg_ex2);
    rc = usys_exec((u32)(uintptr_t)u_exec_new);

    /* 成功时不会返回到这里; 返回了就说明 exec 失败 */
    SAY(msg_ex7);
    say_hex(msg_empty, (u32)rc, msg_nl);
    usys_exit(0);
}

/* =========================== ELF 加载演示 (v0.6 阶段11) =========================== */

static const char path_hello[] USER_RODATA = "/hello.elf";
static const char path_missing[] USER_RODATA = "/missing.elf";
static const char msg_elf0[] USER_RODATA = "[user] u_elf   : exec_elf(/missing.elf) should fail, old image stays alive\n";
static const char msg_elf1[] USER_RODATA = "[user] u_elf   : exec_elf(/hello.elf) — real ELF from RAM disk\n";
static const char msg_elf2[] USER_RODATA = "[user] u_elf   : exec_elf returned error ";
static const char msg_elf3[] USER_RODATA = "[user] u_elf   : (the loaded image exits itself, we should not be here)\n";
static const char msg_elf4[] USER_RODATA = "[user] u_elf   : child execs /hello.elf first (cold), parent waits then re-execs\n";
static const char msg_elf5[] USER_RODATA = "[user] u_elf   : fork failed ";
static const char msg_pc0[]  USER_RODATA = "[user] u_elf   : second exec of /hello.elf — text pages should hit the page cache\n";

/**
 * 从 RAM 盘加载独立编译的 /hello.elf。
 *
 * 这个任务本身的代码在共享的 .user_text 里; 加载成功后, 用户态
 * PC 会跳到 0x09000000, 执行的是另一个编译器单元链接出来的镜像。
 * 成功时 exec_elf 不返回, 所以后面的错误处理只有失败路径会走到。
 */
static void USER_TEXT u_elf(void)
{
    s32 rc;
    s32 pid;
    s32 status = 0;

    SAY(msg_elf0);
    rc = usys_exec_elf(path_missing);
    if (rc < 0) {
        SAY(msg_elf2);
        say_hex(msg_empty, (u32)rc, msg_nl);
    }

    /*
     * 阶段13: 分两次执行同一个文件。
     *
     *   子进程先 exec: 缺页时按页读文件, 只读代码页填进页缓存
     *   父进程 wait 到子进程退出, 再 exec 一次: 代码页直接命中缓存
     *
     * 第二次执行和第一次之间隔着一整个进程的生命周期, 缓存里的页
     * 仍然在 —— 这就是 page cache 和"进程私有页"的区别。
     */
    SAY(msg_elf4);
    pid = usys_fork();
    if (pid == 0) {
        SAY(msg_elf1);
        rc = usys_exec_elf(path_hello);

        SAY(msg_elf2);
        say_hex(msg_empty, (u32)rc, msg_nl);
        SAY(msg_elf3);
        usys_exit(0);
    }

    if (pid < 0) {
        SAY(msg_elf5);
        say_hex(msg_empty, (u32)pid, msg_nl);
        usys_exit(0);
    }

    usys_wait(pid, &status);

    SAY(msg_pc0);
    rc = usys_exec_elf(path_hello);

    SAY(msg_elf2);
    say_hex(msg_empty, (u32)rc, msg_nl);
    SAY(msg_elf3);
    usys_exit(0);
}

/* =========================== wait / 僵尸进程演示 (v0.6 阶段10) =========================== */

static const char msg_wt1[] USER_RODATA = "[user] u_wait  : child A exiting with code 7\n";
static const char msg_wt2[] USER_RODATA = "[user] u_wait  : parent blocks in wait(A) ...\n";
static const char msg_wt3[] USER_RODATA = "[user] u_wait  : wait returned pid = ";
static const char msg_wt4[] USER_RODATA = "[user] u_wait  : exit code = ";
static const char msg_wt5[] USER_RODATA = "[user] u_wait  : WRONG result — wait/exit status FAILED\n";
static const char msg_wt6[] USER_RODATA = "[user] u_wait  : child B exiting with code 9\n";
static const char msg_wt7[] USER_RODATA = "[user] u_wait  : B already exited; wait(0) finds a zombie\n";
static const char msg_wt8[] USER_RODATA = "[user] u_wait  : reaped pid = ";
static const char msg_wt9[] USER_RODATA = "[user] u_wait  : fork FAILED\n";

/**
 * 僵尸进程 + wait 演示。
 *
 *   子进程 A: 退出码 7。父进程紧接着 wait, 先阻塞, A 退出时被唤醒,
 *             此时 TCB 还在 (僵尸), wait 取走退出码后释放。
 *   子进程 B: 退出码 9。父进程先 yield 让 B 跑完, B 变成僵尸挂在
 *             那里; 父进程再 wait(0) 直接收尸。
 *
 * 两种路径覆盖了 wait 的两个分支: 阻塞等活的子进程, 以及回收已经
 * 退出的僵尸。
 */
static void USER_TEXT u_wait(void)
{
    s32 child;
    s32 r;
    s32 code;

    /* ---- 子进程 A: 父进程先 wait, 走阻塞唤醒 ---- */
    child = usys_fork();

    if (child == 0) {
        SAY(msg_wt1);
        usys_exit(7);
    }
    if (child < 0) {
        SAY(msg_wt9);
        usys_exit(0);
    }

    SAY(msg_wt2);
    code = -1;
    r = usys_wait(child, &code);

    SAY(msg_wt3);
    say_hex(msg_empty, (u32)r, msg_nl);
    SAY(msg_wt4);
    say_hex(msg_empty, (u32)code, msg_nl);
    if (r != child || code != 7) {
        SAY(msg_wt5);
    }

    /* ---- 子进程 B: 先退出变僵尸, 父进程再来收 ---- */
    child = usys_fork();

    if (child == 0) {
        SAY(msg_wt6);
        usys_exit(9);
    }
    if (child < 0) {
        SAY(msg_wt9);
        usys_exit(0);
    }

    usys_yield();              /* 让 B 先跑完退出 */

    SAY(msg_wt7);
    code = -1;
    r = usys_wait(0, &code);   /* 0 = 任意子进程 */

    SAY(msg_wt8);
    say_hex(msg_empty, (u32)r, msg_nl);
    SAY(msg_wt4);
    say_hex(msg_empty, (u32)code, msg_nl);
    if (code != 9) {
        SAY(msg_wt5);
    }

    usys_exit(0);
}

/* =========================== 信号演示 (v0.7 阶段2) =========================== */

static const char msg_sg1[] USER_RODATA =
    "[user] u_signal: install SIGUSR1 handler, then fork a sender\n";
static const char msg_sg2[] USER_RODATA =
    "[user] u_signal: handler ran, sig = ";
static const char msg_sg3[] USER_RODATA =
    "[user] u_signal: sigreturn restored the interrupted flow\n";
static const char msg_sg4[] USER_RODATA =
    "[user] u_signal: sender child reaped (exit 0)\n";
static const char msg_sg5[] USER_RODATA =
    "[user] u_signal: child without handler terminated with -12 (-SIGUSR2)\n";
static const char msg_sg6[] USER_RODATA =
    "[user] u_signal: WRONG result — signal delivery FAILED\n";
static const char msg_sg7[] USER_RODATA =
    "[user] u_signal: fork FAILED\n";
static const char msg_sg8[] USER_RODATA =
    "[user] u_signal: sigaction FAILED\n";

/* v0.7 阶段3: 屏蔽字 + 阻塞唤醒 */
static const char msg_sk1[] USER_RODATA =
    "[user] u_signal: block SIGUSR1, then send it to self\n";
static const char msg_sk2[] USER_RODATA =
    "[user] u_signal: handler did not run while blocked — mask OK\n";
static const char msg_sk3[] USER_RODATA =
    "[user] u_signal: unblock — the pending signal should fire now\n";
static const char msg_sk4[] USER_RODATA =
    "[user] u_signal: handler ran after unblock — sigprocmask OK\n";
static const char msg_sk5[] USER_RODATA =
    "[user] u_signal: block in sem_wait(0), child will signal us\n";
static const char msg_sk6[] USER_RODATA =
    "[user] u_signal: sem_wait returned -EINTR — blocked wakeup OK\n";
static const char msg_sk7[] USER_RODATA =
    "[user] u_signal: sem_wait was NOT interrupted — FAILED\n";
static const char msg_sk8[] USER_RODATA =
    "[user] u_signal: blocked signal leaked through — FAILED\n";

static const char msg_rt1[] USER_RODATA =
    "[user] u_signal: block SIGRTMIN, queue it 3 times\n";
static const char msg_rt2[] USER_RODATA =
    "[user] u_signal: RT signal stayed pending while blocked — mask OK\n";
static const char msg_rt3[] USER_RODATA =
    "[user] u_signal: handler ran 3 times after unblock — queue OK\n";
static const char msg_rt_bad[] USER_RODATA =
    "[user] u_signal: RT queue FAILED\n";
static const char msg_prio_ok[] USER_RODATA =
    "[user] u_signal: RT delivered before the lower-numbered USR1 — priority OK\n";
static const char msg_prio_bad[] USER_RODATA =
    "[user] u_signal: signal priority FAILED\n";
static const char msg_ign1[] USER_RODATA =
    "[user] u_signal: SIG_IGN on SIGUSR2, then kill self\n";
static const char msg_ign2[] USER_RODATA =
    "[user] u_signal: still alive — SIG_IGN dropped the signal\n";

/* v0.7 阶段5: 信号值 (sigqueue) */
static const char msg_q1[] USER_RODATA =
    "[user] u_signal: queue SIGUSR2 twice while blocked (1111 then 2222)\n";
static const char msg_q2[] USER_RODATA =
    "[user] u_signal: normal signal merged, first value kept — value OK\n";
static const char msg_q3[] USER_RODATA =
    "[user] u_signal: queue SIGRTMAX 3 times (C1 C2 C3)\n";
static const char msg_q4[] USER_RODATA =
    "[user] u_signal: RT values delivered in FIFO order — queue OK\n";
static const char msg_q5[] USER_RODATA =
    "[user] u_signal: sigqueue 0x42 to a forked child, it exits with that value\n";
static const char msg_q6[] USER_RODATA =
    "[user] u_signal: cross-task value delivered — sigqueue OK\n";
static const char msg_qbad[] USER_RODATA =
    "[user] u_signal: signal value test FAILED\n";

/* v0.7 阶段6: SA_NODEFER + sa_mask */
static const char msg_sa1[] USER_RODATA =
    "[user] u_signal: queue 3 RT signals, default handler mask\n";
static const char msg_sa2[] USER_RODATA =
    "[user] u_signal: handler never nested — auto-block OK\n";
static const char msg_sa3[] USER_RODATA =
    "[user] u_signal: reinstall with SA_NODEFER, nest 3 levels\n";
static const char msg_sa4[] USER_RODATA =
    "[user] u_signal: handler nested inside itself — SA_NODEFER OK\n";
static const char msg_sa5[] USER_RODATA =
    "[user] u_signal: install SIGUSR1 with sa_mask = SIGUSR2\n";
static const char msg_sa6[] USER_RODATA =
    "[user] u_signal: SIGUSR2 waited for the USR1 handler — sa_mask OK\n";
static const char msg_sabad[] USER_RODATA =
    "[user] u_signal: SA_NODEFER / sa_mask test FAILED\n";

/* v0.7 阶段8: siginfo / 队列满 / SA_RESETHAND */
static const char msg_in1[] USER_RODATA =
    "[user] u_signal: install SIGUSR1 with SA_SIGINFO, fork a sender\n";
static const char msg_in2[] USER_RODATA =
    "[user] u_signal: kill: si_pid = child, si_code = SI_USER, value 0 — OK\n";
static const char msg_in3[] USER_RODATA =
    "[user] u_signal: sigqueue self, expect SI_QUEUE and value 0x77\n";
static const char msg_in4[] USER_RODATA =
    "[user] u_signal: sigqueue: si_pid = self, si_code = SI_QUEUE — OK\n";
static const char msg_in5[] USER_RODATA =
    "[user] u_signal: unknown sa_flags rejected with -EINVAL\n";
static const char msg_inbad[] USER_RODATA =
    "[user] u_signal: siginfo test FAILED\n";
static const char msg_qf1[] USER_RODATA =
    "[user] u_signal: fill the RT queue (8 slots), the 9th must fail\n";
static const char msg_qf2[] USER_RODATA =
    "[user] u_signal: 9th sigqueue returned -EAGAIN, 8 delivered — OK\n";
static const char msg_qfbad[] USER_RODATA =
    "[user] u_signal: RT queue-full test FAILED\n";
static const char msg_rs1[] USER_RODATA =
    "[user] u_signal: SA_RESETHAND: first copy handled, second kills the child\n";
static const char msg_rs2[] USER_RODATA =
    "[user] u_signal: child died with -SIGRTMIN after one handler run — OK\n";
static const char msg_rsbad[] USER_RODATA =
    "[user] u_signal: SA_RESETHAND test FAILED\n";

/*
 * 信号处理函数返回后要执行的一小段代码。
 * 内核在进入 handler 之前把用户态 LR 改成这个地址, 所以 handler
 * 正常 return 就会落到这里, 再发 SVC SIGRETURN 让内核恢复现场。
 *
 * 必须是 naked 函数: SIGRETURN 靠 SP_usr 定位内核压好的信号帧,
 * 跳板只要 push/pop 一下 SP 就偏了。usys_* 声明了 lr clobber 之后,
 * 编译器会在普通函数里 push {lr}, 所以这里用纯汇编。
 */
static void USER_TEXT __attribute__((naked)) sig_tramp(void)
{
    __asm__ volatile (
        "mov r0, #0\n\t"
        "SVC #15\n\t"
        "1:\tB 1b\n\t"
    );
}

static void USER_TEXT sig_handler(int sig)
{
    volatile u32 *seen = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;

    *seen = (u32)sig;
}

/*
 * 阶段4 用的 handler: 记录投递顺序。
 *   p[0] = 已经跑到第几次
 *   p[1..] = 依次收到的信号号
 * 数据页只有 1 页, 这里最多记 4 次就够验证排队和优先级。
 */
static void USER_TEXT sig_handler_rt(int sig)
{
    volatile u32 *p = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;
    u32 i = p[0];

    /* 同 sig_handler_val: 先占槽位, 防止嵌套投递改掉计数 */
    p[0] = i + 1u;
    if (i < 4u) {
        p[1 + i] = (u32)sig;
    }
}

/*
 * 阶段5 的 handler: 记录 (信号号, 值) 对。
 * 内核进入 handler 时 r0 = sig, r1 = value。
 *   p[0]     = 次数
 *   p[1+2i]  = 第 i 次的信号号
 *   p[2+2i]  = 第 i 次的值
 */
static void USER_TEXT sig_handler_val(int sig, u32 val)
{
    volatile u32 *p = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;
    u32 i = p[0];

    /*
     * 先占好槽位再写。处理函数执行期间可能被 tick 打断, 而 MOS
     * 目前不会自动屏蔽"正在处理的这条信号", 于是同一个实时信号
     * 可能在 handler 里再嵌套投递一次。如果先读 p[0] 再分两次用,
     * 嵌套的 handler 会把计数改掉, 外层再按新计数写就越界/覆盖。
     */
    p[0] = i + 1u;
    if (i < 6u) {
        p[1 + 2u * i] = (u32)sig;
        p[2 + 2u * i] = val;
    }
}

/* 收到带值信号后直接以该值退出, 给父进程 wait 验证 */
static void USER_TEXT sig_exit_val(int sig, u32 val)
{
    (void)sig;
    usys_exit((s32)val);
}

/*
 * 阶段6: 记录 handler 的嵌套深度。
 *   p[0] = 当前深度
 *   p[1] = 达到过的最大深度
 *
 * handler 里做一次系统调用, 内核在 SVC 返回用户态之前会检查
 * pending; 如果没有屏蔽当前信号, 第二个信号就在这里嵌套进来。
 */
static void USER_TEXT sig_handler_nest(int sig, u32 val)
{
    volatile u32 *p = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;

    (void)sig;
    (void)val;

    p[0] = p[0] + 1u;
    if (p[0] > p[1]) {
        p[1] = p[0];
    }

    /*
     * SA_NODEFER 测试: 由 handler 自己再排同号信号, 一共三层。
     * 这样后两份一定在前一层 handler 已经进入之后才投递,
     * 深度计数器才量得到嵌套 —— 从外面提前排队的话, 后一份可能
     * 在 handler 第一条指令之前就被 IRQ 投递, 看起来像没嵌套。
     * 三层能跑稳, 也说明阶段6 的深层嵌套跑飞问题已经修掉。
     */
    if (p[3] != 0) {
        p[3] = p[3] - 1u;
        usys_sigqueue(0, SIGRTMIN, 0xE2u);
    }

    usys_yield();               /* 投递点 */

    p[0] = p[0] - 1u;
}

/*
 * 阶段6: 自定义 sa_mask 的观察点。
 *   p[0] = SIGUSR2 收到的次数
 *   p[1] = SIGUSR2 到达时是否正处在 SIGUSR1 的 handler 里
 *   p[2] = 当前是否在 SIGUSR1 handler 里
 */
static void USER_TEXT sig_handler_mask(int sig, u32 val)
{
    volatile u32 *p = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;

    (void)val;

    if (sig == SIGUSR1) {
        p[2] = 1;
        usys_yield();
        usys_yield();
        usys_yield();
        p[2] = 0;
    } else {
        p[0] = p[0] + 1u;
        p[1] = p[2];
    }
}

/*
 * 阶段8: SA_SIGINFO 的 handler。
 *   p[0] = 次数
 *   p[1] = 信号号
 *   p[2] = si_pid
 *   p[3] = si_code
 *   p[4] = si_value
 */
static void USER_TEXT sig_handler_info(int sig, mos_siginfo_t *info)
{
    volatile u32 *p = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;

    p[0] = p[0] + 1u;
    p[1] = (u32)sig;
    if (info != 0) {
        p[2] = (u32)info->si_pid;
        p[3] = (u32)info->si_code;
        p[4] = info->si_value;
    } else {
        p[2] = 0xFFFFFFFFu;
    }
}

/* 阶段8: SA_RESETHAND 测试用。handler 跑一次就给父进程的信号量加一 */
static void USER_TEXT sig_handler_reset(int sig, mos_siginfo_t *info)
{
    (void)sig;
    (void)info;
    usys_sem_post(4);
}

/**
 * 信号演示。
 *
 *   子进程 A: yield 若干次后给父进程发 SIGUSR1
 *   父进程  : 装好 handler 后轮转等信号; 收到后 handler 写私有数据页,
 *             sigreturn 回到等待循环, 父进程再 wait 回收子进程
 *   子进程 B: 把 SIGUSR2 恢复成默认动作后给自己发一个 —— 内核在
 *             kill 返回前按默认动作终止它, 父进程 wait 拿到 -12
 */
static void USER_TEXT u_signal(void)
{
    volatile u32 *seen = (volatile u32 *)(uintptr_t)USER_DATA_VADDR;
    s32 child;
    s32 code;
    s32 r;
    u32 i;
    u32 ok;

    *seen = 0;

    SAY(msg_sg1);
    if (usys_sigaction(SIGUSR1, sig_handler, sig_tramp) < 0) {
        SAY(msg_sg8);
        usys_exit(0);
    }

    /* ---- 子进程 A: 发 SIGUSR1 ---- */
    child = usys_fork();
    if (child == 0) {
        for (i = 0; i < 300; i++) {
            usys_yield();
        }
        usys_kill(usys_getppid(), SIGUSR1);
        usys_exit(0);
    }
    if (child < 0) {
        SAY(msg_sg7);
        usys_exit(0);
    }

    for (i = 0; i < 5000 && *seen == 0; i++) {
        usys_yield();
    }

    if (*seen == SIGUSR1) {
        SAY(msg_sg2);
        say_hex(msg_empty, *seen, msg_nl);
        SAY(msg_sg3);
    } else {
        SAY(msg_sg6);
    }

    code = -1;
    usys_wait(child, &code);
    SAY(msg_sg4);

    /* ---- 子进程 B: 默认动作终止 ---- */
    child = usys_fork();
    if (child == 0) {
        usys_sigaction(SIGUSR2, 0, 0);
        usys_kill(0, SIGUSR2);      /* 发给自己的信号在 kill 返回前生效 */
        usys_exit(7);               /* 不会执行到这里 */
    }
    if (child < 0) {
        SAY(msg_sg7);
        usys_exit(0);
    }

    code = 0;
    r = usys_wait(child, &code);
    if (r == child && code == -12) {
        SAY(msg_sg5);
    } else {
        SAY(msg_sg6);
    }

    /* ---- v0.7 阶段3: 屏蔽字 ---- */
    *seen = 0;
    SAY(msg_sk1);
    if (usys_sigprocmask(SIG_BLOCK, 1u << SIGUSR1, 0) < 0) {
        SAY(msg_sk7);
        usys_exit(0);
    }
    usys_kill(0, SIGUSR1);

    for (i = 0; i < 200; i++) {
        usys_yield();
    }

    if (*seen == 0) {
        SAY(msg_sk2);
    } else {
        SAY(msg_sk8);
    }

    SAY(msg_sk3);
    usys_sigprocmask(SIG_UNBLOCK, 1u << SIGUSR1, 0);

    /* 解除屏蔽的系统调用返回时, 内核在回用户态之前就把 handler 投递了 */
    if (*seen == SIGUSR1) {
        SAY(msg_sk4);
    } else {
        SAY(msg_sg6);
    }

    /* ---- v0.7 阶段3: 信号唤醒阻塞在信号量上的任务 ---- */
    SAY(msg_sk5);
    child = usys_fork();
    if (child == 0) {
        for (i = 0; i < 2000; i++) {
            usys_yield();
        }
        usys_kill(usys_getppid(), SIGUSR1);
        usys_exit(0);
    }
    if (child < 0) {
        SAY(msg_sg7);
        usys_exit(0);
    }

    *seen = 0;
    r = usys_sem_wait(0);       /* count=0, 阻塞; 被上面的子进程打断 */
    if (r == -EINTR && *seen == SIGUSR1) {
        SAY(msg_sk6);
    } else {
        SAY(msg_sk7);
    }

    code = 0;
    usys_wait(child, &code);

    /* ================= v0.7 阶段4: 排队 / SIG_IGN / 优先级 ================= */

    /*
     * 实时信号排队: 屏蔽 SIGRTMIN, 连发 3 次, 解除屏蔽后 handler
     * 应该跑 3 次。普通信号只会保留一份, 这个对比就是 POSIX 里
     * "标准信号会丢, 实时信号不会" 的那条规则。
     */
    SAY(msg_rt1);
    if (usys_sigaction(SIGRTMIN, sig_handler_rt, sig_tramp) < 0) {
        SAY(msg_sg7);
        usys_exit(0);
    }

    *seen = 0;
    usys_sigprocmask(SIG_SETMASK, 1u << SIGRTMIN, 0);
    usys_kill(0, SIGRTMIN);
    usys_kill(0, SIGRTMIN);
    usys_kill(0, SIGRTMIN);
    for (i = 0; i < 100; i++) {
        usys_yield();
    }
    if (*seen == 0) {
        SAY(msg_rt2);
    } else {
        SAY(msg_rt_bad);
    }

    usys_sigprocmask(SIG_SETMASK, 0, 0);
    for (i = 0; i < 100; i++) {
        usys_yield();
    }
    if (*seen == 3 && seen[1] == SIGRTMIN && seen[2] == SIGRTMIN &&
        seen[3] == SIGRTMIN) {
        SAY(msg_rt3);
    } else {
        SAY(msg_rt_bad);
    }

    /*
     * 优先级: 实时信号段 (24..31) 整体优先于普通信号。
     * 同时挂起 SIGUSR1 和 SIGRTMIN, RT 应该先投递, 即使 USR1
     * 的编号更小。
     */
    *seen = 0;
    /*
     * USR1 换成记录顺序的 handler, 否则它会覆盖掉计数。
     *
     * RTMIN 的 handler 里把 USR1 一起屏蔽掉 (v0.7 阶段6 的 sa_mask):
     * 不这么做的话, 如果 tick 恰好在 RT handler 刚进入、还没记录
     * 的时候到达, 嵌套的 USR1 会先写槽位, 记录的"顺序"就不代表
     * 内核的投递顺序了。屏蔽之后 RT handler 一定先完整跑完。
     */
    usys_sigaction(SIGUSR1, sig_handler_rt, sig_tramp);
    usys_sigaction_ex(SIGRTMIN, sig_handler_rt, sig_tramp,
                      0, 1u << SIGUSR1);
    usys_sigprocmask(SIG_SETMASK, (1u << SIGRTMIN) | (1u << SIGUSR1), 0);
    usys_kill(0, SIGUSR1);
    usys_kill(0, SIGRTMIN);
    usys_sigprocmask(SIG_SETMASK, 0, 0);
    for (i = 0; i < 100; i++) {
        usys_yield();
    }
    if (*seen == 2 && seen[1] == SIGRTMIN && seen[2] == SIGUSR1) {
        SAY(msg_prio_ok);
    } else {
        SAY(msg_prio_bad);
    }

    /*
     * SIG_IGN: 忽略 SIGUSR2 之后 kill 自己, 不能死。
     */
    SAY(msg_ign1);
    usys_sigaction(SIGUSR2, (void (*)(int))SIG_IGN, 0);
    usys_kill(0, SIGUSR2);
    for (i = 0; i < 50; i++) {
        usys_yield();
    }
    SAY(msg_ign2);

    /* ================= v0.7 阶段5: 信号值 (sigqueue) ================= */

    /*
     * 普通信号不排队: 屏蔽住 SIGUSR2, 连发两个值。解除屏蔽后
     * 只投递一次, 而且保留的是第一个值 (后到的 sigval 丢掉)。
     */
    SAY(msg_q1);
    usys_sigaction(SIGUSR2, (void (*)(int))sig_handler_val, sig_tramp);
    *seen = 0;
    usys_sigprocmask(SIG_SETMASK, 1u << SIGUSR2, 0);
    usys_sigqueue(0, SIGUSR2, 0x1111u);
    usys_sigqueue(0, SIGUSR2, 0x2222u);
    for (i = 0; i < 50; i++) {
        usys_yield();
    }
    if (*seen != 0) {
        SAY(msg_qbad);
    }

    usys_sigprocmask(SIG_SETMASK, 0, 0);
    for (i = 0; i < 100; i++) {
        usys_yield();
    }
    if (*seen == 1u && seen[1] == SIGUSR2 && seen[2] == 0x1111u) {
        SAY(msg_q2);
    } else {
        SAY(msg_qbad);
    }

    /*
     * 实时信号排队: 三个值应该按 FIFO 各投递一次。
     */
    SAY(msg_q3);
    usys_sigaction(SIGRTMAX, (void (*)(int))sig_handler_val, sig_tramp);
    *seen = 0;
    usys_sigprocmask(SIG_SETMASK, 1u << SIGRTMAX, 0);
    usys_sigqueue(0, SIGRTMAX, 0xC1u);
    usys_sigqueue(0, SIGRTMAX, 0xC2u);
    usys_sigqueue(0, SIGRTMAX, 0xC3u);
    usys_sigprocmask(SIG_SETMASK, 0, 0);
    for (i = 0; i < 100; i++) {
        usys_yield();
    }
    if (*seen == 3u &&
        seen[1] == SIGRTMAX && seen[2] == 0xC1u &&
        seen[3] == SIGRTMAX && seen[4] == 0xC2u &&
        seen[5] == SIGRTMAX && seen[6] == 0xC3u) {
        SAY(msg_q4);
    } else {
        SAY(msg_qbad);
    }

    /*
     * 跨任务: 子进程继承 handler, 父进程把 0x42 发过去,
     * 子进程的 handler 直接以这个值退出, 父进程 wait 拿到退出码。
     */
    SAY(msg_q5);
    usys_sigaction(SIGUSR1, (void (*)(int))sig_exit_val, sig_tramp);
    child = usys_fork();
    if (child == 0) {
        for (i = 0; i < 200; i++) {
            usys_yield();
        }
        usys_exit(0);               /* 没收到信号才会走到这里 */
    }
    if (child < 0) {
        SAY(msg_qbad);
    } else {
        for (i = 0; i < 2; i++) {
            usys_yield();
        }
        usys_sigqueue(child, SIGUSR1, 0x42u);

        code = -1;
        r = usys_wait(child, &code);
        if (r == child && code == 0x42) {
            SAY(msg_q6);
        } else {
            SAY(msg_qbad);
        }
    }

    /* ================= v0.7 阶段6: SA_NODEFER / sa_mask ================= */

    /*
     * 默认行为: 同一个实时信号排队 3 份, handler 里不会嵌套自己。
     * handler 每次记录当前深度和最大深度。
     */
    SAY(msg_sa1);
    usys_sigaction(SIGRTMIN, (void (*)(int))sig_handler_nest, sig_tramp);
    seen[0] = 0;
    seen[1] = 0;
    seen[3] = 0;                /* auto-block 测试不重排 */
    usys_sigprocmask(SIG_SETMASK, 1u << SIGRTMIN, 0);
    usys_sigqueue(0, SIGRTMIN, 0xE1u);
    usys_sigqueue(0, SIGRTMIN, 0xE2u);
    usys_sigqueue(0, SIGRTMIN, 0xE3u);
    usys_sigprocmask(SIG_SETMASK, 0, 0);
    for (i = 0; i < 200; i++) {
        usys_yield();
    }

    if (seen[0] == 0 && seen[1] == 1) {
        SAY(msg_sa2);
    } else {
        SAY(msg_sabad);
    }

    /* SA_NODEFER: 不自动屏蔽当前信号, 同一个信号可以嵌套 */
    SAY(msg_sa3);
    if (usys_sigaction_ex(SIGRTMIN, (void (*)(int))sig_handler_nest,
                          sig_tramp, SA_NODEFER, 0) < 0) {
        SAY(msg_sabad);
    }
    seen[0] = 0;
    seen[1] = 0;
    seen[3] = 2;                /* handler 里再排两份, 共三层 */
    usys_sigprocmask(SIG_SETMASK, 1u << SIGRTMIN, 0);
    usys_sigqueue(0, SIGRTMIN, 0xE1u);
    usys_sigprocmask(SIG_SETMASK, 0, 0);
    for (i = 0; i < 200; i++) {
        usys_yield();
    }

    if (seen[0] == 0 && seen[1] >= 3) {
        SAY(msg_sa4);
    } else {
        SAY(msg_sabad);
    }

    /*
     * sa_mask: SIGUSR1 的 handler 期间额外屏蔽 SIGUSR2。
     * 两个信号同时挂起时 USR1 先投递 (编号小), 它在 handler 里
     * 主动让出几次 CPU; 如果 sa_mask 生效, USR2 只能等 handler
     * 返回之后才进来。
     */
    SAY(msg_sa5);
    usys_sigaction_ex(SIGUSR1, (void (*)(int))sig_handler_mask, sig_tramp,
                      0, 1u << SIGUSR2);
    usys_sigaction(SIGUSR2, (void (*)(int))sig_handler_mask, sig_tramp);
    seen[0] = 0;
    seen[1] = 0xFFFFFFFFu;
    seen[2] = 0;
    usys_sigprocmask(SIG_SETMASK, (1u << SIGUSR1) | (1u << SIGUSR2), 0);
    usys_kill(0, SIGUSR1);
    usys_kill(0, SIGUSR2);
    usys_sigprocmask(SIG_SETMASK, 0, 0);
    for (i = 0; i < 200; i++) {
        usys_yield();
    }

    if (seen[0] == 1 && seen[1] == 0) {
        SAY(msg_sa6);
    } else {
        SAY(msg_sabad);
    }

    /* ================= v0.7 阶段8: siginfo / 队列满 / SA_RESETHAND ================= */

    /*
     * SA_SIGINFO: 子进程用 kill 发 SIGUSR1, handler 检查 si_pid 是不是
     * 子进程、si_code 是不是 SI_USER、value 是不是 0。
     */
    SAY(msg_in1);
    usys_sigaction_ex(SIGUSR1, (void (*)(int))sig_handler_info, sig_tramp,
                      SA_SIGINFO, 0);
    seen[0] = 0;
    seen[1] = 0;
    seen[2] = 0;
    seen[3] = 0xFFFFFFFFu;
    seen[4] = 0xFFFFFFFFu;

    child = usys_fork();
    if (child == 0) {
        for (i = 0; i < 300; i++) {
            usys_yield();
        }
        usys_kill(usys_getppid(), SIGUSR1);
        usys_exit(0);
    }
    if (child < 0) {
        SAY(msg_sg7);
        usys_exit(0);
    }

    for (i = 0; i < 5000 && seen[0] == 0; i++) {
        usys_yield();
    }
    if (seen[0] == 1u && seen[1] == SIGUSR1 && seen[2] == (u32)child &&
        seen[3] == SI_USER && seen[4] == 0u) {
        SAY(msg_in2);
    } else {
        SAY(msg_inbad);
    }
    code = -1;
    usys_wait(child, &code);

    /* sigqueue 自己: si_pid 是自己, si_code 是 SI_QUEUE, 值 0x77 */
    SAY(msg_in3);
    seen[0] = 0;
    seen[3] = 0xFFFFFFFFu;
    seen[4] = 0xFFFFFFFFu;
    usys_sigprocmask(SIG_SETMASK, 1u << SIGUSR1, 0);
    usys_sigqueue(0, SIGUSR1, 0x77u);
    usys_sigprocmask(SIG_SETMASK, 0, 0);
    for (i = 0; i < 200; i++) {
        usys_yield();
    }
    if (seen[0] == 1u && seen[2] == (u32)usys_getpid() &&
        seen[3] == SI_QUEUE && seen[4] == 0x77u) {
        SAY(msg_in4);
    } else {
        SAY(msg_inbad);
    }

    /* 未定义的 flags 位必须被拒绝, 不能静默忽略 */
    if (usys_sigaction_ex(SIGUSR2, (void (*)(int))sig_handler_info,
                          sig_tramp, 8u, 0) == -EINVAL) {
        SAY(msg_in5);
    } else {
        SAY(msg_inbad);
    }

    /*
     * 实时队列满: 8 个槽位全部占满之后, 第 9 次 sigqueue 返回 -EAGAIN。
     * 解除屏蔽后 8 份都要投递, 说明前 8 次没有丢。
     */
    SAY(msg_qf1);
    usys_sigaction(SIGRTMAX, (void (*)(int))sig_handler_val, sig_tramp);
    seen[0] = 0;
    usys_sigprocmask(SIG_SETMASK, 1u << SIGRTMAX, 0);
    ok = 1;
    for (i = 0; i < MOS_SIG_MAXPENDING; i++) {
        if (usys_sigqueue(0, SIGRTMAX, i + 1u) < 0) {
            ok = 0;
        }
    }
    r = usys_sigqueue(0, SIGRTMAX, 9u);
    usys_sigprocmask(SIG_SETMASK, 0, 0);
    for (i = 0; i < 300; i++) {
        usys_yield();
    }
    if (ok && r == -EAGAIN && seen[0] == (u32)MOS_SIG_MAXPENDING) {
        SAY(msg_qf2);
    } else {
        SAY(msg_qfbad);
    }

    /*
     * SA_RESETHAND: 子进程装 SA_RESETHAND 后排队两份 SIGRTMIN。
     * 第一份进 handler, 处置方式随即复位; handler 返回、屏蔽字恢复后
     * 第二份按默认动作把子进程杀掉, 父进程 wait 拿到 -SIGRTMIN。
     * 信号量 4 的计数用来证明 handler 确实跑过一次。
     */
    SAY(msg_rs1);
    child = usys_fork();
    if (child == 0) {
        usys_sigaction_ex(SIGRTMIN, (void (*)(int))sig_handler_reset, sig_tramp,
                          SA_RESETHAND | SA_SIGINFO, 0);
        usys_sigprocmask(SIG_SETMASK, 1u << SIGRTMIN, 0);
        usys_sigqueue(0, SIGRTMIN, 1u);
        usys_sigqueue(0, SIGRTMIN, 2u);
        usys_sigprocmask(SIG_SETMASK, 0, 0);
        usys_exit(7);               /* 第二份没杀掉的话会走到这里 */
    }
    if (child < 0) {
        SAY(msg_rsbad);
    } else {
        code = 0;
        r = usys_wait(child, &code);
        if (r == child && code == -SIGRTMIN && usys_sem_wait(4) == 0) {
            SAY(msg_rs2);
        } else {
            SAY(msg_rsbad);
        }
    }

    /* 恢复默认动作, 免得影响后面的 demo */
    usys_sigaction(SIGUSR1, 0, 0);
    usys_sigaction(SIGUSR2, 0, 0);
    usys_sigaction(SIGRTMIN, 0, 0);
    usys_sigaction(SIGRTMAX, 0, 0);
    usys_sigprocmask(SIG_SETMASK, 0, 0);

    usys_exit(0);
}

/* =========================== 内核侧初始化 =========================== */

extern u8 _user_text_start;
extern u8 _user_text_end;
extern u8 _l1_pool_start;
extern u8 _l1_pool_end;

/** 创建用户任务并打印它的地址空间 */
static task_t *create_user(const char *name, void (*entry)(void), u32 prio)
{
    task_t *t = task_create_user(entry, name, prio, STACK_4K, STACK_4K);

    if (!t) {
        printf("[FAIL] create %s\n", name);
        return NULL;
    }

    printf("[AS] %s slot=%u asid=%u prio=%u L1=%X L2=%X (data/stack on demand)\n",
           name, t->as_slot, t->asid, t->priority, t->l1_phys, t->l2_phys);
    return t;
}

/* =========================== 地址空间回收演示 (v0.6 阶段5) =========================== */

#define POOL_WAVE_TASKS 8

static const char msg_spawn[] USER_RODATA =
    "[user] u_spawn : running in a recycled address space\n";

/** 一个用完就退出的短命用户任务, 用来反复占用/释放地址空间槽位 */
static void USER_TEXT u_spawn(void)
{
    usys_write(msg_spawn, (u32)(sizeof(msg_spawn) - 1));
    usys_exit(0);
}

/**
 * 回收演示的驱动任务 (内核态, 最低优先级)。
 *
 * 前面的 6 个用户任务都是优先级 1, 这个任务是 30, 所以它一开始
 * 根本轮不到 —— 等那 6 个都退出了才开始跑, 这时候 8 个槽位理论上
 * 全都空着。
 *
 * 每一轮创建 8 个 u_spawn (正好占满所有槽位), 然后 yield 让它们跑完。
 * 只有槽位真的被归还了, 第二轮才能再创建 8 个 —— 阶段4 的顺序切分
 * 分配器到第一轮结束就用光 8 个槽位, 第二轮必然失败。
 */
static void pool_manager(void)
{
    u32 wave;
    u32 i;
    u32 made;
    u32 free_before;

    /*
     * 先让一次。最后一个退出的 demo 任务在退出那一刻还是
     * current_task, 延迟回收队列会跳过它; 这次 yield 进 schedule()
     * 时已经换成本任务, 它占的槽位才真正还回来。
     */
    task_yield();

    printf("\n[POOL] address-space recycling demo\n");
    printf("[POOL] free slots: %u / %u, free frames: %u / %u\n",
           user_space_free_count(), USER_SPACE_SLOTS,
           frame_free_count(), frame_total());

    for (wave = 0; wave < 3; wave++) {
        free_before = user_space_free_count();
        if (free_before < POOL_WAVE_TASKS) {
            printf("[POOL] wave %u: only %u slots free, stop\n",
                   wave, free_before);
            break;
        }

        /*
         * 优先级用 30, 和本任务相同。task_create 只在"新任务优先级更高"
         * 时立刻调度, 同优先级不会插队, 8 个任务才能同时挂着。
         */
        made = 0;
        for (i = 0; i < POOL_WAVE_TASKS; i++) {
            if (!task_create_user(u_spawn, "u_spawn", 30, STACK_4K, STACK_4K)) {
                break;
            }
            made++;
        }
        printf("[POOL] wave %u: created %u tasks, free slots now %u, free frames %u\n",
               wave, made, user_space_free_count(), frame_free_count());

        /*
         * 让出 CPU。本任务被移到同优先级队尾, 前面 8 个 u_spawn
         * 依次跑完并退出, 全部死掉之后调度器才会重新选中本任务。
         */
        task_yield();

        /*
         * 再让一次。最后一个 u_spawn 退出时, 它是 current_task,
         * 回收队列会跳过它; 这次 yield 进入 schedule() 时已经
         * 不是它了, 资源才真正还回来。
         */
        task_yield();

        printf("[POOL] wave %u: spawn tasks gone, free slots = %u, free frames = %u\n",
               wave, user_space_free_count(), frame_free_count());
    }

    {
        u32 hits, misses, live, evict, bypass;
        u32 reclaimed = 0;

        page_cache_stats(&hits, &misses, &live, &evict, &bypass);
        printf("[POOL] page cache: %u hits, %u misses, %u live, "
               "%u evictions, %u bypass\n",
               hits, misses, live, evict, bypass);
        printf("[POOL] frames free before cache reclaim: %u / %u\n",
               frame_free_count(), frame_total());

        /* 驻留的缓存页是干净的, 可以随时丢掉 —— 证明它们不是泄漏 */
        while (page_cache_reclaim() == 0) {
            reclaimed++;
        }
        printf("[POOL] page cache reclaimed %u idle pages at shutdown\n",
               reclaimed);
    }

    printf("[POOL] done — %u of %u slots free, %u of %u frames free\n",
           user_space_free_count(), USER_SPACE_SLOTS,
           frame_free_count(), frame_total());

    /*
     * 任务入口函数不能 return。
     * context_switch 里 lr 被填成入口地址, return 等于又跳回函数开头,
     * 再返回一次就飞到栈上的随机值去了。
     */
    while (1) {
        __asm__ volatile("WFI");
    }
}

void user_demo_init(void)
{
    printf("========================================\n");
    printf("  User Mode Demo (v0.7: fork/COW + exec + wait + lazy ELF + page cache\n");
    printf("                  + signals/siginfo + sigprocmask/EINTR)\n");
    printf("  user code : %X - %X (shared, read-only)\n",
           (u32)(uintptr_t)&_user_text_start,
           (u32)(uintptr_t)&_user_text_end);
    printf("  user data : %X (1 page, allocated on first touch)\n", USER_DATA_VADDR);
    printf("  guard     : %X (never mapped)\n", USER_GUARD_VADDR);
    printf("  user stack: %X - %X (up to %u KB, grows on demand)\n",
           USER_STACK_VADDR, USER_STACK_TOP, USER_STACK_MAX / 1024u);
    printf("  swap area : %u pages (%u KB), evict on frame exhaustion\n",
           swap_total(), swap_total() * 4u);
    printf("  L1 pool   : %X - %X (16KB each)\n",
           (u32)(uintptr_t)&_l1_pool_start,
           (u32)(uintptr_t)&_l1_pool_end);
    printf("========================================\n\n");

    /*
     * 创建顺序决定初始轮转顺序:
     * waiter 先跑并阻塞 → worker 唤醒它 → fault 打死自己 →
     * badptr 验证指针检查 → iso_a / iso_b 验证地址空间隔离
     */
    if (!create_user("u_waiter",  u_waiter,  1)) return;
    if (!create_user("u_worker",  u_worker,  1)) return;
    if (!create_user("u_fault",   u_fault,   1)) return;
    if (!create_user("u_badptr",  u_badptr,  1)) return;
    if (!create_user("u_iso_a",   u_iso_a,   1)) return;
    if (!create_user("u_iso_b",   u_iso_b,   1)) return;
    if (!create_user("u_fork",    u_fork,    1)) return;
    if (!create_user("u_exec",    u_exec,    1)) return;
    if (!create_user("u_elf",     u_elf,     1)) return;
    /*
     * u_wait 用优先级 2: 等前面 11 个 demo 都退出、槽位空出来之后
     * 再跑。fork 需要一个新的地址空间槽位, 12 个任务同时活着的时候
     * 槽位已经占满, 会直接拿到 -ENOMEM。
     */
    if (!create_user("u_wait",    u_wait,    2)) return;
    if (!create_user("u_signal",  u_signal,  2)) return;
    if (!create_user("u_demand",  u_demand,  1)) return;
    if (!create_user("u_overflow", u_overflow, 1)) return;
    if (!create_user("u_swap",    u_swap,    1)) return;

    printf("\n[DEMO] 14 user tasks created, each with its own L1 page table\n");

    /*
     * 回收演示的驱动任务: 内核态, 优先级 30 (只比 IDLE 高)。
     * 前面 6 个用户任务是优先级 1, 所以它要等它们全部退出才开始跑。
     */
    task_create(pool_manager, "pool_mgr", 30, STACK_4K);
}
