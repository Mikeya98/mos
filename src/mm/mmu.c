/**
 * @file    mmu.c
 * @brief   MOS RTOS MMU — 页表 / 地址空间 / TTBR0 切换 (v0.6 阶段 1/3/4)
 *
 * v0.1-v0.5 的 MOS 运行在 MMU 关闭状态, 所有任务共享同一地址空间。
 *
 * 阶段1 开 MMU, identity mapping (虚拟地址 == 物理地址), 行为不变。
 * 阶段3 用 section 的 AP 位做内核/用户权限隔离 (所有任务仍共用一张页表)。
 * 阶段4 给每个用户任务建独立的 L1 + L2 页表, 上下文切换时换 TTBR0,
 *        任务看到相同的虚拟地址, 但物理页各不相同。
 *
 * 页表结构 (ARMv7-A 短描述符):
 *   L1: 4096 项 × 1MB, 16KB, 16KB 对齐
 *   L2: 256 项 × 4KB, 1KB, 1KB 对齐 (coarse page table)
 *
 * 用户地址布局:
 *   0x08000000  1MB section  → 共享的用户代码/只读数据 (AP=10 用户只读)
 *   0x08100000  1MB 由 L2 表映射:
 *       L2[0]      数据页 4KB
 *       L2[1..16]  栈 64KB
 */

#include "mm/mmu.h"
#include "lib/types.h"

/* =========================== 链接脚本符号 =========================== */
/* kernel.lds 的 .mmu_table 段 (16KB 对齐), 内核主页表 */
extern u32 _mmu_table_base;

/* =========================== 常量 =========================== */
#define L1_ENTRIES      4096u        /* 4096 × 1MB = 4GB */
#define L2_ENTRIES      256u         /* 256 × 4KB  = 1MB */

#define MEM_END_SEC     1024u        /* 0x40000000 普通内存结束 (1GB) */
#define DEV_BASE_SEC    3584u        /* 0xE0000000 外设起始 */
#define DEV_END_SEC     4096u        /* 0xFFFFFFFF 外设结束 */

#define USER_CODE_SEC   128u         /* 0x08000000 */
#define USER_PRIV_SEC   129u         /* 0x08100000 */

/* DACR: 域 0 设为 client (0b01), 页表 AP 位才生效 */
#define DACR_DOMAIN0_CLIENT  0x1u

/* 栈页数: 64KB / 4KB */
#define USER_STACK_PAGES     (USER_STACK_SIZE / 4096u)

/* 当前生效的页表物理地址 (避免无意义的 TLB 刷新) */
static u32 active_l1 = 0;

/* =========================== 协处理器操作 =========================== */

static inline void cp15_write_ttbr0(u32 val)
{
    __asm__ volatile ("MCR p15, 0, %0, c2, c0, 0" : : "r"(val) : "memory");
}

