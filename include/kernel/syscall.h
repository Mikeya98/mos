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
#define SYS_MAX         18

/* =========================== 信号 =========================== */
#define MOS_NSIG        32
#define SIGUSR1         10
#define SIGUSR2         12

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
