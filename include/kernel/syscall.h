/**
 * @file    syscall.h
 * @brief   MOS RTOS 系统调用框架 (v0.6 阶段3)
 *
 * 用户态任务不能直接调用内核函数, 只能通过 SVC 指令陷入内核。
 * 约定:
 *   - SVC 指令的 24 位立即数 = 系统调用号
 *   - 参数放在 r0-r3
 *   - 返回值写回 r0 (负数表示错误码)
 *
 * svc_handler (startup.S) 把用户态的 r0-r12 + lr 压到任务内核栈上,
 * 然后把栈帧指针交给 svc_dispatch, 这样:
 *   - 参数直接从 frame[0..3] 取
 *   - 返回值写 frame[0], 异常返回时 POP 到 r0
 */

#ifndef _MOS_SYSCALL_H
#define _MOS_SYSCALL_H

#include "lib/types.h"
#include "mm/mmu.h"         /* USER_SPACE_BASE / USER_SPACE_LIMIT */

/* =========================== 系统调用号 =========================== */
#define SYS_YIELD       0       /* 让出 CPU */
#define SYS_EXIT        1       /* 结束当前任务 */
#define SYS_WRITE       2       /* write(buf, len) → 串口 */
#define SYS_SLEEP       3       /* sleep(ms) */
#define SYS_GETPID      4       /* 返回当前任务 pid */
#define SYS_SEM_WAIT    5       /* sem_wait(sem_id) */
#define SYS_SEM_POST    6       /* sem_post(sem_id) */
#define SYS_GET_TTBR0   7       /* 返回当前任务的 TTBR0 (调试/验证用) */
#define SYS_VM_STATS    8       /* vm_stats(stat*) 取缺页/页框统计 */
#define SYS_FORK        9       /* fork() 复制当前进程 (v0.6 阶段8) */
#define SYS_EXEC        10      /* exec(entry) 重置地址空间并跳到新入口 (阶段9) */
#define SYS_WAIT        11      /* wait(pid, &code) 回收子进程 (阶段10) */
#define SYS_EXEC_ELF    12      /* exec_elf(path) 从文件系统加载 ELF (阶段11) */
#define SYS_SIGACTION   13      /* sigaction(sig, handler, tramp) (v0.7 阶段2) */
#define SYS_KILL        14      /* kill(pid, sig) */
#define SYS_SIGRETURN   15      /* 从信号处理函数返回 */
#define SYS_GETPPID     16      /* 返回父进程 pid, 没有父亲返回 0 */
#define SYS_SIGPROCMASK 17      /* sigprocmask(how, set, oldset) (v0.7 阶段3) */
#define SYS_SIGQUEUE    18      /* sigqueue(pid, sig, value) (v0.7 阶段5) */
#define SYS_SIGACTION_EX 19     /* sigaction_ex(sig, handler, tramp, flags, mask) (v0.7 阶段6) */
#define SYS_SIGSUSPEND  20      /* sigsuspend(set) 原子换屏蔽字并等信号 (v0.7 阶段9) */
#define SYS_SIGPENDING  21      /* sigpending(&set) 取待处理信号位图 (v0.7 阶段9) */
#define SYS_MAX         22

/* =========================== 信号 =========================== */
#define MOS_NSIG        32
#define SIGUSR1         10
#define SIGUSR2         12
#define SIGCHLD         17      /* 子进程退出时由内核发给父进程 (v0.7 阶段9) */

/*
 * v0.7 阶段4: 实时信号 + SIG_IGN
 *
 * POSIX 把 SIGRTMIN..SIGRTMAX 定义为"会排队"的信号: 同一信号连续
 * 发 3 次, handler 会跑 3 次; 普通信号只保留一份 (后面的丢弃)。
 * 这里取 24..31 作为实时信号段, MOS_SIG_MAXPENDING 是每条信号的
 * 最大排队深度 —— 真实内核一般不设上限, RTOS 里用一个固定数组更可控。
 *
 * SIG_IGN 借用 POSIX 的写法 (handler == 1): 传给 sigaction 表示
 * "忽略这条信号", 内核在 kill 时就把它丢掉, 不进 pending。
 */
#define SIG_IGN         1u
#define MOS_SIGRTMIN    24
#define MOS_SIGRTMAX    31
#define MOS_SIG_MAXPENDING 8

/*
 * v0.7 阶段6: sigaction 的 flags 和附加屏蔽字 (sa_mask)。
 *
 * 默认行为是"handler 执行期间自动屏蔽当前信号" (上一阶段的修复)。
 * 两种方式可以改:
 *   sa_mask   安装时指定一组信号, handler 期间额外屏蔽它们
 *   SA_NODEFER 不支持自定义屏蔽时反过来用 —— handler 里不再自动
 *             屏蔽当前信号, 同一个信号可以嵌套投递
 *
 * v0.7 阶段8 增加两位:
 *   SA_SIGINFO    handler 原型变成 handler(sig, siginfo_t *, void *),
 *                 第二个参数指向信号帧里的 mos_siginfo_t, 第三个参数
 *                 目前固定为 0 (没有实现 ucontext)。
 *   SA_RESETHAND  handler 进入后处置方式复位成默认动作。同一个实时
 *                 信号还有排队副本时, 从第二份开始按默认动作终止。
 *
 * 未定义的位仍然报 EINVAL, 不静默忽略。
 */
