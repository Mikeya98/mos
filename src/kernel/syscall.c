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
 *   用户态传给内核的每个指针都要先按当前任务的页表检查。
 *   内核不能直接解引用用户指针 —— 用户可能传一个内核地址, 甚至传
 *   NULL/越界地址。copy_from_user/copy_to_user 是唯一的跨域数据通道。
 */

#include "kernel/syscall.h"
#include "kernel/task.h"
#include "ipc/semaphore.h"
#include "mm/mmu.h"
#include "mm/vm.h"
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

/*
 * 阶段4 之后每个任务有自己的页表, 用户区里不是每一页都映射过。
 * 所以不能只查地址范围, 必须走一遍当前任务的页表: mmu_user_access_ok
 * 会逐页检查 L1/L2 描述符, 同时按读/写区分 AP 权限。
 *
 * 阶段6 之后多了"合法但还没分配"的页。内核替用户读写内存时不会走
 * Data Abort (内核态踩 fault 是直接停机), 所以要先调 vm_touch 把这些
 * 页补上, 再做权限检查。顺序不能反:
 *   - 先 vm_touch 再查权限: 刚补的页是 AP=11, 一定能过;
 *   - 要是先查权限, 合法但未映射的页会被判成 EFAULT。
 *
 * vm_touch 只碰私有区里的合法页, 内核地址和用户代码区它一概不管,
 * 这些地址仍然由 mmu_user_access_ok 拒绝 —— u_badptr 那两个用例
 * 依赖的就是这一点。
 */

static int user_range_ok(u32 start, u32 len, int write)
{
    if (vm_touch(start, len, write) != VM_OK) {
        return 0;
    }
    return mmu_user_access_ok(start, len, write);
}

int copy_from_user(void *dst, const void *usrc, u32 len)
{
    u8 *d = (u8 *)dst;
    const volatile u8 *s = (const volatile u8 *)usrc;
    u32 i;

    if (!user_range_ok((u32)(uintptr_t)usrc, len, 0)) {
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

    if (!user_range_ok((u32)(uintptr_t)udst, len, 1)) {
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

    /*
     * 按块校验、按块拷贝。
     *
     * 阶段7 之后页框可以被换出: 如果先把整个 len 范围一次性 vm_touch
     * 再检查, 范围超过"页框 + swap 槽"容量时, 前面补上的页会在后面
     * 补页时被换出去, 整段权限检查就过不了。按块处理时每块只有几十
     * 字节, 最多涉及两页, 不会出现自己把自己换出去的情况。
     */
    while (done < len) {
        u32 chunk = len - done;
        if (chunk > sizeof(kbuf) - 1) {
            chunk = sizeof(kbuf) - 1;
        }
        if (copy_from_user(kbuf, ubuf + done, chunk) < 0) {
            printf("[SYS] write: pointer %X len=%u rejected → EFAULT\n",
                   (u32)(uintptr_t)(ubuf + done), chunk);
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

/**
 * 缺页/页框统计。
 *
 * 数据是内核态填的, 但要写进用户缓冲区 —— 这条路径本身就是
 * "内核替用户补页"的演示: 用户传一页从没碰过的栈地址进来,
 * copy_to_user 里的 vm_touch 会把页框补上, 然后才拷贝。
 */
static int sys_vm_stats(void *uout)
{
    vm_stats_t ks;

    /*
     * 先补页再取数, 顺序不能反。
     * 反过来的话, 补页本身产生的那次缺页不会被算进 faults, 用户
     * 看到的数字永远比实际少一 (第一版就是这么错的)。
     */
    if (vm_touch((u32)(uintptr_t)uout, (u32)sizeof(ks), 1) != VM_OK) {
        printf("[SYS] vm_stats: bad user pointer %X\n", (u32)(uintptr_t)uout);
        return -EFAULT;
    }

    vm_fill_stats(&ks);
    if (copy_to_user(uout, &ks, (u32)sizeof(ks)) < 0) {
        printf("[SYS] vm_stats: bad user pointer %X\n", (u32)(uintptr_t)uout);
        return -EFAULT;
    }
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

    case SYS_GET_TTBR0:
        /* 调试用: 用户任务读自己的 TTBR0 (页表基址 | ASID) */
        ret = (int)(mmu_current_l1() | mmu_current_asid());
        break;

    case SYS_VM_STATS:
        ret = sys_vm_stats((void *)(uintptr_t)frame[FRAME_R0]);
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
     * id 0: watcher/worker 的同步信号量, 初始计数 0。
     * 等待方先阻塞, 生产者 post 后唤醒 —— 正好演示用户态任务
     * 阻塞在信号量上、由内核调度切走的完整路径。
     *
     * id 1 / 2 (v0.6 阶段4): u_iso_a 和 u_iso_b 的先后顺序控制。
     */
    user_sem_used[0] = 1;
    user_sem_used[1] = 1;
    user_sem_used[2] = 1;
}
