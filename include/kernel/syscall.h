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

/* =========================== 系统调用号 =========================== */
#define SYS_YIELD       0       /* 让出 CPU */
#define SYS_EXIT        1       /* 结束当前任务 */
#define SYS_WRITE       2       /* write(buf, len) → 串口 */
#define SYS_SLEEP       3       /* sleep(ms) */
#define SYS_GETPID      4       /* 返回当前任务 pid */
#define SYS_SEM_WAIT    5       /* sem_wait(sem_id) */
#define SYS_SEM_POST    6       /* sem_post(sem_id) */
#define SYS_MAX         7

/* =========================== 错误码 =========================== */
/* 采用 Linux 风格的负值返回, 用户态看到的是 -errno */
#define EINVAL          22      /* 无效参数 (信号量 id 越界等) */
#define EFAULT          14      /* 用户指针非法 */
#define EPERM           1       /* 权限不足 */

/* =========================== 用户地址范围 =========================== */
/* 与 kernel.lds / mmu.h 保持一致: 0x08000000 - 0x0A000000 */
#define USER_SPACE_BASE     0x08000000u
#define USER_SPACE_LIMIT    0x0A000000u

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
