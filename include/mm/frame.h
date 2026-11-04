/**
 * @file    frame.h
 * @brief   物理页框分配器 (v0.6 阶段6)
 *
 * 按需分页之后, 数据页和栈页不再跟着任务一起预分配, 而是在第一次
 * 访问时从页框池里现取。页框池就是 kernel.lds 里划出来的一段
 * 4KB 对齐的静态内存, 一共 FRAME_COUNT 帧。
 *
 * 分配器本身很简单: 一个位图 + 一个空闲计数。所有者 (pid) 单独存
 * 一份, 只用于调试打印 —— 有了它, 一个页框是谁的、有没有泄漏,
 * 一眼就能看出来。
 *
 * v0.6 阶段8 增加每帧引用计数, 为 fork 的写时复制 (COW) 服务:
 * 父子进程共享同一帧时 refcount = 2, 谁写谁复制, 最后一个
 * 引用者退出时才真正归还物理帧。
 */

#ifndef _MOS_FRAME_H
#define _MOS_FRAME_H

#include "lib/types.h"

#define FRAME_SIZE      4096u
#define FRAME_COUNT     64u         /* 与 kernel.lds 的 .frame_pool 一致 */

/** 初始化页框池 (kernel_main 里, mmu_init 之后调用) */
void frame_init(void);

/**
 * @brief  分配一帧 4KB 物理内存
 * @param  owner  所有者 pid (只用于调试)
 * @return 物理地址 (4KB 对齐), 池子耗尽时返回 0
 */
u32 frame_alloc(u32 owner);

/**
 * @brief  归还一帧 (递减引用计数)
 *
 * 阶段8 之后这个函数是"释放一个引用"而不是"无条件归还":
 * refcount 减到 0 才真正把帧还给池子。fork 出来的父子进程
 * 共用一帧时, 谁先退出都不会把别人的数据抹掉。
 *
 * @param  phys   帧的物理地址
 * @param  owner  调用者 pid (只用于调试打印)
 * @return 0 成功 (已归还或计数已递减), -1 不是池子里的帧 / 已经空闲
 */
int frame_free(u32 phys, u32 owner);

/**
 * @brief  增加一帧的引用计数 (fork 共享页时调用)
 * @return 增加后的引用计数, 帧非法时返回 0
 */
u32 frame_ref_inc(u32 phys);

/**
 * @brief  查询一帧的引用计数 (COW 判断用)
 * @return 引用计数, 帧非法/空闲时返回 0
 */
u32 frame_refcount(u32 phys);

u32 frame_free_count(void);
u32 frame_total(void);

/** 打印池子状态: 空闲数 / 高水位 / 每个已分配帧的 pid */
void frame_dump(const char *tag);

#endif /* _MOS_FRAME_H */
