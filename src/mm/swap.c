/**
 * @file    swap.c
 * @brief   页面换出区 (v0.6 阶段7)
 *
 * 一个位图管 SWAP_PAGES 个槽, 一次换出/换入各拷贝 4KB。
 * 槽位分配和释放都在关中断的保护下完成: 换出发生在 Data Abort
 * 里 (中断本来就关着), 任务退出的批量回收发生在 schedule() 里
 * (中断开着), 两条路径可能交错。
 */

#include "mm/swap.h"
#include "mm/mmu.h"
#include "lib/irq.h"
#include "lib/printf.h"

extern u8 _swap_pool_start;
extern u8 _swap_pool_end;

#define BITS_PER_WORD   32u
#define WORD_COUNT      ((SWAP_PAGES + BITS_PER_WORD - 1u) / BITS_PER_WORD)
#define WORDS_PER_PAGE  (4096u / 4u)

/* bit i = 1: 槽 i 已占用 */
static u32 swap_used_map[WORD_COUNT];
static u32 swap_used_count = 0;

/* 历史最高占用, 演示时能看出换出了多少页 */
static u32 swap_high_water = 0;

static u32 *slot_addr(u32 slot)
{
    return (u32 *)(uintptr_t)(&_swap_pool_start + slot * 4096u);
}

static int slot_is_used(u32 slot)
{
    return (swap_used_map[slot / BITS_PER_WORD] >> (slot % BITS_PER_WORD)) & 1u;
}

static void slot_set(u32 slot, int on)
{
    u32 w = slot / BITS_PER_WORD;
    u32 b = 1u << (slot % BITS_PER_WORD);

    if (on) {
        swap_used_map[w] |= b;
    } else {
        swap_used_map[w] &= ~b;
    }
}

static void copy_page(volatile u32 *dst, volatile const u32 *src)
{
    u32 i;

    for (i = 0; i < WORDS_PER_PAGE; i++) {
        dst[i] = src[i];
    }
}

void swap_init(void)
{
    u32 pool_bytes;
    u32 i;

    pool_bytes = (u32)(uintptr_t)(&_swap_pool_end - &_swap_pool_start);
    if (pool_bytes < SWAP_PAGES * 4096u) {
        printf("[SWAP] pool too small: %u bytes < %u\n",
               pool_bytes, SWAP_PAGES * 4096u);
    }

    for (i = 0; i < WORD_COUNT; i++) {
        swap_used_map[i] = 0;
    }
    swap_used_count = 0;
    swap_high_water = 0;
}

static int swap_alloc_slot(void)
{
    u32 slot;

    for (slot = 0; slot < SWAP_PAGES; slot++) {
        if (!slot_is_used(slot)) {
            slot_set(slot, 1);
            swap_used_count++;
            if (swap_used_count > swap_high_water) {
                swap_high_water = swap_used_count;
            }
            return (int)slot;
        }
    }
    return -1;
}

int swap_store(u32 phys)
{
    int slot;
    u32 cpsr;

    if (phys == 0 || (phys & 0xFFFu) != 0) {
        return -1;
    }

    cpsr = irq_save();
    slot = swap_alloc_slot();
    if (slot < 0) {
        irq_restore(cpsr);
        return -1;
    }
    copy_page(slot_addr((u32)slot), (volatile const u32 *)(uintptr_t)phys);
    irq_restore(cpsr);

    return slot;
}

int swap_load(u32 slot, u32 phys)
{
    u32 cpsr;

    if (slot >= SWAP_PAGES || phys == 0 || (phys & 0xFFFu) != 0) {
        return -1;
    }

    cpsr = irq_save();
    if (!slot_is_used(slot)) {
        irq_restore(cpsr);
        return -1;
    }
    copy_page((volatile u32 *)(uintptr_t)phys, slot_addr(slot));
    slot_set(slot, 0);
    swap_used_count--;
    irq_restore(cpsr);

    return 0;
}

void swap_free_slot(u32 slot)
{
    u32 cpsr;

    if (slot >= SWAP_PAGES) {
        return;
    }

    cpsr = irq_save();
    if (slot_is_used(slot)) {
        slot_set(slot, 0);
        swap_used_count--;
    }
    irq_restore(cpsr);
}

void swap_release_l2(u32 l2_phys)
{
    const u32 *l2;
    u32 i;
    u32 freed = 0;

    if (l2_phys == 0) {
        return;
    }

    l2 = (const u32 *)(uintptr_t)l2_phys;
    for (i = 0; i < 256u; i++) {
        if (swap_pte_is(l2[i])) {
            swap_free_slot(swap_pte_slot(l2[i]));
            freed++;
        }
    }
    if (freed > 0) {
        printf("[SWAP] reclaimed %u swap slots\n", freed);
    }
}

u32 swap_used(void)
{
    u32 cpsr;
    u32 n;

    cpsr = irq_save();
    n = swap_used_count;
    irq_restore(cpsr);
    return n;
}

u32 swap_total(void)
{
    return SWAP_PAGES;
}

/* 阶段7 的调试输出: 在 vm.c 里换出/换入时打印, 这里保留高水位查询 */
u32 swap_high_water_mark(void)
{
    return swap_high_water;
}
