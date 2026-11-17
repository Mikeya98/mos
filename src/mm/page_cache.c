/**
 * @file    page_cache.c
 * @brief   文件页缓存 (v0.6 阶段13)
 *
 * 一张 16 项的线性表就够了: 缓存的只是 ELF 正文页, 一个程序通常
 * 一两页代码, 同一时间跑的程序也不会太多。真要做大再换哈希/基数树,
 * 现在没必要。
 *
 * 驱逐策略是"只在 refs == 1 的项里挑最旧的" —— 被任何进程映射着的
 * 页不能动, 否则那个进程的页表会指向一帧已经属于别人的内存。
 */

#include "mm/page_cache.h"
#include "mm/frame.h"
#include "lib/string.h"
#include "lib/printf.h"

typedef struct {
    u8   valid;
    char path[PAGE_CACHE_PATH_MAX];
    u32  file_off;
    u32  frame;
    u32  refs;          /* 1 (缓存自己) + 映射它的进程数 */
    u32  stamp;         /* 最近一次访问的时钟, 越大越新 */
} pc_entry_t;

static pc_entry_t cache[PAGE_CACHE_SLOTS];
static u32 clock_stamp;

static u32 stat_hits;
static u32 stat_misses;
static u32 stat_evictions;
static u32 stat_bypass;

/* 缓存操作的原子性: 缺页可能被 tick 打断, 表项读到一半被改会算错
 * 引用计数, 留下永远驱逐不掉的脏项。保存/恢复 CPSR, 支持嵌套。 */
static inline u32 pc_irq_lock(void)
{
    u32 cpsr;
    __asm__ volatile ("MRS %0, CPSR\n\tCPSID i" : "=r"(cpsr) :: "memory", "cc");
    return cpsr;
}

static inline void pc_irq_unlock(u32 cpsr)
{
    __asm__ volatile ("MSR CPSR_c, %0" :: "r"(cpsr) : "memory", "cc");
}

