/**
 * @file    uart.c
 * @brief   MOS RTOS UART 驱动实现 (Cadence UART)
 *
 * Cadence UART 寄存器 (32-bit 访问, 不同于 PL011!):
 *   0x00  CR       — 控制寄存器
 *   0x04  MR       — 模式寄存器
 *   0x08  IER      — 中断使能
 *   0x0C  IDR      — 中断禁用
 *   0x10  IMR      — 中断屏蔽
 *   0x14  ISR      — 中断状态
 *   0x18  BAUDGEN  — 波特率发生器
 *   0x1C  RXTOUT   — 接收超时
 *   0x20  RXWM     — RX FIFO 触发等级
 *   0x24  MODEMCR  — 调制解调器控制
 *   0x28  MODEMSR  — 调制解调器状态
 *   0x2C  SR       — 通道状态寄存器
 *   0x30  FIFO     — TX/RX FIFO (写=发送, 读=接收)
 *   0x34  BAUD_DIV — 波特率除数寄存器
 *
 * CR 位:
 *   bit0 = UARTEN  (UART 使能)
 *   bit4 = TXEN    (发送使能)
 *   bit6 = RXEN    (接收使能)
 *
 * MR 位:
 *   bit1:0 = CHRL  (字符长度: 10=8-bit)
 *   bit5:3 = PAR   (校验: 100=无校验)
 *   bit7:6 = CHMODE (通道模式: 00=正常)
 *   bit9:8 = NBSTOP (停止位: 00=1 位)
 *
 * SR 位 (状态寄存器):
 *   bit1 = REMPTY  (RX FIFO 空)
 *   bit4 = TXFULL  (TX FIFO 满)
 *   bit5 = TXEMPTY (TX FIFO 空)
 */

#include "drivers/uart.h"

/* =========================== 寄存器偏移 =========================== */
#define UART_CR         0x00
#define UART_MR         0x04
#define UART_BAUDGEN    0x18
#define UART_SR         0x2C
#define UART_FIFO       0x30
#define UART_BAUD_DIV   0x34

/* CR 位 */
#define CR_UARTEN       (1 << 0)
#define CR_TXEN         (1 << 4)
#define CR_RXEN         (1 << 6)

/* MR 位 — 8N1 */
#define MR_CHRL_8       (1 << 1)        /* bit1=1, bit0=0 → 8-bit */
#define MR_PAR_NONE     (1 << 5)        /* bit5=1, bit4:3=0 → 无校验 */
#define MR_CHMODE_NORM  0               /* 正常模式 */
#define MR_NBSTOP_1     0               /* 1 停止位 */
#define MR_8N1          (MR_CHRL_8 | MR_PAR_NONE | MR_CHMODE_NORM | MR_NBSTOP_1)

/* SR 位 */
#define SR_REMPTY       (1 << 1)        /* RX FIFO 空 */
#define SR_TXFULL       (1 << 4)        /* TX FIFO 满 */

/* =========================== 寄存器访问 =========================== */
#define REG32(addr)     (*(volatile u32 *)(addr))
#define UART_REG(off)   REG32(UART0_BASE + (off))

/* =========================== SLCR 时钟使能 =========================== */
/**
 * @brief  通过 SLCR 使能 UART0 时钟
 *
 * Zynq-7000 启动后外设时钟默认关闭, 使用前必须使能。
 * 与 Zynq-7045 Cadence UART 兼容。
 */
static void uart_clk_enable(void)
{
    /* APER_CLK_CTRL: bit20 = UART0 CPU_1X 时钟使能 */
    REG32(SLCR_APER_CLK_CTRL) |= (1 << 20);

    /* UART_CLK_CTRL: bit0 = UART0 时钟激活, bit8:13 = 时钟源 (0 = PLL) */
    REG32(SLCR_UART_CLK_CTRL) = (REG32(SLCR_UART_CLK_CTRL) & ~0x3F00U) | (1 << 0);
}

/* =========================== 公共 API =========================== */

void uart_init(u32 baud)
{
    /*
     * 1. 使能 UART0 时钟 (SLCR)
     */
    uart_clk_enable();

    /*
     * 2. 禁用 UART, 复位 TX/RX
     *    CR = 0 使 UART 进入禁用状态
     */
    UART_REG(UART_CR) = 0;

    /*
     * 3. 配置模式: 8N1
     */
    UART_REG(UART_MR) = MR_8N1;

    /*
     * 4. 配置波特率
     *    公式: baud = ref_clk / (BAUDGEN × (BAUD_DIV + 1))
     *
     *    选 BAUD_DIV = 4 → ref_clk / (BAUDGEN × 5) = 115200
     *       BAUDGEN = 50,000,000 / (115200 × 5) ≈ 87
     *    验证: 50,000,000 / (87 × 5) = 114,942 (误差 0.22%)
     */
    {
        u32 div = 4;  /* BAUD_DIV + 1 = 5 */
        u32 gen = UART_CLK_FREQ / (baud * (div + 1));
        if (gen == 0) gen = 1;

        UART_REG(UART_BAUDGEN) = gen;
        UART_REG(UART_BAUD_DIV) = div;
    }

    /*
     * 5. 使能 UART (TX + RX)
     */
    UART_REG(UART_CR) = CR_UARTEN | CR_TXEN | CR_RXEN;
}

void uart_putc(char c)
{
    /* 等 TX FIFO 不满 (TXFULL=0) */
    while (UART_REG(UART_SR) & SR_TXFULL)
        ;

    UART_REG(UART_FIFO) = (u32)c;
}

char uart_getc(void)
{
    /* 等 RX FIFO 非空 (REMPTY=0) */
    while (UART_REG(UART_SR) & SR_REMPTY)
        ;

    return (char)(UART_REG(UART_FIFO) & 0xFF);
}

void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}
