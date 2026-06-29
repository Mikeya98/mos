/**
 * @file    msgq.c
 * @brief   MOS RTOS 消息队列实现
 *
 * 环形缓冲区 + 双等待队列 (发送者/接收者)。
 *
 * 等待队列: 按优先级排序的单向链表。
 *   队头 = 优先级最高的等待者 (优先级值最小)。
 *
 * 临界区保护: 所有操作在 ENTER_CRITICAL/EXIT_CRITICAL 内进行。
 */

#include "ipc/msgq.h"
#include "kernel/task.h"
#include "drivers/gic.h"
#include "lib/string.h"       /* memcpy */

/* =========================== 临界区宏 =========================== */
#define ENTER_CRITICAL()  __asm__ volatile("CPSID i" ::: "memory", "cc")
#define EXIT_CRITICAL()   __asm__ volatile("CPSIE i" ::: "memory", "cc")

/* =========================== 公共 API =========================== */

/**
 * @brief  初始化消息队列
 *
 * 所有字段清零/赋值。buf 由调用者分配, 生命周期由调用者管理。
 */
void msgq_init(msgq_t *q, void *buf, u32 msg_size, u32 capacity)
{
    if (!q || !buf || msg_size == 0 || capacity == 0) return;

    ENTER_CRITICAL();
    q->buf        = (u8 *)buf;
    q->msg_size   = msg_size;
    q->capacity   = capacity;
    q->count      = 0;
    q->head       = 0;
    q->tail       = 0;
    q->send_queue = NULL;
    q->recv_queue = NULL;
    EXIT_CRITICAL();
}

/**
 * @brief  按优先级插入等待队列 (内部辅助)
 *
 * 与 sem_wait / mutex_lock 中的插入逻辑相同:
 * 遍历链表, 找到第一个优先级比 cur 更低 (值更大) 的位置, 插在它前面。
 * 优先级值小的排在队头 (更早被唤醒)。
 */
static void insert_by_priority(task_t **queue, task_t *cur)
{
    while (*queue && (*queue)->priority <= cur->priority) {
        queue = &(*queue)->next;
    }
    cur->next = *queue;
    *queue    = cur;
}

/**
 * @brief  从等待队列中取出并唤醒队头任务 (内部辅助)
 *
 * 队头 = 优先级最高的等待者。
 * 将其从等待队列移出, 放回就绪队列。
 * 如果被唤醒者优先级高于当前任务, 立即让出 CPU。
 *
 * 注意: 会临时退出临界区来调用 schedule(),
 * 返回后重新进入临界区 (调用者继续持有临界区)。
 */
static void wake_one(task_t **queue)
{
    task_t *waiter = *queue;
    *queue = (task_t *)waiter->next;
    waiter->next      = NULL;
    waiter->block_obj = NULL;
    waiter->state     = TASK_READY;
    ready_enqueue(waiter);

    /*
     * 与 sem_post 一致: 如果被唤醒者优先级更高, 立即调度。
     * EXIT_CRITICAL → schedule() → ENTER_CRITICAL
     * schedule() 返回时当前任务已被换回, 继续持有临界区。
     */
    task_t *cur = get_current();
    if (cur && waiter->priority < cur->priority) {
        EXIT_CRITICAL();
        schedule();
        ENTER_CRITICAL();
    }
}

/**
 * @brief  发送消息
 *
 * 三条路径:
 *   1. count < capacity → 直接拷入环形缓冲区, 唤醒等待接收者 (如果有)
 *   2. count == capacity && !blocking → 返回 MSGQ_FULL
 *   3. count == capacity && blocking → 阻塞在 send_queue
 *
 * 路径 3 被唤醒后: data 指针仍在调用者栈上有效, 拷入缓冲区后返回。
 * 被唤醒可能是因为接收者取走了一条消息腾出了空位。
 */
