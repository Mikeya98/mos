/**
 * @file    page_cache.h
 * @brief   文件页缓存 (v0.6 阶段13)
 *
 * 阶段12 的按需装载每次缺页都要 open/seek/read/close 一遍文件。同一个
 * 程序被第二个进程执行时, 内容一模一样, 却还要再读一遍、再占一组页框。
 *
 * 这一层把"文件内容"和"进程页表"分开:
 *
 *   页缓存   (path, 文件偏移) -> 物理帧, 自带引用计数和 LRU 驱逐
 *   进程页表 → 只读代码页直接指向缓存帧, 多个进程共享同一份物理内存
 *
 * 只有只读段进缓存。数据段每个进程必须有自己的副本 (可写), 缓存了也
 * 没用; 私有数据页仍然走 frame 池。
 *
 * 引用计数的约定:
 *   - 插入时 refs = 1, 这个 1 属于缓存自己 (缓存钉住这一帧)
 *   - 每多一个进程映射这一页, refs++
 *   - 进程退出/exec 时 page_cache_put 减引用
 *   - refs 回到 1 的页可以被 LRU 驱逐; refs > 1 的页驱逐不了
 *
 * 和 Linux 的 page cache 相比, 这里没有 radix tree、没有回写、没有
 * 脏页 —— 缓存的都是只读的 ELF 正文, 天然干净。
 */

#ifndef _MOS_PAGE_CACHE_H
#define _MOS_PAGE_CACHE_H

#include "lib/types.h"

#define PAGE_CACHE_SLOTS     16             /* 缓存 16 页 = 64KB */
#define PAGE_CACHE_PATH_MAX  64             /* 和 PROG_PATH_MAX 一致 */

/** 初始化缓存表 (frame 池初始化之后调用) */
void page_cache_init(void);

/**
 * @brief  查缓存并增加引用
 * @param  path      程序文件路径
 * @param  file_off  这一页内容在文件里的起始偏移
 * @return 命中返回物理帧地址 (调用者持有一个新引用), 未命中返回 0
 */
u32 page_cache_lookup(const char *path, u32 file_off);

/**
 * @brief  把一个已经填好内容的页框插入缓存
 *
 * 成功时帧的所有权转给缓存 (refs = 1), 调用者若要同时把它映射进
 * 页表, 需要再调 frame_ref_inc。失败 (缓存全是被映射的页) 时帧
 * 仍归调用者, 按私有页处理。
 *
 * @return 0 成功, -1 没地方放
 */
int page_cache_insert(const char *path, u32 file_off, u32 frame);

/** 这一帧是否由页缓存管理 (fork 时决定共享还是复制) */
int page_cache_owns(u32 frame);

/**
 * @brief  为一个新的页表映射增加缓存引用
 *
 * 插入缓存之后第一次映射、以及 fork 共享代码页时调用。缓存页的
 * 生命周期由缓存表自己的 refs 决定, 不依赖 frame 池的引用计数
 * (帧的 refcount 始终停在 1, 由缓存代表)。
 *
 * @return 增加后的引用数, 帧不在缓存里返回 0
 */
u32 page_cache_ref(u32 frame);

/**
 * @brief  进程释放一页程序页
 *
 * 缓存页: 引用计数减一, 减到只剩缓存自己时继续驻留;
 * 私有页: 不在缓存表里, 直接还帧。
 */
void page_cache_put(u32 frame);

/**
 * @brief  回收一页空闲缓存 (refs == 1, LRU 最旧)
 * @return 0 回收到一帧, -1 没有可回收的页
 *
 * 页框池耗尽时先试这个: 文件页是干净的, 丢掉比换出一页私有数据便宜。
 */
int page_cache_reclaim(void);

/** 读统计: 命中 / 未命中 / 当前驻留 / 驱逐次数 / 放不进去的次数 */
void page_cache_stats(u32 *hits, u32 *misses, u32 *live,
                      u32 *evictions, u32 *bypass);

#endif /* _MOS_PAGE_CACHE_H */
