/**
 * @file    msgq.h
 * @brief   MOS RTOS 消息队列 — 固定大小消息的环形缓冲区
 *
 * 消息队列是任务间传递数据的 IPC 机制。
 * 与信号量的区别: 信号量只传"计数", 消息队列传"数据"。
 *
 * 核心设计:
 *   - 固定大小消息 (msg_size): 确定性, 无碎片
 *   - 环形缓冲区: O(1) 读写
 *   - 拷贝语义: send 拷入, recv 拷出
 *   - 双等待队列: 发送者等空位, 接收者等数据
 *
 * 典型场景: producer-consumer
 *   Task A (producer): msgq_send(&q, &data, 1)  → 发送数据, 队满则阻塞
 *   Task B (consumer): msgq_recv(&q, &data, 1)  → 接收数据, 队空则阻塞
 */

#ifndef _MOS_MSGQ_H
#define _MOS_MSGQ_H

#include "lib/types.h"

/* =========================== 返回码 =========================== */
#define MSGQ_OK      0     /* 操作成功 */
#define MSGQ_FULL    (-1)  /* 队列已满 (非阻塞模式返回) */
#define MSGQ_EMPTY   (-2)  /* 队列为空 (非阻塞模式返回) */

/* =========================== 消息队列结构 =========================== */
typedef struct msgq {
    u8          *buf;           /* 环形缓冲区 (由调用者提供) */
    u32         msg_size;       /* 单条消息大小 (字节) */
    u32         capacity;       /* 最大消息数 */
    u32         count;          /* 当前消息数 (0 ~ capacity) */
    u32         head;           /* 读索引 (0 ~ capacity-1, 指向下一条要读的消息) */
    u32         tail;           /* 写索引 (0 ~ capacity-1, 指向下一个写入位置) */
    void        *send_queue;    /* 发送等待队列 (队满时阻塞的发送者, 按优先级排序) */
    void        *recv_queue;    /* 接收等待队列 (队空时阻塞的接收者, 按优先级排序) */
} msgq_t;

/* =========================== 公共 API =========================== */

/**
 * @brief  初始化消息队列
 * @param  q         消息队列指针
 * @param  buf       环形缓冲区 (由调用者分配, 大小 = msg_size * capacity)
 * @param  msg_size  单条消息大小 (字节)
 * @param  capacity  最大消息数
 *
 * 调用者负责分配缓冲区内存:
 *   static u8 msg_buf[4 * 16];  // 16 条消息, 每条 4 字节
 *   msgq_t q;
 *   msgq_init(&q, msg_buf, 4, 16);
 */
void msgq_init(msgq_t *q, void *buf, u32 msg_size, u32 capacity);

/**
 * @brief  发送消息 (可能阻塞)
 * @param  q        消息队列指针
 * @param  data     要发送的数据指针 (拷贝 msg_size 字节)
 * @param  blocking 1=阻塞等待, 0=非阻塞 (队满时立即返回 MSGQ_FULL)
 * @return MSGQ_OK 成功, MSGQ_FULL 队满且非阻塞
 *
 * 队列未满:
 *   拷贝 data → buf[tail], count++, 若有等待接收者则唤醒队头
 * 队列已满:
 *   blocking=0 → 立即返回 MSGQ_FULL
 *   blocking=1 → 当前任务加入 send_queue (按优先级排序), 阻塞
 */
int msgq_send(msgq_t *q, const void *data, int blocking);

/**
 * @brief  接收消息 (可能阻塞)
 * @param  q        消息队列指针
 * @param  data     接收缓冲区 (拷贝 msg_size 字节到此)
 * @param  blocking 1=阻塞等待, 0=非阻塞 (队空时立即返回 MSGQ_EMPTY)
 * @return MSGQ_OK 成功, MSGQ_EMPTY 队空且非阻塞
 *
 * 队列非空:
 *   拷贝 buf[head] → data, count--, 若有等待发送者则唤醒队头
 * 队列为空:
 *   blocking=0 → 立即返回 MSGQ_EMPTY
 *   blocking=1 → 当前任务加入 recv_queue (按优先级排序), 阻塞
 */
int msgq_recv(msgq_t *q, void *data, int blocking);

#endif /* _MOS_MSGQ_H */
