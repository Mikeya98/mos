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
 * @brief  归还一帧
 * @param  phys   帧的物理地址
 * @param  owner  调用者 pid (只用于调试打印)
 * @return 0 成功, -1 不是池子里的帧 / 已经空闲
 */
int frame_free(u32 phys, u32 owner);

u32 frame_free_count(void);
u32 frame_total(void);

/** 打印池子状态: 空闲数 / 高水位 / 每个已分配帧的 pid */
void frame_dump(const char *tag);

#endif /* _MOS_FRAME_H */
