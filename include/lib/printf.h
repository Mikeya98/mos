/**
 * @file    printf.h
 * @brief   MOS RTOS 简易格式化输出
 *
 * 仅支持以下格式:
 *   %d  — 有符号十进制整数
 *   %u  — 无符号十进制整数
 *   %x  — 十六进制 (小写)
 *   %X  — 十六进制 (大写)
 *   %s  — 字符串
 *   %c  — 单个字符
 *   %%  — 百分号字面量
 *
 * 不支持: 宽度指定、精度、浮点数、%p、%o、%ld 等
 */

#ifndef _MOS_PRINTF_H
#define _MOS_PRINTF_H

#include "types.h"

/**
 * @brief  向串口输出格式化字符串
 * @param  fmt  格式字符串
 * @param  ...  变长参数
 * @return 输出的字符总数
 *
 * 这是用户代码中使用频率最高的函数。
 * 内部调用 uart_putc() 逐字符输出。
 */
int printf(const char *fmt, ...);

/**
 * @brief  格式化字符串到缓冲区 (不输出到串口)
 * @param  buf  输出缓冲区
 * @param  size 缓冲区大小
 * @param  fmt  格式字符串
 * @param  ...  变长参数
 * @return 写入的字符数 (不含 '\0')
 *
 * 用途: 构造字符串、调试信息暂存
 */
int snprintf(char *buf, u32 size, const char *fmt, ...);

#endif /* _MOS_PRINTF_H */
