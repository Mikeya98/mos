/**
 * @file    task.c
 * @brief   MOS RTOS 任务管理与优先级抢占式调度器实现
 *
 * 这是整个 RTOS 的核心文件。包含:
 *   - 任务创建 / 栈初始化
 *   - 就绪队列 + 位图管理
 *   - O(1) 优先级调度 (CLZ 指令)
 *   - 上下文切换调度逻辑
 *   - 系统 tick 处理 (睡眠唤醒)
 */

#include "kernel/task.h"
#include "lib/string.h"
#include "drivers/gic.h"        /* ENTER_CRITICAL / EXIT_CRITICAL */
#include "drivers/timer.h"

/* =========================== 外部引用 =========================== */
/*
 * context_switch 是汇编实现的 (sched_asm.S)
 * 这里声明为 extern, 链接时汇编文件提供符号
 *
 * 调用约定:
 *   void context_switch(task_t *old, task_t *new);
 *   r0 = old (当前任务), r1 = new (下一个任务)
 */

/* =========================== 临界区宏 =========================== */
#define ENTER_CRITICAL()  __asm__ volatile("CPSID i" ::: "memory", "cc")
#define EXIT_CRITICAL()   __asm__ volatile("CPSIE i" ::: "memory", "cc")

/* =========================== 调度器全局变量 =========================== */

/** 32-bit 位图: bit31=prio0, bit0=prio31, 1 表示该优先级有待运行任务 */
static u32 ready_bitmap = 0;

/** 每级优先级的就绪循环链表头 */
static task_t *ready_queue[PRIO_COUNT];

/** 当前正在运行的任务 (全局 — sched_asm.S 需要引用) */
task_t *current_task = NULL;

/** 任务 ID 分配器 (递增) */
static u32 next_pid = 0;

/** 是否需要重新调度 (中断返回时检查) */
static volatile u32 need_resched = 0;

/** IDLE 任务 TCB (静态分配, 因为是最先创建的) */
static task_t idle_tcb;

/** 全局 TCB 池 + 计数 (供 sys_tick_handler 遍历唤醒) */
static task_t *tcb_list[16 + 1];  /* 16 user + 1 idle */
static u32 tcb_count = 0;

/* =========================== 任务栈池 =========================== */
/* 由 kernel.lds 提供的外部符号 */
extern u8 _task_stack_pool;
extern u8 _task_stack_pool_end;

/** 栈分配指针 (简单递增分配, 不回收) */
static u8 *stack_alloc_ptr = &_task_stack_pool;
static u32 stack_pool_remaining = 0;  /* 在 task_init 中计算 */

/* =========================== IDLE 任务 =========================== */

/**
 * @brief  IDLE 任务 — 没有其他就绪任务时运行
 *
 * 在 WFI (Wait For Interrupt) 指令上等待,
 * 任何中断都会唤醒 CPU 并检查是否需要调度。
 */
static void idle_entry(void)
{
    /*
     * IDLE 任务 — 没有其他就绪任务时运行
     * WFI: 使 CPU 进入低功耗等待状态,
     * 任何 IRQ/FIQ 都会唤醒 → 中断处理 → schedule()
     */
    while (1) {
        __asm__ volatile("WFI");
    }
    /* 如果 WFI 意外返回 (无中断唤醒), 循环继续 */
}

/* =========================== 栈管理 =========================== */

/**
 * @brief  从静态栈池分配一块栈
 * @param  size  栈大小 (字节)
 * @return 栈顶指针 (栈向下增长, 返回的是分配区域的顶端)
 *
 * v0.1 使用最简单的递增分配, 不回收。
 * 任务数受栈池大小限制 (当前 64KB, 约 16 个 4KB 任务)。
 */
static u32 *stack_alloc(u32 size)
{
    u8 *stack = stack_alloc_ptr;

    if (stack + size > &_task_stack_pool_end) {
        /* 栈池耗尽 — 这是严重错误 */
        return NULL;
    }

    stack_alloc_ptr += size;
    stack_pool_remaining -= size;
    return (u32 *)(uintptr_t)(stack + size);  /* 返回栈顶 (栈向下增长) */
}

/* =========================== 任务栈帧初始化 =========================== */

/**
 * @brief  初始化任务栈 — 伪造被 STMDB 保存过的上下文
 *
 * context_switch (sched_asm.S) 使用 STMDB/LDMIA:
 *   STMDB sp!, {r4-r11, lr}    → push 9 个寄存器, sp -= 36
 *   LDMIA sp!, {r4-r11, lr}    → pop 9 个寄存器, sp += 36
 *
 * STMDB 存储顺序 (内存低→高):
 *   [sp+0 ] = r4
 *   [sp+4 ] = r5
 *   ...
 *   [sp+28] = r11
 *   [sp+32] = lr
 *
 * context_switch 流程:
 *   保存: STMDB sp!, {r4-r11, lr}  → sp -= 36, 然后 STR sp → old->sp
 *   恢复: LDR sp ← new->sp, 然后 LDMIA sp!, {r4-r11, lr}  → sp += 36
 *   返回: BX lr
 *
 * 首次启动时 lr = entry, r4-r11 = 0, sp = 栈顶 - 36。
 *
 * @param  sp_top  栈顶指针 (stack_alloc 返回的)
 * @return 初始化后的 sp 值 (指向 r4 槽), 应存入 task->sp
 */