#define SA_NODEFER      1u
#define SA_SIGINFO      2u
#define SA_RESETHAND    4u
#define SA_VALID_FLAGS  (SA_NODEFER | SA_SIGINFO | SA_RESETHAND)

/*
 * v0.7 阶段8: 信号发送方信息 (对应 POSIX siginfo_t 里常用的四个字段)。
 *
 * 不算 si_uid/si_addr 这些 MOS 暂时用不到的东西, 只保留能填对的部分:
 *   si_signo  信号号
 *   si_code   SI_USER (kill) / SI_QUEUE (sigqueue)
 *   si_pid    发送者 pid
 *   si_value  sigqueue 携带的值, kill 发的是 0
 *
 * 结构体压在信号帧里 (见 syscall.c 的布局), SA_SIGINFO 时 handler 的
 * 第二个参数指向它。普通 handler 仍然只从 r1 拿 si_value。
 */
#define SI_USER         0
#define SI_QUEUE        1
#define SI_CHLD         2       /* 子进程退出 (v0.7 阶段9) */

typedef struct {
    u32 si_signo;
    s32 si_code;
    s32 si_pid;
    u32 si_value;
} mos_siginfo_t;

/* POSIX 习惯的别名 */
#define SIGRTMIN        MOS_SIGRTMIN
#define SIGRTMAX        MOS_SIGRTMAX
#define MOS_RT_SIGS     (MOS_SIGRTMAX - MOS_SIGRTMIN + 1)

/* sigprocmask 的 how */
#define SIG_BLOCK       0       /* 把 set 里的信号加入屏蔽字 */
#define SIG_UNBLOCK     1       /* 从屏蔽字里去掉 set */
#define SIG_SETMASK     2       /* 直接替换屏蔽字 */

/* =========================== 错误码 =========================== */
/* 采用 Linux 风格的负值返回, 用户态看到的是 -errno */
#define EINVAL          22      /* 无效参数 (信号量 id 越界等) */
#define EFAULT          14      /* 用户指针非法 */
#define ENOMEM          12      /* 内存不足 (fork 地址空间/页框耗尽) */
#define EPERM           1       /* 权限不足 */
#define ENOENT          2       /* 文件不存在 */
#define ENOEXEC         8       /* 不是可加载的可执行文件 */
#define ECHILD          10      /* 没有这个子进程 (wait) */
#define ESRCH           3       /* 没有这个进程 (kill) */
#define EINTR           4       /* 阻塞的系统调用被信号打断 (v0.7 阶段3) */
#define EAGAIN          11      /* 实时信号队列满 (v0.7 阶段8) */

/* =========================== 用户地址范围 =========================== */
/* 定义在 mm/mmu.h: 0x08000000 - 0x08111000 */

/**
 * @brief  初始化系统调用子系统 (注册内核态信号量等)
 *
 * 在 task_init() 之后、sched_start() 之前调用。
 */
void syscall_init(void);

/**
 * @brief  处理一次系统调用 (由 startup.S 的 svc_handler 调用)
 *
 * @param  svc_num  SVC 指令的低 24 位立即数
 * @param  frame    压栈的用户态上下文, frame[0]=r0 ... frame[12]=r12
 *
 * 返回值通过 frame[0] 回传给用户态。
 * SYS_EXIT 不会返回 (当前任务被销毁并切走)。
 */
void svc_dispatch(u32 svc_num, u32 *frame);

/**
 * @brief  返回用户态之前检查并投递信号 (v0.7 阶段2)
 *
 * @param  frame  用户态 r0-r12 + lr 的保存区 (SVC/IRQ 入口压在内核栈上的)
 * @param  pc     中断/系统调用后的用户返回地址
 * @return 实际应该返回的用户 PC: 没有信号就是 pc, 有信号就是 handler
 *
 * 有信号时会把用户现场 (r0-r12, pc, sp, lr) 压到用户栈上, 设置
 * SP_usr 指向信号帧, 把用户态 LR 设成 tramp, 然后返回 handler 地址。
 * handler 执行完返回时会跳到 tramp, 由 tramp 发 SVC SIGRETURN 恢复现场。
 */
u32 user_signal_check(u32 *frame, u32 pc);

/**
 * @brief  从用户空间拷贝数据到内核空间
 *
 * 先检查 [usrc, usrc+len) 是否完全落在用户区, 再逐字节拷贝。
 * 不做缺页处理: 用户区是 identity mapping 的连续普通内存,
 * 只要地址范围检查通过就不会触发异常。
 *
 * @return 0 成功, -EFAULT 指针非法
 */
int copy_from_user(void *dst, const void *usrc, u32 len);

/**
 * @brief  从内核空间拷贝数据到用户空间
 * @return 0 成功, -EFAULT 指针非法
 */
int copy_to_user(void *udst, const void *ksrc, u32 len);

#endif /* _MOS_SYSCALL_H */
