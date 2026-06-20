/**
 * @file    string.c
 * @brief   MOS RTOS 基础字符串与内存操作实现
 */

#include "lib/string.h"

/* =========================== 内存操作 =========================== */

void *memset(void *dst, int val, u32 n)
{
    u8 *d = (u8 *)dst;

    while (n--) {
        *d++ = (u8)val;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

/* =========================== 字符串操作 =========================== */

u32 strlen(const char *s)
{
    u32 len = 0;
    while (*s++) {
        len++;
    }
    return len;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

int strncmp(const char *a, const char *b, u32 n)
{
    if (n == 0) return 0;

    while (--n && *a && *a == *b) {
        a++;
        b++;
    }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}
