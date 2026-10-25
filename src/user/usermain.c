/**
 * @file    usermain.c
 * @brief   用户态 demo 任务 (v0.6 阶段3) — 权限隔离 / 系统调用 / 异常恢复
 *
 * 这里的函数全部放在 .user_text 段 (链接到 0x08000000), 在 USR 模式下
 * 执行, 只能通过 SVC 进内核。不能调用 printf / memcpy 等内核函数,
 * 也不能直接读内核里的字符串 —— 内核区在页表里是 AP=01, 用户态访问
 * 会触发 Permission Fault (Data Abort)。
 *
 * 演示四个场景:
 *   u_waiter — 调 sem_wait 阻塞, 被 u_worker 唤醒
 *   u_worker — 正常系统调用: getpid / write / sem_post
 *   u_fault  — 直接读内核地址 → Data Abort → 任务被杀, 系统继续跑
 *   u_badptr — 把内核指针传给 sys_write → copy_from_user 返回 EFAULT
 */

#include "lib/types.h"
#include "lib/printf.h"
#include "kernel/task.h"
#include "kernel/syscall.h"
#include "user/user.h"
#include "user/user_syscall.h"

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

static const char msg_work1[]  USER_RODATA = "[user] u_worker: getpid() > 0, user stack in 0x08100000-0x0A000000\n";
static const char msg_work2[]  USER_RODATA = "[user] u_worker: sem_post(0) done, exiting\n";
static const char msg_stackbad[] USER_RODATA = "[user] u_worker: user stack check FAILED\n";

static const char msg_fault1[] USER_RODATA = "[user] u_fault : now reading kernel address 0x00100000 ...\n";
static const char msg_fault2[] USER_RODATA = "[user] u_fault : still alive — isolation FAILED!\n";

static const char msg_bad1[]   USER_RODATA = "[user] u_badptr: passing kernel pointer 0x00100000 to sys_write\n";
static const char msg_bad2[]   USER_RODATA = "[user] u_badptr: rejected with negative errno — pointer check OK\n";
static const char msg_bad3[]   USER_RODATA = "[user] u_badptr: accepted — copy_from_user FAILED!\n";

#define SAY(s)  usys_write((s), (u32)(sizeof(s) - 1))

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
 * 这三个都跑通, 说明 SVC 参数传递、返回值、信号量唤醒都正常。
 */
static void USER_TEXT u_worker(void)
{
    /* 验证用户栈: 地址必须落在 0x08100000-0x0A000000, 而且可读可写。
     * volatile 防止编译器把"写进去再读回来"优化掉。 */
    volatile u32 stack_probe[4];
    u32 sp_addr;
    s32 pid;
    int ok;

    sp_addr = (u32)(uintptr_t)&stack_probe[0];
    stack_probe[0] = 0x5A5A1234u;
    stack_probe[1] = stack_probe[0] ^ 0xFFFFFFFFu;

    ok = (sp_addr >= 0x08100000u) && (sp_addr < 0x0A000000u) &&
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
 * 如果这一行之后还能打印, 说明隔离失效。
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
 * 把内核地址当作用户缓冲区传给 sys_write。
 * 内核的 copy_from_user 先做范围检查, 直接返回 -EFAULT,
 * 不会替用户去读那块内存, 也不会因为读失败而崩溃。
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

    usys_exit();
}

/* =========================== 内核侧初始化 =========================== */

extern u8 _user_text_start;
extern u8 _user_text_end;
extern u8 _user_stack_base;
extern u8 _user_stack_end;

void user_demo_init(void)
{
    task_t *t;

    printf("========================================\n");
    printf("  User Mode Demo (v0.6 阶段3)\n");
    printf("  user code : %X - %X\n",
           (u32)(uintptr_t)&_user_text_start,
           (u32)(uintptr_t)&_user_text_end);
    printf("  user stack: %X - %X\n",
           (u32)(uintptr_t)&_user_stack_base,
           (u32)(uintptr_t)&_user_stack_end);
    printf("========================================\n\n");

    /* 创建顺序决定初始轮转顺序:
     * waiter 先跑并阻塞 → worker 唤醒它 → fault 打死自己 → badptr 退出 */
    t = task_create_user(u_waiter, "u_waiter", 1, STACK_4K, STACK_4K);
    if (!t) { printf("[FAIL] create u_waiter\n"); return; }

    t = task_create_user(u_worker, "u_worker", 1, STACK_4K, STACK_4K);
    if (!t) { printf("[FAIL] create u_worker\n"); return; }

    t = task_create_user(u_fault, "u_fault", 1, STACK_4K, STACK_4K);
    if (!t) { printf("[FAIL] create u_fault\n"); return; }

    t = task_create_user(u_badptr, "u_badptr", 1, STACK_4K, STACK_4K);
    if (!t) { printf("[FAIL] create u_badptr\n"); return; }

    printf("[DEMO] 4 user tasks created (all prio=1)\n");
}
