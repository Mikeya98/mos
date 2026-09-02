/**
 * @file    mmu.c
 * @brief   MOS RTOS MMU — L1 页表建立 + identity mapping + 开启 MMU (v0.6 阶段 1)
 *
 * v0.1-v0.5 的 MOS 一直运行在 MMU 关闭状态, 代码/数据直接按物理地址
 * 访问 (链接脚本把整个系统放在 0x00100000 起的一块连续 DDR)。所有任务
 * 共享同一地址空间, 也没有用户态/内核态隔离。
 *
 * 本文件做最基础的一步: 把 MMU 开起来, 但先用 identity mapping
 * (虚拟地址 == 物理地址), 让系统行为不变, 只把"地址翻译"这条通路打通。
 * 用户态切换和每任务独立地址空间在后续阶段做。
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
 * 普通内存: TEX=000 C=1 B=1 AP=11 domain=0 → WBWA, 可读可写可执行
 * 外设设备: TEX=000 C=0 B=0 AP=11 domain=0 XN=1 → 强序, 不可缓存不可执行 */
#define L1_MEM_ATTR     0xC0Eu
#define L1_DEV_ATTR     0xC12u

/* identity mapping 覆盖的 section 区间 */
#define MEM_BASE_SEC    0u          /* 0x00000000 普通内存起始 */
#define MEM_END_SEC     1024u       /* 0x40000000 普通内存结束 (1GB) */
#define DEV_BASE_SEC    3584u       /* 0xE0000000 外设起始 */
#define DEV_END_SEC     4096u       /* 0xFFFFFFFF 外设结束 */

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

    /* 2. 普通内存区 (1GB): identity map, cacheable */
    for (i = MEM_BASE_SEC; i < MEM_END_SEC; i++) {
        l1[i] = (i << 20) | L1_MEM_ATTR;
    }

    /* 3. 外设区 (512MB): identity map, device (不可 cache) */
    for (i = DEV_BASE_SEC; i < DEV_END_SEC; i++) {
        l1[i] = (i << 20) | L1_DEV_ATTR;
    }

    /* 4. 确保页表写入完成, 对后续页表遍历 (TTW) 可见 */
    mmu_dsb();

    /* 5. 域访问控制 + 页表基址
     *    DACR 域0 = manager (0b11): 域内 AP 位不生效, 直接完全访问。
     *    阶段1 不做权限隔离, manager 最简单。 */
    cp15_write_dacr(0x3u);
    cp15_write_ttbr0((u32)(uintptr_t)&_mmu_table_base);
    mmu_dsb();

    /* 6. 无效化整个 TLB (清掉 FSBL 时代的旧映射) */
    cp15_invalidate_tlb();
    mmu_dsb();

    /* 7. 开 MMU: SCTLR.M = 1 (bit0)
     *    只开 MMU, 不动 Cache (bit2/bit12), Cache 以后单独开。 */
    cp15_write_sctlr(cp15_read_sctlr() | 0x1u);
    mmu_isb();
}
