/**
 * @file    addr_space.c
 * @brief   用户地址空间的物理资源分配 (v0.6 阶段4/5)
 *
 * 三个静态池 (kernel.lds):
 *   _l1_pool     8 × 16KB   L1 页表
 *   _l2_pool     8 × 1KB    L2 页表
 *   _user_phys   8 × 68KB   每任务 4KB 数据页 + 64KB 栈
 *
 * 三份池子用同一个槽位号索引, 分配时找一个空闲槽位, 释放时把
 * 槽位还回去。槽位号同时用来生成 ASID (slot + 1), ASID 0 留给内核。
 */

#include "mm/addr_space.h"
#include "mm/mmu.h"
#include "lib/printf.h"

/* =========================== 链接脚本符号 =========================== */
extern u8 _l1_pool_start;
extern u8 _l1_pool_end;
extern u8 _l2_pool_start;
extern u8 _l2_pool_end;
extern u8 _user_phys_pool_start;
extern u8 _user_phys_pool_end;

/* =========================== 槽位位图 =========================== */
/* bit i = 1 表示槽位 i 已被占用 */
static u32 slot_used = 0;

/*
 * 位图更新必须是原子的。
 *
 * 分配发生在任务上下文 (中断开着), 释放发生在 schedule() 的回收
 * 路径上 (同样开着中断)。如果一边在读改写 slot_used, 中间被 tick
 * 打断, 中断里又释放了另一个槽位, 回去写回旧值就会把刚清掉的位
 * 又"复活" —— 这个槽位从此没人认领, 永远回收不回来。
 *
 * 用保存/恢复 CPSR 的方式关中断, 这样嵌套调用也不会把中断提前打开。
 */
static inline u32 irq_lock(void)
{
    u32 cpsr;
    __asm__ volatile ("MRS %0, CPSR\n\tCPSID i" : "=r"(cpsr) :: "memory", "cc");
    return cpsr;
}

static inline void irq_unlock(u32 cpsr)
{
    __asm__ volatile ("MSR CPSR_c, %0" :: "r"(cpsr) : "memory", "cc");
}

static u8 *l1_of(u32 slot)
{
    return &_l1_pool_start + slot * 0x4000u;
}

static u8 *l2_of(u32 slot)
{
    return &_l2_pool_start + slot * 0x400u;
}

static u8 *phys_of(u32 slot)
{
    return &_user_phys_pool_start + slot * (4096u + USER_STACK_SIZE);
}

static u32 free_count(void)
{
    u32 i;
    u32 n = 0;

    for (i = 0; i < USER_SPACE_SLOTS; i++) {
        if ((slot_used & (1u << i)) == 0) {
            n++;
        }
    }
    return n;
}

/* =========================== 分配 =========================== */

int user_space_create(user_space_t *us)
{
    u32 slot;
    u32 cpsr;
    u8 *l1;
    u8 *l2;
    u8 *data;
    u8 *stack;

    if (!us) return -1;

    /* 1. 找第一个空闲槽位, 并且原子地占住它 */
    cpsr = irq_lock();
    for (slot = 0; slot < USER_SPACE_SLOTS; slot++) {
        if ((slot_used & (1u << slot)) == 0) {
            break;
        }
    }
    if (slot == USER_SPACE_SLOTS) {
        irq_unlock(cpsr);
        printf("[AS] no free address space slot\n");
        return -1;
    }
    slot_used |= (1u << slot);
    irq_unlock(cpsr);

    /* 顺手确认池子的大小和槽位数对得上, 链接脚本改错了能早点发现 */
    if (l1_of(slot) + 0x4000u > &_l1_pool_end ||
        l2_of(slot) + 0x400u  > &_l2_pool_end ||
        phys_of(slot) + 4096u + USER_STACK_SIZE > &_user_phys_pool_end) {
        cpsr = irq_lock();          /* 占住的位要还回去 */
        slot_used &= ~(1u << slot);
        irq_unlock(cpsr);
        printf("[AS] slot %u out of pool range\n", slot);
        return -1;
    }

    l1    = l1_of(slot);
    l2    = l2_of(slot);
    data  = phys_of(slot);
    stack = data + 4096u;

    /* 2. ASID = 槽位号 + 1 */
    us->slot       = slot;
    us->asid       = slot + 1u;
    us->l1_phys    = (u32)(uintptr_t)l1;
    us->l2_phys    = (u32)(uintptr_t)l2;
    us->data_phys  = (u32)(uintptr_t)data;
    us->stack_phys = (u32)(uintptr_t)stack;

    /*
     * 3. 清掉这个 ASID 可能残留的 TLB 项。
     *
     * 正常路径上 user_space_destroy 已经刷过一次, 这里再刷一次是
     * 防御性的: 只要页表内容重建了, 就不能让任何旧翻译活着。
     * 新槽位第一次使用时这条指令是空操作。
     */
    mmu_flush_asid(us->asid);

    /* 4. 填页表 */
    mmu_build_user_l1((u32 *)(uintptr_t)l1, us->l2_phys);
    mmu_build_user_l2((u32 *)(uintptr_t)l2, us->data_phys, us->stack_phys);

    /* 5. 数据页清零: 不能把上一个任务的残留内容带出去 */
    {
        volatile u32 *p = (volatile u32 *)(uintptr_t)us->data_phys;
        u32 i;
        for (i = 0; i < 1024; i++) {
            p[i] = 0;
        }
    }

    return 0;
}

/* =========================== 释放 =========================== */

void user_space_destroy(user_space_t *us)
{
    u32 cpsr;
    u32 slot;
    u32 asid;

    if (!us || us->slot >= USER_SPACE_SLOTS) {
        return;
    }

    /* 检查 + 清位必须在一个原子区间里, 否则和 create 的 |= 打架 */
    cpsr = irq_lock();
    slot = us->slot;
    asid = us->asid;
    if ((slot_used & (1u << slot)) == 0) {
        irq_unlock(cpsr);
        return;                     /* 已经还过了 */
    }
    slot_used &= ~(1u << slot);
    irq_unlock(cpsr);

    /*
     * 先还槽位再刷 TLB。ASID 马上会被下一个任务复用, 如果 TLB 里
     * 还留着上一个任务在 0x08100000 的翻译, 新任务一上来就会读到
     * 别人的栈。
     */
    mmu_flush_asid(asid);

    us->slot = 0xFFFFFFFFu;         /* 防止重复释放 */
}

u32 user_space_free_count(void)
{
    return free_count();
}

void user_space_dump(const user_space_t *us, const char *name)
{
    const u32 *l1 = (const u32 *)(uintptr_t)us->l1_phys;
    const u32 *l2 = (const u32 *)(uintptr_t)us->l2_phys;

    printf("[AS] %s (slot %u, asid %u)\n", name, us->slot, us->asid);
    printf("     L1 @ %X  L2 @ %X\n", us->l1_phys, us->l2_phys);
    printf("     L1[%u] (0x08100000) -> %X (coarse table)\n",
           (u32)(USER_PRIV_VADDR >> 20), l1[USER_PRIV_VADDR >> 20]);
    printf("     L2[0] (data ) -> %X   phys %X\n",
           l2[0], us->data_phys);
    printf("     L2[1] (stack) -> %X   phys %X\n",
           l2[1], us->stack_phys);
    printf("     free slots: %u / %u\n", free_count(), USER_SPACE_SLOTS);
}
