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
#include "fs/vfs.h"

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
    TASK_DEAD       = 4,   /* 终止: 任务已退出 */
    TASK_ZOMBIE     = 5,   /* 僵尸: 已退出, 等父进程 wait 回收 (v0.6 阶段10) */
    TASK_EXITING    = 6    /* 正在退出: 已经离开就绪队列, 但还站在自己的内核栈上 */
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

    /* --- 文件系统 (v0.5) --- */
    file_desc_t     fd_table[MAX_FD_PER_TASK];  /* 每个任务的文件描述符表 */

    /* --- 用户态 (v0.6 阶段2) --- */
    u32             user_sp;       /* 用户态栈指针 (SP_usr, 首次进入时用) */
    u8              is_user;       /* 1=用户态任务 (USR), 0=内核态任务 (SVC) */

    /* --- 地址空间 (v0.6 阶段4) --- */
    /*
     * l1_phys 是上下文切换时要写进 TTBR0 的页表物理地址。
     * 内核态任务统一用内核主页表; 用户态任务各有一张独立页表。
     * l2 只在用户态任务上有意义, 内核任务为 0。
     *
     * 阶段6 之后数据页/栈页不再预分配, 具体映射见 l2 表。
     * pf_count 是本任务累计缺页次数 (调试/统计用)。
     */
    u32             l1_phys;
    u32             l2_phys;
    u32             pf_count;

    /* --- 页面换出 (v0.6 阶段7) ---
     * vm_clock 是轮转置换的起点, swap_out/swap_in 用于统计。 */
    u32             vm_clock;
    u32             swap_out;
    u32             swap_in;

    /* --- fork / 写时复制 (v0.6 阶段8) ---
     * cow_count 是写时复制了多少页; forked_from 记录父进程 pid,
     * 0 表示不是 fork 出来的。 */
    u32             cow_count;
    u32             forked_from;

    /* --- 地址空间编号 (v0.6 阶段5) ---
     * asid 写进 TTBR0 低 8 位, 私有页按它打标签。内核任务用 0。
     * as_slot 是静态池槽位号, 任务退出时按它归还资源。
     * as_valid = 1 表示这几块物理资源还没还回去。 */
    u32             asid;
    u32             as_slot;
    u8              as_valid;

    /* --- 进程退出 / wait (v0.6 阶段10) ---
     * exit_code 在任务变成僵尸后由父进程取走。
     * wait_pid / wait_active 描述父进程此刻在等谁:
     *   wait_active=1 表示父进程正阻塞在 task_wait 上, 0 表示没有。
     *   光看 wait_pid 不够 —— 阻塞在信号量上的任务 wait_pid 也是 0,
     *   子进程退出时误唤醒会让父进程在 sem_wait 里凭空醒一次。 */
    s32             exit_code;
    u32             wait_pid;      /* 0 = 等任意子进程 */
    u8              wait_active;

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

/**
 * @brief  创建一个用户态任务 (v0.6 阶段2)
 *
 * @param  entry        任务入口函数 (将在 USR 用户态执行)
 * @param  name         任务名
 * @param  priority     优先级 (0=最高)
 * @param  kstack_size  内核栈大小 (SVC 模式, 保存上下文/处理系统调用)
 * @param  ustack_size  用户栈大小 (USR 模式, 任务自身代码使用)
 *
 * @return 新任务的 task_t 指针, 失败返回 NULL
 *
 * 与 task_create 的区别:
 *   - 分配一套独立的用户地址空间: L1/L2 页表 + 4KB 数据页 + 64KB 栈,
 *     虚拟地址固定为 0x08000000 (代码) / 0x08100000 (数据) / 0x08101000 (栈)
 *   - 首次被调度到时, 内核先换 TTBR0 到本任务的页表, 再通过
 *     trampoline 切换到 USR 模式进入入口函数
 *   - 任务在用户态只能通过 SVC 系统调用 (见 kernel/syscall.h) 请求内核服务
 */
task_t *task_create_user(void (*entry)(void), const char *name,
                         u32 priority, u32 kstack_size, u32 ustack_size);

/**
 * @brief  复制当前用户任务 (fork, v0.6 阶段8)
 *
 * 子进程获得:
 *   - 一份新的地址空间, 已映射的页与父进程共享 (只读, 写时复制)
 *   - 一套新的 TCB / 内核栈
 *   - 与父进程相同的用户 PC 和用户栈
 *
 * 子进程第一次被调度时从 fork_return_trampoline 返回用户态,
 * fork() 的返回值为 0; 父进程的返回值是子进程 pid。
 *
 * @param  parent   父任务 (必须是用户态任务)
 * @param  user_pc  父进程 SVC 之后要返回的用户 PC (即 frame[13])
 * @return 子任务, 失败返回 NULL
 */
task_t *task_fork(task_t *parent, u32 user_pc);

/** 把当前任务的 SP_usr 保存进 TCB (进入内核时调用) */
void task_save_user_sp(void);

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

/**
 * @brief  结束当前任务 (SYS_EXIT 的实现)
 *
 * 有父进程: 变成 TASK_ZOMBIE, 保留 TCB 和内核栈等父进程 wait 回收;
 * 地址空间当场释放 (页表/页框/swap 槽已经不用了)。
 * 没有父进程: 走延迟回收队列, 由调度器下次切换时释放。
 *
 * 函数永不返回 — 当前任务的内核栈连同这次调用的现场一起被丢弃。
 *
 * @param  code  退出码, 父进程 wait 时取走
 */
void task_exit(s32 code) __attribute__((noreturn));

/**
 * @brief  等待子进程退出并取走退出码 (SYS_WAIT 的实现)
 *
 * @param  pid   要等的子进程 pid, 0 = 任意子进程
 * @param  code  输出参数, 子进程的退出码
 * @return 被回收的子进程 pid; 没有子进程返回 -ECHILD
 *
 * 有僵尸子进程就直接回收; 没有就阻塞, 子进程退出时唤醒。
 */
s32 task_wait(s32 pid, s32 *code);

/**
 * @brief  杀死指定任务 (异常处理用)
 *
 * 只做状态清理: 从就绪队列 / TCB 列表移除, 状态置 DEAD。
 * 不触发调度 — 由调用者在合适的时机调用 schedule()。
 *
 * @param  t  目标任务 (NULL / IDLE 时直接返回)
 */
void task_kill(task_t *t);

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
