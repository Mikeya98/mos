/**
 * @file    timer.h
 * @brief   MOS RTOS Cortex-A9 私有定时器驱动
 *
 * 私有定时器是 Cortex-A9 每个核自带的定时器，独立于全局的 TTC。
 * 时钟源 = CPU_CLK / 2 (通常 400MHz @ 800MHz CPU)
 *
 * 寄存器基地址: 0xF8F00600
 */

#ifndef _MOS_TIMER_H
#define _MOS_TIMER_H

#include "lib/types.h"

/* =========================== 系统 tick =========================== */
#define TICK_MS         1         /* tick 间隔 (毫秒) */
#define TICK_HZ         1000      /* tick 频率 (Hz) */

/* =========================== 公共 API =========================== */

/**
 * @brief  初始化私有定时器为周期模式
 * @param  tick_ms  tick 间隔 (毫秒)
 *
 * 操作:
 *   1. 计算 LOAD 值 (timer_clk / 1000 * tick_ms - 1)
 *   2. 配置 CTRL: 使能 + 自动重装载 + IRQ 使能
 *   3. 通过 gic_enable_irq(29) 使能中断
 *
 * 调用此函数前必须先调 gic_init()
 */
void timer_init(u32 tick_ms);

/**
 * @brief  获取系统启动以来的 tick 总数
 * @return tick 计数 (每 tick = TICK_MS 毫秒)
 */
u32 timer_get_ticks(void);

/**
 * @brief  定时器中断服务程序 (ISR)
 *
 * 由 irq_handler 在确认中断 ID=29 后调用。
 * 职责:
 *   1. 清除定时器中断标志
 *   2. 系统 tick 计数 +1
 *   3. 遍历所有睡眠任务, 递减 sleep_ticks
 *   4. 检查是否需要调度 (更高优先级就绪?)
 */
void timer_isr(void);

/**
 * @brief  获取当前时间 (毫秒)
 * @return 启动以来经过的毫秒数
 */
INLINE u32 timer_get_ms(void)
{
    return timer_get_ticks() * TICK_MS;
}

#endif /* _MOS_TIMER_H */
