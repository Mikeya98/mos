/**
 * @file    vm.h
 * @brief   用户地址空间按需分页 (v0.6 阶段6)
 *
 * 私有区 (0x08100000 起的 1MB) 的 L2 表一开始全是 fault。用户第一次
 * 读/写某一页时, MMU 报 Data Abort, 内核在这里把页框填进页表, 然后
 * 让故障指令重新执行一遍。
 *
 * 哪些地址可以按需映射:
 *   L2[0]       0x08100000  数据页
 *   L2[1]       0x08101000  保护页 —— 永远拒绝, 用来发现栈溢出
 *   L2[2..255]  0x08102000  起  栈, 向下增长, 最多 1016KB
 *   其余地址    拒绝 (越界/代码区无写权限等)
 */

#ifndef _MOS_VM_H
#define _MOS_VM_H

#include "lib/types.h"

/* vm_fault / vm_touch 的返回值 */
#define VM_OK           0
#define VM_INVALID     (-1)     /* 不在可映射范围 */
#define VM_GUARD       (-2)     /* 踩到栈保护页 */
#define VM_NOMEM       (-3)     /* 页框池耗尽 */

/* 用户态通过 SYS_VM_STATS 拿到的统计 (v0.6 阶段6/7) */
typedef struct {
    u32 faults;         /* 本任务累计缺页次数 */
    u32 pages;          /* 本任务当前映射的私有页数 */
    u32 frames_free;    /* 全局空闲页框 */
    u32 frames_total;   /* 全局页框总数 */
    u32 swap_out;       /* 本任务累计换出页数 */
    u32 swap_in;        /* 本任务累计换入页数 */
    u32 swap_used;      /* 全局已占用的 swap 槽 */
    u32 swap_total;     /* 全局 swap 槽总数 */
} vm_stats_t;

/**
 * @brief  为一个缺页地址分配页框并填进当前任务的 L2 表
 *
 * @param  va     缺页的虚拟地址 (不必页对齐)
 * @param  write  1 = 写, 0 = 读 (只用于日志)
 * @return VM_OK / VM_INVALID / VM_GUARD / VM_NOMEM
 *
 * 调用者必须保证当前任务就是缺页的那个用户任务。
 */
int vm_fault(u32 va, u32 write);

/**
 * @brief  系统调用拷贝用户内存之前, 把范围内还没映射的合法页补上
 *
 * 内核替用户读写内存时不会经过 Data Abort (内核态访问踩 fault 会停机),
 * 所以 copy_*_user 必须先调这个函数。它只处理私有区里的页, 对内核
 * 地址/用户代码区不做任何事, 那些地址仍然由 mmu_user_access_ok 拒绝。
 *
 * @return VM_OK, 或者第一个失败页的错误码
 */
int vm_touch(u32 start, u32 len, int write);

/** 统计一个 L2 表里映射了多少页 (调试用) */
u32 vm_count_pages(const u32 *l2);

/** 填充当前任务的 vm_stats_t (内核态调用) */
void vm_fill_stats(vm_stats_t *st);

#endif /* _MOS_VM_H */