static inline u32 cp15_read_ttbr0(void)
{
    u32 val;
    __asm__ volatile ("MRC p15, 0, %0, c2, c0, 0" : "=r"(val));
    return val;
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

/* =========================== 内核页表 =========================== */

void mmu_init(void)
{
    u32 *l1 = &_mmu_table_base;
    u32 i;

    /* 1. 全部置 fault */
    for (i = 0; i < L1_ENTRIES; i++) {
        l1[i] = 0;
    }

    /* 2. 普通内存区 (1GB): identity map, cacheable, AP=01 仅特权 */
    for (i = 0; i < MEM_END_SEC; i++) {
        l1[i] = (i << 20) | L1_MEM_PRIV_ATTR;
    }

    /* 3. 用户代码区: 1MB section, 用户只读 + 可执行
     *    内核作为模板复制到每张用户页表, 所以物理地址 = 虚拟地址。 */
    l1[USER_CODE_SEC] = USER_CODE_PHYS | L1_USER_RO_ATTR;

    /* 4. 外设区 (512MB): device, 不可缓存, XN */
    for (i = DEV_BASE_SEC; i < DEV_END_SEC; i++) {
        l1[i] = (i << 20) | L1_DEV_PRIV_ATTR;
    }

    mmu_dsb();

    /* 5. DACR 域 0 = client, 页表 AP 位生效 */
    cp15_write_dacr(DACR_DOMAIN0_CLIENT);
    cp15_write_ttbr0((u32)(uintptr_t)&_mmu_table_base);
    mmu_dsb();

    /* 6. 无效化 TLB, 开 MMU */
    cp15_invalidate_tlb();
    mmu_dsb();

    cp15_write_sctlr(cp15_read_sctlr() | 0x1u);
    mmu_isb();

    active_l1 = (u32)(uintptr_t)&_mmu_table_base;
}

u32 mmu_kernel_l1(void)
{
    return (u32)(uintptr_t)&_mmu_table_base;
}

/* =========================== 用户页表 =========================== */

void mmu_build_user_l1(u32 *l1, u32 l2_phys)
{
    const u32 *master = &_mmu_table_base;
    u32 i;

    /*
     * 先整表复制内核页表。内核区、外设区的映射在每张用户页表里
     * 必须完全一致 —— 任务通过 SVC 进内核时, 当前生效的就是任务
     * 自己的页表, 内核代码和栈必须能访问。
     *
     * 复制之后唯一要改的是用户私有区那一项:
     *   0x08100000 本来是 1MB section (identity), 换成指向本任务
     *   L2 表的粗页表描述符。L1[31:10] 存 L2 物理地址, 低两位 01。
     */
    for (i = 0; i < L1_ENTRIES; i++) {
        l1[i] = master[i];
    }

    l1[USER_PRIV_SEC] = (l2_phys & 0xFFFFFC00u) | L1_COARSE_TABLE;

    mmu_dsb();
}

void mmu_build_user_l2(u32 *l2, u32 data_phys, u32 stack_phys)
{
    u32 i;

    /* 全部 fault: 私有区里没映射的页, 用户访问直接 Data Abort */
    for (i = 0; i < L2_ENTRIES; i++) {
        l2[i] = 0;
    }

    /* L2[0] → VA 0x08100000, 私有数据页 */
    l2[0] = (data_phys & 0xFFFFF000u) | L2_USER_RW_XN;

    /* L2[1..16] → VA 0x08101000 - 0x08110FFF, 用户栈 */
    for (i = 0; i < USER_STACK_PAGES; i++) {
        l2[1 + i] = ((stack_phys + i * 4096u) & 0xFFFFF000u) | L2_USER_RW_XN;
    }

    mmu_dsb();
}

/* =========================== TTBR0 切换 =========================== */

void mmu_activate(u32 l1_phys)
{
    if (l1_phys == active_l1) {
        return;             /* 同一个地址空间, 不用动 TLB */
    }

    cp15_write_ttbr0(l1_phys);
    mmu_dsb();

    /*
     * 换页表必须清 TLB, 否则新任务可能命中旧任务留下的
     * 地址翻译 (同一虚拟地址 → 旧任务的物理页)。
     *
     * 这里用最保守的全量无效化。ARMv7 的 ASID 机制可以做到
     * 不刷 TLB, 需要在页表项里打 nG 位并给每个任务分配 ASID,
     * 留给后面优化。
     */
    cp15_invalidate_tlb();
    mmu_dsb();
    mmu_isb();

    active_l1 = l1_phys;
}

u32 mmu_current_l1(void)
{
    return cp15_read_ttbr0();
}

/* =========================== 用户指针检查 =========================== */

/**
 * @brief  检查单个 4KB 页对用户是否可访问
 * @param  l1    当前 L1 页表
 * @param  va    虚拟地址 (已按页对齐)
 * @param  write  0 = 读, 1 = 写
 */
static int user_page_ok(const u32 *l1, u32 va, int write)
{
    u32 desc = l1[va >> 20];
    u32 ap;

    switch (desc & 0x3u) {
    case 0x2:   /* section (1MB) */
        ap = (desc >> 10) & 0x3u;
        if (ap < 2) return 0;               /* 仅特权 */
        if (write && ap != 3) return 0;     /* 用户只读 */
        return 1;

    case 0x1: { /* coarse page table */
        const u32 *l2 = (const u32 *)(uintptr_t)(desc & 0xFFFFFC00u);
        u32 entry = l2[(va >> 12) & 0xFFu];

        if ((entry & 0x2u) == 0) return 0;  /* fault 或 large page (未使用) */

        /* small page 的 AP[2] 在 bit9, AP[1:0] 在 bit[5:4] */
        if ((entry & (1u << 9)) != 0) return 0;     /* 保留编码, 不给访问 */
        ap = (entry >> 4) & 0x3u;
        if (ap < 2) return 0;
        if (write && ap != 3) return 0;
        return 1;
    }

    default:    /* fault */
        return 0;
    }
}

int mmu_user_access_ok(u32 start, u32 len, int write)
{
    u32 end;
    u32 page;
    const u32 *l1;

    if (len == 0) {
        return 1;
    }

    end = start + len - 1u;
    if (end < start) {                      /* 32 位回绕 */
        return 0;
    }

    /* 先做粗范围检查, 避免走表时访问到不存在的 L1 项 */
    if (start < USER_SPACE_BASE || end >= USER_SPACE_LIMIT) {
        return 0;
    }

    l1 = (const u32 *)(uintptr_t)(cp15_read_ttbr0() & 0xFFFFC000u);

    for (page = start & ~0xFFFu; ; page += 4096u) {
        if (!user_page_ok(l1, page, write)) {
            return 0;
        }
        if (page >= (end & ~0xFFFu)) {
            break;
        }
    }

    return 1;
}
