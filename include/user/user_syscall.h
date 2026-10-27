/**
 * @file    user_syscall.h
 * @brief   用户态系统调用封装 (v0.6 阶段3)
 *
 * 用户态没有 libc, 也不能调用任何内核函数。这里用内联汇编把
 * "把参数放进 r0-r3 + 执行 SVC #num" 封装成普通 C 函数的样子。
 *
 * 约定 (和 kernel/syscall.h 一致):
 *   SVC 立即数 = 系统调用号, r0-r3 = 参数, 返回值在 r0。
 *
 * 为什么必须是 always_inline:
 *   这些函数会被内联进 .user_text 里的任务函数。如果编译器给它们
 *   单独生成一份代码, 那份代码会落在内核 .text (AP=01), 用户态调用时
 *   就会取指异常。always_inline 保证不会出现这种"半内核半用户"的代码。
 *
 * 用户态寄存器约定:
 *   内核在 svc_handler 里保存/恢复 r0-r12, 只改写 r0 (返回值),
 *   所以 r1-r12 对用户态是透明的, 不需要额外声明 clobber。
 */

#ifndef _MOS_USER_SYSCALL_H
#define _MOS_USER_SYSCALL_H

#include "lib/types.h"
#include "kernel/syscall.h"

/* 系统调用号和下面的 SVC 立即数必须同步修改 */
#if SYS_YIELD != 0
#error "SVC immediate for SYS_YIELD is stale"
#endif
#if SYS_EXIT != 1
#error "SVC immediate for SYS_EXIT is stale"
#endif
#if SYS_WRITE != 2
#error "SVC immediate for SYS_WRITE is stale"
#endif
#if SYS_SLEEP != 3
#error "SVC immediate for SYS_SLEEP is stale"
#endif
#if SYS_GETPID != 4
#error "SVC immediate for SYS_GETPID is stale"
#endif
#if SYS_SEM_WAIT != 5
#error "SVC immediate for SYS_SEM_WAIT is stale"
#endif
#if SYS_SEM_POST != 6
#error "SVC immediate for SYS_SEM_POST is stale"
#endif
#if SYS_GET_TTBR0 != 7
#error "SVC immediate for SYS_GET_TTBR0 is stale"
#endif
#if SYS_VM_STATS != 8
#error "SVC immediate for SYS_VM_STATS is stale"
#endif

#define USYS_INLINE  static inline __attribute__((always_inline))

/** 让出 CPU (同优先级任务轮转) */
USYS_INLINE s32 usys_yield(void)
{
    register u32 r0 __asm__("r0") = 0;
    __asm__ volatile ("SVC #0" : "+r"(r0) : : "memory");
    return (s32)r0;
}

/** 结束当前任务, 永不返回 */
USYS_INLINE __attribute__((noreturn)) void usys_exit(void)
{
    register u32 r0 __asm__("r0") = 0;
    __asm__ volatile ("SVC #1" : "+r"(r0) : : "memory");
    /* 内核不会返回到这里; 万一返回也只是死循环, 不再碰用户态数据 */
    while (1) {
        __asm__ volatile ("NOP");
    }
}

/** 输出 len 字节到串口, 返回写入字节数或负错误码 */
USYS_INLINE s32 usys_write(const char *buf, u32 len)
{
    register u32 r0 __asm__("r0") = (u32)(uintptr_t)buf;
    register u32 r1 __asm__("r1") = len;
    __asm__ volatile ("SVC #2" : "+r"(r0) : "r"(r1) : "memory");
    return (s32)r0;
}

/** 睡眠 ms 毫秒 (依赖系统 tick) */
USYS_INLINE s32 usys_sleep(u32 ms)
{
    register u32 r0 __asm__("r0") = ms;
    __asm__ volatile ("SVC #3" : "+r"(r0) : : "memory");
    return (s32)r0;
}

/** 取当前任务 pid */
USYS_INLINE s32 usys_getpid(void)
{
    register u32 r0 __asm__("r0") = 0;
    __asm__ volatile ("SVC #4" : "+r"(r0) : : "memory");
    return (s32)r0;
}

/** 等待信号量 (可能阻塞) */
USYS_INLINE s32 usys_sem_wait(u32 id)
{
    register u32 r0 __asm__("r0") = id;
    __asm__ volatile ("SVC #5" : "+r"(r0) : : "memory");
    return (s32)r0;
}

/** 释放信号量 */
USYS_INLINE s32 usys_sem_post(u32 id)
{
    register u32 r0 __asm__("r0") = id;
    __asm__ volatile ("SVC #6" : "+r"(r0) : : "memory");
    return (s32)r0;
}

/** 读取当前任务的 TTBR0 (页表基址), 调试/验证地址空间切换用 */
USYS_INLINE u32 usys_ttbr0(void)
{
    register u32 r0 __asm__("r0") = 0;
    __asm__ volatile ("SVC #7" : "+r"(r0) : : "memory");
    return r0;
}

/**
 * 取缺页/页框统计, 结果写进用户缓冲区 (out 指向 mm/vm.h 的 vm_stats_t)。
 *
 * 内核用 copy_to_user 写这块内存 —— 如果 out 指向一页还没碰过的栈,
 * 这次调用本身就会触发一次"内核代用户补页", 正好演示 vm_touch。
 */
USYS_INLINE s32 usys_vm_stats(void *out)
{
    register u32 r0 __asm__("r0") = (u32)(uintptr_t)out;
    __asm__ volatile ("SVC #8" : "+r"(r0) : : "memory");
    return (s32)r0;
}

#endif /* _MOS_USER_SYSCALL_H */
