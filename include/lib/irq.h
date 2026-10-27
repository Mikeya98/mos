/**
 * @file    irq.h
 * @brief   保存/恢复 CPSR 的临界区原语
 *
 * CPSID i / CPSIE i 这种写法在嵌套时是错的: 内层开中断会把外层
 * 刚关掉的中断提前打开。这里改成保存 CPSR、恢复 CPSR, 内层恢复的
 * 还是"开"的状态, 外层恢复的才是它进来时的状态。
 */

#ifndef _MOS_IRQ_H
#define _MOS_IRQ_H

#include "lib/types.h"

static inline u32 irq_save(void)
{
    u32 cpsr;
    __asm__ volatile ("MRS %0, CPSR\n\tCPSID i" : "=r"(cpsr) :: "memory", "cc");
    return cpsr;
}

static inline void irq_restore(u32 cpsr)
{
    __asm__ volatile ("MSR CPSR_c, %0" :: "r"(cpsr) : "memory", "cc");
}

#endif /* _MOS_IRQ_H */
