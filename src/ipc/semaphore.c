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
#include "kernel/syscall.h"     /* -EINTR */
#include "drivers/gic.h"

/* =========================== 临界区宏 =========================== */
#define ENTER_CRITICAL()  __asm__ volatile("CPSID i" ::: "memory", "cc")
#define EXIT_CRITICAL()   __asm__ volatile("CPSIE i" ::: "memory", "cc")

/**
 * @brief  信号打断阻塞时, 把自己从信号量等待队列摘下来
 *
 * 注册到 TCB 的 block_remove。调用者 (task_signal_send) 已经关中断。
 *
 * 两件事必须一起做:
 *   1. 从链表里摘掉, 否则 sem_post 之后还会对着一个已经醒来的任务
 *      发 ready_enqueue, 把就绪链表改坏;
 *   2. count 加回去 —— sem_wait 在阻塞前已经减过 1, 这次没拿到
 *      资源, 不退回去的话每来一次信号信号量就少一个名额。
 */
static void sem_cancel_wait(void *obj, task_t *t)
{
    sem_t *sem = (sem_t *)obj;
    task_t **q = (task_t **)&sem->wait_queue;

    while (*q && *q != t) {
        q = &(*q)->next;
    }
    if (*q == t) {
        *q = t->next;
    }
    t->next = NULL;
    sem->count++;
}

/* =========================== 公共 API =========================== */

void sem_init(sem_t *sem, int init_count)
{
    if (!sem) return;

    ENTER_CRITICAL();
    sem->count      = init_count;
    sem->wait_queue = NULL;
    EXIT_CRITICAL();
}

int sem_wait(sem_t *sem)
{
    task_t *cur;

    if (!sem) return 0;

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
        cur = get_current();
        task_t **queue = (task_t **)&sem->wait_queue;

        /*
         * v0.7 阶段3: 阻塞之前先看有没有可投递的信号。
         *
         * 信号可能在上一次返回用户态之后、进入这次系统调用之前就到了
         * (比如同一 tick 里另一个任务先 kill 了本任务)。这时候直接
         * 睡下去就没人再唤醒了 —— sem_post 不会来, 信号投递又只在
         * 返回用户态时发生。先退回去, 让返回路径把信号投递出去。
         *
         * 这里在临界区里, 单核上不会有信号插进来, 检查和阻塞是原子的。
         */
        if (cur->sig_pending & ~cur->sig_blocked & 0xFFFFFFFEu) {
            sem->count++;               /* 退回 sem_wait 预减的名额 */
            EXIT_CRITICAL();
            return -EINTR;
        }

        cur->state        = TASK_BLOCKED;
        cur->block_obj    = sem;
        cur->block_remove = sem_cancel_wait;   /* v0.7 阶段3 */

        /*
         * 关键: 先从就绪队列移除。
         * MOS 的 RUNNING 任务仍然挂在就绪链表上 (和 mutex/msgq 一样),
         * 直接改 cur->next 会把就绪环形链表改坏, pick_next_task()
         * 还可能再选中这个已经阻塞的任务。
         */
        ready_dequeue(cur);

        /* 按优先级插入 (更小的优先级值 = 更高优先级, 排在前面) */
        while (*queue && (*queue)->priority <= cur->priority) {
            queue = &(*queue)->next;
        }
        cur->next = *queue;
        *queue = cur;

        EXIT_CRITICAL();
        schedule();     /* 切到其他任务 */

        /*
         * schedule() 返回时有两条路:
         *   - sem_post 正常唤醒: block_obj 已经被清掉, 资源到手
         *   - 信号打断: task_signal_send 调 sem_cancel_wait 摘链并还了
         *     count, 这里按 POSIX 的约定返回 -EINTR
         */
        cur->block_remove = NULL;
        cur->block_obj    = NULL;

        if (cur->sig_interrupted) {
            cur->sig_interrupted = 0;
            return -EINTR;
        }
        return 0;
    }

    EXIT_CRITICAL();
    /* count >= 0: 获取成功, 立即返回 */
    return 0;
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
        waiter->block_obj    = NULL;
        waiter->block_remove = NULL;

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
