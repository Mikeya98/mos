/**
 * @file    gic.c
 * @brief   MOS RTOS GIC PL390 中断控制器驱动实现
 *
 * 寄存器地址:
 *   GIC 分配器 (Distributor) 基址: 0xF8F00100
 *   GIC CPU 接口 (CPU Interface) 基址: 0xF8F01000
 *
 * 关键寄存器:
 *   GICD_CTRL       0xF8F01000  — 分配器控制 (bit0=使能)
 *   GICD_ISENABLER   0xF8F01100+ — 中断使能设置 (每 bit 对应一个中断 ID)
 *   GICD_ICENABLER   0xF8F01180+ — 中断使能清除
 *   GICD_IPRIORITYR  0xF8F01400+ — 中断优先级 (每字节对应一个中断 ID)
 *   GICD_ITARGETSR   0xF8F01800+ — 中断目标 CPU (每字节对应一个中断 ID)
 *   GICD_ICFGR       0xF8F01C00+ — 中断配置 (电平/边沿触发)
 *
 *   GICC_CTRL       0xF8F00100  — CPU 接口控制 (bit0=使能)
 *   GICC_PMR        0xF8F00104  — 优先级掩码 (高于此值的中断不响应)
 *   GICC_IAR        0xF8F0010C  — 中断确认 (读=获取当前中断 ID)
 *   GICC_EOIR       0xF8F00110  — 中断结束 (写=确认中断处理完毕)
 */

#include "drivers/gic.h"

/* =========================== 基地址 =========================== */
#define GIC_DIST_BASE   0xF8F01000UL   /* 分配器 */
#define GIC_CPU_BASE    0xF8F00100UL   /* CPU 接口 */

/* =========================== 寄存器访问 =========================== */
#define REG32(addr)  (*(volatile u32 *)(addr))

/* 分配器寄存器 */
#define GICD_CTRL        REG32(GIC_DIST_BASE + 0x000)
#define GICD_TYPER       REG32(GIC_DIST_BASE + 0x004)
#define GICD_ISENABLER(n) REG32(GIC_DIST_BASE + 0x100 + 4 * (n))
#define GICD_ICENABLER(n) REG32(GIC_DIST_BASE + 0x180 + 4 * (n))
#define GICD_IPRIORITYR(n) REG32(GIC_DIST_BASE + 0x400 + 4 * (n))
#define GICD_ITARGETSR(n)  REG32(GIC_DIST_BASE + 0x800 + 4 * (n))
#define GICD_ICFGR(n)      REG32(GIC_DIST_BASE + 0xC00 + 4 * (n))

/* CPU 接口寄存器 */
#define GICC_CTRL        REG32(GIC_CPU_BASE + 0x00)
#define GICC_PMR         REG32(GIC_CPU_BASE + 0x04)
#define GICC_IAR         REG32(GIC_CPU_BASE + 0x0C)
#define GICC_EOIR        REG32(GIC_CPU_BASE + 0x10)

/* =========================== 常量 =========================== */
#define GIC_MAX_IRQ      1020      /* 最多支持的中断 ID */
#define GIC_SPI_BASE     32        /* SPI 中断从 ID 32 开始 */
#define GIC_DEFAULT_PRIO 0xA0      /* 默认优先级 (最低) */
#define GIC_DEFAULT_TARGET 0x01    /* 默认目标 CPU0 */

/* =========================== 公共 API =========================== */

void gic_init(void)
{
    u32 i;
    u32 max_irq;

    /* 1. 禁用分配器 */
    GICD_CTRL = 0;

    /* 2. 读取 GIC 类型寄存器, 获取支持的最大中断数 */
    max_irq = (GICD_TYPER & 0x1F) + 1;
    max_irq *= 32;

    /* 3. 禁用所有中断源 (写 ICENABLER) */
    for (i = 0; i < max_irq / 32; i++) {
        GICD_ICENABLER(i) = 0xFFFFFFFF;
    }

    /* 4. 设所有中断为默认优先级 (最低 0xA0) */
    for (i = 0; i < max_irq / 4; i++) {
        GICD_IPRIORITYR(i) = 0xA0A0A0A0;
    }

    /* 5. 所有 SPI 中断默认路由到 CPU0 */
    for (i = 0; i < max_irq / 4; i++) {
        GICD_ITARGETSR(i) = GIC_DEFAULT_TARGET * 0x01010101;
    }

    /* 6. 所有中断默认为电平触发 (ICFGR 保持复位值 0) */

    /* 7. 使能分配器 */
    GICD_CTRL = 1;

    /* 8. 设 CPU 接口优先级掩码 = 0xFF (接受所有优先级中断) */
    GICC_PMR = 0xFF;

    /* 9. 使能 CPU 接口 */
    GICC_CTRL = 1;
}

void gic_enable_irq(u32 irq_id)
{
    u32 reg = irq_id / 32;
    u32 bit = irq_id % 32;
    GICD_ISENABLER(reg) = (1u << bit);
}

void gic_disable_irq(u32 irq_id)
{
    u32 reg = irq_id / 32;
    u32 bit = irq_id % 32;
    GICD_ICENABLER(reg) = (1u << bit);
}

void gic_set_priority(u32 irq_id, u8 priority)
{
    u32 reg  = irq_id / 4;
    u32 byte = (irq_id % 4) * 8;

    u32 val = GICD_IPRIORITYR(reg);
    val &= ~(0xFFu << byte);            /* 清除当前优先级 */
    val |= ((u32)priority << byte);     /* 设置新优先级 */
    GICD_IPRIORITYR(reg) = val;
}

u32 gic_get_ack(void)
{
    return GICC_IAR & 0x3FF;   /* 低 10 位是中断 ID */
}

void gic_send_eoi(u32 irq_id)
{
    GICC_EOIR = irq_id;
}
