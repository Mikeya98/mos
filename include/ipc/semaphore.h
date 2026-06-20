/**
 * @file    semaphore.h
 * @brief   MOS RTOS 计数信号量
 *
 * 信号量是最基本的任务间同步原语。
 *
 * 经典场景: producer-consumer
 *   Task A (consumer): sem_wait(&sem) → 等待数据
 *   Task B (producer): ...生成数据... → sem_post(&sem) → 唤醒 A
 *
 * 等待队列按优先级排序: sem_post 时唤醒优先级最高的等待者,
 * 保证 RTOS 的确定性。
 */

#ifndef _MOS_SEMAPHORE_H
#define _MOS_SEMAPHORE_H

#include "lib/types.h"

/* =========================== 信号量结构 =========================== */
typedef struct semaphore {
    int32_t     count;          /* 信号量计数
                                 *   >0: 可用资源数
                                 *    0: 正好用完
                                 *   <0: 有等待者, -N 表示 N 个任务在等 */
    void        *wait_queue;    /* 等待队列头 (task_t 链表, 按优先级排序) */
} sem_t;

/* =========================== 公共 API =========================== */

/**
 * @brief  初始化信号量
 * @param  sem        信号量指针
 * @param  init_count 初始计数
 *        >0: 初始就有资源可用
 *         0: 初始不可用 (用于同步两个任务)
 *
 * 示例:
 *   sem_t uart_tx_sem;
 *   sem_init(&uart_tx_sem, 1);   // 互斥信号量 (类似 mutex)
 *
 *   sem_t data_ready;
 *   sem_init(&data_ready, 0);    // 同步信号量
 */
void sem_init(sem_t *sem, int init_count);

/**
 * @brief  P 操作 (Wait / Down) — 获取信号量
 * @param  sem  信号量指针
 *
 * 如果 count > 0:
 *   count--, 立即返回 (获取成功)
 *
 * 如果 count <= 0:
 *   当前任务 BLOCKED, 加入等待队列 (按优先级排序),
 *   然后触发调度器切换到其他任务。
 *
 * 注意: 可以嵌套调用 (同一任务调用多次 wait 会造成死锁),
 * v0.1 不做重入检测。
 */
void sem_wait(sem_t *sem);

/**
 * @brief  V 操作 (Signal / Up) — 释放信号量
 * @param  sem  信号量指针
 *
 * count++
 *
 * 如果有等待者:
 *   唤醒等待队列中优先级最高的任务,
 *   如果被唤醒者优先级 > current → 标记需要调度
 *
 * 注意: 可以在 ISR 中调用 (中断中释放信号量唤醒任务)。
 */
void sem_post(sem_t *sem);

#endif /* _MOS_SEMAPHORE_H */
