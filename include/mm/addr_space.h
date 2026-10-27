/**
 * @file    addr_space.h
 * @brief   用户地址空间管理 (v0.6 阶段4/5)
 *
 * 每个用户任务需要四块物理内存:
 *   - L1 页表   16KB, 16KB 对齐, 4096 个 section 描述符
 *   - L2 页表   1KB,  1KB  对齐, 256 个 4KB 页描述符 (私有区)
 *   - 数据页    4KB,  4KB  对齐
 *   - 栈        64KB, 4KB  对齐
 *
 * 这些内存必须是页对齐的。buddy_alloc 返回的是 "块头 + 4 字节",
 * 拿不到页对齐地址, 所以这里用链接脚本划出的静态池。
 *
 * 阶段4 是顺序切分、不回收; 阶段5 改成按槽位管理:
 *
 *   槽位 i  →  L1  池的第 i 个 16KB
 *              L2  池的第 i 个 1KB
 *              物理池的第 i 个 (4KB + 64KB)
 *              ASID = i + 1
 *
 * 四份资源共用同一个槽位号, 分配和释放只操作一个位图。任务退出后
 * 槽位归还, 下一个任务可以复用同一套资源 —— 这也是 ASID 必须
 * 在归还时刷掉的原因。
 */

#ifndef _MOS_ADDR_SPACE_H
#define _MOS_ADDR_SPACE_H

#include "lib/types.h"

/* 静态池的槽位数量, 与 kernel.lds 里的池大小一致 */
#define USER_SPACE_SLOTS    8

/* 一个用户地址空间的物理资源 */
typedef struct {
    u32 l1_phys;        /* L1 页表物理地址 */
    u32 l2_phys;        /* L2 页表物理地址 */
    u32 data_phys;      /* 私有数据页物理地址 */
    u32 stack_phys;     /* 栈起始物理地址 (64KB) */
    u32 asid;           /* 地址空间编号 (1..USER_SPACE_SLOTS) */
    u32 slot;           /* 池槽位 (0..USER_SPACE_SLOTS-1) */
} user_space_t;

/**
 * @brief  分配并初始化一个用户地址空间
 *
 * 从池里找一个空闲槽位, 切出 L1 / L2 / 数据页 / 栈, 然后调用
 * mmu_build_user_l1 / mmu_build_user_l2 填好页表。
 *
 * @param  us  输出参数
 * @return 0 成功, -1 槽位耗尽
 */
int user_space_create(user_space_t *us);

/**
 * @brief  归还一个用户地址空间
 *
 * 先按 ASID 清 TLB, 再把槽位标记为空闲。页表内容不用清,
 * 下一次 user_space_create 会整表重建。
 *
 * @param  us  要归还的地址空间 (重复归还会被忽略)
 */
void user_space_destroy(user_space_t *us);

/**
 * @brief  当前空闲的地址空间槽位数 (调试/演示用)
 */
u32 user_space_free_count(void);

/**
 * @brief  打印地址空间的关键映射 (启动时调试用)
 */
void user_space_dump(const user_space_t *us, const char *name);

#endif /* _MOS_ADDR_SPACE_H */
