/**
 * @file    mutex.c
 * @brief   MOS RTOS 互斥量实现 — 递归锁 + 优先级继承
 *
 * 核心算法:
 *
 *   1. 递归锁: count 记录同一任务加锁次数, count==0 时才算释放。
 *
 *   2. 优先级继承 (Priority Inheritance):
 *       当高优先级任务 A 阻塞在互斥量 M 上, 而 M 被低优先级任务 C 持有时,
 *       C 的优先级被临时提升到 A 的级别, 让 C 尽快执行完释放 M。
 *
 *       这解决了经典的"优先级反转"问题:
 *         Task H (prio=1): 需要 mutex M
 *         Task M (prio=2): 不需要 mutex, 但在忙等
 *         Task L (prio=3): 持有 mutex M
 *         无 PI: M 抢走 CPU, H 被 L 阻塞, L 又被 M 抢 → H 无限等待
 *         有 PI: L 提升到 prio=1 → L 运行 → L 释放 M → H 获得 M → H 运行
 *
 *   3. 链式继承:
 *       如果 C 持有 M1 (被 A 等待), 同时 C 阻塞在 M2 (被 B 持有),
 *       则 B 也需要被提升到 A 的优先级。
 *       实现: boost_owner_chain 递归遍历 mutex->owner->block_obj 链。
 *
 *   4. 优先级恢复:
 *       解锁时, 如果没有其他等待者, 持有者恢复到 original_priority。
 *       如果 unlock 后立即将所有权转移给等待者, 新持有者不需要恢复
 *       (它本来就是自己的优先级)。
 */

#include "ipc/mutex.h"
#include "kernel/task.h"
#include "drivers/gic.h"

/* =========================== 临界区宏 =========================== */
#define ENTER_CRITICAL()  __asm__ volatile("CPSID i" ::: "memory", "cc")
#define EXIT_CRITICAL()   __asm__ volatile("CPSIE i" ::: "memory", "cc")

/* =========================== 内部辅助 =========================== */

/**
 * @brief  递归提升互斥量持有者链的优先级
 *
 * 从当前阻塞在 m 上的最高优先级等待者开始,
 * 如果其优先级高于 m 的持有者 → 提升持有者。
 * 如果持有者自身阻塞在另一个互斥量上 → 递归。
 *
 * @param  m  当前互斥量
 *
 * 调用时机: mutex_lock 阻塞后、mutex_unlock 转移所有权后
 *
 * 示例链:
 *   Task A (prio=2) 持有 mutex X
 *   Task B (prio=4) 持有 mutex Y
 *   Task A 阻塞在 mutex Y 上 (等待 B 释放)
 *   Task C (prio=1) 阻塞在 mutex X 上 (等待 A 释放)
 *
 *   boost_owner_chain(X): C(1) > A(2) → 提升 A 到 1
 *     → A 阻塞在 Y 上 → boost_owner_chain(Y): A(1) > B(4) → 提升 B 到 1
 *
 *   结果: A 和 B 都被提升到 priority=1, C 想要运行就必须等 A→B→C 链全部释放。
 */
static void boost_owner_chain(mutex_t *m)
{
    task_t *owner;
    task_t *waiter;
    u32     highest_waiter_prio;

    if (!m || !m->owner) return;

    owner = (task_t *)m->owner;

    /*
     * 找等待队列中优先级最高的 (队头, 因为按优先级排序)。
     * wait_queue 是单向链表, 队头就是最高优先级等待者。
     */
    if (!m->wait_queue) return;

    waiter = (task_t *)m->wait_queue;
    highest_waiter_prio = waiter->priority;

    /*
     * 如果最高优先级等待者比持有者优先级更高 → 提升持有者
     */
    if (highest_waiter_prio < owner->priority) {
        /* 保存原始优先级 (只在第一次提升时保存) */
        if (owner->priority == owner->original_priority) {
            /* 第一次被提升, original_priority 就是当前优先级 */
            /* (在 task_create 中已设置为初始优先级) */
        }

        task_change_priority(owner, highest_waiter_prio);

        /*
         * 链式继承: 如果持有者正阻塞在另一个互斥量上,
         * 递归提升那个互斥量的持有者。
         */
        if (owner->state == TASK_BLOCKED && owner->block_obj) {
            /*
             * block_obj 可能是 mutex 或 semaphore。
             * 只有 mutex 有所有权概念, 但要安全地判断类型。
             * v0.2 设计: block_obj 指向的对象如果是 mutex_t,
             * 其第一个字段也是 void* (owner/wait_queue)。
             *
             * 简化处理: 直接当 mutex_t 处理。
             * 如果不是 mutex (是 sem_t), boost_owner_chain
             * 会在 owner==NULL 检查时安全返回。
             */
            boost_owner_chain((mutex_t *)owner->block_obj);
        }
    }
}