static u32 stack_init(u32 *sp_top, void (*entry)(void))
{
    /*
     * 从栈顶向下预留 9 个字 (36 字节) 给 r4-r11+lr
     * sp 指向 r4 槽 (最低地址)
     */
    u32 *sp = sp_top - 9;  /* 9 words: r4-r11(8) + lr(1) */

    sp[0] = 0;              /* r4  */
    sp[1] = 0;              /* r5  */
    sp[2] = 0;              /* r6  */
    sp[3] = 0;              /* r7  */
    sp[4] = 0;              /* r8  */
    sp[5] = 0;              /* r9  */
    sp[6] = 0;              /* r10 */
    sp[7] = 0;              /* r11 */
    sp[8] = (u32)entry;     /* lr → entry function */

    return (u32)sp;
}

/* =========================== 就绪队列操作 =========================== */

void ready_enqueue(task_t *t)
{
    u32 p = t->priority;

    t->state = TASK_READY;

    if (ready_queue[p] == NULL) {
        /* 该优先级首次有任务 → 环形单节点链表 */
        ready_queue[p] = t;
        t->next = t;
        t->prev = t;
    } else {
        /* 插入队尾 (环形双向链表) */
        task_t *head = ready_queue[p];
        task_t *tail = head->prev;

        t->next = head;
        t->prev = tail;
        tail->next = t;
        head->prev = t;
    }

    /* 设置位图中对应位 */
    ready_bitmap |= (1u << (31 - p));
}

void ready_dequeue(task_t *t)
{
    u32 p = t->priority;

    if (t->next == t) {
        /* 该优先级只有一个任务 */
        ready_queue[p] = NULL;
    } else {
        /* 从环形链表中移除 */
        t->prev->next = t->next;
        t->next->prev = t->prev;

        if (ready_queue[p] == t) {
            ready_queue[p] = t->next;
        }
    }

    t->next = NULL;
    t->prev = NULL;

    /* 如果该优先级队列为空, 清除位图位 */
    if (ready_queue[p] == NULL) {
        ready_bitmap &= ~(1u << (31 - p));
    }
}

/* =========================== 调度器核心 =========================== */

task_t *pick_next_task(void)
{
    if (ready_bitmap == 0) {
        /* 没有就绪任务 (异常情况) → 返回 IDLE */
        return &idle_tcb;
    }

    /* __builtin_clz: 计算前导零个数
     * ready_bitmap 中 bit31=prio0, bit0=prio31
     *
     * 示例:
     *   ready_bitmap = 0x80000000  → CLZ = 0  → prio=0 有任务
     *   ready_bitmap = 0x00008000  → CLZ = 16 → prio=16 有任务
     *   ready_bitmap = 0x00000001  → CLZ = 31 → prio=31 有任务 (IDLE)
     */
    int highest = __builtin_clz(ready_bitmap);

    /* 返回该优先级链表的第一个 (环形链表的头) */
    return ready_queue[highest];
}

void schedule(void)
{
    task_t *next = pick_next_task();

    if (next != current_task) {
        task_t *old = current_task;

        current_task = next;
        next->state = TASK_RUNNING;

        context_switch(old, next);
    }
}

void sched_start(void)
{
    /*
     * 启动第一个任务:
     *   取最高优先级就绪任务, 直接 context_switch(NULL, first)。
     *   context_switch 检测到 old==NULL 时只恢复不保存。
     */
    task_t *first = pick_next_task();

    if (!first) {
        first = &idle_tcb;
    }

    current_task = first;
    first->state = TASK_RUNNING;

    context_switch(NULL, first);

    /* 不会到达这里 */
}

task_t *get_current(void)
{
    return current_task;
}

/* =========================== 任务控制 =========================== */

void task_yield(void)
{
    ENTER_CRITICAL();

    /* 把当前任务放回就绪队列队尾 */
    ready_enqueue(current_task);

    EXIT_CRITICAL();
    schedule();
}

void task_sleep(u32 ms)
{
    if (ms == 0) return;

    ENTER_CRITICAL();

    current_task->sleep_ticks = ms / TICK_MS;
    if (current_task->sleep_ticks == 0) {
        current_task->sleep_ticks = 1;  /* 至少睡 1 tick */
    }
    current_task->state = TASK_SLEEPING;

    /* 关键: 从就绪队列移除, 否则 pick_next_task 会再次选中自己 */
    ready_dequeue(current_task);

    EXIT_CRITICAL();
    schedule();
}

/* =========================== 系统 tick =========================== */

