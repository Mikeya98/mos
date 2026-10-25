/**
 * @file    user.h
 * @brief   用户态 demo 任务入口 (v0.6 阶段3)
 */

#ifndef _MOS_USER_H
#define _MOS_USER_H

/**
 * @brief  创建用户态 demo 任务
 *
 * 必须在 task_init() / syscall_init() 之后、sched_start() 之前调用。
 * 任务:
 *   u_waiter — 阻塞在信号量上, 被唤醒后退出
 *   u_worker — 正常调用 getpid/write/sem_post 后退出
 *   u_fault  — 直接读内核地址, 触发 Data Abort 被杀死
 *   u_badptr — 把内核指针 / 未映射地址传给 sys_write, 被指针检查拒绝
 *   u_iso_a  — 写私有数据页, 等 u_iso_b 跑完再读回, 验证地址空间隔离
 *   u_iso_b  — 写同一虚拟地址, 两边互不影响
 */
void user_demo_init(void);

#endif /* _MOS_USER_H */
