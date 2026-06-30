/**
 * @file    buddy.c
 * @brief   MOS RTOS 二分伙伴内存分配器实现
 *
 * 数据结构:
 *   - allocated block: [order: u32] [user data...]
 *     buddy_alloc 返回 order 字段之后的地址
 *   - free block:     [next: ptr] [unused...]
 *     next 指向同级空闲链表的下一个块
 *
 * 关键公式:
 *   block_size(order) → (1 << order) × 64B
 *   buddy(addr)       → addr XOR block_size
 *   user_ptr → block = user_ptr - 4, order = *(u32*)block
 */

#include "lib/types.h"
#include "lib/printf.h"
#include "mm/buddy.h"

/*
 * buddy 分配器内部对字节数组做指针转换写入,
 * 堆在链接脚本中已 8 字节对齐, 64B 最小块天然对齐,
 * 关闭 cast-align 警告 (这是分配器的标准做法)
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"

/* =========================== 常量 =========================== */
#define HEADER_SIZE     4          /* order 字段大小 (u32) */
#define BUDDY_COUNT     (BUDDY_MAX_ORDER + 1)

/* =========================== 类型 =========================== */

/** 空闲块节点 (嵌入在空闲块的数据区) */
typedef struct buddy_free {
    struct buddy_free *next;
} buddy_free_t;

/* =========================== 全局状态 =========================== */

/** 每阶的空闲链表头 */
static buddy_free_t *free_list[BUDDY_COUNT];

/** 堆范围 */
static u8 *heap_base;
static u32 heap_total;
static u32 heap_max_order;   /* 实际可用的最大 order */

/* =========================== 内部辅助 =========================== */

/**
 * @brief  向上取整到 2 的幂 (最小 BUDDY_MIN_BLOCK)
 */
static u32 round_up_pow2(u32 size)
{
    u32 n = BUDDY_MIN_BLOCK;

    if (size <= n) return n;

    /* 持续翻倍直到 ≥ size */
    while (n < size) {
        n <<= 1;
        if (n == 0) return 0;  /* 溢出 — 请求太大 */
    }
    return n;
}

/**
 * @brief  计算 size 对应的 order
 * @pre    size 是 2 的幂且 ≥ BUDDY_MIN_BLOCK
 */
static u32 size_to_order(u32 size)
{
    u32 order = 0;
    u32 n = size;

    /* n = BUDDY_MIN_BLOCK × 2^order  →  order = log2(n / 64) */
    n >>= BUDDY_MIN_SHIFT;  /* n = 2^order */
    while (n > 1) {
        n >>= 1;
        order++;
    }
    return order;
}

/* =========================== 空闲链表操作 =========================== */

/**
 * @brief  从空闲链表中移除一个块
 */
static void list_remove(u32 order, u8 *block)
{
    buddy_free_t **p = &free_list[order];
    while (*p) {
        if ((u8 *)(*p) == block) {
            *p = (*p)->next;
            return;
        }
        p = &(*p)->next;
    }
}

/**
 * @brief  在空闲链表中检查某个块是否存在
 */
static bool list_contains(u32 order, u8 *block)
{
    buddy_free_t *p = free_list[order];
    while (p) {
        if ((u8 *)p == block) return true;
        p = p->next;
    }
    return false;
}

/**
 * @brief  将一个块插入空闲链表头部
 */
static void list_push(u32 order, u8 *block)
{
    buddy_free_t *node = (buddy_free_t *)block;
    node->next = free_list[order];
    free_list[order] = node;
}

/* =========================== 核心算法 =========================== */

/**
 * @brief  初始化伙伴分配器
 *
 * 将整个堆初始化为一个最大阶的空闲块,
 * 并将其插入对应阶的空闲链表。
 */
void buddy_init(void *heap, u32 size)
{
    u32 i;

    heap_base = (u8 *)heap;
    heap_total = size;

    /* 计算实际可用的最大 order */
    heap_max_order = size_to_order(round_up_pow2(size));
    if (heap_max_order > BUDDY_MAX_ORDER) {
        heap_max_order = BUDDY_MAX_ORDER;
    }

    /* 初始化所有空闲链表为空 */
    for (i = 0; i < BUDDY_COUNT; i++) {
        free_list[i] = NULL;
    }

    /* 创建初始块: 最大阶, 覆盖整个堆 */
    list_push(heap_max_order, heap_base);
}

