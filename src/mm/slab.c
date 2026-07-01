/**
 * @file    slab.c
 * @brief   MOS RTOS SLAB 分配器实现
 *
 * 核心思想:
 *   SLAB 在 Buddy Allocator 之上构建固定大小的对象缓存。
 *   每个 cache 维护一个空闲链表 (free_list), alloc 从头部取,
 *   free 放回头部 — 均为 O(1)。
 *
 *   当 free_list 为空时, 调用 slab_grow() 向 buddy 申请一个新 slab,
 *   将其切分为 obj_per_slab 个等大小对象, 全部链入 free_list。
 *
 * 对象内存布局:
 *   [cache 指针 | 4B] [用户数据 | obj_size 字节]
 *                    ^
 *                    slab_alloc 返回的指针
 *
 *   空闲对象的前 4 字节复用为 next 指针 (指向下一个空闲对象)。
 *   已分配对象的前 4 字节存储 cache 指针 (供 slab_free 定位 cache)。
 *
 * 为什么 slab_free 不需要 cache 参数:
 *   释放时从 ptr - 4 读取 cache 指针即可确定对象归属。
 *   这是 SLAB 相比通用 malloc 的一个重要优势 —
 *   调用者不需要记住 "这块内存是从哪个池分配的"。
 */

#include "lib/types.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/buddy.h"
#include "mm/slab.h"

/*
 * SLAB 分配器对 buddy 返回的内存做指针转换写入,
 * buddy 保证 64B 对齐, 关闭 cast-align 警告。
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"

/* =========================== 内部常量 =========================== */
#define HEADER_SIZE  4           /* 对象头部大小 (cache 指针 / next 指针) */
#define BUDDY_BASE   (64u)       /* buddy 最小块大小, 用于 order→size 换算 */

/* =========================== 全局状态 =========================== */

/** 全局 cache 链表头 */
static slab_cache_t *cache_list = NULL;

/* =========================== 内部辅助 =========================== */

/**
 * @brief  向上对齐到 4 字节边界
 */
static u32 align4(u32 n)
{
    return (n + 3u) & ~3u;
}

/**
 * @brief  根据 total_size 选择最优 slab_order
 *
 * 规则: 确保每个 slab 至少容纳 SLAB_MIN_OBJ_PER 个对象,
 *       在满足条件的前提下选择最小的 order。
 */
static u32 choose_slab_order(u32 total_size)
{
    u32 order = SLAB_MIN_ORDER;
    u32 slab_size;
    u32 obj_count;

    while (order <= SLAB_MAX_ORDER) {
        slab_size = BUDDY_BASE << order;
        obj_count = slab_size / total_size;

        if (obj_count >= SLAB_MIN_OBJ_PER) {
            return order;
        }
        order++;
    }

    /* 对象太大, 使用最大 order 也能容纳至少 1 个 */
    return SLAB_MAX_ORDER;
}

/* =========================== 核心实现 =========================== */

/**
 * @brief  创建一个 SLAB 缓存
 *
 * 为指定大小的对象创建专用缓存。
 * 缓存本身从 buddy 分配 (作为第一个对象)。
 */
slab_cache_t *slab_create(const char *name, u32 obj_size)
{
    slab_cache_t *cache;
    u32 i;

    if (obj_size == 0) {
        return NULL;
    }

    /* 分配缓存结构体本身 (从 buddy 获取, 复用 slab_alloc 模式前的手动构造) */
    cache = (slab_cache_t *)buddy_alloc(sizeof(slab_cache_t));
    if (!cache) {
        return NULL;
    }

    /* 计算对象布局 */
    cache->obj_size   = obj_size;
    cache->total_size = HEADER_SIZE + align4(obj_size);

    /* 检查溢出 */
    if (cache->total_size <= HEADER_SIZE) {
        buddy_free(cache);
        return NULL;
    }

    /* 选择 slab 大小 */
    cache->slab_order   = choose_slab_order(cache->total_size);
    cache->slab_size    = BUDDY_BASE << cache->slab_order;
    cache->obj_per_slab = cache->slab_size / cache->total_size;

    /* 初始化统计 */
    cache->free_list  = NULL;
    cache->allocated  = 0;
    cache->free_count = 0;
    cache->slab_count = 0;

    /* 复制名称 (最多 15 字符) */
    for (i = 0; i < 15 && name[i] != '\0'; i++) {
        cache->name[i] = name[i];
    }
    cache->name[i] = '\0';

    /* 注册到全局链表 */
    cache->next = cache_list;
    cache_list  = cache;

    return cache;
}

