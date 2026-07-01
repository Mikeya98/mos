/**
 * @file    slab.h
 * @brief   MOS RTOS SLAB 分配器 (对象缓存)
 *
 * 定位:
 *   SLAB 位于 Buddy Allocator 之上, 负责固定大小对象的快速分配/释放。
 *   Buddy 管理 64B~1MB 的 2^order 块, SLAB 将一个大块 (slab) 切分为
 *   等大的 "对象", 用空闲链表管理, 实现 O(1) alloc/free。
 *
 * 为什么需要 SLAB:
 *   1. 内部碎片: buddy 分配 65B → 128B block (浪费 49%)
 *      SLAB 按精确大小分配, 无内部碎片
 *   2. 速度: buddy O(log N), SLAB O(1) (仅操作链表头)
 *   3. 对象缓存: 同类型对象集中存储, cache-friendly
 *
 * 典型用法:
 *   slab_cache_t *tcb_cache = slab_create("tcb", sizeof(task_t));
 *   task_t *t = slab_alloc(tcb_cache);
 *   slab_free(t);  // 无需传 cache — 对象 header 自动记录
 */

#ifndef _MOS_SLAB_H
#define _MOS_SLAB_H

#include "lib/types.h"

/* =========================== 配置常量 =========================== */
#define SLAB_MIN_ORDER      4     /* 最小 slab: 2^4 × 64B = 1KB */
#define SLAB_MAX_ORDER      10    /* 最大 slab: 2^10 × 64B = 64KB */
#define SLAB_MIN_OBJ_PER    8     /* 每 slab 至少容纳的对象数 */

/* =========================== 类型定义 =========================== */

/**
 * @brief  SLAB 缓存描述符
 *
 * 每个 cache 管理一种固定大小的对象。
 * 全局 cache 链表用于 slab_dump_all() 遍历。
 */
typedef struct slab_cache {
    char        name[16];       /* 缓存名称 (调试用) */
    u32         obj_size;       /* 用户可见的对象大小 */
    u32         total_size;     /* 对象总大小 = 4(header) + obj_size 向上对齐 */
    u32         slab_order;     /* 每次 grow 向 buddy 申请的 order */
    u32         obj_per_slab;   /* 每个 slab 可容纳的对象数 */
    u32         slab_size;      /* 每个 slab 的字节数 */

    void       *free_list;      /* 空闲对象链表头 */
    u32         allocated;      /* 累计分配次数 (统计用) */
    u32         free_count;     /* 当前空闲对象数 */
    u32         slab_count;     /* 已分配的 slab 数量 */

    struct slab_cache *next;    /* 全局 cache 链表 */
} slab_cache_t;

/* =========================== 公共 API =========================== */

/**
 * @brief  创建一个 SLAB 缓存
 * @param  name      缓存名称 (最多 15 字符)
 * @param  obj_size  对象大小 (字节)
 * @return 新缓存指针, 失败返回 NULL (obj_size == 0)
 *
 * 操作:
 *   1. 计算 total_size = 4 + ALIGN(obj_size, 4)
 *   2. 选择 slab_order: 确保每 slab ≥ SLAB_MIN_OBJ_PER 对象
 *   3. 注册到全局 cache 链表
 */
slab_cache_t *slab_create(const char *name, u32 obj_size);

/**
 * @brief  从缓存分配一个对象
 * @param  cache  SLAB 缓存指针
 * @return 对象指针 (已初始化为零), 失败返回 NULL (OOM)
 *
 * 操作:
 *   1. 若 free_list 为空 → slab_grow() 从 buddy 获取新 slab
 *   2. 从 free_list 头部弹出对象
 *   3. 在 header 写入 cache 指针 (供 slab_free 使用)
 *   4. 更新统计并返回 user_ptr
 *
 * 时间复杂度: O(1)
 */
void *slab_alloc(slab_cache_t *cache);

/**
 * @brief  释放一个对象
 * @param  ptr  由 slab_alloc 返回的指针 (可为 NULL, 此时无操作)
 *
 * 操作:
 *   1. 从 ptr - 4 读取 cache 指针
 *   2. 将对象插入 free_list 头部
 *   3. 更新统计
 *
 * 时间复杂度: O(1)
 * 注意:   无需显式传 cache — 对象 header 自动记录了归属
 */
void slab_free(void *ptr);

/**
 * @brief  打印单个缓存的统计信息
 */
void slab_cache_dump(slab_cache_t *cache);

/**
 * @brief  打印所有缓存的统计信息
 */
void slab_dump_all(void);

#endif /* _MOS_SLAB_H */
