/**
 * @file    syscall.c
 * @brief   MOS RTOS 系统调用实现 (v0.6 阶段3)
 *
 * 系统调用是用户态任务进入内核的唯一合法入口:
 *
 *   用户态                     内核态 (SVC)
 *   ------                     ------------
 *   SVC #num  ──────────────→  svc_handler
 *    r0-r3 = 参数               压栈 r0-r12+lr
 *                               svc_dispatch(num, frame)
 *                                 ├─ 参数校验 (用户指针范围)
 *                                 ├─ copy_from_user / copy_to_user
 *                                 └─ 调用内核函数
 *                               frame[0] = 返回值
 *   r0 = 返回值 ←────────────── POP {r0-r12, lr}; MOVS pc, lr
 *
 * 指针安全:
 *   用户态传给内核的每个指针都要先检查是否落在 0x08000000-0x0A000000。
 *   内核不能直接解引用用户指针 —— 用户可能传一个内核地址, 甚至传
 *   NULL/越界地址。copy_from_user/copy_to_user 是唯一的跨域数据通道。
 */

#include "kernel/syscall.h"
#include "kernel/task.h"
#include "ipc/semaphore.h"
#include "lib/printf.h"

/* =========================== 临界区宏 =========================== */
#define ENTER_CRITICAL()  __asm__ volatile("CPSID i" ::: "memory", "cc")
#define EXIT_CRITICAL()   __asm__ volatile("CPSIE i" ::: "memory", "cc")

/* =========================== 栈帧布局 =========================== */
/* startup.S 的 svc_handler 用 PUSH {r0-r12, lr} 保存用户态上下文,
 * 内存从低到高依次是 r0..r12, lr */
#define FRAME_R0        0
#define FRAME_R1        1
#define FRAME_R2        2
#define FRAME_R3        3
#define FRAME_COUNT     14      /* r0-r12 + lr */

/* =========================== 用户态信号量表 =========================== */
/* 用户态拿不到内核 sem_t 的指针, 只能用整数 id。内核启动时注册好,
 * 用户通过 SYS_SEM_WAIT / SYS_SEM_POST 操作。 */
#define SYS_SEM_MAX     8

static sem_t user_sems[SYS_SEM_MAX];
static u8    user_sem_used[SYS_SEM_MAX];

/* =========================== 指针校验 =========================== */

/**
 * @brief  检查 [start, start+len) 是否完全落在用户区
 *
 * 只检查范围和溢出。用户区是 identity mapping 的连续普通内存,
 * 范围合法就一定能访问, 不会触发缺页 —— 这也是当前不做 demand paging
 * 的好处, 内核里不需要处理"拷贝过程中缺页"的复杂情况。
 */
static int user_range_ok(u32 start, u32 len)
{
    u32 end = start + len;

    if (end < start) {                      /* 32 位加回绕 */
        return 0;
    }
    if (start < USER_SPACE_BASE) {
        return 0;
    }
    if (end > USER_SPACE_LIMIT) {
        return 0;
    }
    return 1;
}

int copy_from_user(void *dst, const void *usrc, u32 len)
{
    u8 *d = (u8 *)dst;
    const volatile u8 *s = (const volatile u8 *)usrc;
    u32 i;

    if (!user_range_ok((u32)(uintptr_t)usrc, len)) {
        return -EFAULT;
    }

    for (i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return 0;
}

int copy_to_user(void *udst, const void *ksrc, u32 len)
{
    volatile u8 *d = (volatile u8 *)udst;
    const u8 *s = (const u8 *)ksrc;
    u32 i;

    if (!user_range_ok((u32)(uintptr_t)udst, len)) {
        return -EFAULT;
    }

    for (i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return 0;
}

/* =========================== 各个系统调用 =========================== */

/**
 * @brief  write(buf, len) — 把用户缓冲区的内容输出到串口
 *
 * 分批拷贝到内核栈上的小缓冲区, 再交给 printf。
 * 不能直接把用户指针传给 printf: 那样内核会以特权身份读用户指针,
 * 指针合法性就没人检查了。
 */
static int sys_write(const char *ubuf, u32 len)
{
    char kbuf[64];
    u32 done = 0;

    if (!user_range_ok((u32)(uintptr_t)ubuf, len)) {
        printf("[SYS] write: pointer %X len=%u rejected → EFAULT\n",
               (u32)(uintptr_t)ubuf, len);
        return -EFAULT;
    }

    while (done < len) {
        u32 chunk = len - done;
        if (chunk > sizeof(kbuf) - 1) {
            chunk = sizeof(kbuf) - 1;
        }
        if (copy_from_user(kbuf, ubuf + done, chunk) < 0) {
            return -EFAULT;
        }
        kbuf[chunk] = '\0';
        printf("%s", kbuf);
        done += chunk;
    }
    return (int)len;
}

static int sys_getpid(void)
{
    task_t *cur = get_current();
    return cur ? (int)cur->pid : -1;
}

static int sys_sem_wait(u32 id)
{
    if (id >= SYS_SEM_MAX || !user_sem_used[id]) {
        return -EINVAL;
    }
    sem_wait(&user_sems[id]);
    return 0;
}

static int sys_sem_post(u32 id)
{
    if (id >= SYS_SEM_MAX || !user_sem_used[id]) {
        return -EINVAL;
    }
    sem_post(&user_sems[id]);
    return 0;
}

/* =========================== 分发 =========================== */

void svc_dispatch(u32 svc_num, u32 *frame)
{
    int ret = 0;

    switch (svc_num) {
    case SYS_YIELD:
        task_yield();
        break;

    case SYS_EXIT:
        /*
         * 不返回: task_exit 会杀掉当前任务并切到下一个。
         * 它之后如果意外返回, 也只是在死任务的内核栈上空转。
         */
        task_exit();
        break;

    case SYS_WRITE:
        ret = sys_write((const char *)(uintptr_t)frame[FRAME_R0],
                        frame[FRAME_R1]);
        break;

    case SYS_SLEEP:
        task_sleep(frame[FRAME_R0]);
        break;

    case SYS_GETPID:
        ret = sys_getpid();
        break;

    case SYS_SEM_WAIT:
        ret = sys_sem_wait(frame[FRAME_R0]);
        break;

    case SYS_SEM_POST:
        ret = sys_sem_post(frame[FRAME_R0]);
        break;

    default: {
        task_t *cur = get_current();
        printf("[SYS] unknown syscall %u from task %s\n",
               svc_num, cur ? cur->name : "?");
        ret = -EINVAL;
        break;
    }
    }

    /* 返回值写回 r0, 异常返回后用户态从 r0 读结果 */
    frame[FRAME_R0] = (u32)ret;
}

/* =========================== 初始化 =========================== */

void syscall_init(void)
{
    u32 i;

    for (i = 0; i < SYS_SEM_MAX; i++) {
        user_sem_used[i] = 0;
        sem_init(&user_sems[i], 0);
    }

    /*
     * id 0: demo 用的同步信号量, 初始计数 0。
     * 等待方先阻塞, 生产者 post 后唤醒 —— 正好演示用户态任务
     * 阻塞在信号量上、由内核调度切走的完整路径。
     */
    user_sem_used[0] = 1;
}
