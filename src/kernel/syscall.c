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
#include "kernel/elf_user.h"
#include "ipc/semaphore.h"
#include "mm/mmu.h"
#include "mm/vm.h"
#include "mm/addr_space.h"
#include "fs/vfs.h"
#include "lib/printf.h"
#include "lib/irq.h"

/* fork/exec 返回用户态共用的跳板 (sched_asm.S) */
extern void fork_return_trampoline(void);

/* 用户态 SP / LR 的读写 (sched_asm.S, v0.6 阶段8 / v0.7 阶段2) */
extern void write_user_sp(u32 sp);
extern u32  read_user_lr(void);
extern void write_user_lr(u32 lr);

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
#define FRAME_R4        4
#define FRAME_R5        5
#define FRAME_LR        13      /* r0-r12 之后的返回地址 (fork/exec 需要) */
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
    /* v0.7 阶段3: 可能返回 -EINTR (信号打断了阻塞) */
    return sem_wait(&user_sems[id]);
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
 * @brief 读用户信号量的当前计数 (v0.7 阶段10)
 *
 * 主要给 demo/调试用: u_stop 要靠计数变化判断子进程真的停住了。
 * count 为负表示有任务在等待, 和错误码的负值会混淆, 所以这里
 * 只返回非负计数; 有等待者时返回 0。
 */
