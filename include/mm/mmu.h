/**
 * @file    mmu.h
 * @brief   MOS RTOS MMU 接口 (ARMv7-A 页表 / 地址空间 / TTBR0 切换)
 *
 * v0.6 阶段1: identity mapping + 开启 MMU, 把地址翻译打通。
 * v0.6 阶段3: DACR 域 0 改为 client, 用 section 的 AP 位做权限隔离。
 * v0.6 阶段4: 每个用户任务一张 L1 页表 + 一张 L2 表,
 *             上下文切换时换 TTBR0, 任务之间地址空间互不可见。
 * v0.6 阶段5: TTBR0 低 8 位带 ASID, 私有页打 nG 位 —— 切换地址空间
 *             不再刷整个 TLB, 只换 ASID。
 *
 * 所有用户任务使用同一套虚拟地址布局, 但页表指向不同的物理页:
 *
 *   0x08000000 - 0x080FFFFF  用户代码 + 只读数据 (共享物理页, AP=10 用户只读)
 *   0x08100000 - 0x08100FFF  用户私有数据页   (1 页, 首次访问时分配)
 *   0x08101000 - 0x08101FFF  栈溢出保护页     (永远 fault)
 *   0x08102000 - 0x081FFFFF  用户栈           (最多 1016KB, 按需增长)
 *
 * 0x08100000 起的 1MB 由 L2 表按 4KB 粒度映射, 其余用户地址一律 fault。
 * 阶段6 之后 L2 表初始是全 fault 的, 页框在第一次访问时才分配。
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
#define USER_DATA_VADDR     0x08100000u     /* 私有数据页 (L2[0], 4KB) */
#define USER_GUARD_VADDR    0x08101000u     /* 栈溢出保护页 (L2[1], 永远不映射) */
#define USER_STACK_VADDR    0x08102000u     /* 栈可以长到的最低地址 (L2[2]) */
#define USER_STACK_TOP      0x08200000u     /* 栈顶 = 私有区之外的第一页 */
#define USER_STACK_MAX      (USER_STACK_TOP - USER_STACK_VADDR)  /* 1016KB */

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
 * 0x83F = 上面的 0x03F + nG(bit11)=1, 非全局
 * 0x82E = 上面的 0x02E + nG(bit11)=1, 非全局
 *
 * nG 位是阶段5 的 ASID 方案成立的前提: 只有打了 nG 的页才会按照
 * TTBR0 里的 ASID 打标签, 全局页 (nG=0) 被所有 ASID 共享 —— 内核
 * 映射正是靠这一点在换地址空间时继续有效。
 */
#define L2_USER_RW_XN       0x83Fu
#define L2_USER_RO          0x82Eu
#define L2_NG_BIT           0x800u

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
 * @brief  初始化用户 L2 表: 全部 fault
 *
 * 阶段6 之后页框是第一次访问时分配的 (mm/vm.c 的 vm_fault),
 * 所以建表时一项都不填。L2[0]=数据页、L2[1]=保护页、L2[2..255]=栈,
 * 具体哪一页能映射由 vm.c 的地址分类决定。
 *
 * @param  l2  L2 表 (1KB 对齐, 256 项)
 */
void mmu_build_user_l2(u32 *l2);

/**
 * @brief  切换 TTBR0 到指定页表 + ASID (上下文切换时调用)
 *
 * TTBR0 = l1_phys | asid。内核区和用户代码区的映射是全局的
 * (nG=0), 所有 ASID 共用; 只有数据页/栈这些私有页打 nG 位,
 * 按 ASID 分开缓存。所以切换时不需要刷 TLB。
 *
 * @param  l1_phys  L1 页表物理地址 (16KB 对齐)
 * @param  asid     地址空间编号 (0 保留给内核, 1..255 给用户任务)
 */
void mmu_activate(u32 l1_phys, u32 asid);

/**
 * @brief  无效化某个 ASID 的全部 TLB 项
 *
 * ASID 被回收再分配给新任务之前必须调用, 否则 TLB 里可能还留着
 * 上一个任务在同一个 ASID 下的翻译。
 */
void mmu_flush_asid(u32 asid);

/**
 * @brief  读当前 TTBR0 的页表基址 (调试用, 已去掉 ASID)
 */
u32 mmu_current_l1(void);

/**
 * @brief  读当前 ASID (调试用)
 */
u32 mmu_current_asid(void);

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
