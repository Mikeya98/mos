/**
 * @file    mmu.h
 * @brief   MOS RTOS MMU 接口 (ARMv7-A 页表与地址映射)
 *
 * v0.6 阶段1 只做 identity mapping + 开启 MMU, 把"地址翻译"这条
 * 通路先打通, 系统行为不变。
 *
 * v0.6 阶段3 加入权限隔离:
 *   - 内核区 (0x00100000 起) 映射为 AP=01: 仅特权态可访问
 *   - 用户区 (0x08000000 起) 映射为 AP=11: 用户态可访问
 *   - DACR 域 0 由 manager 改为 client, 让 AP 位真正生效
 */

#ifndef _MOS_MMU_H
#define _MOS_MMU_H

#include "lib/types.h"

/* =========================== 地址空间划分 =========================== */
/* 与 kernel.lds 中的 .user_text / .user_stacks 保持一致 */
#define KERNEL_BASE         0x00100000u
#define USER_BASE           0x08000000u     /* 用户代码/只读/数据 1MB */
#define USER_CODE_END       0x08100000u
#define USER_STACK_BASE     0x08100000u     /* 用户栈 31MB */
#define USER_STACK_END      0x0A000000u
#define USER_LIMIT          USER_STACK_END  /* 用户区上界 (不含) */

/**
 * @brief  建立 L1 页表并开启 MMU
 *
 * 映射策略 (identity mapping, 虚拟地址 == 物理地址):
 *   - 0x00100000 - 0x08000000 (127MB)   内核区, AP=01 仅特权
 *   - 0x08000000 - 0x08100000 (1MB)     用户代码/数据, AP=11, 可执行
 *   - 0x08100000 - 0x0A000000 (31MB)    用户栈, AP=11, XN (不可执行)
 *   - 0x0A000000 - 0x40000000 (856MB)   普通内存, AP=01 仅特权
 *   - 0xE0000000 - 0xFFFFFFFF (512MB)   外设, device + AP=01
 *   - 其余区域                          fault (访问触发 Data Abort)
 *
 * 调用时机: kernel_main 中 uart_init 之后 (此时 UART 已可用, 便于观察),
 *           gic_init 之前。
 * 调用之后 CPU 进入 MMU 使能状态, 但所有代码/数据地址不变。
 */
void mmu_init(void);

#endif /* _MOS_MMU_H */
