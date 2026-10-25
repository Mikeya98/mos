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
 *   0x08100000  私有数据页 (4KB, 每任务一块)
 *   0x08101000  私有栈 (64KB, 每任务一块)
 *
 * 演示六个场景:
 *   u_waiter — 调 sem_wait 阻塞, 被 u_worker 唤醒
 *   u_worker — 正常系统调用: getpid / write / sem_post
 *   u_fault  — 直接读内核地址 → Data Abort → 任务被杀, 系统继续跑
 *   u_badptr — 把内核指针/未映射地址传给 sys_write → copy_from_user 返回 EFAULT
 *   u_iso_a  — 独立地址空间: 写自己的 0x08100000, 等 u_iso_b 跑完再读回
 *   u_iso_b  — 独立地址空间: 写同一虚拟地址, 两边互不影响
 */

#include "lib/types.h"
#include "lib/printf.h"
#include "kernel/task.h"
#include "kernel/syscall.h"
#include "user/user.h"
#include "user/user_syscall.h"
#include "mm/mmu.h"

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

static const char msg_work1[]  USER_RODATA = "[user] u_worker: getpid() > 0, private stack at 0x08101000-0x08110FFF\n";
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
static const char msg_isa_ttbr[] USER_RODATA = "[user] u_iso_a : my TTBR0 = ";
static const char msg_isb_ttbr[] USER_RODATA = "[user] u_iso_b : my TTBR0 = ";

static const char msg_isa1[]   USER_RODATA = "[user] u_iso_a : wrote A to my 0x08100000, waiting for B\n";
static const char msg_isa2[]   USER_RODATA = "[user] u_iso_a : read back A — address spaces are separate\n";
static const char msg_isa3[]   USER_RODATA = "[user] u_iso_a : read back B — SHARED MEMORY, isolation FAILED!\n";

static const char msg_isb1[]   USER_RODATA = "[user] u_iso_b : wrote B to the same virtual address\n";
static const char msg_isb2[]   USER_RODATA = "[user] u_iso_b : read back B — my own page\n";
static const char msg_isb3[]   USER_RODATA = "[user] u_iso_b : read back A — SHARED MEMORY, isolation FAILED!\n";

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
    usys_exit();
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
    usys_exit();
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
    usys_exit();
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

    usys_exit();
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

    usys_exit();
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

    usys_exit();
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

    printf("[AS] %s L1=%X L2=%X data=%X stack=%X\n",
           name, t->l1_phys, t->l2_phys, t->data_phys, t->stack_phys);
    return t;
}

void user_demo_init(void)
{
    printf("========================================\n");
    printf("  User Mode Demo (v0.6 阶段4)\n");
    printf("  user code : %X - %X (shared, read-only)\n",
           (u32)(uintptr_t)&_user_text_start,
           (u32)(uintptr_t)&_user_text_end);
    printf("  user data : %X (4KB per task)\n", USER_DATA_VADDR);
    printf("  user stack: %X - %X (64KB per task)\n",
           USER_STACK_VADDR, USER_STACK_TOP);
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

    printf("\n[DEMO] 6 user tasks created, each with its own L1 page table\n");
}
