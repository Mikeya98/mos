/**
 * @file    uart.h
 * @brief   MOS RTOS UART 驱动 (Cadence UART — Zynq-7000)
 *
 * Cadence UART 寄存器布局 (32-bit 访问):
 *   0x00  CR       — 控制寄存器
 *   0x04  MR       — 模式寄存器 (数据位/校验/停止位)
 *   0x18  BAUDGEN  — 波特率发生器
 *   0x2C  SR       — 状态寄存器 (TX/RX FIFO 标志)
 *   0x30  FIFO     — 数据 FIFO (写=TX, 读=RX)
 *   0x34  BAUD_DIV — 波特率除数
 *
 * Zynq-7045 UART0 基地址: 0xE0000000
 */

#ifndef _MOS_UART_H
#define _MOS_UART_H

#include "lib/types.h"

/* =========================== 硬件参数 =========================== */
#define UART0_BASE      0xE0000000UL
#define UART_CLK_FREQ   50000000UL   /* UART 参考时钟 = 50MHz */

/* =========================== SLCR 时钟控制 =========================== */
/* Zynq SLCR 基地址, 必须在用 UART 前使能时钟 */
#define SLCR_BASE           0xF8000000UL
#define SLCR_UART_CLK_CTRL  (SLCR_BASE + 0x0154)
#define SLCR_APER_CLK_CTRL  (SLCR_BASE + 0x012C)

/* =========================== 公共 API =========================== */

/**
 * @brief  初始化 UART0
 * @param  baud  波特率 (常用 115200)
 *
 * 配置: 8 数据位, 无校验, 1 停止位 (8N1)
 * 此函数会先使能 UART0 时钟, 然后配置波特率和模式。
 */
void uart_init(u32 baud);

/**
 * @brief  发送一个字符 (阻塞, 等 TX FIFO 有空位)
 * @param  c  要发送的字符
 */
void uart_putc(char c);

/**
 * @brief  接收一个字符 (阻塞, 等 RX FIFO 有数据)
 * @return 收到的字符
 */
char uart_getc(void);

/**
 * @brief  发送字符串
 * @param  s  以 '\0' 结尾的字符串
 */
void uart_puts(const char *s);

#endif /* _MOS_UART_H */
