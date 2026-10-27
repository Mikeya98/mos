/**
 * @file    frame.c
 * @brief   物理页框分配器 (v0.6 阶段6)
 *
 * 一张位图管 FRAME_COUNT 个 4KB 帧, 分配就是找第一个 0 位。
 * 页框池本身在 kernel.lds 的 .frame_pool 段 (4KB 对齐)。
 *
 * 所有操作都在关中断的保护下完成:
 *   - 缺页处理发生在 Data Abort 里, 这时中断本来就是屏蔽的;
 *   - 任务退出时的批量归还发生在 schedule() 里, 中断开着。
 * 两条路径可能交错, 位图必须当成共享变量对待。
 *
 * 注意这里没有"锁竞争"的概念 (单核), 关中断只是为了不被 tick
 * 打断成两步: 找到空帧和占住它必须是一个原子操作, 否则两个
 * 任务可能拿到同一帧。
 */

#include "mm/frame.h"
#include "lib/irq.h"
#include "lib/printf.h"

extern u8 _frame_pool_start;
extern u8 _frame_pool_end;

#define BITS_PER_WORD   32u
#define WORD_COUNT      ((FRAME_COUNT + BITS_PER_WORD - 1u) / BITS_PER_WORD)
#define FRAME_INVALID   0xFFFFFFFFu

/* bit i = 1: 第 i 帧已分配 */
static u32 frame_used[WORD_COUNT];

/* 每帧的所有者 pid, 只用于调试 */
static u32 frame_owner_pid[FRAME_COUNT];

static u32 frame_used_count = 0;
static u32 frame_high_water = 0;      /* 历史最高占用, 演示"惰性分配省了多少" */

static u32 pool_base(void)
{
    return (u32)(uintptr_t)&_frame_pool_start;
}

/** 物理地址 → 帧号, 不是池子里的帧返回 -1 */
static int frame_index(u32 phys)
{
    u32 base = pool_base();
    u32 off;

    if (phys < base) {
        return -1;
    }
    off = phys - base;
    if ((off & (FRAME_SIZE - 1u)) != 0) {
        return -1;                  /* 没有 4KB 对齐 */
    }
    off /= FRAME_SIZE;
    if (off >= FRAME_COUNT) {
        return -1;                  /* 超出池子 */
    }
    return (int)off;
}

static int frame_is_used(u32 idx)
{
    return (frame_used[idx / BITS_PER_WORD] >> (idx % BITS_PER_WORD)) & 1u;
}

static void frame_set(u32 idx, int on)
{
    u32 w = idx / BITS_PER_WORD;
    u32 b = 1u << (idx % BITS_PER_WORD);

    if (on) {
        frame_used[w] |= b;
    } else {
        frame_used[w] &= ~b;
    }
}

void frame_init(void)
{
    u32 pool_bytes;
    u32 i;

    /* 池子大小必须和 FRAME_COUNT 对得上, 链接脚本改错了早点发现 */
    pool_bytes = (u32)(uintptr_t)(&_frame_pool_end - &_frame_pool_start);
    if (pool_bytes < FRAME_COUNT * FRAME_SIZE) {
        printf("[FRAME] pool too small: %u bytes < %u\n",
               pool_bytes, FRAME_COUNT * FRAME_SIZE);
    }

    for (i = 0; i < WORD_COUNT; i++) {
        frame_used[i] = 0;
    }
    for (i = 0; i < FRAME_COUNT; i++) {
        frame_owner_pid[i] = 0;
    }
    frame_used_count = 0;
    frame_high_water = 0;
}

u32 frame_alloc(u32 owner)
{
    u32 idx;
    u32 w;
    u32 cpsr;

    cpsr = irq_save();

    for (idx = 0; idx < FRAME_COUNT; idx++) {
        if (!frame_is_used(idx)) {
            break;
        }
    }
    if (idx >= FRAME_COUNT) {
        irq_restore(cpsr);
        printf("[FRAME] out of memory (owner pid %u)\n", owner);
        return 0;
    }

    w = idx / BITS_PER_WORD;
    frame_used[w] |= (1u << (idx % BITS_PER_WORD));
    frame_owner_pid[idx] = owner;
    frame_used_count++;
    if (frame_used_count > frame_high_water) {
        frame_high_water = frame_used_count;
    }

    irq_restore(cpsr);

    return pool_base() + idx * FRAME_SIZE;
}

int frame_free(u32 phys, u32 owner)
{
    int idx;
    u32 cpsr;

    idx = frame_index(phys);
    if (idx < 0) {
        printf("[FRAME] free %X: not a pool frame (pid %u)\n", phys, owner);
        return -1;
    }

    cpsr = irq_save();
    if (!frame_is_used((u32)idx)) {
        irq_restore(cpsr);
        printf("[FRAME] free %X: already free (pid %u)\n", phys, owner);
        return -1;
    }
    frame_set((u32)idx, 0);
    frame_owner_pid[idx] = 0;
    frame_used_count--;
    irq_restore(cpsr);

    return 0;
}

u32 frame_free_count(void)
{
    u32 cpsr;
    u32 free_n;

    cpsr = irq_save();
    free_n = FRAME_COUNT - frame_used_count;
    irq_restore(cpsr);
    return free_n;
}

u32 frame_total(void)
{
    return FRAME_COUNT;
}

void frame_dump(const char *tag)
{
    u32 i;
    u32 cpsr;

    cpsr = irq_save();
    printf("[FRAME] %s: %u/%u used, %u free, high-water %u\n",
           tag, frame_used_count, FRAME_COUNT,
           FRAME_COUNT - frame_used_count, frame_high_water);

    printf("[FRAME] owners:");
    for (i = 0; i < FRAME_COUNT; i++) {
        if (frame_owner_pid[i] != 0) {
            printf(" %u@%u", i, frame_owner_pid[i]);
        }
    }
    printf("\n");
    irq_restore(cpsr);
}
