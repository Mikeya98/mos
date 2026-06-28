/**
 * @file    task.h
 * @brief   MOS RTOS 任务管理与调度器接口
 *
 * 调度策略: 优先级抢占式 + O(1) 位图查找
 *   32 级优先级 (0=最高, 31=IDLE)
 *   ready_bitmap: 32-bit 位图, bit31=prio0, bit0=prio31
 *   pick_next_task() = __builtin_clz(ready_bitmap) → 1 个 CPU 周期
 */

#ifndef _MOS_TASK_H
#define _MOS_TASK_H

#include "lib/types.h"

/* =========================== 常量 =========================== */
#define PRIO_COUNT      32         /* 优先级级数 */
#define PRIO_IDLE       (PRIO_COUNT - 1)  /* IDLE 优先级 = 31 */
#define TASK_NAME_MAX   32         /* 任务名最大长度 */
#define STACK_4K        4096       /* 4KB 栈 */
#define STACK_8K        8192       /* 8KB 栈 */

/* =========================== 任务状态 =========================== */
typedef enum {
    TASK_READY      = 0,   /* 就绪: 在就绪队列中等待 CPU */
    TASK_RUNNING    = 1,   /* 运行: 正在占用 CPU (即 current) */
    TASK_BLOCKED    = 2,   /* 阻塞: 等待信号量/互斥量 */
    TASK_SLEEPING   = 3,   /* 睡眠: 等待超时唤醒 */
    TASK_DEAD       = 4    /* 终止: 任务已退出 */
} task_state_t;

/* =========================== 任务控制块 =========================== */
typedef struct task_struct {
    /* --- 基本信息 --- */
    u32             pid;           /* 任务 ID (分配时递增) */
    char            name[TASK_NAME_MAX];  /* 任务名 (调试用) */
    task_state_t    state;         /* 当前状态 */
    u32             priority;      /* 优先级 (0=最高, 31=最低) */

    /* --- 时间 --- */
    u32             sleep_ticks;   /* 剩余睡眠 tick 数 (>0 表示在睡觉) */

    /* --- 上下文 --- */
    u32             sp;            /* 任务栈指针 (被换出时保存 sp 到这里) */
    u32             *stack_base;   /* 栈底 (用于溢出检测) */
    u32             stack_size;    /* 栈总大小 (字节) */

    /* --- 入口 --- */
    void            (*entry)(void);  /* 任务入口函数 */
    void            *arg;          /* 入口参数 (v0.1 简单, 不使用) */

    /* --- 链表 --- */
    struct task_struct *next;      /* 同优先级链表 / 等待队列的下一个 */
    struct task_struct *prev;      /* 双向链表 */

    /* --- 阻塞信息 --- */
    void            *block_obj;    /* 阻塞在哪个对象上 (信号量/互斥量) */

    /* --- 优先级继承 --- */
    u32             original_priority;  /* PI 提升前的原始优先级 */
    void            *held_mutex;   /* 当前持有的互斥量 (NULL = 没持有) */

} task_t;

/* =========================== 公共 API =========================== */

/* === 任务管理 === */

/**
 * @brief  初始化任务子系统
 *
 * 操作:
 *   - 初始化 ready_bitmap = 0
 *   - 初始化所有就绪队列 = NULL
 *   - 创建 IDLE 任务 (优先级 PRIO_IDLE)
 */
void task_init(void);

/**
 * @brief  创建一个新任务
 *
 * @param  entry       任务入口函数
 * @param  name        任务名 (调试用, 字符串)
 * @param  priority    优先级 (0=最高, 31=最低)
 * @param  stack_size  栈大小 (字节, 常用 STACK_4K)
 *
 * @return 新任务的 task_t 指针
 *
 * 操作:
 *   1. 从静态栈池分配栈
 *   2. 初始化栈帧 (伪造一个"被换出过"的上下文)
 *   3. 填充 TCB
 *   4. 加入就绪队列 (ready_enqueue)
 *   5. 如果新任务优先级 > 当前任务 → 标记 need_resched
 */
task_t *task_create(void (*entry)(void), const char *name,
                    u32 priority, u32 stack_size);

/* === 调度器 === */

/**
 * @brief  检查是否需要调度, 如果需要则执行 context_switch
 *
 * 在以下时机调用:
 *   - 中断返回前 (irq_handler 末尾)
 *   - task_create 后 (新任务可能更高优先级)
 *   - sem_post 后 (被唤醒者可能更高优先级)
 *   - task_yield 中 (主动放弃 CPU)
 *
 * 算法:
 *   next = pick_next_task()
 *   if next != current: context_switch(current, next)
 */
void schedule(void);

/**
 * @brief  启动调度器 (永不返回)
 *
 * 调用前提: 至少已创建 IDLE 任务 + 1 个用户任务
 *
 * 操作:
 *   - 选最高优先级任务
 *   - 切换到该任务的上下文
 *   - 从该任务的入口函数开始执行
 */
void sched_start(void);

/**
 * @brief  获取当前正在运行的任务
 */
task_t *get_current(void);

/* === 任务控制 === */

/**
 * @brief  让当前任务睡眠指定毫秒
 * @param  ms  睡眠时长 (毫秒)
 *
 * 操作:
 *   1. 设置 current->sleep_ticks = ms / TICK_MS
 *   2. 状态改为 TASK_SLEEPING
 *   3. 从就绪队列移除
 *   4. 调用 schedule()
 */
void task_sleep(u32 ms);

/**
 * @brief  主动放弃 CPU (让给同优先级下一个任务)
 */
void task_yield(void);

/* === 睡眠中断处理 (由 timer_isr 触发) === */

/**
 * @brief  处理一次系统 tick (由 timer_isr 调用)
 *
 * 操作:
 *   1. 遍历所有 TASK_SLEEPING 任务, 递减 sleep_ticks
 *   2. sleep_ticks 减到 0 的 → 状态改为 TASK_READY → 放回就绪队列
 */
void sys_tick_handler(void);

/* === 内部 API (sched_asm.S 需要) === */

/**
 * @brief  上下文切换: 保存 current 上下文, 恢复 next 上下文
 * @param  old  当前任务
 * @param  new  要切换到的任务
 *
 * 汇编实现 (见 src/kernel/sched_asm.S)
 * 保存: r4-r11, lr, sp → old 的内核栈
 * 恢复: new 的内核栈 → r4-r11, lr, sp
 */
void context_switch(task_t *old, task_t *new);

/**
 * @brief  将任务加入就绪队列
 */
void ready_enqueue(task_t *t);

/**
 * @brief  将任务从就绪队列移除
 */
void ready_dequeue(task_t *t);

/**
 * @brief  O(1) 选择最高优先级的就绪任务
 * @return 最高优先级的就绪任务, 无任务时返回 IDLE
 */
task_t *pick_next_task(void);

/**
 * @brief  动态修改任务优先级 (用于优先级继承)
 * @param  t            目标任务
 * @param  new_priority 新的优先级值
 *
 * 如果任务在就绪队列中 → 先移出, 改优先级, 再放回新优先级队列。
 * 如果任务是 RUNNING/BLOCKED → 只改字段。
 *
 * 注意: 此函数不检查 new_priority 是否有效, 调用者保证。
 *       不触发 schedule(), 由调用者决定是否需要重新调度。
 */
void task_change_priority(task_t *t, u32 new_priority);

/**
 * @brief  标记需要重新调度 (供 IPC/中断使用)
 */
void sched_request(void);

#endif /* _MOS_TASK_H */
