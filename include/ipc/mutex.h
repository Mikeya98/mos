/**
 * @file    mutex.h
 * @brief   MOS RTOS 互斥量 — 带优先级继承的递归锁
 *
 * 互斥量与信号量的核心区别:
 *   - 所有权: 只有锁持有者才能解锁 (信号量可以在 ISR 中 sem_post)
 *   - 递归: 同一任务可重复加锁 (count 计数)
 *   - 优先级继承: 防止优先级反转 (v0.2 特性)
 *
 * 使用约束:
 *   - 不可在 ISR 中调用 mutex_lock/mutex_unlock (中断没有"任务"身份)
 *   - 持有者必须显式解锁, 任务退出时不会自动释放
 */

#ifndef _MOS_MUTEX_H
#define _MOS_MUTEX_H

#include "lib/types.h"

/* =========================== 互斥量结构 =========================== */
typedef struct mutex {
    void        *owner;                 /* 当前持有者 (task_t *) */
    u32         count;                  /* 递归计数
                                         *   0: 未锁定
                                         *   1: 锁定一次 (非递归)
                                         *   N: 同一任务锁了 N 次 */
    u32         owner_original_prio;    /* 持有者的原始优先级 (用于 PI 恢复) */
    void        *wait_queue;            /* 等待队列头 (按优先级排序) */
} mutex_t;

/* =========================== 公共 API =========================== */

/**
 * @brief  初始化互斥量
 * @param  m  互斥量指针
 *
 * 初始化后状态: 未锁定, count=0, wait_queue=NULL
 *
 * 示例:
 *   mutex_t uart_lock;
 *   mutex_init(&uart_lock);
 */
void mutex_init(mutex_t *m);

/**
 * @brief  加锁 (可能阻塞)
 * @param  m  互斥量指针
 *
 * 三种路径:
 *   1. m->owner == NULL    → 没有持有者, 当前任务成为持有者 (count=1)
 *   2. m->owner == current → 递归加锁 (count++)
 *   3. m->owner != current → 有持有者, 当前任务阻塞, 触发优先级继承
 *
 * 优先级继承:
 *   如果阻塞任务的优先级 > 持有者优先级 → 提升持有者优先级
 *   如果持有者正阻塞在另一个互斥量上 → 递归提升 (链式继承)
 *
 * 注意: 不可在临界区内调用 (会释放临界区后再阻塞)
 */
void mutex_lock(mutex_t *m);

/**
 * @brief  解锁
 * @param  m  互斥量指针
 *
 * count--:
 *   如果 count > 0 → 仍被递归持有, 直接返回
 *   如果 count == 0 → 释放所有权:
 *     - 恢复原始终优先级 (如果曾被 PI 提升)
 *     - 如果有等待者: 唤醒优先级最高者, 转移所有权
 *
 * 注意: 只有持有者才能解锁, 非持有者解锁是未定义行为 (v0.2 不检测)
 */
void mutex_unlock(mutex_t *m);

#endif /* _MOS_MUTEX_H */
