/**
 * @file    buddy.h
 * @brief   MOS RTOS 二分伙伴内存分配器 (Buddy Allocator)
 *
 * 算法核心:
 *   所有块大小都是 2 的幂, 最小块 64B, 最大块 1MB (MAX_ORDER=14)
 *   alloc: 向上取整到 2 的幂 → 找空闲块 → 不足则从上级分裂
 *   free:  找兄弟块 (buddy) → 如果也空闲则合并 → 向上递归
 *   buddy地址 = block_addr XOR block_size
 *
 * 复杂度: O(log N) alloc/free, 无外部碎片
 */

#ifndef _MOS_BUDDY_H
#define _MOS_BUDDY_H

#include "lib/types.h"

/* =========================== 配置常量 =========================== */
#define BUDDY_MIN_SHIFT   6         /* 最小块 2^6 = 64B */
#define BUDDY_MAX_ORDER   14        /* 最大块 2^14 × 64B = 1MB */
#define BUDDY_MIN_BLOCK   (1u << BUDDY_MIN_SHIFT)  /* 64 */

/* =========================== 公共 API =========================== */

/**
 * @brief  初始化伙伴分配器
 * @param  heap_start  堆起始地址
 * @param  heap_size   堆总大小 (字节, 必须是 MIN_BLOCK 的倍数)
 *
 * 操作:
 *   - 将整个堆作为 1 个最大阶的空闲块
 *   - 初始化所有阶的空闲链表
 */
void buddy_init(void *heap_start, u32 heap_size);

/**
 * @brief  分配一块内存
 * @param  size  请求大小 (字节)
 * @return 分配的内存指针, 失败返回 NULL
 *
 * 操作:
 *   1. 将 size 向上取整到 2 的幂 (最小 64B)
 *   2. 确定 order = log2(size / 64)
 *   3. 从 free_list[order] 向上搜索, 找到第一个非空链表
 *   4. 若没找到 → NULL
 *   5. 若找到的块比需要的 order 大 → 递归分裂直到刚好
 *   6. 在块头部写入 order, 返回 header 之后的地址
 */
void *buddy_alloc(u32 size);

/**
 * @brief  释放一块内存
 * @param  ptr  由 buddy_alloc 返回的指针
 *
 * 操作:
 *   1. 读 header 获取 order
 *   2. 计算 buddy 地址: block_addr XOR block_size
 *   3. 检查 buddy 是否在 free_list[order] 中
 *   4. 若在 → 从链表中移除 buddy → 合并为 order+1 的块 → 递归
 *   5. 若不在或已达 MAX_ORDER → 插入 free_list[order]
 */
void buddy_free(void *ptr);

/**
 * @brief  打印伙伴分配器状态 (调试用)
 *
 * 输出每个阶的空闲链表长度。
 */
void buddy_dump(void);

#endif /* _MOS_BUDDY_H */
