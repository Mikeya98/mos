/**
 * @file    mmu.h
 * @brief   MOS RTOS MMU 接口 (ARMv7-A 页表与地址映射)
 *
 * v0.6 阶段 1 只做 identity mapping + 开启 MMU, 把"地址翻译"这条
 * 通路先打通, 系统行为不变。地址空间隔离/用户态在后续阶段做。
 */

#ifndef _MOS_MMU_H
#define _MOS_MMU_H

#include "lib/types.h"

/**
 * @brief  建立 L1 页表并开启 MMU
 *
 * 映射策略 (identity mapping, 虚拟地址 == 物理地址):
 *   - 0x00000000 - 0x40000000 (1GB)   普通内存, cacheable
 *   - 0xE0000000 - 0xFFFFFFFF (512MB) 外设, device (不可 cache)
 *   - 其余区域                          fault (访问触发 Data Abort)
 *
 * 调用时机: kernel_main 中 uart_init 之后 (此时 UART 已可用, 便于观察),
 *           gic_init 之前。
 * 调用之后 CPU 进入 MMU 使能状态, 但所有代码/数据地址不变。
 */
void mmu_init(void);

#endif /* _MOS_MMU_H */
