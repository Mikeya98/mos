/**
 * @file    printf.c
 * @brief   MOS RTOS 简易格式化输出实现
 *
 * 设计取舍:
 *   - 裸机环境没有 libc, 需要自己实现 printf
 *   - 只支持最常用的格式 (%d %u %x %s %c %%), 代码量可控
 *   - 浮点数不支持 (ARM bare-metal 默认关闭硬件浮点, 且 RTOS 一般不涉及浮点)
 *   - 依赖 uart_putc() 输出, 这个函数在 uart.c 中实现
 */

#include "lib/printf.h"
#include "lib/string.h"
#include "drivers/uart.h"

/* =========================== 内部辅助 =========================== */

/**
 * @brief  将无符号整数转为指定进制的字符串 (写到 buf)
 * @param  buf   输出缓冲区
 * @param  val   要转换的值
 * @param  base  进制 (10 或 16)
 * @param  upper true=大写字母 (用于 %X)
 * @return 写入的字符数
 */
static int itoa(char *buf, u32 val, u32 base, bool upper)
{
    const char *digits;

    if (upper) {
        digits = "0123456789ABCDEF";
    } else {
        digits = "0123456789abcdef";
    }

    char tmp[32];   /* 32-bit 最大 10 位十进制 / 8 位十六进制 */
    int i = 0;

    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            tmp[i++] = digits[val % base];
            val /= base;
        }
    }

    /* 反转 (上述生成的是逆序) */
    int len = i;
    while (i > 0) {
        *buf++ = tmp[--i];
    }
    return len;
}

/* =========================== 核心函数 =========================== */

/**
 * @brief  格式化字符串核心 (由 printf 和 snprintf 共用)
 *
 * @param  putc_fn  输出回调: 每输出一个字符时调用
 * @param  ctx      透传给 putc_fn 的上下文指针
 * @param  fmt      格式字符串
 * @param  args     变长参数列表 (va_list)
 * @return 输出的字符总数
 */
int vsnprintf(
    void (*putc_fn)(char c, void *ctx),
    void *ctx,
    const char *fmt,
    __builtin_va_list args)
{
    int count = 0;

    while (*fmt) {
        if (*fmt != '%') {
            /* 普通字符: 直接输出 */
            putc_fn(*fmt++, ctx);
            count++;
            continue;
        }

        /* 处理 % 格式说明符 */
        fmt++;  /* 跳过 '%' */

        if (*fmt == '\0') break;

        switch (*fmt) {
        case '%':
            putc_fn('%', ctx);
            count++;
            break;

        case 'c': {
            char c = (char)__builtin_va_arg(args, int);
            putc_fn(c, ctx);
            count++;
            break;
        }

        case 's': {
            const char *s = __builtin_va_arg(args, const char *);
            if (!s) s = "(null)";
            while (*s) {
                putc_fn(*s++, ctx);
                count++;
            }
            break;
        }

        case 'd': {
            int val = __builtin_va_arg(args, int);
            if (val < 0) {
                putc_fn('-', ctx);
                count++;
                val = -val;
            }
            char buf[16];
            int len = itoa(buf, (u32)val, 10, false);
            for (int i = 0; i < len; i++) {
                putc_fn(buf[i], ctx);
                count++;
            }
            break;
        }

        case 'u': {
            u32 val = __builtin_va_arg(args, u32);
            char buf[16];
            int len = itoa(buf, val, 10, false);
            for (int i = 0; i < len; i++) {
                putc_fn(buf[i], ctx);
                count++;
            }
            break;
        }

        case 'x': {
            u32 val = __builtin_va_arg(args, u32);
            char buf[16];
            int len = itoa(buf, val, 16, false);
            for (int i = 0; i < len; i++) {
                putc_fn(buf[i], ctx);
                count++;
            }
            break;
        }

        case 'X': {
            u32 val = __builtin_va_arg(args, u32);
            char buf[16];
            int len = itoa(buf, val, 16, true);
            for (int i = 0; i < len; i++) {
                putc_fn(buf[i], ctx);
                count++;
            }
            break;
        }

        default:
            /* 不支持的格式: 原样输出 '%' + 字符 */
            putc_fn('%', ctx);
            count++;
            putc_fn(*fmt, ctx);
            count++;
            break;
        }
        fmt++;
    }

    return count;
}

/* =========================== 输出回调 =========================== */

/** 直接输出到串口的回调 */
static void uart_putc_cb(char c, void *ctx)
{
    UNUSED(ctx);
    uart_putc(c);
}

/** 缓冲区写入上下文 (用于 snprintf 有界写入) */
struct buf_ctx {
    char *cur;
    char *end;
    int  count;
};

/** 有界缓冲区写入回调: 每个字符都检查边界, 避免溢出 */
static void buf_putc_cb(char c, void *ctx)
{
    struct buf_ctx *bc = (struct buf_ctx *)ctx;
    if (bc->cur < bc->end) {
        *bc->cur = c;
        bc->cur++;
    }
    bc->count++;
}

/* =========================== 公开 API =========================== */

int printf(const char *fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int ret = vsnprintf(uart_putc_cb, NULL, fmt, args);
    __builtin_va_end(args);
    return ret;
}

int snprintf(char *buf, u32 size, const char *fmt, ...)
{
    if (!buf || size == 0) return 0;

    struct buf_ctx bc;
    bc.cur = buf;
    bc.end = buf + size - 1;  /* 留 1 字节给 '\0' */
    bc.count = 0;

    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    vsnprintf(buf_putc_cb, &bc, fmt, args);
    __builtin_va_end(args);

    *bc.cur = '\0';
    return bc.count;
}
