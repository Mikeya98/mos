/**
 * @file    timer.c
 * @brief   MOS RTOS Cortex-A9 私有定时器驱动实现
 *
 * 私有定时器寄存器 (基址 0xF8F00600):
 *   0x00  LOAD     — 重装载值 (递减到 0 后自动回填)
 *   0x04  COUNTER  — 当前计数值 (只读, 递减)
 *   0x08  CTRL     — 控制寄存器
 *                   bit0 = Timer 使能
 *                   bit1 = 自动重装载 (Auto-Reload)
 *                   bit2 = IRQ 使能
 *   0x0C  ISR      — 中断状态 (bit0=1 表示中断挂起, 写 1 清除)
 *
 * 定时器时钟:
 *   如果 CPU = 800MHz, 则 Timer Clock = 400MHz
 *   1ms tick → LOAD = 400,000,000 / 1,000 - 1 = 399,999
 *
 * 注意: QEMU xilinx-zynq-a9 的 CPU 频率可能与硬件不同,
 * 需根据 QEMU 实际频率调整 (默认 QEMU CPU 频率较低)。
 * 为此, tick 配置接受 ms 值而不是直接写 LOAD 值,
 * 实际 LOAD 值由运行时校准或固定即可 (QEMU 上主要验证逻辑)。
 */

#include "drivers/timer.h"
#include "drivers/gic.h"

/* =========================== 寄存器 =========================== */
#define PTIMER_BASE     0xF8F00600UL

#define PTIMER_LOAD     REG32(PTIMER_BASE + 0x00)
#define PTIMER_COUNTER  REG32(PTIMER_BASE + 0x04)
#define PTIMER_CTRL     REG32(PTIMER_BASE + 0x08)
#define PTIMER_ISR      REG32(PTIMER_BASE + 0x0C)

/* CTRL 位 */
#define CTRL_ENABLE     (1 << 0)
#define CTRL_AUTORELOAD (1 << 1)
#define CTRL_IRQEN      (1 << 2)

/* =========================== 寄存器访问 =========================== */
#define REG32(addr)  (*(volatile u32 *)(addr))

/* =========================== 时钟频率 =========================== */
/*
 * QEMU xilinx-zynq-a9 的私有定时器频率。
 * 实际硬件: CPU_CLK / 2 = 400MHz
 * QEMU: 通常较低 (~50-100MHz 量级, 取决于 QEMU 版本)
 *
 * 这里设置一个 QEMU 上可用的频率: 50MHz
 * 上板时根据实际测量值调整
 */
#define TIMER_CLK_HZ    50000000UL   /* 50MHz (QEMU 兼容) */

/* =========================== 全局变量 =========================== */

/** 系统启动以来的 tick 总数 */
static volatile u32 _sys_ticks = 0;

/* =========================== 公共 API =========================== */

void timer_init(u32 tick_ms)
{
    /*
     * LOAD 值计算:
     *   ticks_per_ms = TIMER_CLK_HZ / 1000
     *   LOAD = ticks_per_ms * tick_ms - 1
     *
     * 减 1 是因为定时器从 LOAD 递减到 0 触发中断
     * (递减 0→触发 和 LOAD→0 总共 LOAD+1 个周期)
     */
    u32 ticks_per_ms = TIMER_CLK_HZ / 1000;
    u32 load_val = ticks_per_ms * tick_ms - 1;

    /* 1. 停止定时器 */
    PTIMER_CTRL = 0;

    /* 2. 设置重装载值 */
    PTIMER_LOAD = load_val;

    /* 3. 清除挂起的中断 */
    PTIMER_ISR = 1;

    /* 4. 使能定时器: 自动重装载 + IRQ 使能 + 启动 */
    PTIMER_CTRL = CTRL_AUTORELOAD | CTRL_IRQEN | CTRL_ENABLE;

    /* 5. 在 GIC 中使能私有定时器中断 (ID=29) */
    gic_enable_irq(IRQ_PTIMER);

    _sys_ticks = 0;
}

u32 timer_get_ticks(void)
{
    return _sys_ticks;
}

/*
 * timer_isr 的实现位于 task.c 中, 因为它需要访问:
 *   - 就绪队列 (唤醒睡眠任务)
 *   - 调度器 (检查是否需要抢占)
 *
 * 这里只提供一个轻量的入口, 实际 tick 逻辑在 task.c 中。
 * 参见: task.c → sys_tick_handler()
 */
void timer_isr(void)
{
    /* 1. 清除定时器中断标志 (写 1 清除, 必须第一步做) */
    PTIMER_ISR = 1;

    /* 2. 系统 tick 计数 +1 */
    _sys_ticks++;

    /*
     * 3. 余下的 tick 处理 (睡眠唤醒、调度检查) 由
     *    irq_handler 在调用 timer_isr 之后统一处理。
     *    这样避免 timer_isr 依赖 task 模块。
     */
}
