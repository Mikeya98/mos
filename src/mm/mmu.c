/**
 * @file    mmu.c
 * @brief   MOS RTOS MMU — L1 页表建立 + identity mapping + 权限隔离 (v0.6 阶段 1/3)
 *
 * v0.1-v0.5 的 MOS 一直运行在 MMU 关闭状态, 代码/数据直接按物理地址
 * 访问 (链接脚本把整个系统放在 0x00100000 起的一块连续 DDR)。所有任务
 * 共享同一地址空间, 也没有用户态/内核态隔离。
 *
 * 阶段1 先把 MMU 开起来, 用 identity mapping (虚拟地址 == 物理地址),
 * 系统行为不变, 只把"地址翻译"这条通路打通。
 * 阶段3 在同一个页表上做权限隔离: 内核区 AP=01, 用户区 AP=11,
 * DACR 域 0 改成 client, 让 AP 位生效。
 *
 * 页表: ARMv7-A 第一级页表 (L1) 有 4096 个 32-bit 条目, 每个描述 1MB
 * 地址空间, 整张表 16KB, 要求 16KB (14-bit) 对齐。这里直接用 section
 * 映射 (1MB 粒度), 不引入第二级页表。
 */

#include "mm/mmu.h"
#include "lib/types.h"

/* =========================== 链接脚本符号 =========================== */
/* kernel.lds 的 .mmu_table 段 (16KB 对齐), 用作 L1 页表 */
extern u32 _mmu_table_base;

/* =========================== 常量 =========================== */
#define L1_ENTRIES      4096u        /* L1 条目数 (4096 × 1MB = 4GB) */

/* section descriptor 属性位 (ARMv7-A short-descriptor)
 * 普通内存: TEX=000 C=1 B=1 domain=0 → WBWA
 * 外设设备: TEX=000 C=0 B=0 domain=0 XN=1 → 强序, 不可缓存不可执行
 *
 * AP[11:10] 权限编码 (DACR 域为 client 时生效):
 *   00 = 特权/用户都不可访问
 *   01 = 特权可读写, 用户不可访问
 *   10 = 特权读写, 用户只读
 *   11 = 特权/用户都可读写
 *
 * 阶段3 的隔离靠两种 AP:
 *   内核区 AP=01 → 用户态访问触发 Permission Fault (Data Abort)
 *   用户区 AP=11 → 用户态正常读写执行
 */
#define L1_MEM_PRIV_ATTR  0x40Eu     /* AP=01 仅特权, 可执行 */
#define L1_DEV_PRIV_ATTR  0x412u     /* AP=01 仅特权, XN (不可执行) */
#define L1_USER_ATTR      0xC0Eu     /* AP=11 用户可访问, 可执行 */
#define L1_USER_XN_ATTR   0xC1Eu     /* AP=11 用户可访问, XN (栈不可执行) */

/* DACR: 域 0 设为 client (0b01), 让页表里的 AP 位真正生效。
 * manager (0b11) 会完全忽略 AP 位 — 阶段2 用的是 manager,
 * 所以那时候"用户态"其实能访问全部内存。 */
#define DACR_DOMAIN0_CLIENT  0x1u

/* identity mapping 覆盖的 section 区间 (每个 section = 1MB) */
#define MEM_BASE_SEC    0u          /* 0x00000000 普通内存起始 */
#define MEM_END_SEC     1024u       /* 0x40000000 普通内存结束 (1GB) */
#define DEV_BASE_SEC    3584u       /* 0xE0000000 外设起始 */
#define DEV_END_SEC     4096u       /* 0xFFFFFFFF 外设结束 */

/* 用户区 section 索引 (与 mmu.h 的地址宏对应) */
#define USER_CODE_SEC      128u     /* 0x08000000 - 0x080FFFFF */
#define USER_STACK_SEC     129u     /* 0x08100000 */
#define USER_STACK_END_SEC 160u     /* 0x0A000000 */

/* =========================== 协处理器操作 =========================== */
/* 这些操作只能通过 MCR/MRC 完成, C 无法直接表达, 用内联汇编封装 */

static inline void cp15_write_ttbr0(u32 val)
{
    __asm__ volatile ("MCR p15, 0, %0, c2, c0, 0" : : "r"(val) : "memory");
}

static inline void cp15_write_dacr(u32 val)
{
    __asm__ volatile ("MCR p15, 0, %0, c3, c0, 0" : : "r"(val) : "memory");
}

static inline void cp15_invalidate_tlb(void)
{
    u32 zero = 0;
    __asm__ volatile ("MCR p15, 0, %0, c8, c7, 0" : : "r"(zero) : "memory");
}

static inline u32 cp15_read_sctlr(void)
{
    u32 val;
    __asm__ volatile ("MRC p15, 0, %0, c1, c0, 0" : "=r"(val));
    return val;
}

static inline void cp15_write_sctlr(u32 val)
{
    __asm__ volatile ("MCR p15, 0, %0, c1, c0, 0" : : "r"(val) : "memory");
}

static inline void mmu_dsb(void)
{
    __asm__ volatile ("DSB" ::: "memory");
}

static inline void mmu_isb(void)
{
    __asm__ volatile ("ISB" ::: "memory");
}

/* =========================== 页表建立 =========================== */

void mmu_init(void)
{
    u32 *l1 = &_mmu_table_base;
    u32 i;

    /* 1. 全部置 fault: 未映射区域访问 → Data Abort */
    for (i = 0; i < L1_ENTRIES; i++) {
        l1[i] = 0;
    }

    /* 2. 普通内存区 (1GB): identity map, cacheable
     *    默认 AP=01 (仅特权)。用户区下面单独覆盖。 */
    for (i = MEM_BASE_SEC; i < MEM_END_SEC; i++) {
        l1[i] = (i << 20) | L1_MEM_PRIV_ATTR;
    }

    /* 3. 用户区 (v0.6 阶段3):
     *    0x08000000 - 0x080FFFFF  用户代码/只读/数据, 可执行
     *    0x08100000 - 0x09FFFFFF  用户栈, 不可执行 */
    l1[USER_CODE_SEC] = (USER_CODE_SEC << 20) | L1_USER_ATTR;
    for (i = USER_STACK_SEC; i < USER_STACK_END_SEC; i++) {
        l1[i] = (i << 20) | L1_USER_XN_ATTR;
    }

    /* 4. 外设区 (512MB): identity map, device (不可 cache) */
    for (i = DEV_BASE_SEC; i < DEV_END_SEC; i++) {
        l1[i] = (i << 20) | L1_DEV_PRIV_ATTR;
    }

    /* 5. 确保页表写入完成, 对后续页表遍历 (TTW) 可见 */
    mmu_dsb();

    /* 6. 域访问控制 + 页表基址
     *    DACR 域0 = client (0b01): 页表 AP 位生效, 权限隔离从这里开始。 */
    cp15_write_dacr(DACR_DOMAIN0_CLIENT);
    cp15_write_ttbr0((u32)(uintptr_t)&_mmu_table_base);
    mmu_dsb();

    /* 7. 无效化整个 TLB (清掉 FSBL 时代的旧映射) */
    cp15_invalidate_tlb();
    mmu_dsb();

    /* 8. 开 MMU: SCTLR.M = 1 (bit0)
     *    只开 MMU, 不动 Cache (bit2/bit12), Cache 以后单独开。 */
    cp15_write_sctlr(cp15_read_sctlr() | 0x1u);
    mmu_isb();
}