/**
 * @brief  将持有者的优先级恢复到原始值
 *
 * 调用时机: mutex_unlock 中 count 降到 0 时
 *
 * 如果没有等待者: 恢复到 original_priority
 * 如果有等待者: 所有权转移给新持有者, 旧持有者恢复优先级
 *
 * @param  m  互斥量
 */
static void restore_owner_priority(mutex_t *m)
{
    task_t *owner = (task_t *)m->owner;

    if (!owner) return;

    /*
     * 如果当前优先级不等于原始优先级 → 曾被 PI 提升过 → 恢复
     */
    if (owner->priority != owner->original_priority) {
        task_change_priority(owner, owner->original_priority);
    }
}

/* =========================== 公共 API =========================== */

void mutex_init(mutex_t *m)
{
    if (!m) return;

    ENTER_CRITICAL();
    m->owner               = NULL;
    m->count               = 0;
    m->owner_original_prio = 0;
    m->wait_queue          = NULL;
    EXIT_CRITICAL();
}

void mutex_lock(mutex_t *m)
{
    task_t *cur;

    if (!m) return;

    ENTER_CRITICAL();

    cur = get_current();
    if (!cur) {
        EXIT_CRITICAL();
        return;
    }

    /*
     * 路径 1: 互斥量空闲 → 直接持有
     */
    if (m->owner == NULL) {
        m->owner               = cur;
        m->count               = 1;
        m->owner_original_prio = cur->priority;
        cur->held_mutex        = m;
        EXIT_CRITICAL();
        return;
    }

    /*
     * 路径 2: 已经持有 → 递归加锁
     */
    if (m->owner == (void *)cur) {
        m->count++;
        EXIT_CRITICAL();
        return;
    }

    /*
     * 路径 3: 被其他任务持有 → 阻塞 + 优先级继承
     */
    {
        task_t  **queue = (task_t **)&m->wait_queue;

        cur->state     = TASK_BLOCKED;
        cur->block_obj = m;

        /*
         * 关键: 先从就绪队列移除, 否则 pick_next_task() 可能
         * 再次选中本任务 (因为它仍挂在旧优先级的就绪链表里)。
         */
        ready_dequeue(cur);

        /* 按优先级插入等待队列 (低数值 = 高优先级排在前面) */
        while (*queue && (*queue)->priority <= cur->priority) {
            queue = &(*queue)->next;
        }
        cur->next = *queue;
        *queue    = cur;

        /*
         * 优先级继承: 尝试提升持有者链
         */
        boost_owner_chain(m);
    }

    EXIT_CRITICAL();

    /*
     * 放弃 CPU, 让持有者 (现在优先级提升了) 尽快执行。
     * 当 mutex_unlock 唤醒本任务后, schedule() 返回,
     * 此时 m->owner 已经是本任务, 互斥量已被持有。
     */
    schedule();

    /*
     * 被唤醒后 — 互斥量已被 sem_post 风格的逻辑转移给本任务。
     * 不需要再次设置 m->owner, unlock 中已经做了。
     */
}

void mutex_unlock(mutex_t *m)
{
    task_t *cur;

    if (!m) return;

    ENTER_CRITICAL();

    cur = get_current();

    /*
     * v0.2 不检测非持有者解锁 (未定义行为, 由用户保证正确性)
     */
    if (m->owner != (void *)cur) {
        EXIT_CRITICAL();
        return;
    }

    /* 递归解锁: count-- 后如果仍 >0, 继续持有 */
    m->count--;
    if (m->count > 0) {
        EXIT_CRITICAL();
        return;
    }

    /*
     * count == 0 → 完全释放所有权
     */
    cur->held_mutex = NULL;

    /* 恢复优先级 (如果曾因 PI 被提升) */
    restore_owner_priority(m);

    /*
     * 如果有等待者 → 唤醒优先级最高者 (队头), 转移所有权
     * 如果没有等待者 → 互斥量变空闲
     */
    if (m->wait_queue) {
        task_t *waiter = (task_t *)m->wait_queue;

        /* 从等待队列移出 */
        m->wait_queue = (void *)waiter->next;
        waiter->next      = NULL;
        waiter->block_obj = NULL;

        /* 转移所有权 */
        m->owner               = waiter;
        m->count               = 1;
        m->owner_original_prio = waiter->priority;
        waiter->held_mutex     = m;
        waiter->state          = TASK_READY;
        ready_enqueue(waiter);

        /*
         * 如果新持有者优先级 > 当前任务 → 立即调度
         */
        if (waiter->priority < cur->priority) {
            EXIT_CRITICAL();
            schedule();
            return;
        }
    } else {
        /* 没有等待者 → 互斥量空闲 */
        m->owner               = NULL;
        m->count               = 0;
        m->owner_original_prio = 0;
    }

    EXIT_CRITICAL();
}
