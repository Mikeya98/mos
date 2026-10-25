/**
 * @file    mmu.h
 * @brief   MOS RTOS MMU 接口 (ARMv7-A 页表 / 地址空间 / TTBR0 切换)
 *
 * v0.6 阶段1: identity mapping + 开启 MMU, 把地址翻译打通。
 * v0.6 阶段3: DACR 域 0 改为 client, 用 section 的 AP 位做权限隔离。
 * v0.6 阶段4: 每个用户任务一张 L1 页表 + 一张 L2 表,
 *             上下文切换时换 TTBR0, 任务之间地址空间互不可见。
 *
 * 所有用户任务使用同一套虚拟地址布局, 但页表指向不同的物理页:
 *
 *   0x08000000 - 0x080FFFFF  用户代码 + 只读数据 (共享物理页, AP=10 用户只读)
 *   0x08100000 - 0x08100FFF  用户私有数据页   (每任务 4KB, AP=11)
 *   0x08101000 - 0x08110FFF  用户栈           (每任务 64KB, AP=11, XN)
 *
 * 0x08100000 起的 1MB 由 L2 表按 4KB 粒度映射, 其余用户地址一律 fault。
 */

#ifndef _MOS_MMU_H
#define _MOS_MMU_H

#include "lib/types.h"

/* =========================== 内核地址空间 =========================== */
#define KERNEL_BASE         0x00100000u
#define KERNEL_MAP_SIZE     0x40000000u     /* 0x00000000 - 0x40000000 普通内存 */

/* =========================== 用户虚拟地址布局 =========================== */
/* 每个用户任务看到的地址布局完全相同, 物理页不同 —— 这就是"地址空间" */
#define USER_CODE_VADDR     0x08000000u     /* 代码 + .user_rodata, 1MB section */
#define USER_PRIV_VADDR     0x08100000u     /* L2 表覆盖的 1MB 私有区 */
#define USER_DATA_VADDR     0x08100000u     /* 私有数据页 (4KB) */
#define USER_STACK_VADDR    0x08101000u     /* 用户栈起始 (64KB = 16 页) */
#define USER_STACK_SIZE     (64u * 1024u)
#define USER_STACK_TOP      (USER_STACK_VADDR + USER_STACK_SIZE)

#define USER_SPACE_BASE     USER_CODE_VADDR
#define USER_SPACE_LIMIT    USER_STACK_TOP  /* 不含 */

/* 与 kernel.lds 一致: 用户代码段的物理地址 */
#define USER_CODE_PHYS      USER_CODE_VADDR

/* =========================== 页表属性 =========================== */
/* section descriptor (L1, 1MB 粒度) */
#define L1_MEM_PRIV_ATTR    0x40Eu      /* AP=01 仅特权, 可执行 */
#define L1_DEV_PRIV_ATTR    0x412u      /* AP=01 仅特权, XN */
#define L1_USER_RO_ATTR     0x80Eu      /* AP=10 特权可写/用户只读, 可执行 */

/* small page descriptor (L2, 4KB 粒度)
 *
 * 注意: 二级页表的 AP 位布局和一级 section 不同!
 *   section  : AP[1:0] = bit[11:10], AP[2] = bit[16]
 *   small page: AP[1:0] = bit[5:4],  AP[2] = bit[9]
 * 按 section 的写法填二级页表会得到 AP=000 (谁都不可访问),
 * 症状是用户任务一碰自己的栈就 permission fault。
 *
 * 0x03F = AP=011 用户读写 + C/B=11 + XN=1
 * 0x02E = AP=010 用户只读 + C/B=11 + 可执行
 */
#define L2_USER_RW_XN       0x03Fu
#define L2_USER_RO          0x02Eu

/* 粗页表 L1 描述符: 低两位 01, [31:10] = L2 表物理地址 */
#define L1_COARSE_TABLE     0x1u

/* =========================== 公共 API =========================== */

/**
 * @brief  建立内核页表并开启 MMU
 *
 * 映射策略 (identity mapping):
 *   - 0x00000000 - 0x40000000  普通内存, AP=01, cacheable
 *   - 0x08000000 - 0x080FFFFF  用户代码区, AP=10 (用户只读)
 *   - 0xE0000000 - 0xFFFFFFFF  外设, device, AP=01
 *   - 其余区域 fault
 *
 * 用户任务的私有区 (0x08100000 起) 在内核页表里不特殊处理,
 * 用户页表会用自己的 L2 表覆盖这一项。
 */
void mmu_init(void);

/**
 * @brief  取内核页表的物理地址 (内核态任务的 TTBR0 值)
 */
u32 mmu_kernel_l1(void);

/**
 * @brief  用内核页表做模板, 建立一张用户 L1 页表
 *
 * 先整表复制内核页表 (保证内核映射完全一致), 再把
 * L1[129] (0x08100000) 换成指向私有 L2 表的粗页表描述符。
 *
 * @param  l1        L1 页表 (16KB 对齐)
 * @param  l2_phys   L2 表物理地址 (1KB 对齐)
 */
void mmu_build_user_l1(u32 *l1, u32 l2_phys);

/**
 * @brief  填充用户 L2 表: 数据页 + 栈页
 *
 * L2[0]      → data_phys   (VA 0x08100000, 4KB)
 * L2[1..16]  → stack_phys + (i-1)*4KB (VA 0x08101000 - 0x08110FFF)
 * 其余条目 fault。
 *
 * @param  l2           L2 表 (1KB 对齐, 256 项)
 * @param  data_phys    数据页物理地址 (4KB 对齐)
 * @param  stack_phys   栈起始物理地址 (4KB 对齐, 64KB 连续)
 */
void mmu_build_user_l2(u32 *l2, u32 data_phys, u32 stack_phys);

/**
 * @brief  切换 TTBR0 到指定页表 (上下文切换时调用)
 *
 * 与当前页表相同时直接返回。切换后无效化整个 TLB。
 * 内核区在所有页表里映射一致, 所以切换过程中当前 PC/SP 始终有效。
 */
void mmu_activate(u32 l1_phys);

/**
 * @brief  读当前 TTBR0 (调试用)
 */
u32 mmu_current_l1(void);

/**
 * @brief  检查用户指针 [start, start+len) 是否可访问 (走页表)
 *
 * @param  start  用户虚拟地址
 * @param  len    字节数
 * @param  write  0 = 只读检查, 1 = 需要写权限
 * @return 1 可访问, 0 非法
 *
 * 系统调用不能只做范围检查: 用户区里也不是每一页都映射了。
 * 这里直接遍历当前页表, 顺便把"内核替用户读内存"的边界收紧到
 * 真正映射过的页。
 */
int mmu_user_access_ok(u32 start, u32 len, int write);

#endif /* _MOS_MMU_H */
