/**
 * @file    vm.c
 * @brief   用户地址空间按需分页 (v0.6 阶段6)
 *
 * 一次缺页处理的完整流程:
 *
 *   用户态 STR r0, [r1]        ← 目标页在 L2 表里是 fault
 *     ↓ 硬件: Data Abort, FAR = 地址, FSR = translation fault
 *   data_handler (startup.S)  → 保存 lr_abt / spsr_abt, 进 SVC
 *     ↓
 *   data_abort_handler        → 判断是用户态 + translation fault
 *     ↓
 *   vm_fault(va, write)
 *     ├─ 地址分类: 数据页 / 栈页 / 保护页 / 越界
 *     ├─ frame_alloc() 取一帧, 清零
 *     ├─ 写 L2[idx] = phys | AP=11 | nG | XN
 *     ├─ DSB + 按地址无效化 TLB (只清这一项)
 *     └─ 返回 VM_OK
 *     ↓
 *   RFEFD 回到用户态, PC = 刚才那条 STR, 重新执行 —— 这次能访问了
 *
 * 关键点: 只刷一个 TLB 项。阶段5 刚把"切换地址空间不刷 TLB"做出来,
 * 缺页路径要是不加区分地全量刷, 前面省下的开销又白花了。
 */

#include "mm/vm.h"
#include "mm/mmu.h"
#include "mm/frame.h"
#include "mm/swap.h"
#include "kernel/task.h"
#include "lib/printf.h"

/* =========================== 协处理器 / 屏障 =========================== */

static inline void vm_dsb(void)
{
    __asm__ volatile ("DSB" ::: "memory");
}

static inline void vm_isb(void)
{
    __asm__ volatile ("ISB" ::: "memory");
}

/*
 * TLBIMVA: 按虚拟地址无效化一项。
 *
 * 阶段5 之后 TLB 项是按 ASID 打标签的, 这一条只影响当前 ASID,
 * 不会碰到别的任务在同一个虚拟地址上的翻译。
 * (c8, c6, 1) 是 ARMv7 的 "invalidate by MVA" 操作。
 */
static inline void vm_tlb_invalidate_mva(u32 va)
{
    __asm__ volatile ("MCR p15, 0, %0, c8, c6, 1" : : "r"(va) : "memory");
}

/** 页表项改动后的固定收尾: DSB → 按地址刷 TLB → DSB → ISB */
static void vm_flush_page(u32 va)
{
    vm_dsb();
    vm_tlb_invalidate_mva(va);
    vm_dsb();
    vm_isb();
}

/* =========================== 页面换出 =========================== */

/**
 * @brief  从当前任务的页表里挑一页换出
 *
 * 轮转 (clock) 策略: 从上次换出的下一页开始找第一个已映射的页,
 * 跳过正在缺页的那一页。找到后:
 *   1. 把物理页内容写进 swap 槽
 *   2. L2 表项改成换出描述符 (硬件仍然把它当 fault)
 *   3. 刷掉这一页的 TLB 项
 *   4. 归还页框
 *
 * @return 0 成功, -1 没有可换出的页 / swap 区已满
 */
static int vm_evict_one(task_t *t, u32 avoid_idx)
{
    u32 *l2 = (u32 *)(uintptr_t)t->l2_phys;
    u32 n;
    u32 idx;
    u32 phys;
    u32 va;
    int slot;

    for (n = 0; n < 256u; n++) {
        idx = (t->vm_clock + n) & 0xFFu;
        if (idx == avoid_idx) {
            continue;
        }
        if ((l2[idx] & 0x2u) == 0) {
            continue;               /* 没映射或是换出描述符 */
        }

        phys = l2[idx] & 0xFFFFF000u;
        slot = swap_store(phys);
        if (slot < 0) {
            return -1;              /* swap 区满了, 这一页换不出去 */
        }

        l2[idx] = swap_pte_make((u32)slot);
        va = USER_PRIV_VADDR + idx * 4096u;
        vm_flush_page(va);
        frame_free(phys, t->pid);

        t->vm_clock = (idx + 1u) & 0xFFu;
        t->swap_out++;
        printf("[SWAP] %s: %X -> slot %u (frame %X freed)\n",
               t->name, va, (u32)slot, phys);
        return 0;
    }

    return -1;
}

/* =========================== 地址分类 =========================== */

typedef enum {
    PAGE_DATA = 0,      /* 私有数据页 */
    PAGE_GUARD,         /* 栈保护页, 永远拒绝 */
    PAGE_STACK,         /* 栈页 */
    PAGE_ILLEGAL        /* 不属于本任务的私有区 */
} page_kind_t;

static page_kind_t classify(u32 page)
{
    u32 idx;

    if (page < USER_PRIV_VADDR || page >= USER_STACK_TOP) {
        return PAGE_ILLEGAL;
    }

    idx = (page - USER_PRIV_VADDR) >> 12;

    if (idx == 0) {
        return PAGE_DATA;
    }
    if (idx == 1) {
        return PAGE_GUARD;
    }
    return PAGE_STACK;          /* idx 2..255 */
}

/* =========================== 缺页处理 =========================== */

