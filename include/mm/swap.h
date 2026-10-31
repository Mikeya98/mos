/**
 * @file    swap.h
 * @brief   页面换出区 (v0.6 阶段7)
 *
 * 按需分页解决的是"用多少拿多少", 但页框池只有 64 帧。一个任务
 * 如果同时碰到的页超过池子容量, 再缺页时 frame_alloc 就会失败。
 * 阶段7 的处理办法是把暂时不用的页写到 swap 区, 腾出页框; 下次
 * 访问这一页时再从 swap 区读回来。
 *
 * swap 区是 kernel.lds 里划出的一段普通内存 (SWAP_PAGES × 4KB),
 * 相当于一块"磁盘"。内核通过 CPU 读写它, 不涉及 DMA, 所以不需要
 * cache 维护。
 *
 * 换出的页在 L2 页表里不再指向物理页框, 而是写一个特殊描述符:
 *
 *   bit[1:0] = 00        —— 硬件看到它仍然报 translation fault
 *   bit[31:12] = SWAP_MAGIC
 *   bit[11:0]  = 槽号
 *
 * 这样不需要给每个任务再开一张"哪页被换出"的侧表, 页表本身就是账本。
 * 好处和阶段6 一样: 不会出现侧表和页表对不上的情况。
 */

#ifndef _MOS_SWAP_H
#define _MOS_SWAP_H

#include "lib/types.h"

#define SWAP_PAGES      16u          /* 与 kernel.lds 的 .swap_pool 一致 */

/* 换出页描述符。低两位必须为 0 (fault descriptor), 高 20 位做标记。 */
#define SWAP_MAGIC      0x5A000000u
#define SWAP_MAGIC_MASK 0xFFFFF000u
#define SWAP_SLOT_SHIFT 2u
#define SWAP_SLOT_MASK  0x000003FFu
#define SWAP_PTE_NONE   0xFFFFFFFFu

/** 初始化 swap 区 (frame_init 之后调用) */
void swap_init(void);

/** 判断一个 L2 表项是不是换出描述符 */
static inline int swap_pte_is(u32 pte)
{
    return (pte & SWAP_MAGIC_MASK) == SWAP_MAGIC && (pte & 0x3u) == 0u;
}

/** 从换出描述符里取槽号 (槽号放在 bit[11:2], 低两位必须保持 00) */
static inline u32 swap_pte_slot(u32 pte)
{
    return (pte >> SWAP_SLOT_SHIFT) & SWAP_SLOT_MASK;
}

/** 构造换出描述符 */
static inline u32 swap_pte_make(u32 slot)
{
    return SWAP_MAGIC | ((slot & SWAP_SLOT_MASK) << SWAP_SLOT_SHIFT);
}

/**
 * @brief  把一帧物理页写到 swap 区
 * @param  phys  页框物理地址 (4KB 对齐)
 * @return 槽号 (0..SWAP_PAGES-1), 没有空槽返回 -1
 */
int swap_store(u32 phys);

/**
 * @brief  把 swap 槽读回一帧物理页, 并释放该槽
 * @param  slot  槽号
 * @param  phys  目标页框物理地址
 * @return 0 成功, -1 槽号非法/未占用
 */
int swap_load(u32 slot, u32 phys);

/** 释放一个槽 (任务退出时回收) */
void swap_free_slot(u32 slot);

/**
 * @brief  回收一张 L2 表里所有换出页对应的槽
 *
 * user_space_destroy 在任务退出时调用。页表里的换出描述符是这些
 * 槽唯一的记录, 不扫一遍就会泄漏。
 */
void swap_release_l2(u32 l2_phys);

u32 swap_used(void);
u32 swap_total(void);
u32 swap_high_water_mark(void);

#endif /* _MOS_SWAP_H */