static void pc_copy_path(char *dst, const char *src)
{
    u32 i;

    for (i = 0; i + 1u < PAGE_CACHE_PATH_MAX && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/** 调用者持锁 */
static int pc_find(const char *path, u32 file_off)
{
    u32 i;

    for (i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (cache[i].valid &&
            cache[i].file_off == file_off &&
            strcmp(cache[i].path, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/** 调用者持锁: 找一个空位, 没有就驱逐最旧的空闲项 */
static int pc_alloc_slot(void)
{
    u32 i;
    int victim = -1;
    u32 oldest = 0xFFFFFFFFu;

    for (i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (!cache[i].valid) {
            return (int)i;
        }
        if (cache[i].refs == 1u && cache[i].stamp < oldest) {
            oldest = cache[i].stamp;
            victim = (int)i;
        }
    }

    if (victim >= 0) {
        u32 old_frame = cache[victim].frame;

        printf("[PC] evict %s+%X (frame %X)\n",
               cache[victim].path, cache[victim].file_off, old_frame);
        frame_free(old_frame, 0);
        cache[victim].valid = 0;
        stat_evictions++;
    }

    return victim;
}

void page_cache_init(void)
{
    memset(cache, 0, sizeof(cache));
    clock_stamp = 0;
    stat_hits = 0;
    stat_misses = 0;
    stat_evictions = 0;
    stat_bypass = 0;
}

u32 page_cache_lookup(const char *path, u32 file_off)
{
    u32 cpsr;
    int idx;
    u32 frame = 0;

    if (!path) {
        return 0;
    }

    cpsr = pc_irq_lock();
    idx = pc_find(path, file_off);
    if (idx >= 0) {
        cache[idx].refs++;
        cache[idx].stamp = ++clock_stamp;
        stat_hits++;
        frame = cache[idx].frame;
        printf("[PC] hit  %s+%X -> frame %X (refs %u)\n",
               path, file_off, frame, cache[idx].refs);
    } else {
        stat_misses++;
    }
    pc_irq_unlock(cpsr);

    return frame;
}

int page_cache_insert(const char *path, u32 file_off, u32 frame)
{
    u32 cpsr;
    int idx;

    if (!path || frame == 0) {
        return -1;
    }

    cpsr = pc_irq_lock();

    /* 同一个键已经在了: 说明有人抢先填了, 调用者的帧留作私有页 */
    if (pc_find(path, file_off) >= 0) {
        stat_bypass++;
        pc_irq_unlock(cpsr);
        return -1;
    }

    idx = pc_alloc_slot();
    if (idx < 0) {
        stat_bypass++;
        pc_irq_unlock(cpsr);
        return -1;
    }

    cache[idx].valid    = 1;
    pc_copy_path(cache[idx].path, path);
    cache[idx].file_off = file_off;
    cache[idx].frame    = frame;
    cache[idx].refs     = 1;            /* 缓存自己钉住 */
    cache[idx].stamp    = ++clock_stamp;

    printf("[PC] miss %s+%X -> frame %X (cached)\n", path, file_off, frame);

    pc_irq_unlock(cpsr);
    return 0;
}

int page_cache_owns(u32 frame)
{
    u32 cpsr;
    u32 i;
    int owned = 0;

    if (frame == 0) {
        return 0;
    }

    cpsr = pc_irq_lock();
    for (i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (cache[i].valid && cache[i].frame == (frame & 0xFFFFF000u)) {
            owned = 1;
            break;
        }
    }
    pc_irq_unlock(cpsr);

    return owned;
}

u32 page_cache_ref(u32 frame)
{
    u32 cpsr;
    u32 i;
    u32 ref = 0;

    if (frame == 0) {
        return 0;
    }

    cpsr = pc_irq_lock();
    for (i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (cache[i].valid && cache[i].frame == (frame & 0xFFFFF000u)) {
            cache[i].refs++;
            cache[i].stamp = ++clock_stamp;
            ref = cache[i].refs;
            break;
        }
    }
    pc_irq_unlock(cpsr);

    return ref;
}

void page_cache_put(u32 frame)
{
    u32 cpsr;
    u32 i;

    if (frame == 0) {
        return;
    }

    cpsr = pc_irq_lock();
    for (i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (cache[i].valid && cache[i].frame == (frame & 0xFFFFF000u)) {
            if (cache[i].refs > 0u) {
                cache[i].refs--;
            }
            /* 减到 0 说明账错了 (缓存自己那 1 个引用不该被还掉),
             * 保底钉住, 宁可泄漏一页也不让页表指向空闲帧。 */
            if (cache[i].refs == 0u) {
                cache[i].refs = 1u;
                printf("[PC] WARN refcount underflow on %s+%X\n",
                       cache[i].path, cache[i].file_off);
            }
            pc_irq_unlock(cpsr);
            return;
        }
    }
    pc_irq_unlock(cpsr);

    /* 不在缓存里: 私有程序页 (数据段), 直接还帧 */
    frame_free(frame, 0);
}

int page_cache_reclaim(void)
{
    u32 cpsr;
    u32 i;
    int victim = -1;
    u32 oldest = 0xFFFFFFFFu;

    cpsr = pc_irq_lock();
    for (i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (cache[i].valid && cache[i].refs == 1u && cache[i].stamp < oldest) {
            oldest = cache[i].stamp;
            victim = (int)i;
        }
    }
    if (victim >= 0) {
        u32 f = cache[victim].frame;

        printf("[PC] reclaim %s+%X (frame %X) for private page\n",
               cache[victim].path, cache[victim].file_off, f);
        cache[victim].valid = 0;
        stat_evictions++;
        frame_free(f, 0);
    }
    pc_irq_unlock(cpsr);

    return (victim >= 0) ? 0 : -1;
}

void page_cache_stats(u32 *hits, u32 *misses, u32 *live,
                      u32 *evictions, u32 *bypass)
{
    u32 cpsr;
    u32 i;
    u32 n = 0;

    cpsr = pc_irq_lock();
    for (i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (cache[i].valid) {
            n++;
        }
    }
    if (hits)      *hits = stat_hits;
    if (misses)    *misses = stat_misses;
    if (live)      *live = n;
    if (evictions) *evictions = stat_evictions;
    if (bypass)    *bypass = stat_bypass;
    pc_irq_unlock(cpsr);
}
