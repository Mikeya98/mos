/**
 * @file    semaphore.c
 * @brief   MOS RTOS 计数信号量实现
 *
 * 等待队列: 按优先级排序的单向链表。
 *   sem_post 时取出队头 (最高优先级等待者) 即可。
 *
 * 临界区保护: 所有操作在 ENTER_CRITICAL/EXIT_CRITICAL 内进行。
 */

#include "ipc/semaphore.h"
#include "kernel/task.h"
#include "drivers/gic.h"

/* =========================== 临界区宏 =========================== */
#define ENTER_CRITICAL()  __asm__ volatile("CPSID i" ::: "memory", "cc")
#define EXIT_CRITICAL()   __asm__ volatile("CPSIE i" ::: "memory", "cc")

/* =========================== 公共 API =========================== */

void sem_init(sem_t *sem, int init_count)
{
    if (!sem) return;

    ENTER_CRITICAL();
    sem->count      = init_count;
    sem->wait_queue = NULL;
    EXIT_CRITICAL();
}

void sem_wait(sem_t *sem)
{
    if (!sem) return;

    ENTER_CRITICAL();

    sem->count--;

    if (sem->count < 0) {
        /*
         * 资源不足 → 当前任务阻塞
         *
         * 1. 将当前任务加入等待队列 (按优先级排序插入)
         * 2. 状态改为 BLOCKED
         * 3. 记录阻塞对象
         * 4. 触发调度 (当前任务不会再被选中, 因为不在就绪队列中)
         */
        task_t *cur = get_current();
        task_t **queue = (task_t **)&sem->wait_queue;

        cur->state     = TASK_BLOCKED;
        cur->block_obj = sem;

        /* 按优先级插入 (更小的优先级值 = 更高优先级, 排在前面) */
        while (*queue && (*queue)->priority <= cur->priority) {
            queue = &(*queue)->next;
        }
        cur->next = *queue;
        *queue = cur;

        EXIT_CRITICAL();
        schedule();     /* 切到其他任务 */
        return;         /* schedule() 返回时已经被其他任务唤醒 */
    }

    EXIT_CRITICAL();
    /* count >= 0: 获取成功, 立即返回 */
}

void sem_post(sem_t *sem)
{
    if (!sem) return;

    ENTER_CRITICAL();

    sem->count++;

    if (sem->count <= 0) {
        /*
         * 有任务在等待 → 唤醒优先级最高者 (队头)
         *
         * 1. 从等待队列移出第一个
         * 2. 状态改为 READY
         * 3. 放入就绪队列
         * 4. 如果被唤醒者优先级更高 → 标记 need_resched
         *
         * 注意: 在 ISR 中调用 sem_post 时,
         * schedule() 的调用由 irq_handler 末尾统一负责。
         */
        task_t *waiter = (task_t *)sem->wait_queue;
        sem->wait_queue = (void *)waiter->next;
        waiter->next = NULL;
        waiter->block_obj = NULL;

        waiter->state = TASK_READY;
        ready_enqueue(waiter);

        /*
         * 如果被唤醒的任务优先级比当前任务更高,
         * 标记需要调度。在 ISR 中调用时, need_resched
         * 会在中断返回前被 irq_handler 检查。
         */
        task_t *cur = get_current();
        if (cur && waiter->priority < cur->priority) {
            /* need_resched 定义在 task.c 中, 这里通过 schedule() 触发 */
            EXIT_CRITICAL();
            schedule();  /* 立即抢占 */
            return;
        }
    }

    EXIT_CRITICAL();
}