/**
 * @brief  向 buddy 申请一个新 slab, 切分为对象并链入 free_list
 * @return true=成功, false=OOM
 */
static bool slab_grow(slab_cache_t *cache)
{
    u32 i;
    u8 *slab_base;
    u8 *obj;
    u32 slab_size;

    slab_size = cache->slab_size;

    /* 1. 向 buddy 申请内存 */
    slab_base = (u8 *)buddy_alloc(slab_size);
    if (!slab_base) {
        return false;
    }

    /* 2. 切分为 obj_per_slab 个对象, 链入 free_list (从后往前链, 使地址升序) */
    for (i = 0; i < cache->obj_per_slab; i++) {
        obj = slab_base + (u32)(i * cache->total_size);

        /* 前 4 字节: next 指针 → 当前 free_list 头 */
        *(void **)obj = cache->free_list;
        cache->free_list = obj;
    }

    /* 3. 更新统计 */
    cache->free_count += cache->obj_per_slab;
    cache->slab_count++;

    return true;
}

/**
 * @brief  从缓存分配一个对象
 *
 * 快速路径: free_list 非空 → 弹出头部 → O(1)
 * 慢速路径: free_list 为空 → slab_grow → 再弹出 → O(1) amortized
 */
void *slab_alloc(slab_cache_t *cache)
{
    u8 *obj;

    if (!cache) {
        return NULL;
    }

    /* 1. 若无空闲对象, 申请新 slab */
    if (!cache->free_list) {
        if (!slab_grow(cache)) {
            return NULL;  /* OOM */
        }
    }

    /* 2. 从 free_list 弹出头部 */
    obj = (u8 *)cache->free_list;
    cache->free_list = *(void **)obj;

    /* 3. 在 header 写入 cache 指针 (供 slab_free 使用) */
    *(slab_cache_t **)obj = cache;

    /* 4. 更新统计 */
    cache->allocated++;
    cache->free_count--;

    /* 5. 返回用户数据指针 (跳过 header) */
    return (void *)(obj + HEADER_SIZE);
}

/**
 * @brief  释放一个对象
 *
 * 从 ptr - 4 读取 cache 指针, 将对象插回 free_list 头部 — O(1)
 */
void slab_free(void *ptr)
{
    u8 *obj;
    slab_cache_t *cache;

    if (!ptr) {
        return;
    }

    /* 1. 获取对象头 (包含 cache 指针) */
    obj = (u8 *)ptr - HEADER_SIZE;
    cache = *(slab_cache_t **)obj;

    if (!cache) {
        return;  /* 损坏检测: cache 指针为空 */
    }

    /* 2. 将对象插入 free_list 头部 */
    *(void **)obj = cache->free_list;
    cache->free_list = obj;

    /* 3. 更新统计 */
    cache->free_count++;
    if (cache->allocated > 0) {
        cache->allocated--;
    }
}

/* =========================== 调试 =========================== */

/**
 * @brief  打印单个缓存的统计信息
 */
void slab_cache_dump(slab_cache_t *cache)
{
    if (!cache) {
        return;
    }

    printf("  [%s] obj=%uB total=%uB order=%u slab=%uB "
           "objs/slab=%u slabs=%u alloc=%u free=%u\n",
           cache->name,
           cache->obj_size,
           cache->total_size,
           cache->slab_order,
           cache->slab_size,
           cache->obj_per_slab,
           cache->slab_count,
           cache->allocated,
           cache->free_count);
}

/**
 * @brief  打印所有缓存的统计信息
 */
void slab_dump_all(void)
{
    slab_cache_t *cache;

    printf("\n[SLAB] === Cache Summary ===\n");

    if (!cache_list) {
        printf("  (no caches)\n");
        return;
    }

    for (cache = cache_list; cache; cache = cache->next) {
        slab_cache_dump(cache);
    }
    printf("\n");
}

#pragma GCC diagnostic pop