/**
 * @brief  分配一块内存
 *
 * 流程:
 *   1. 将 size 向上取整到 2 的幂 (最小 64B)
 *   2. 确定 order
 *   3. 从 order 开始向上搜索非空空闲链表
 *   4. 从高阶向低阶分裂直到所需 order
 *   5. 写 header (order) 并返回 user_ptr
 */
void *buddy_alloc(u32 size)
{
    u32 rounded, order, cur_order;
    u8 *block;

    if (size == 0) return NULL;

    /* 1. 向上取整 */
    rounded = round_up_pow2(size);
    if (rounded == 0) return NULL;  /* 溢出 */

    /* 2. 确定 order */
    order = size_to_order(rounded);

    /* 3. 向上搜索 */
    cur_order = order;
    while (cur_order <= heap_max_order && free_list[cur_order] == NULL) {
        cur_order++;
    }

    if (cur_order > heap_max_order) {
        /* 无可用内存 */
        return NULL;
    }

    /* 4. 取出该块 */
    {
        buddy_free_t *head = free_list[cur_order];
        block = (u8 *)head;
        free_list[cur_order] = head->next;
    }

    /* 5. 从上向下分裂 */
    while (cur_order > order) {
        cur_order--;
        {
            u32 half = (BUDDY_MIN_BLOCK) << cur_order;   /* 每一半的大小 */
            u8  *buddy_block = block + half;

            /* 将 buddy 放入空闲链表 */
            list_push(cur_order, buddy_block);
        }
    }

    /* 6. 写 header (order) */
    *(u32 *)block = order;

    /* 7. 返回 user_ptr */
    return (void *)(block + HEADER_SIZE);
}

/**
 * @brief  释放一块内存
 *
 * 流程:
 *   1. 读 header 获取 order
 *   2. 计算 buddy 地址
 *   3. 检查 buddy 是否空闲 → 若空闲则合并为更高阶的块
 *   4. 重复直到不能合并或达到最大阶
 *   5. 将最终块插入对应阶的空闲链表
 */
void buddy_free(void *ptr)
{
    u8  *block;
    u32 order;

    if (!ptr) return;

    /* 1. 获取真实块地址和 order */
    block = (u8 *)ptr - HEADER_SIZE;
    order = *(u32 *)block;

    if (order > BUDDY_MAX_ORDER) return;  /* 损坏检测 */

    /* 2-4. 循环合并 */
    while (order < heap_max_order) {
        u32 offset;
        u32 block_size;
        u8  *buddy_block;

        block_size = (BUDDY_MIN_BLOCK) << order;
        offset     = (u32)(uintptr_t)(block - heap_base);
        buddy_block = heap_base + (offset ^ block_size);

        /*
         * 检查 buddy 是否在 order 的空闲链表中:
         *   - buddy 空闲 AND
         *   - buddy 没有被进一步分裂 (如果被分裂, 子块会在更低阶链表中,
         *     而 buddy 本身不会出现在当前阶——所以检查通过)
         */
        if (!list_contains(order, buddy_block)) {
            break;  /* buddy 不空闲 → 不能合并 */
        }

        /* 从空闲链表中移除 buddy */
        list_remove(order, buddy_block);

        /* 合并: 取地址较小的作为合并后的块 */
        if (buddy_block < block) {
            block = buddy_block;
        }
        order++;
    }

    /* 5. 插入最终块 */
    list_push(order, block);
}

/* =========================== 调试 =========================== */

/**
 * @brief  打印伙伴分配器状态 (调试用)
 *
 * 输出每阶的空闲链表长度,
 * 帮助开发者理解分裂/合并后的内存布局。
 */
void buddy_dump(void)
{
    u32 i;

    printf("\n[BUDDY] heap=%X size=%u (max_order=%u)\n",
           (u32)(uintptr_t)heap_base, heap_total, heap_max_order);

    for (i = 0; i <= heap_max_order; i++) {
        u32 count = 0;
        u32 blk_size = (BUDDY_MIN_BLOCK) << i;
        buddy_free_t *p = free_list[i];

        while (p) {
            count++;
            p = p->next;
        }

        if (count > 0) {
            printf("  order %u (%u B): %u free\n", i, blk_size, count);
        }
    }
    printf("\n");
}

#pragma GCC diagnostic pop
