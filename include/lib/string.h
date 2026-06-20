/**
 * @file    string.h
 * @brief   MOS RTOS 基础字符串与内存操作
 */

#ifndef _MOS_STRING_H
#define _MOS_STRING_H

#include "types.h"

/* =========================== 内存操作 =========================== */

/**
 * @brief  用 val 填充内存区域的前 n 个字节
 * @param  dst  目标地址
 * @param  val  填充值 (只取低 8 位)
 * @param  n    字节数
 * @return dst
 *
 * 用途: 清零 BSS、初始化数据结构为 0
 */
void *memset(void *dst, int val, u32 n);

/**
 * @brief  复制 n 字节从 src 到 dst
 * @param  dst  目标地址
 * @param  src  源地址
 * @param  n    字节数
 * @return dst
 *
 * 注意: 不处理重叠区域 (如需重叠请用 memmove)
 */
void *memcpy(void *dst, const void *src, u32 n);

/* =========================== 字符串操作 =========================== */

/**
 * @brief  计算字符串长度 (不含 '\0')
 * @param  s  以 '\0' 结尾的字符串
 * @return 字符个数
 */
u32 strlen(const char *s);

/**
 * @brief  比较两个字符串
 * @return 0 相等, <0 a<b, >0 a>b
 */
int strcmp(const char *a, const char *b);

/**
 * @brief  比较两个字符串的前 n 个字符
 * @return 0 相等, <0 a<b, >0 a>b
 */
int strncmp(const char *a, const char *b, u32 n);

/**
 * @brief  复制字符串 src 到 dst (含 '\0')
 * @return dst
 *
 * 注意: 不检查缓冲区溢出, 调用者确保 dst 够大
 */
char *strcpy(char *dst, const char *src);

#endif /* _MOS_STRING_H */
