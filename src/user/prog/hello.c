/**
 * @file    hello.c
 * @brief   独立的用户程序 (v0.6 阶段11)
 *
 * 这个文件不参与内核编译, 而是单独链接成一个 ELF 文件, 打包进
 * 内核镜像, 启动时写进 RAM 盘, 最后由 SYS_EXEC_ELF 从文件系统
 * 读出来加载到 0x09000000。它和内核共享的 .user_text 完全无关。
 *
 * 编译:
 *   arm-none-eabi-gcc -mcpu=cortex-a9 -marm -ffreestanding -nostdlib \
 *       -fno-builtin -fno-stack-protector -O2 \
 *       -Wl,-T,src/user/prog/user.lds -o build/user_hello.elf hello.c
 */

#include "lib/types.h"

/* 这里的系统调用号和 kernel/syscall.h 一致; 程序不包含内核头文件 */
#define SYS_EXIT        1
#define SYS_WRITE       2
#define SYS_GETPID      4
#define SYS_FORK        9
#define SYS_WAIT        11

static inline s32 prog_write(const char *buf, u32 len)
{
    register u32 r0 __asm__("r0") = (u32)(uintptr_t)buf;
    register u32 r1 __asm__("r1") = len;

    __asm__ volatile ("SVC #2" : "+r"(r0) : "r"(r1) : "memory");
    return (s32)r0;
}

static inline s32 prog_getpid(void)
{
    register u32 r0 __asm__("r0") = 0;

    __asm__ volatile ("SVC #4" : "+r"(r0) : : "memory");
    return (s32)r0;
}

static inline s32 prog_fork(void)
{
    register u32 r0 __asm__("r0") = 0;

    __asm__ volatile ("SVC #9" : "+r"(r0) : : "memory");
    return (s32)r0;
}

static inline s32 prog_wait(s32 pid, s32 *code)
{
    register u32 r0 __asm__("r0") = (u32)pid;
    register u32 r1 __asm__("r1") = (u32)(uintptr_t)code;

    __asm__ volatile ("SVC #11" : "+r"(r0) : "r"(r1) : "memory");
    return (s32)r0;
}

static inline __attribute__((noreturn)) void prog_exit(s32 code)
{
    register u32 r0 __asm__("r0") = (u32)code;

    __asm__ volatile ("SVC #1" : "+r"(r0) : : "memory");
    for (;;) {
    }
}

/* 这两个变量用来验证加载器的数据段路径:
 *   counter 在 .data, 初值必须从文件拷进来
 *   bss_buf 在 .bss, 不占文件字节, 加载后必须是 0
 * volatile 防止编译器把只读的 counter 优化进 .rodata。 */
volatile u32 counter = 0x12345678u;
volatile u32 bss_buf[8];

static const char msg_data[] = "[elf] .data loaded, .bss is zero\n";
static const char msg_data_bad[] = "[elf] data/bss check FAILED\n";
static const char msg_writable[] = "[elf] .data is writable\n";
static const char msg_hello[] =
    "[elf] hello — this image was loaded from /hello.elf\n";
static const char msg_detail[] =
    "[elf] code/data live at 0x09000000, not in the kernel's shared segment\n";
static const char msg_pid[] = "[elf] pid = ";
static const char msg_child[] = "[elf] child : forked, program pages copied to new frames\n";
static const char msg_reaped[] = "[elf] parent: wait reaped child, code = ";
static const char msg_forkfail[] = "[elf] fork failed (no free address-space slot)\n";
static const char msg_done[] = "[elf] exit(0)\n";

/* 极简整数转十进制, 不依赖 libc */
static void put_uint(u32 v)
{
    char buf[12];
    int i = 0;

    if (v == 0) {
        buf[i++] = '0';
    } else {
        while (v > 0 && i < (int)sizeof(buf)) {
            buf[i++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
    }
    while (i > 0) {
        i--;
        prog_write(&buf[i], 1);
    }
}

void _start(void)
{
    /* .data 初值 + .bss 清零 */
    {
        u32 i;
        int ok = (counter == 0x12345678u);

        for (i = 0; i < 8u; i++) {
            if (bss_buf[i] != 0u) {
                ok = 0;
            }
        }
        prog_write(ok ? msg_data : msg_data_bad,
                   ok ? sizeof(msg_data) - 1u : sizeof(msg_data_bad) - 1u);

        counter = 0xA5A5A5A5u;
        if (counter == 0xA5A5A5A5u) {
            prog_write(msg_writable, sizeof(msg_writable) - 1u);
        }
    }

    prog_write(msg_hello, sizeof(msg_hello) - 1u);
    prog_write(msg_detail, sizeof(msg_detail) - 1u);

    prog_write(msg_pid, sizeof(msg_pid) - 1u);
    put_uint((u32)prog_getpid());
    prog_write("\n", 1);

    /* 验证 fork 能复制"从 ELF 加载出来的程序页" */
    {
        s32 child;
        s32 status = 0;

        child = prog_fork();
        if (child == 0) {
            prog_write(msg_child, sizeof(msg_child) - 1u);
            prog_exit(3);
        } else if (child > 0) {
            prog_wait(child, &status);
            prog_write(msg_reaped, sizeof(msg_reaped) - 1u);
            put_uint((u32)status);
            prog_write("\n", 1);
        } else {
            prog_write(msg_forkfail, sizeof(msg_forkfail) - 1u);
        }
    }

    prog_write(msg_done, sizeof(msg_done) - 1u);
    prog_exit(0);
}
