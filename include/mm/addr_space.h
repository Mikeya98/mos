/**
 * @file    addr_space.h
 * @brief   用户地址空间管理 (v0.6 阶段4)
 *
 * 每个用户任务需要四块物理内存:
 *   - L1 页表   16KB, 16KB 对齐, 4096 个 section 描述符
 *   - L2 页表   1KB,  1KB  对齐, 256 个 4KB 页描述符 (私有区)
 *   - 数据页    4KB,  4KB  对齐
 *   - 栈        64KB, 4KB  对齐
 *
 * 这些内存必须是物理页对齐的。buddy_alloc 返回的是 "块头 + 4 字节",
 * 拿不到页对齐地址, 所以这里用链接脚本划出的静态池, 按顺序切分。
 * 池用完之后 user_space_create 返回 -1, 任务创建失败。
 */

#ifndef _MOS_ADDR_SPACE_H
#define _MOS_ADDR_SPACE_H

#include "lib/types.h"

/* 一个用户地址空间的物理资源 */
typedef struct {
    u32 l1_phys;        /* L1 页表物理地址 */
    u32 l2_phys;        /* L2 页表物理地址 */
    u32 data_phys;      /* 私有数据页物理地址 */
    u32 stack_phys;     /* 栈起始物理地址 (64KB) */
} user_space_t;

/**
 * @brief  分配并初始化一个用户地址空间
 *
 * 从静态池里切出 L1 / L2 / 数据页 / 栈, 然后调用
 * mmu_build_user_l1 / mmu_build_user_l2 填好页表。
 *
 * @param  us  输出参数
 * @return 0 成功, -1 池耗尽
 */
int user_space_create(user_space_t *us);

/**
 * @brief  打印地址空间的关键映射 (启动时调试用)
 */
void user_space_dump(const user_space_t *us, const char *name);

#endif /* _MOS_ADDR_SPACE_H */