static int sys_sem_value(u32 id)
{
    if (id >= SYS_SEM_MAX || !user_sem_used[id]) {
        return -EINVAL;
    }
    return (user_sems[id].count > 0) ? (int)user_sems[id].count : 0;
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

/* =========================== 信号 (v0.7 阶段2) =========================== */

/*
 * 信号帧压在用户栈上, 布局 (24 个字, 96 字节):
 *   [0..12]  r0-r12
 *   [13]     被打断的用户 PC
 *   [14]     被打断时的用户 SP
 *   [15]     魔数 (sigreturn 校验用)
 *   [16]     被打断时的用户 LR
 *   [17]     进入 handler 前的屏蔽字
 *   [18..21] mos_siginfo_t (signo / code / pid / value)
 *   [22..23] 保留, 以后放 ucontext 或 si_addr
 *
 * 为什么 LR 也要存: LR 是 banked 寄存器, 进入 handler 时内核把它改成
 * tramp, 原值不存下来就恢复不了。
 */
#define SIG_FRAME_WORDS 24
#define SIG_FRAME_MAGIC 0x5349474Eu     /* "SIGN" */

/*
 * v0.7 阶段5: 信号帧第 17 个字存"进入 handler 前的屏蔽字"。
 * POSIX 默认在 handler 执行期间把当前信号自动屏蔽 (sa_mask 的语义),
 * SIGRETURN 时恢复。这样同一个实时信号就不会在它自己的 handler
 * 里嵌套投递, 把用户侧的状态改乱。
 */
#define SIG_FRAME_BLOCKED 17

/*
 * v0.7 阶段8: siginfo 接在屏蔽字后面。SA_SIGINFO 时 handler 的
 * 第二个参数就是 new_sp + SIG_FRAME_SI * 4。
 */
#define SIG_FRAME_SI      18
#define SIG_FRAME_SI_WORDS 4

static int user_code_addr_ok(u32 addr)
{
    if (addr >= USER_CODE_VADDR && addr < USER_PRIV_VADDR) {
        return 1;
    }
    if (addr >= USER_PROG_VADDR && addr < USER_PROG_LIMIT) {
        return 1;
    }
    return 0;
}

/**
 * @brief 安装信号处理函数 (v0.7 阶段6 起带 flags / sa_mask)
 *
 * flags 目前只认 SA_NODEFER。mask 是安装时给的附加屏蔽字,
 * handler 执行期间额外屏蔽这些信号; 默认还会屏蔽当前信号本身,
 * SA_NODEFER 可以关掉这一条。
 */
static int sys_sigaction_ex(task_t *cur, u32 sig, u32 handler, u32 tramp,
                            u32 flags, u32 mask)
{
    if (!cur || !cur->is_user) {
        return -EPERM;
    }
    if (sig == 0 || sig >= MOS_NSIG) {
        return -EINVAL;
    }
    /*
     * v0.7 阶段10: SIGKILL / SIGSTOP 不可捕获、不可忽略。
     * 装 handler 和设 SIG_IGN 都按无效参数拒绝。
     */
    if (sig == SIGKILL || sig == SIGSTOP) {
        return -EINVAL;
    }
    if (flags & ~(unsigned)SA_VALID_FLAGS) {
        return -EINVAL;
    }

    /*
     * v0.7 阶段4: handler == SIG_IGN 表示忽略这条信号。
     * 按 POSIX, 改成立即忽略时要把已经 pending 的那一份丢掉。
     */
    if (handler == SIG_IGN) {
        cur->sig_handler[sig] = 0;
        cur->sig_tramp[sig]   = 0;
        cur->sig_flags[sig]   = 0;
        cur->sig_mask[sig]    = 0;
        cur->sig_ignore      |= (1u << sig);
        cur->sig_pending     &= ~(1u << sig);
        cur->sig_count[sig]   = 0;
        cur->sig_value[sig]   = 0;              /* v0.7 阶段5: 值一起丢掉 */
        cur->sig_sender[sig]  = 0;              /* v0.7 阶段8: 发送方一起清 */
        cur->sig_code[sig]    = SI_USER;
        if (sig >= MOS_SIGRTMIN && sig <= MOS_SIGRTMAX) {
            cur->sig_rt_head[sig - MOS_SIGRTMIN] = 0;
        }
        return 0;
    }

    if (handler != 0) {
        if (!user_code_addr_ok(handler) || !user_code_addr_ok(tramp)) {
            return -EINVAL;
        }
    }

    cur->sig_handler[sig] = handler;
    cur->sig_tramp[sig]   = (handler != 0) ? tramp : 0;
    cur->sig_flags[sig]   = (handler != 0) ? (u8)(flags & SA_VALID_FLAGS) : 0;
    cur->sig_mask[sig]    = (handler != 0) ? (mask & 0xFFFFFFFEu) : 0;
    cur->sig_ignore      &= ~(1u << sig);
    return 0;
}

/* 老接口: 没有 flags / sa_mask, 行为保持上一阶段的样子 */
static int sys_sigaction(task_t *cur, u32 sig, u32 handler, u32 tramp)
{
    return sys_sigaction_ex(cur, sig, handler, tramp, 0, 0);
}

static int sys_kill(task_t *cur, u32 pid, u32 sig)
{
    s32 r;

    if (sig == 0 || sig >= MOS_NSIG) {
        return -EINVAL;
    }
    r = task_signal_send_info((s32)pid, sig, 0,
                              cur ? cur->pid : 0, SI_USER);
    return (r < 0) ? (int)r : 0;
}

/**
 * @brief SIGQUEUE: 发一个带值的信号 (v0.7 阶段5)
 *
 * 值随信号一起排队, 投递时放进 handler 的 r1 (r0 仍是信号号)。
 * 普通信号不排队, 已 pending 时后到的值丢弃; 实时信号按 FIFO 排。
 */
static int sys_sigqueue(task_t *cur, u32 pid, u32 sig, u32 value)
{
    s32 r;

    if (sig == 0 || sig >= MOS_NSIG) {
        return -EINVAL;
    }
    r = task_signal_send_info((s32)pid, sig, value,
                              cur ? cur->pid : 0, SI_QUEUE);
    return (r < 0) ? (int)r : 0;
}

/**
 * @brief SIGPROCMASK: 读/改信号屏蔽字 (v0.7 阶段3)
 *
 * @param how   SIG_BLOCK / SIG_UNBLOCK / SIG_SETMASK
 * @param set   要操作的位图
 * @param oldp  非 0 时把修改前的屏蔽字写回用户
 */
static int sys_sigprocmask(task_t *cur, u32 how, u32 set, u32 oldp)
{
    if (!cur || !cur->is_user) {
        return -EPERM;
    }
    if (how > SIG_SETMASK) {
        return -EINVAL;
    }

    if (oldp != 0) {
        u32 old = cur->sig_blocked;
        if (copy_to_user((void *)(uintptr_t)oldp, &old, sizeof(old)) < 0) {
            return -EFAULT;
        }
    }

    set &= 0xFFFFFFFEu;         /* 信号 0 保留 */

    if (how == SIG_BLOCK) {
        cur->sig_blocked |= set;
    } else if (how == SIG_UNBLOCK) {
        cur->sig_blocked &= ~set;
    } else {
        cur->sig_blocked = set;
    }
    return 0;
}

/**
 * @brief SIGSUSPEND: 原子地替换屏蔽字并挂起, 直到有可投递信号 (v0.7 阶段9)
 *
 * 它解决的是"检查 pending"和"睡眠"之间的窗口: 先用 sigprocmask 屏蔽、
 * 看一遍 pending、再 sleep 的话, 两步之间到达的信号会让任务睡死。
 * 这里换屏蔽字和挂起在同一个临界区里完成。
 *
 * 返回时恢复调用前的屏蔽字并返回 -EINTR; 信号本身在返回用户态的
 * 路上由 user_signal_check 投递, handler 跑完之后用户才看到这个返回值。
 */
static int sys_sigsuspend(task_t *cur, u32 set)
{
    u32 old;
    u32 flags;

    if (!cur || !cur->is_user) {
        return -EPERM;
    }

    set &= 0xFFFFFFFEu;         /* 信号 0 保留 */

    flags = irq_save();

    old = cur->sig_blocked;
    cur->sig_blocked = set;

    /*
     * 换完屏蔽字之后已经有一条能送达的信号: 不睡, 直接恢复。
     * 返回用户态时投递, 语义和"刚睡下就被唤醒"一样。
     */
    if (cur->sig_pending & ~cur->sig_blocked & ~cur->sig_ignore &
        0xFFFFFFFEu) {
        cur->sig_blocked = old;
        irq_restore(flags);
        return -EINTR;
    }

    cur->sig_suspended = 1;
    cur->state         = TASK_BLOCKED;
    cur->block_obj     = NULL;
    cur->block_remove  = NULL;
    ready_dequeue(cur);

    irq_restore(flags);

    schedule();

    /*
     * task_signal_send_info (或子进程退出的 SIGCHLD 路径) 把任务放回
     * 就绪队列时已经清掉 sig_suspended。这里恢复旧屏蔽字、清掉
     * sig_interrupted —— sigsuspend 本来就要被信号唤醒, 不是"打断"。
     */
    flags = irq_save();
    cur->sig_blocked    = old;
    cur->sig_suspended  = 0;
    cur->sig_interrupted = 0;
    irq_restore(flags);

    return -EINTR;
}

/**
 * @brief SIGPENDING: 取当前待处理信号位图 (v0.7 阶段9)
 *
 * 只返回"已收到但还没投递"的信号, 被屏蔽的也算 (它们正等着解除
 * 屏蔽), 被 SIG_IGN 丢掉的不算。和 POSIX 的 sigpending 一致。
 */
static int sys_sigpending(task_t *cur, u32 up)
{
    u32 p;

    if (!cur || !cur->is_user) {
        return -EPERM;
    }

    p = cur->sig_pending & ~cur->sig_ignore & 0xFFFFFFFEu;

    if (up != 0 && copy_to_user((void *)(uintptr_t)up, &p,
                                (u32)sizeof(p)) < 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * @brief SIGRETURN: 从信号帧恢复被打断的用户现场
 *
 * 此时用户 SP 指向 handler 进入前内核压好的信号帧 (handler 正常
 * return 后 SP 回到入口值)。恢复 r0-r12 / PC / SP / LR 之后返回一个
 * 特殊值, 由调用者跳过 frame[0] 的覆盖 —— 否则会把恢复出来的
 * r0 又改写成 0。
 */
#define SIGRETURN_RESTORED  0x53494721      /* 内部标记, 不会返回给用户 */

static int sys_sigreturn(u32 *frame)
{
    task_t *cur = get_current();
    u32 kframe[SIG_FRAME_WORDS];
    u32 sp;
    u32 i;

    if (!cur || !cur->is_user) {
        return -EPERM;
    }

    sp = cur->user_sp;
    if (copy_from_user(kframe, (const void *)(uintptr_t)sp,
                       (u32)sizeof(kframe)) < 0) {
        return -EFAULT;
    }
    if (kframe[15] != SIG_FRAME_MAGIC) {
        printf("[SIG] %s: bad signal frame at %X\n", cur->name, sp);
        return -EINVAL;
    }

    for (i = 0; i < 13; i++) {
        frame[i] = kframe[i];
    }
    frame[FRAME_LR] = kframe[13];
    cur->user_sp    = kframe[14];

    /* v0.7 阶段5: 恢复 handler 执行前的屏蔽字 */
    cur->sig_blocked = kframe[SIG_FRAME_BLOCKED];

    /*
     * 恢复 banked SP/LR。写 LR 之后到异常返回之间不能调度,
     * 否则别的任务跑一轮会把用户态 LR 覆盖掉。
     */
    irq_save();
    write_user_sp(kframe[14]);
    write_user_lr(kframe[16]);

    return SIGRETURN_RESTORED;
}

/**
 * @brief 返回用户态前投递一个待处理信号
 *
 * SVC 和 IRQ 两条返回路径共用。选择编号最小的 pending 信号:
 *   - 没装 handler → 默认动作: 终止进程 (task_exit 永不返回)
 *   - 装了 handler → 现场压用户栈, 返回 handler 地址
 *
 * @param frame  r0-r12 + lr 的保存区
 * @param pc     本应返回的用户 PC
 * @return       实际返回 PC (handler 或原 pc)
 */
u32 user_signal_check(u32 *frame, u32 pc)
{
    task_t *cur = get_current();
    u32 sig;
    u32 deliverable;
    u32 handler;
    u32 tramp;
    u32 old_sp;
    u32 new_sp;
    u32 value = 0;
    u32 sender = 0;
    u32 si_code = SI_USER;
    u32 flags = 0;
    u32 kframe[SIG_FRAME_WORDS];
    u32 i;

    /*
     * v0.7 阶段3: 被屏蔽的信号留在 pending 里, 这里跳过。
     * 解除屏蔽后, 下一次从内核返回用户态时会正常投递。
     */
    if (!cur || !cur->is_user) {
        return pc;
    }

    deliverable = cur->sig_pending & ~cur->sig_blocked &
                  ~cur->sig_ignore & 0xFFFFFFFEu;
    if (deliverable == 0) {
        return pc;
    }

    /*
     * v0.7 阶段4: 实时信号优先于普通信号。POSIX 的规则是
     * "先按优先级, 同优先级按编号小的先投递"; 这里把 RT 段整体
     * 排在前面, 段内仍然取最小号。
     *
     * 普通信号取最小号 —— 和 Linux 上 SIGKILL(9) 先于 SIGUSR1(10)
     * 的直觉一致。
     */
    sig = 0;
    for (i = MOS_SIGRTMIN; i <= MOS_SIGRTMAX; i++) {
        if (deliverable & (1u << i)) {
            sig = i;
            break;
        }
    }
    if (sig == 0) {
        for (i = 1; i < MOS_SIGRTMIN; i++) {
            if (deliverable & (1u << i)) {
                sig = i;
                break;
            }
        }
    }
    if (sig == 0) {
        return pc;
    }

    /*
     * v0.7 阶段5: 取出这次投递携带的值。
     * 实时信号从环形队列头部取并推进队头; 普通信号只有一份,
     * 直接读 sig_value 后清掉。
     */
    {
        if (sig >= MOS_SIGRTMIN && sig <= MOS_SIGRTMAX) {
            u32 rt   = sig - MOS_SIGRTMIN;
            u32 head = cur->sig_rt_head[rt];
            u32 pos  = head % MOS_SIG_MAXPENDING;

            value   = cur->sig_rt_queue[rt][pos];
            sender  = cur->sig_rt_sender[rt][pos];
            si_code = cur->sig_rt_code[rt][pos];
            cur->sig_rt_head[rt] = (u8)((head + 1u) % MOS_SIG_MAXPENDING);
        } else {
            value   = cur->sig_value[sig];
            sender  = cur->sig_sender[sig];
            si_code = cur->sig_code[sig];
            cur->sig_value[sig]  = 0;
            cur->sig_sender[sig] = 0;
            cur->sig_code[sig]   = SI_USER;
        }

        /*
         * 每次投递消耗一份。实时信号还有下一份时保留 pending 位,
         * 下一次从 SVC/IRQ 返回用户态会接着投递; 普通信号只有一份,
         * 直接清位。
         */
        if (cur->sig_count[sig] > 1) {
            cur->sig_count[sig]--;
        } else {
            cur->sig_count[sig] = 0;
            cur->sig_pending &= ~(1u << sig);
        }

    }

    handler = cur->sig_handler[sig];

    if (handler == 0) {
        /*
         * v0.7 阶段9: SIGCHLD 的默认动作是"忽略"。pending 位和值
         * 在上面取队列时已经消费掉了, 直接回去继续跑。
         *
         * 其它信号仍然按默认动作终止进程 (task_exit 永不返回)。
         */
        if (sig == SIGCHLD) {
            return pc;
        }
        printf("[SIG] %s: signal %u has no handler, default = terminate\n",
               cur->name, sig);
        task_exit(-(s32)sig);
    }

    tramp  = cur->sig_tramp[sig];
    flags  = cur->sig_flags[sig];
    old_sp = cur->user_sp;
    new_sp = (old_sp - SIG_FRAME_WORDS * 4u) & ~0x7u;

    for (i = 0; i < 13; i++) {
        kframe[i] = frame[i];
    }
    kframe[13] = pc;
    kframe[14] = old_sp;
    kframe[15] = SIG_FRAME_MAGIC;
    kframe[SIG_FRAME_BLOCKED] = cur->sig_blocked;   /* 阶段5: 保存旧屏蔽字 */

    /*
     * v0.7 阶段8: 发送方信息跟着信号帧一起进用户栈。
     * SA_SIGINFO 的 handler 拿到的指针就指向这 4 个字, handler 里
     * 不管做什么系统调用, 这块内存都还在栈上, 不会被复用。
     */
    kframe[SIG_FRAME_SI + 0] = sig;
    kframe[SIG_FRAME_SI + 1] = si_code;
    kframe[SIG_FRAME_SI + 2] = sender;
    kframe[SIG_FRAME_SI + 3] = value;
    for (i = SIG_FRAME_SI + SIG_FRAME_SI_WORDS; i < SIG_FRAME_WORDS; i++) {
        kframe[i] = 0;
    }

    /*
     * v0.7 阶段6 修的坑: handler 的第二个参数必须等现场存进 kframe
     * 之后再写。嵌套投递时 frame 是被打断的那个 handler 的上下文,
     * 提前写 r1 会把它的局部变量覆盖掉, SIGRETURN 恢复回来就是错的值。
     *
     * v0.7 阶段8: SA_SIGINFO 时 r1 换成 siginfo 指针, r2 放 ucontext
     * (暂时没有实现, 置 0); 普通 handler 的 r1 仍然是信号值。
     */
    if (flags & SA_SIGINFO) {
        frame[FRAME_R1] = new_sp + SIG_FRAME_SI * 4u;
    } else {
        frame[FRAME_R1] = value;
    }
    frame[FRAME_R2] = 0;

    /*
     * 从这里到异常返回之间关中断:
     *   - 读到的是本任务被打断时的用户 LR;
     *   - 写完 banked SP/LR 之后不允许别的任务跑进用户态。
     * 异常返回会从 SPSR 恢复用户 CPSR, 中断自然重新打开。
     */
    irq_save();
    kframe[16] = read_user_lr();

    if (copy_to_user((void *)(uintptr_t)new_sp, kframe,
                     (u32)sizeof(kframe)) < 0) {
        printf("[SIG] %s: cannot build signal frame at %X\n",
               cur->name, new_sp);
        task_exit(-11);
    }

    /*
     * v0.7 阶段5/6: handler 期间的屏蔽字。
     *
     * 默认在旧屏蔽字上再加当前信号 (POSIX 行为), 同一个实时信号
     * 剩下的几份会等 SIGRETURN 恢复之后再投递; 安装时给的 sa_mask
     * 也一起加进去。SA_NODEFER 表示不自动屏蔽当前信号, 允许同一个
     * 信号嵌套进自己的 handler。
     */
    cur->sig_blocked |= cur->sig_mask[sig];
    if (!(flags & SA_NODEFER)) {
        cur->sig_blocked |= (1u << sig);
    }

    /*
     * v0.7 阶段8: SA_RESETHAND 在 handler 进入时复位处置方式。
     * 屏蔽字照常设置, 所以 handler 本身不会被打断; 但 SIGRETURN
     * 恢复屏蔽字之后, 还排着的副本就会按默认动作处理 —— 该终止
     * 就终止。复位放在屏蔽字计算之后, 不能影响本次投递。
     */
    if (flags & SA_RESETHAND) {
        cur->sig_handler[sig] = 0;
        cur->sig_tramp[sig]   = 0;
        cur->sig_flags[sig]   = 0;
        cur->sig_mask[sig]    = 0;
    }

    cur->user_sp = new_sp;
    write_user_sp(new_sp);
    write_user_lr(tramp);

    frame[FRAME_R0] = sig;

    printf("[SIG] %s: sig %u val %X from %u (%s) → handler %X "
           "(frame at %X, sp %X)\n",
           cur->name, sig, value, sender,
           (si_code == SI_QUEUE) ? "queue" :
           (si_code == SI_CHLD)  ? "chld"  : "kill",
           handler, new_sp, old_sp);
    return handler;
}

/* =========================== 分发 =========================== */

void svc_dispatch(u32 svc_num, u32 *frame)
{
    int ret = 0;
    int sigreturn_done = 0;

    /*
     * v0.6 阶段8: 进内核第一件事就是把 SP_usr 存进 TCB。
     * 系统调用可能阻塞/让出 CPU, 调度器切到别的用户任务时会改写
     * banked SP_usr, 不先保存的话, 本任务恢复运行后用户栈就错了。
     */
    task_save_user_sp();

    switch (svc_num) {
    case SYS_YIELD:
        task_yield();
        break;

    case SYS_EXIT:
        /*
         * 不返回: task_exit 会杀掉当前任务并切到下一个。
         * r0 是退出码, 父进程 wait 时能拿到。
         */
        task_exit((s32)frame[FRAME_R0]);
        break;

    case SYS_WRITE:
        ret = sys_write((const char *)(uintptr_t)frame[FRAME_R0],
                        frame[FRAME_R1]);
        break;

    case SYS_SLEEP:
        ret = task_sleep(frame[FRAME_R0]);
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

    case SYS_SEM_VALUE:
        ret = sys_sem_value(frame[FRAME_R0]);
        break;

    case SYS_GET_TTBR0:
        /* 调试用: 用户任务读自己的 TTBR0 (页表基址 | ASID) */
        ret = (int)(mmu_current_l1() | mmu_current_asid());
        break;

    case SYS_VM_STATS:
        ret = sys_vm_stats((void *)(uintptr_t)frame[FRAME_R0]);
        break;

    case SYS_FORK: {
        /*
         * 子进程从 fork_return_trampoline 返回用户态, 走的是
         * frame[FRAME_LR] 这个返回地址; 父进程在这里拿到子进程 pid。
         */
        task_t *child = task_fork(get_current(), frame[FRAME_LR], frame);
        ret = child ? (int)child->pid : -ENOMEM;
        break;
    }

    case SYS_WAIT: {
        /*
         * wait(pid, &code): pid=0 表示任意子进程。
         * 可能阻塞 —— task_wait 内部会把自己移出就绪队列,
         * 子进程退出时再唤醒重扫。
         */
        s32 code = 0;
        s32 r = task_wait((s32)frame[FRAME_R0], &code);

        if (r > 0) {
            u32 ustatus = frame[FRAME_R1];

            if (ustatus != 0) {
                if (copy_to_user((void *)(uintptr_t)ustatus, &code,
                                 (u32)sizeof(code)) < 0) {
                    r = -EFAULT;
                }
            }
        }
        ret = r;
        break;
    }

    case SYS_EXEC: {
        /*
         * 简化版 exec (v0.6 阶段9):
         *   - 只接受用户代码段内的入口地址 (0x08000000 - 0x080FFFFF)
         *   - 清空当前任务的私有页表和 swap 槽, 保留地址空间槽位/L1
         *   - 通过 fork_return_trampoline 回到用户态的新入口
         *
         * 地址空间重置后, 旧的数据页/栈页都没了, 新入口第一次访问
         * 会自动缺页分配 —— 这正是"换了一个新程序"的效果。
         */
        task_t *cur = get_current();
        u32 entry = frame[FRAME_R0];

        if (!cur || !cur->is_user) {
            ret = -EPERM;
            break;
        }
        if (entry < USER_CODE_VADDR || entry >= USER_PRIV_VADDR ||
            (entry & 3u) != 0) {
            ret = -EINVAL;
            break;
        }

        {
            user_space_t us;
            us.l1_phys      = cur->l1_phys;
            us.l2_phys      = cur->l2_phys;
            us.l2_code_phys = cur->l2_code_phys;
            us.asid         = cur->asid;
            us.slot         = cur->as_slot;
            user_space_reset(&us);
        }

        /* 新程序不再是文件映射的, 清掉旧的段表 */
        cur->prog_valid = 0;
        cur->elf_loads  = 0;

        cur->user_sp    = USER_STACK_TOP;
        cur->pf_count   = 0;
        cur->cow_count  = 0;
        cur->vm_clock   = 0;
        cur->swap_out   = 0;
        cur->swap_in    = 0;
        cur->entry      = (void (*)(void))(uintptr_t)entry;

        /*
         * 复用 fork 的返回跳板。跳板从 TCB 读 user_sp / entry,
         * 不再依赖 r4/r5 (v0.7 阶段8 起 r4-r11 必须原样保留)。
         */
        frame[FRAME_LR] = (u32)(uintptr_t)fork_return_trampoline;

        /*
         * 关键: svc_handler 返回时执行 MOVS pc, lr, 会同时把 SPSR_svc
         * 恢复到 CPSR。SPSR_svc 里现在是用户态的 CPSR, 直接返回会以
         * USR 模式跳到跳板地址, 而跳板在内核区 —— 取指异常。
         *
         * 把 SPSR_svc 改成 SVC 模式, 让异常返回先留在 SVC 跑到跳板;
         * 跳板再把 SPSR 设成 USR, 返回用户态的新入口。
         */
        {
            /*
             * v0.7 阶段7 修复: SPSR_svc 是 CPU 全局寄存器, 不跟任务走。
             * 这里改成 SVC 之后如果被时钟中断切走, 别的任务从系统调用
             * 返回时 MOVS pc, lr 会读到 SVC, 留在内核态执行用户代码。
             * 信号 handler 因此会在 SVC 模式跑, SIGRETURN 永远不执行,
             * 用户 SP 停在信号帧上。改 SPSR 到跳板执行之间必须关中断。
             */
            u32 svc_mode = 0x13u;
            ENTER_CRITICAL();
            __asm__ volatile ("MSR SPSR_cxsf, %0" :: "r"(svc_mode) : "memory");
        }

        /* 新程序不继承旧程序的信号处理函数 */
        {
            u32 si;
            for (si = 0; si < MOS_NSIG; si++) {
                cur->sig_handler[si] = 0;
                cur->sig_tramp[si]   = 0;
                cur->sig_flags[si]   = 0;
                cur->sig_mask[si]    = 0;
                cur->sig_count[si]   = 0;
                cur->sig_value[si]   = 0;
                cur->sig_sender[si]  = 0;
                cur->sig_code[si]    = SI_USER;
            }
            for (si = 0; si < MOS_RT_SIGS; si++) {
                u32 k;

                cur->sig_rt_head[si] = 0;
                for (k = 0; k < MOS_SIG_MAXPENDING; k++) {
                    cur->sig_rt_sender[si][k] = 0;
                    cur->sig_rt_code[si][k]   = SI_USER;
                }
            }
            cur->sig_pending = 0;
            cur->sig_ignore  = 0;
        }

        printf("[EXEC] %s: new image at %X, private pages reset\n",
               cur->name, entry);
        ret = 0;
        break;
    }

    case SYS_EXEC_ELF: {
        /*
         * 从文件系统装载一个独立编译的 ELF (v0.6 阶段11/12)。
         *
         * 阶段12: 只读 ELF 头部, 正文页在第一次访问时由 vm_prog_fault
         * 按页从文件读入 —— exec 的代价和程序大小无关。
         *
         * 路径字符串在用户内存里, 逐字节拷贝到内核缓冲 —— 用户
         * 字符串通常只有十几字节, 按固定长度 copy_from_user 会越过
         * 字符串尾部踩到别的页, 反而误判 EFAULT。
         */
        task_t *cur = get_current();
        char path[PROG_PATH_MAX];
        u32 i;
        int terminated = 0;
        user_space_t us;
        u32 entry = 0;
        int rc;

        if (!cur || !cur->is_user) {
            ret = -EPERM;
            break;
        }

        for (i = 0; i < sizeof(path) - 1u; i++) {
            char c;
            if (copy_from_user(&c,
                               (const void *)(uintptr_t)(frame[FRAME_R0] + i),
                               1) < 0) {
                ret = -EFAULT;
                break;
            }
            path[i] = c;
            if (c == '\0') {
                terminated = 1;
                break;
            }
        }
        if (ret < 0) {
            break;
        }
        if (!terminated) {
            ret = -EINVAL;
            break;
        }

        us.l1_phys      = cur->l1_phys;
        us.l2_phys      = cur->l2_phys;
        us.l2_code_phys = cur->l2_code_phys;
        us.asid         = cur->asid;
        us.slot         = cur->as_slot;

        rc = user_elf_load(&us, path, &cur->prog, &entry);
        if (rc != ELF_USER_OK) {
            printf("[EXEC] %s: load %s failed (%d), old image kept\n",
                   cur->name, path, rc);
            ret = (rc == ELF_USER_ENOMEM) ? -ENOMEM : -ENOEXEC;
            break;
        }

        cur->prog_valid = 1;
        cur->elf_loads  = 0;
        cur->user_sp    = USER_STACK_TOP;
        cur->entry      = (void (*)(void))(uintptr_t)entry;
        cur->pf_count   = 0;
        cur->cow_count  = 0;
        cur->vm_clock   = 0;
        cur->swap_out   = 0;
        cur->swap_in    = 0;

        /* 和 SYS_EXEC 一样, 复用 fork 跳板回到用户态的新入口 */
        frame[FRAME_LR] = (u32)(uintptr_t)fork_return_trampoline;
        {
            /*
             * v0.7 阶段7 修复: SPSR_svc 是 CPU 全局寄存器, 不跟任务走。
             * 这里改成 SVC 之后如果被时钟中断切走, 别的任务从系统调用
             * 返回时 MOVS pc, lr 会读到 SVC, 留在内核态执行用户代码。
             * 信号 handler 因此会在 SVC 模式跑, SIGRETURN 永远不执行,
             * 用户 SP 停在信号帧上。改 SPSR 到跳板执行之间必须关中断。
             */
            u32 svc_mode = 0x13u;
            ENTER_CRITICAL();
            __asm__ volatile ("MSR SPSR_cxsf, %0" :: "r"(svc_mode) : "memory");
        }

        /* 新程序不继承旧程序的信号处理函数 */
        {
            u32 si;
            for (si = 0; si < MOS_NSIG; si++) {
                cur->sig_handler[si] = 0;
                cur->sig_tramp[si]   = 0;
                cur->sig_flags[si]   = 0;
                cur->sig_mask[si]    = 0;
                cur->sig_count[si]   = 0;
                cur->sig_value[si]   = 0;
                cur->sig_sender[si]  = 0;
                cur->sig_code[si]    = SI_USER;
            }
            for (si = 0; si < MOS_RT_SIGS; si++) {
                u32 k;

                cur->sig_rt_head[si] = 0;
                for (k = 0; k < MOS_SIG_MAXPENDING; k++) {
                    cur->sig_rt_sender[si][k] = 0;
                    cur->sig_rt_code[si][k]   = SI_USER;
                }
            }
            cur->sig_pending = 0;
            cur->sig_ignore  = 0;
        }

        printf("[EXEC] %s: %s mapped, entry %X (code pages load on demand)\n",
               cur->name, path, entry);
        ret = 0;
        break;
    }

    case SYS_SIGACTION: {
        task_t *cur = get_current();
        ret = sys_sigaction(cur, frame[FRAME_R0], frame[FRAME_R1],
                            frame[FRAME_R2]);
        break;
    }

    case SYS_SIGACTION_EX: {
        task_t *cur = get_current();
        ret = sys_sigaction_ex(cur, frame[FRAME_R0], frame[FRAME_R1],
                               frame[FRAME_R2], frame[FRAME_R3],
                               frame[FRAME_R4]);
        break;
    }

    case SYS_KILL:
        ret = sys_kill(get_current(), frame[FRAME_R0], frame[FRAME_R1]);
        break;

    case SYS_SIGQUEUE:
        ret = sys_sigqueue(get_current(), frame[FRAME_R0], frame[FRAME_R1],
                           frame[FRAME_R2]);
        break;

    case SYS_SIGRETURN:
        ret = sys_sigreturn(frame);
        if (ret == SIGRETURN_RESTORED) {
            sigreturn_done = 1;
        }
        break;

    case SYS_GETPPID: {
        task_t *cur = get_current();
        ret = cur ? (int)cur->forked_from : 0;
        break;
    }

    case SYS_SIGPROCMASK:
        ret = sys_sigprocmask(get_current(), frame[FRAME_R0],
                              frame[FRAME_R1], frame[FRAME_R2]);
        break;

    case SYS_SIGSUSPEND:
        ret = sys_sigsuspend(get_current(), frame[FRAME_R0]);
        break;

    case SYS_SIGPENDING:
        ret = sys_sigpending(get_current(), frame[FRAME_R0]);
        break;

    default: {
        task_t *cur = get_current();
        printf("[SYS] unknown syscall %u from task %s\n",
               svc_num, cur ? cur->name : "?");
        ret = -EINVAL;
        break;
    }
    }

    /*
     * 返回值写回 r0, 异常返回后用户态从 r0 读结果。
     * SYS_SIGRETURN 已经把原始现场恢复进 frame, 不能再覆盖 r0。
     */
    if (!sigreturn_done) {
        frame[FRAME_R0] = (u32)ret;
    }

    /*
     * 返回用户态前投递 pending 信号 (v0.7 阶段2)。
     * 放在最后: 信号处理函数保存的 r0 就是这次系统调用的返回值,
     * sigreturn 恢复现场后用户态能正常拿到它。
     *
     * v0.7 阶段7: exec 路径为了先回到 SVC 跳板, 会把 SPSR_svc 临时
     * 改成 SVC。这种情况下不能投递信号 —— 异常返回会按 SPSR 留在
     * SVC, handler 就在内核态执行, 永远不会走 SIGRETURN。信号留在
     * pending 里, 等下一次从用户态返回再投递。
     */
    {
        u32 spsr_v;
        __asm__ volatile ("mrs %0, spsr" : "=r"(spsr_v));
        if ((spsr_v & 0x1Fu) == 0x10u) {
            frame[FRAME_LR] = user_signal_check(frame, frame[FRAME_LR]);
        }
    }
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
    user_sem_used[3] = 1;       /* v0.6 阶段8: u_fork 父子同步 */
    user_sem_used[4] = 1;       /* v0.7 阶段8: u_signal 验证 SA_RESETHAND */
    user_sem_used[5] = 1;       /* v0.7 阶段10: u_stop 子进程心跳 */
}