int vm_fault(u32 va, u32 write)
{
    task_t *t = get_current();
    u32 page;
    u32 *l2;
    u32 idx;
    u32 frame;
    volatile u32 *p;
    u32 i;
    page_kind_t kind;

    if (!t || !t->is_user || t->l2_phys == 0) {
        return VM_INVALID;
    }

    page = va & ~0xFFFu;
    kind = classify(page);

    if (kind == PAGE_ILLEGAL) {
        return VM_INVALID;
    }
    if (kind == PAGE_GUARD) {
        /*
         * 保护页。栈是从 0x08200000 往下长的, 长到 0x08102000 以下
         * 说明已经越界, 再往下就是数据页了 —— 如果让它继续映射,
         * 栈会把数据页悄悄覆盖掉, 这种 bug 极难查。
         */
        return VM_GUARD;
    }

    idx = (page - USER_PRIV_VADDR) >> 12;
    l2  = (u32 *)(uintptr_t)t->l2_phys;

    /* ------------------------------------------------------------------
     * 情况 1: 这一页之前被换出去了。
     * 先找一个页框 (没有就再换出别的页), 把 swap 内容读回来, 然后
     * 把 L2 表项恢复成正常的页描述符。
     * ------------------------------------------------------------------ */
    if (swap_pte_is(l2[idx])) {
        u32 slot = swap_pte_slot(l2[idx]);

        frame = frame_alloc(t->pid);
        if (frame == 0) {
            if (vm_evict_one(t, idx) != 0) {
                return VM_NOMEM;
            }
            frame = frame_alloc(t->pid);
            if (frame == 0) {
                return VM_NOMEM;
            }
        }

        if (swap_load(slot, frame) != 0) {
            frame_free(frame, t->pid);
            return VM_INVALID;
        }

        l2[idx] = (frame & 0xFFFFF000u) | L2_USER_RW_XN;
        vm_flush_page(page);

        t->pf_count++;
        t->swap_in++;
        printf("[SWAP] %s: slot %u -> %X (frame %X, swap-in #%u)\n",
               t->name, slot, page, frame, t->swap_in);
        return VM_OK;
    }

    /* 已经映射过 (可能是 TLB 里残留了旧的 fault 项), 直接返回 */
    if ((l2[idx] & 0x2u) != 0) {
        vm_flush_page(page);
        return VM_OK;
    }

    /* ------------------------------------------------------------------
     * 情况 2: 真正的第一次访问。
     * 页框池耗尽时先换出一页, 再重新试一次; swap 也满了才算失败。
     * ------------------------------------------------------------------ */
    frame = frame_alloc(t->pid);
    if (frame == 0) {
        if (vm_evict_one(t, idx) != 0) {
            return VM_NOMEM;
        }
        frame = frame_alloc(t->pid);
        if (frame == 0) {
            return VM_NOMEM;
        }
    }

    /*
     * 清零。新页必须清零, 否则任务能读到上一个使用者留下的内容 ——
     * 内核给用户的内存永远是干净的, 这是最基本的隔离要求之一。
     */
    p = (volatile u32 *)(uintptr_t)frame;
    for (i = 0; i < 1024u; i++) {
        p[i] = 0;
    }

    l2[idx] = (frame & 0xFFFFF000u) | L2_USER_RW_XN;

    /*
     * 顺序很重要:
     *   1. DSB  — 页表项必须真的写进内存, 再做 TLB 操作
     *   2. TLB  — 清掉这一页可能残留的 fault 项
     *   3. ISB  — 保证后面的取指/访存看到新的翻译
     */
    vm_flush_page(page);

    t->pf_count++;

    printf("[PF] %s: %X %s -> frame %X (fault #%u)\n",
           t->name, page, write ? "write" : "read", frame, t->pf_count);

    return VM_OK;
}

int vm_touch(u32 start, u32 len, int write)
{
    u32 end;
    u32 page;
    task_t *t = get_current();

    if (len == 0) {
        return VM_OK;
    }

    /* 内核态任务没有私有区 (访问不到 fault 页), 不需要处理 */
    if (!t || !t->is_user || t->l2_phys == 0) {
        return VM_OK;
    }

    end = start + len - 1u;
    if (end < start) {                      /* 32 位回绕 */
        return VM_INVALID;
    }

    for (page = start & ~0xFFFu; ; page += 4096u) {
        if (classify(page) != PAGE_ILLEGAL) {
            int rc = vm_fault(page, write);
            if (rc != VM_OK) {
                return rc;
            }
        }
        if (page >= (end & ~0xFFFu)) {
            break;
        }
    }

    return VM_OK;
}

/* =========================== 统计 =========================== */

u32 vm_count_pages(const u32 *l2)
{
    u32 i;
    u32 n = 0;

    if (!l2) {
        return 0;
    }
    for (i = 0; i < 256u; i++) {
        if ((l2[i] & 0x2u) != 0) {
            n++;
        }
    }
    return n;
}

void vm_fill_stats(vm_stats_t *st)
{
    task_t *t = get_current();

    if (!st) {
        return;
    }

    st->faults       = t ? t->pf_count : 0;
    st->pages        = (t && t->is_user)
                       ? vm_count_pages((const u32 *)(uintptr_t)t->l2_phys)
                       : 0;
    st->frames_free  = frame_free_count();
    st->frames_total = frame_total();
    st->swap_out     = t ? t->swap_out : 0;
    st->swap_in      = t ? t->swap_in : 0;
    st->swap_used    = swap_used();
    st->swap_total   = swap_total();
}