/*
 * 遍历所有 TCB, 递减睡眠任务的 sleep_ticks。
 * v0.1 任务数少 (< 16), O(n) 遍历即可。
 * 后续 v0.2 可优化为单独的睡眠链表。
 */
void sys_tick_handler(void)
{
    u32 i;

    for (i = 0; i < tcb_count; i++) {
        task_t *t = tcb_list[i];

        if (t && t->state == TASK_SLEEPING) {
            if (t->sleep_ticks > 0) {
                t->sleep_ticks--;
                if (t->sleep_ticks == 0) {
                    /* 睡够了 → 放回就绪队列 */
                    t->state = TASK_READY;
                    ready_enqueue(t);
                    need_resched = 1;
                }
            }
        }
    }
}

/* =========================== 任务子系统初始化 =========================== */

void task_init(void)
{
    int i;

    /* 初始化就绪队列 */
    ready_bitmap = 0;
    for (i = 0; i < PRIO_COUNT; i++) {
        ready_queue[i] = NULL;
    }

    /* 初始化栈池 */
    stack_pool_remaining = (u32)(&_task_stack_pool_end - &_task_stack_pool);
    stack_alloc_ptr = &_task_stack_pool;

    current_task = NULL;
    next_pid = 0;
    need_resched = 0;

    /* 创建 IDLE 任务 */
    idle_tcb.pid         = next_pid++;
    idle_tcb.priority    = PRIO_IDLE;
    idle_tcb.state       = TASK_READY;
    idle_tcb.sleep_ticks = 0;
    idle_tcb.sp          = 0;
    idle_tcb.stack_base  = NULL;
    idle_tcb.stack_size  = 0;
    idle_tcb.entry       = idle_entry;
    idle_tcb.arg         = NULL;
    idle_tcb.next        = NULL;
    idle_tcb.prev        = NULL;
    idle_tcb.block_obj   = NULL;

    /* IDLE 使用自己的静态栈 (512B, 8 字节对齐) */
    static u8 idle_stack[512] __attribute__((aligned(8)));
    idle_tcb.stack_base = (u32 *)(uintptr_t)idle_stack;
    idle_tcb.stack_size = sizeof(idle_stack);
    /* 用 stack_init 创建正确的上下文帧, 避免 LDMIA 加载到垃圾值 */
    {
        u32 *sp_top = (u32 *)(uintptr_t)(idle_stack + sizeof(idle_stack));
        idle_tcb.sp = stack_init(sp_top, idle_entry);
    }

    /* 注册 IDLE 到全局 TCB 列表 */
    tcb_list[0] = &idle_tcb;
    tcb_count = 1;

    /* IDLE 入就绪队列 */
    idle_tcb.state = TASK_READY;
    ready_enqueue(&idle_tcb);

    /*
     * IDLE 任务的名称
     * 使用 memcpy 避免 strcpy (这里简单处理)
     */
    const char *idle_name = "idle";
    for (i = 0; i < TASK_NAME_MAX - 1 && idle_name[i]; i++) {
        idle_tcb.name[i] = idle_name[i];
    }
    idle_tcb.name[i] = '\0';
}

task_t *task_create(void (*entry)(void), const char *name,
                    u32 priority, u32 stack_size)
{
    task_t *t;
    int i;

    if (!entry) return NULL;
    if (priority >= PRIO_COUNT) return NULL;

    /* 1. 分配栈 */
    u32 *stack = stack_alloc(stack_size);
    if (!stack) return NULL;

    /* 栈向下增长, stack 指向分配区域的顶, 8 字节对齐 */
    u32 *sp_top = (u32 *)(((u32)(uintptr_t)stack) & ~0x7u);

    /* 2. 初始化栈帧 */
    u32 init_sp = stack_init(sp_top, entry);

    /* 3. 分配 TCB (v0.1: 固定 16 个静态 TCB 池) */
    static task_t tcb_pool[16];

    if (tcb_count >= 17) return NULL;  /* 16 user + 1 idle = 17 max */
    t = &tcb_pool[tcb_count - 1];      /* -1 因为 slot 0 是 IDLE */
    tcb_list[tcb_count] = t;
    tcb_count++;

    /* 4. 填充 TCB */
    t->pid         = next_pid++;
    t->priority    = priority;
    t->state       = TASK_READY;
    t->sleep_ticks = 0;
    t->sp          = init_sp;
    t->stack_base  = (u32 *)stack;
    t->stack_size  = stack_size;
    t->entry       = entry;
    t->arg         = NULL;
    t->next        = NULL;
    t->prev        = NULL;
    t->block_obj   = NULL;

    /* 填充名称 */
    for (i = 0; i < TASK_NAME_MAX - 1 && name && name[i]; i++) {
        t->name[i] = name[i];
    }
    t->name[i] = '\0';

    /* 5. 加入就绪队列 */
    ENTER_CRITICAL();
    ready_enqueue(t);

    /* 6. 如果新任务优先级更高 → 标记需要调度 */
    if (current_task && priority < current_task->priority) {
        need_resched = 1;
    }
    EXIT_CRITICAL();

    return t;
}
