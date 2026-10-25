/**
 * @file    addr_space.c
 * @brief   用户地址空间的物理资源分配 (v0.6 阶段4)
 *
 * 三个静态池 (kernel.lds):
 *   _l1_pool     8 × 16KB   L1 页表
 *   _l2_pool     8 × 1KB    L2 页表
 *   _user_phys   8 × 68KB   每任务 4KB 数据页 + 64KB 栈
 *
 * 顺序切分, 不回收。任务被杀死后地址空间也不会释放 ——
 * 演示版本先这样, 真实内核需要引用计数和延迟释放。
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

/* =========================== 池游标 =========================== */
static u8 *l1_cursor   = &_l1_pool_start;
static u8 *l2_cursor   = &_l2_pool_start;
static u8 *phys_cursor = &_user_phys_pool_start;

static u32 spaces_created = 0;

/* =========================== 分配 =========================== */

int user_space_create(user_space_t *us)
{
    u8 *l1;
    u8 *l2;
    u8 *data;
    u8 *stack;

    if (!us) return -1;

    /* 1. 从池里切出四块内存 */
    if (l1_cursor + 0x4000 > &_l1_pool_end) {
        printf("[AS] L1 pool exhausted\n");
        return -1;
    }
    if (l2_cursor + 0x400 > &_l2_pool_end) {
        printf("[AS] L2 pool exhausted\n");
        return -1;
    }
    if (phys_cursor + 4096u + USER_STACK_SIZE > &_user_phys_pool_end) {
        printf("[AS] user phys pool exhausted\n");
        return -1;
    }

    l1    = l1_cursor;
    l2    = l2_cursor;
    data  = phys_cursor;
    stack = phys_cursor + 4096u;

    l1_cursor   += 0x4000;
    l2_cursor   += 0x400;
    phys_cursor += 4096u + USER_STACK_SIZE;

    /* 2. 填页表 */
    us->l1_phys    = (u32)(uintptr_t)l1;
    us->l2_phys    = (u32)(uintptr_t)l2;
    us->data_phys  = (u32)(uintptr_t)data;
    us->stack_phys = (u32)(uintptr_t)stack;

    mmu_build_user_l1((u32 *)(uintptr_t)l1, us->l2_phys);
    mmu_build_user_l2((u32 *)(uintptr_t)l2, us->data_phys, us->stack_phys);

    /* 3. 数据页清零: 不能把上一个任务的残留内容带出去 */
    {
        volatile u32 *p = (volatile u32 *)(uintptr_t)us->data_phys;
        u32 i;
        for (i = 0; i < 1024; i++) {
            p[i] = 0;
        }
    }

    spaces_created++;
    return 0;
}

void user_space_dump(const user_space_t *us, const char *name)
{
    const u32 *l1 = (const u32 *)(uintptr_t)us->l1_phys;
    const u32 *l2 = (const u32 *)(uintptr_t)us->l2_phys;

    printf("[AS] %s\n", name);
    printf("     L1 @ %X  L2 @ %X\n", us->l1_phys, us->l2_phys);
    printf("     L1[%u] (0x08100000) -> %X (coarse table)\n",
           (u32)(USER_PRIV_VADDR >> 20), l1[USER_PRIV_VADDR >> 20]);
    printf("     L2[0] (data ) -> %X   phys %X\n",
           l2[0], us->data_phys);
    printf("     L2[1] (stack) -> %X   phys %X\n",
           l2[1], us->stack_phys);
    printf("     total spaces: %u\n", spaces_created);
}
