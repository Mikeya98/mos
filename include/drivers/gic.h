/**
 * @file    gic.h
 * @brief   MOS RTOS GIC PL390 中断控制器驱动
 *
 * ARM GIC PL390 (Zynq-7045 兼容)。
 *
 * GIC 分为两部分:
 *   - 分配器 (Distributor): 全局, 管理所有中断源的使能和优先级
 *   - CPU 接口 (CPU Interface): 每核独立, 处理中断确认和结束
 */

#ifndef _MOS_GIC_H
#define _MOS_GIC_H

#include "lib/types.h"

/* =========================== 中断 ID 定义 =========================== */
#define IRQ_PTIMER      29   /* 私有定时器 (系统 tick) */
#define IRQ_WDT         30   /* 私有看门狗 */
#define IRQ_UART0       59   /* UART0 中断 (SPI) */
#define IRQ_SDIO0       79   /* SD 卡 0 (SPI, 后续使用) */

/* =========================== 公共 API =========================== */

/**
 * @brief  初始化 GIC
 *
 * 操作流程:
 *   1. 禁用分配器
 *   2. 禁用所有中断源
 *   3. 设所有中断为最低优先级
 *   4. 所有中断默认路由到 CPU0
 *   5. 使能分配器
 *   6. 使能 CPU 接口 (设置优先级阈值 = 0xFF, 接受所有优先级)
 */
void gic_init(void);

/**
 * @brief  使能指定中断号
 * @param  irq_id  中断 ID (0-1019)
 *
 * 操作:
 *   - 在 GICD_ISENABLER 中设置对应位
 *   - 中断号对应的优先级和目标 CPU 在 gic_init 中已设默认值
 */
void gic_enable_irq(u32 irq_id);

/**
 * @brief  禁用指定中断号
 * @param  irq_id  中断 ID
 */
void gic_disable_irq(u32 irq_id);

/**
 * @brief  设置中断优先级
 * @param  irq_id   中断 ID
 * @param  priority 优先级 (0=最高, 0xA0=最低)
 *
 * GIC 优先级: 值越小优先级越高, 0x00 最高, 0xA0 最低
 */
void gic_set_priority(u32 irq_id, u8 priority);

/**
 * @brief  获取当前中断 ID (读 GICC_IAR)
 * @return 中断 ID (0-1019), 或 1023 (无中断挂起)
 *
 * 在 ISR 入口调用, 获取后需在 ISR 出口调用 gic_send_eoi
 */
u32 gic_get_ack(void);

/**
 * @brief  发送中断结束信号 (写 GICC_EOIR)
 * @param  irq_id  之前 gic_get_ack 返回的中断 ID
 *
 * 必须在 ISR 最后调用, 否则同优先级及更低中断永远不会响应
 */
void gic_send_eoi(u32 irq_id);

#endif /* _MOS_GIC_H */