int msgq_send(msgq_t *q, const void *data, int blocking)
{
    task_t *cur;
    u32 offset;

    if (!q || !data) return MSGQ_FULL;

    ENTER_CRITICAL();

    if (q->count < q->capacity) {
        /* 路径 1: 有空间, 直接拷入 */
        offset = q->tail * q->msg_size;
        memcpy(&q->buf[offset], data, q->msg_size);
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;

        /* 如果有接收者在等, 唤醒优先级最高的那个 */
        if (q->recv_queue) {
            wake_one((task_t **)&q->recv_queue);
        }

        EXIT_CRITICAL();
        return MSGQ_OK;
    }

    /* 队列已满 */
    if (!blocking) {
        /* 路径 2: 非阻塞 → 立即返回 */
        EXIT_CRITICAL();
        return MSGQ_FULL;
    }

    /* 路径 3: 阻塞等待 */
    cur = get_current();
    cur->state     = TASK_BLOCKED;
    cur->block_obj = q;
    ready_dequeue(cur);                     /* 移出就绪队列 */
    insert_by_priority((task_t **)&q->send_queue, cur);

    EXIT_CRITICAL();
    schedule();                             /* 切到其他任务 */

    /*
     * 被唤醒后回到这里 — 某个接收者取走了消息,
     * 现在有空位了。data 指针仍有效 (在调用者栈上)。
     */
    ENTER_CRITICAL();
    offset = q->tail * q->msg_size;
    memcpy(&q->buf[offset], data, q->msg_size);
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    /* 链式唤醒: 如果还有接收者在等, 继续唤醒 */
    if (q->recv_queue) {
        wake_one((task_t **)&q->recv_queue);
    }
    EXIT_CRITICAL();

    return MSGQ_OK;
}

/**
 * @brief  接收消息
 *
 * 三条路径:
 *   1. count > 0 → 直接从环形缓冲区拷出, 唤醒等待发送者 (如果有)
 *   2. count == 0 && !blocking → 返回 MSGQ_EMPTY
 *   3. count == 0 && blocking → 阻塞在 recv_queue
 *
 * 路径 3 被唤醒后: data 指针仍在调用者栈上有效, 从缓冲区拷出后返回。
 */
int msgq_recv(msgq_t *q, void *data, int blocking)
{
    task_t *cur;
    u32 offset;

    if (!q || !data) return MSGQ_EMPTY;

    ENTER_CRITICAL();

    if (q->count > 0) {
        /* 路径 1: 有数据, 直接拷出 */
        offset = q->head * q->msg_size;
        memcpy(data, &q->buf[offset], q->msg_size);
        q->head = (q->head + 1) % q->capacity;
        q->count--;

        /* 如果有发送者在等空位, 唤醒优先级最高的那个 */
        if (q->send_queue) {
            wake_one((task_t **)&q->send_queue);
        }

        EXIT_CRITICAL();
        return MSGQ_OK;
    }

    /* 队列为空 */
    if (!blocking) {
        /* 路径 2: 非阻塞 → 立即返回 */
        EXIT_CRITICAL();
        return MSGQ_EMPTY;
    }

    /* 路径 3: 阻塞等待 */
    cur = get_current();
    cur->state     = TASK_BLOCKED;
    cur->block_obj = q;
    ready_dequeue(cur);                     /* 移出就绪队列 */
    insert_by_priority((task_t **)&q->recv_queue, cur);

    EXIT_CRITICAL();
    schedule();                             /* 切到其他任务 */

    /*
     * 被唤醒后回到这里 — 某个发送者放入了消息,
     * 现在可以读了。data 指针仍有效 (在调用者栈上)。
     */
    ENTER_CRITICAL();
    offset = q->head * q->msg_size;
    memcpy(data, &q->buf[offset], q->msg_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    /* 链式唤醒: 如果还有发送者在等, 继续唤醒 */
    if (q->send_queue) {
        wake_one((task_t **)&q->send_queue);
    }
    EXIT_CRITICAL();

    return MSGQ_OK;
}
