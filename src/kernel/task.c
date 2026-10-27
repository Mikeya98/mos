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
#include "lib/printf.h"
#include "drivers/gic.h"        /* ENTER_CRITICAL / EXIT_CRITICAL */
#include "drivers/timer.h"
#include "drivers/uart.h"
#include "mm/slab.h"
#include "mm/mmu.h"
#include "mm/addr_space.h"

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

/**
 * 调度锁: >0 时 schedule() 直接返回。
 *
 * task_create_user 需要"分配地址空间 → 建 TCB → 填页表 → 伪造用户栈帧"
 * 这一串步骤全部完成后, 新任务才能被调度。但 task_create 内部会把任务
 * 挂进就绪队列并开中断, 这中间如果来一个 tick, 调度器就可能选中一个
 * 还没初始化完的任务。用调度锁把这段窗口盖住。
 */
static volatile u32 sched_lock = 0;

/** 调度流程重入保护: schedule() 执行期间不允许被中断里的 schedule() 打断 */
static volatile u32 sched_active = 0;

/** IDLE 任务 TCB (静态分配, 因为是最先创建的) */
static task_t idle_tcb;

/** TCB SLAB cache (task_init 中创建) */
static slab_cache_t *task_cache = NULL;

/** 全局 TCB 指针列表 + 计数 (供 sys_tick_handler 遍历唤醒) */
static task_t *tcb_list[32];  /* 动态增长上限 */
static u32 tcb_count = 0;

/* 延迟回收 (实现在本文件后半部分, schedule 里要提前调用) */
static void reclaim_drain(void);

/* =========================== 任务栈池 =========================== */
/* 由 kernel.lds 提供的外部符号 */
extern u8 _task_stack_pool;
extern u8 _task_stack_pool_end;

/** 栈分配指针 (没有合适空闲块时从这里切) */
static u8 *stack_alloc_ptr = &_task_stack_pool;
static u32 stack_pool_remaining = 0;  /* 在 task_init 中计算 */

/* =========================== 内核栈空闲表 (v0.6 阶段5) =========================== */
/*
 * 任务退出后把栈还回来, 下次 task_create 优先复用。first fit,
 * 不做相邻块合并 —— 任务栈大小是 4KB 的整数倍, 反复申请/释放同样
 * 大小的块时 first fit 就够了, 碎片只在混用 4K/8K 时才会出现。
 */
#define STACK_FREE_MAX  16

typedef struct {
    u8 *base;
    u32 size;
} stack_block_t;

static stack_block_t stack_free_list[STACK_FREE_MAX];
static u32 stack_free_n = 0;

/* =========================== 延迟回收队列 (v0.6 阶段5) =========================== */
/*
 * 当前任务是在自己的内核栈上调用 task_kill / task_exit 的, TCB、
 * 内核栈、地址空间此刻都还在用。所以先挂进这个队列, 等调度挑中了
 * 别的任务 (running == 不是它) 再释放。
 */
#define RECLAIM_MAX     16

static task_t *reclaim_list[RECLAIM_MAX];
static u32 reclaim_n = 0;


/* =========================== 用户地址空间 (v0.6 阶段4) =========================== */
/*
 * 用户栈不再是内核地址空间里的一段共享内存, 而是每个任务私有的一张
 * 物理页, 映射到固定的虚拟地址 USER_STACK_VADDR。分配逻辑在
 * mm/addr_space.c, 这里只负责把结果填进 TCB。
 *
 * 内核态任务的栈仍然从上面的 .task_stacks 池分配。
 */

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
 * v0.6 阶段5 起先查空闲表 (first fit), 找不到再从 bump 指针切。
 * 返回的是栈顶 (栈向下增长)。
 */
static u32 *stack_alloc(u32 size)
{
    u8 *stack;
    u32 i;

    size = (size + 7u) & ~7u;

    /* 1. 复用已经归还的块; 块比请求大就切掉前半部分 */
    for (i = 0; i < stack_free_n; i++) {
        if (stack_free_list[i].size >= size) {
            u8 *base = stack_free_list[i].base;

            stack_free_list[i].base += size;
            stack_free_list[i].size -= size;

            if (stack_free_list[i].size == 0) {
                stack_free_n--;
                stack_free_list[i] = stack_free_list[stack_free_n];
            }
            return (u32 *)(uintptr_t)(base + size);
        }
    }

    /* 2. 没有合适的空闲块 → 从 bump 指针切 */
    stack = stack_alloc_ptr;
    if (stack + size > &_task_stack_pool_end) {
        /* 栈池耗尽 — 这是严重错误 */
        return NULL;
    }

    stack_alloc_ptr += size;
    stack_pool_remaining -= size;
    return (u32 *)(uintptr_t)(stack + size);
}

/**
 * @brief  归还一块内核栈
 *
 * @param  top   栈顶 (stack_alloc 的返回值)
 * @param  size  栈大小
 *
 * 当前任务的栈不能马上还: task_kill 之后调用者还站在这块栈上。
 * 由延迟回收队列保证调用时序。
 */
static void stack_free(u8 *top, u32 size)
{
    u8 *base;
    u32 i;

    if (!top || size == 0) {
        return;
    }

    size = (size + 7u) & ~7u;
    base = top - size;

    /* 已经在空闲表里就不重复添加 (防御性检查) */
    for (i = 0; i < stack_free_n; i++) {
        if (stack_free_list[i].base == base &&
            stack_free_list[i].size == size) {
            return;
        }
    }

    if (stack_free_n < STACK_FREE_MAX) {
        stack_free_list[stack_free_n].base = base;
        stack_free_list[stack_free_n].size = size;
        stack_free_n++;
    }
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

/**
 * @brief  用户态任务首次进入的跳板 (汇编实现, sched_asm.S)
 *
 * 首次被调度到时, context_switch 会 BX 到它的地址。
 * 它负责: 设置用户栈 SP_usr, 然后通过异常返回切换到 USR 模式。
 */
extern void user_entry_trampoline(void);

/**
 * @brief  初始化用户态任务的内核栈帧
 *
 * 与 stack_init 的区别: lr 不再是入口函数, 而是 trampoline。
 * 入口函数和用户栈顶分别塞进 r4 / r5, 由 trampoline 读取。
 *
 * context_switch 恢复 {r4-r11, lr} 后 BX lr 跳到 trampoline:
 *   r4 = 用户入口, r5 = 用户栈顶, lr = trampoline
 *
 * @param  sp_top      内核栈顶
 * @param  entry       用户入口函数
 * @param  user_sp_top 用户栈顶
 * @return 初始化后的内核栈指针, 存入 task->sp
 */
static u32 stack_init_user(u32 *sp_top, void (*entry)(void), u32 *user_sp_top)
{
    u32 *sp = sp_top - 9;   /* 9 words: r4-r11(8) + lr(1) */

    sp[0] = (u32)(uintptr_t)entry;         /* r4 → 用户入口 */
    sp[1] = (u32)(uintptr_t)user_sp_top;   /* r5 → 用户栈顶 */
    sp[2] = 0;                              /* r6  */
    sp[3] = 0;                              /* r7  */
    sp[4] = 0;                              /* r8  */
    sp[5] = 0;                              /* r9  */
    sp[6] = 0;                              /* r10 */
    sp[7] = 0;                              /* r11 */
    sp[8] = (u32)(uintptr_t)user_entry_trampoline;  /* lr → trampoline */

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
    task_t *next;

    /*
     * 调度器还没启动 (current_task == NULL) 时不调度。
     *
     * 这个保护很关键: sched_start 之前 timer 中断也会进来,
     * 此时任务可能还没创建完 (task_create 尚未 return),
     * 如果这里直接 context_switch(NULL, next) 就会"提前启动"新任务,
     * 把 task_create 的剩余流程 (含 task_create_user 的 stack_init_user)
     * 整个跳过, 导致用户栈没建、trampoline 没生效。
     */
    if (current_task == NULL) {
        return;
    }

    /* 有任务正在初始化 → 先不切, 免得跑到半成品上 */
    if (sched_lock != 0) {
        return;
    }

    /*
     * 不允许嵌套调度。
     *
     * schedule() 这一段跑在"当前任务"的栈上。如果里面开中断时来了
     * 一个 tick, 中断里的 schedule() 会在这里再切一次, 把外层这次
     * 调度整个丢掉 —— 外层的 current_task 已经改过了, 后面的回收
     * 代码再也不会执行。
     *
     * 症状很隐蔽: 任务退出时地址空间偶尔回收不回来 (6 个任务只收回
     * 5 个), 而且每次跑的结果都不一样。
     *
     * 中断想调度的话, 把 need_resched 留着, 等这次切完自然会处理。
     */
    if (sched_active) {
        return;
    }
    sched_active = 1;

    /*
     * 回收上一轮被杀掉的任务。
     * 这里已经不在它们的栈上了 (current_task 不是它们), 所以
     * 地址空间、内核栈、TCB 可以一起还回去。
     */
    reclaim_drain();

    next = pick_next_task();

    if (next != current_task) {
        task_t *old = current_task;

        current_task = next;
        next->state = TASK_RUNNING;

        /*
         * 换地址空间 (v0.6 阶段4/5)。
         * 必须在 context_switch 之前: 新任务的内核栈和 TCB 都在内核区,
         * 每张页表里映射一致, 所以换表过程中当前指令和栈都还有效。
         *
         * 阶段5 带 ASID: 只写 TTBR0, 不刷 TLB。
         */
        mmu_activate(next->l1_phys, next->asid);

        /* 切换前解除, 新任务看到的应该是可以正常调度的状态 */
        sched_active = 0;
        context_switch(old, next);
    } else {
        sched_active = 0;
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

    mmu_activate(first->l1_phys, first->asid);
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

    /*
     * MOS 模型: RUNNING 任务也在就绪队列里。
     * 所以必须先 dequeue 再 enqueue, 把当前任务移到队尾,
     * 否则会重复插入导致环形链表损坏 (当前任务自环)。
     */
    ready_dequeue(current_task);
    ready_enqueue(current_task);

    EXIT_CRITICAL();
    schedule();
}

void task_change_priority(task_t *t, u32 new_priority)
{
    if (!t) return;
    if (t->priority == new_priority) return;

    /*
     * 如果任务在就绪队列中 → 从旧优先级移出, 放入新优先级。
     * RUNNING 任务也在就绪队列中 (MOS 设计), 所以统一处理。
     * BLOCKED/SLEEPING 任务不在就绪队列 → 只改字段。
     */
    if (t->state == TASK_READY || t->state == TASK_RUNNING) {
        ready_dequeue(t);
        t->priority = new_priority;
        ready_enqueue(t);
    } else {
        t->priority = new_priority;
    }
}

void sched_request(void)
{
    need_resched = 1;
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
    idle_tcb.original_priority = PRIO_IDLE;
    idle_tcb.held_mutex  = NULL;
    idle_tcb.is_user     = 0;
    idle_tcb.user_sp     = 0;
    idle_tcb.l1_phys     = mmu_kernel_l1();
    idle_tcb.l2_phys     = 0;
    idle_tcb.data_phys   = 0;
    idle_tcb.stack_phys  = 0;

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

    /* 创建 TCB SLAB cache — 替代原来的 tcb_pool[16] 静态数组 */
    task_cache = slab_create("task_t", sizeof(task_t));
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

    /* 3. 分配 TCB (v0.4: SLAB 动态分配, 不再有 16 个的静态限制) */
    if (tcb_count >= 32) return NULL;  /* tcb_list 指针数组上限 */
    t = (task_t *)slab_alloc(task_cache);
    if (!t) return NULL;               /* SLAB 内存不足 */

    /* SLAB 复用已释放的内存, 需要清零 (next/prev/block_obj/held_mutex 等) */
    for (i = 0; i < (int)(sizeof(task_t) / 4); i++) {
        ((u32 *)t)[i] = 0;
    }

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
    t->original_priority = priority;
    t->held_mutex  = NULL;
    t->is_user     = 0;
    t->user_sp     = 0;

    /* v0.6 阶段4/5: 内核态任务使用内核主页表 (ASID 0), 没有私有地址空间 */
    t->l1_phys     = mmu_kernel_l1();
    t->l2_phys     = 0;
    t->data_phys   = 0;
    t->stack_phys  = 0;
    t->asid        = 0;
    t->as_slot     = 0;
    t->as_valid    = 0;

    /* v0.5: 初始化文件描述符表 */
    {
        u32 fi;
        for (fi = 0; fi < MAX_FD_PER_TASK; fi++) {
            t->fd_table[fi].in_use    = 0;
            t->fd_table[fi].inode_num = 0;
            t->fd_table[fi].of_idx    = 0;
            t->fd_table[fi].offset    = 0;
            t->fd_table[fi].flags     = 0;
        }
    }

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

/* =========================== 用户态任务 (v0.6 阶段2) =========================== */

task_t *task_create_user(void (*entry)(void), const char *name,
                         u32 priority, u32 kstack_size, u32 ustack_size)
{
    task_t *t;
    user_space_t as;
    u32 *kstack_top;

    if (!entry) return NULL;

    sched_lock++;               /* 初始化完成前不允许调度 */

    /* 1. 分配独立地址空间 (L1/L2 页表 + 数据页 + 64KB 栈) */
    if (user_space_create(&as) != 0) {
        sched_lock--;
        return NULL;
    }

    /* 2. 复用 task_create 分配内核栈 + TCB 并挂进就绪队列。
     *    它伪造的是"内核态入口"帧, 下面会覆盖成用户态跳板帧。 */
    t = task_create(entry, name, priority, kstack_size);
    if (!t) {
        /* TCB/内核栈没拿到, 地址空间要还回去 */
        user_space_destroy(&as);
        sched_lock--;
        return NULL;
    }

    /*
     * 3. 绑定地址空间。
     *    用户栈大小目前固定 64KB (16 个 4KB 页), ustack_size 参数
     *    保留是为了接口稳定, 以后做按需分页时会用到。
     */
    (void)ustack_size;

    t->l1_phys    = as.l1_phys;
    t->l2_phys    = as.l2_phys;
    t->data_phys  = as.data_phys;
    t->stack_phys = as.stack_phys;
    t->asid       = as.asid;
    t->as_slot    = as.slot;
    t->as_valid   = 1;
    t->user_sp    = USER_STACK_TOP;
    t->is_user    = 1;

    /* 4. 重新伪造内核栈帧: 首次调度时经 trampoline 切到用户态。
     *    用户栈顶用固定虚拟地址, 物理页由本任务的页表决定。 */
    kstack_top = (u32 *)(((u32)(uintptr_t)t->stack_base) & ~0x7u);
    t->sp = stack_init_user(kstack_top, entry, (u32 *)(uintptr_t)USER_STACK_TOP);

    /* 5. 解除调度锁, 此时任务已经完整, 可以安全调度 */
    sched_lock--;
    if (sched_lock == 0 && need_resched) {
        schedule();
    }

    return t;
}

/* =========================== 任务终止 (v0.6 阶段3) =========================== */

/**
 * @brief  杀死指定任务 — 只清理状态, 不触发调度
 *
 * "杀死"在这里的意思是把任务从所有调度结构里摘掉:
 *   1. 从就绪队列移除 (RUNNING 任务也在队列里, 这是 MOS 的设计)
 *   2. 状态置 TASK_DEAD
 *   3. 从全局 TCB 列表移除, 避免 sys_tick_handler 再访问它
 *
 * 不释放 TCB 和内核栈: 被杀死任务的栈上此刻还躺着调用者的现场
 * (系统调用/异常处理帧), 必须等调度切走之后才能回收。演示版本先不做
 * 回收, 后续可以加一个 reaper 任务统一释放。
 */
/**
 * @brief  真正释放任务的资源 (地址空间 / 内核栈 / TCB)
 *
 * 只在"这个任务不在 CPU 上, 也不在自己栈上"的时候调用。
 */
static void task_release(task_t *t)
{
    user_space_t us;
    u32 valid;

    if (!t || t == &idle_tcb) {
        return;
    }

    /*
     * 先把地址空间"认领"下来再释放。
     *
     * 释放过程要开关中断、还要刷 TLB, 中途被打断是正常的。如果
     * 读完 as_valid 才去做释放, 中间任何一次重入都会看到"还有
     * 地址空间", 同一份槽位就被释放两次。这里先原子地清零,
     * 谁读到 0 谁就不管。
     */
    ENTER_CRITICAL();
    valid = t->as_valid;
    t->as_valid = 0;
    us.l1_phys    = t->l1_phys;
    us.l2_phys    = t->l2_phys;
    us.data_phys  = t->data_phys;
    us.stack_phys = t->stack_phys;
    us.asid       = t->asid;
    us.slot       = t->as_slot;
    EXIT_CRITICAL();

    /* 1. 用户地址空间: 页表 + 数据页 + 用户栈 */
    if (valid) {
        user_space_destroy(&us);
    }

    /* 2. 内核栈 */
    if (t->stack_base) {
        stack_free((u8 *)(uintptr_t)t->stack_base, t->stack_size);
        t->stack_base = NULL;
    }

    /* 3. TCB 本身 (从 slab 来的) */
    slab_free(t);
}

/**
 * @brief  把任务挂进延迟回收队列
 */
static void reclaim_push(task_t *t)
{
    if (reclaim_n < RECLAIM_MAX) {
        reclaim_list[reclaim_n++] = t;
    }
}

/**
 * @brief  回收所有"已经不是当前任务"的死任务
 *
 * 在 schedule() 开头调用。被杀的任务永远不可能再被选中, 所以只要
 * current_task 不是它, 就说明 CPU 已经不在它的栈上, 可以安全释放。
 */
static void reclaim_drain(void)
{
    task_t *todo[RECLAIM_MAX];
    u32 n = 0;
    u32 i;

    /*
     * 分两步: 先在关中断的情况下把待回收的任务从队列里摘下来,
     * 再开中断做真正的释放。
     *
     * task_release 要刷 TLB、动栈池和 slab, 时间不短。如果一边释放
     * 一边让 tick 进来, 中断里的 schedule() 会再次调用本函数,
     * 同一个任务就会被释放两次, 或者从队列里被静默丢掉。
     */
    ENTER_CRITICAL();

    i = 0;
    while (i < reclaim_n) {
        if (reclaim_list[i] == current_task) {
            i++;                    /* 就是自己, 还站在人家栈上, 下次再说 */
            continue;
        }

        todo[n++] = reclaim_list[i];
        reclaim_list[i] = reclaim_list[reclaim_n - 1];
        reclaim_n--;
    }

    EXIT_CRITICAL();

    for (i = 0; i < n; i++) {
        task_release(todo[i]);
    }
}

void task_kill(task_t *t)
{
    u32 i;
    int self;

    if (!t || t == &idle_tcb || t->state == TASK_DEAD) {
        return;
    }

    self = (t == current_task);

    ENTER_CRITICAL();

    if (t->state == TASK_READY || t->state == TASK_RUNNING) {
        ready_dequeue(t);
    }
    t->state = TASK_DEAD;

    for (i = 0; i < tcb_count; i++) {
        if (tcb_list[i] == t) {
            u32 j;
            for (j = i; j + 1 < tcb_count; j++) {
                tcb_list[j] = tcb_list[j + 1];
            }
            tcb_count--;
            tcb_list[tcb_count] = NULL;
            break;
        }
    }

    /*
     * 自己杀自己: 现场还在自己的内核栈上, 不能当场回收。
     * 别的任务杀它: 它已经不在 CPU 上, 直接释放。
     *
     * 挂队列这一步必须留在临界区里。任务状态已经改成 DEAD, 也从
     * 就绪队列摘掉了, 如果这里开中断, 一个 tick 就可能把 CPU 切走,
     * 而它还没进回收队列 —— 它就再也不会被调度, 资源永久泄漏。
     * (这个窗口是实际踩出来的: 6 个任务退出, 偶尔只回收 5 个。)
     */
    if (self) {
        reclaim_push(t);
    }

    EXIT_CRITICAL();

    if (!self) {
        task_release(t);
    }
}

/**
 * @brief  结束当前任务 (SYS_EXIT) — 永不返回
 */
void task_exit(void)
{
    task_kill(current_task);

    /*
     * 切到下一个任务。current_task 已经被摘出就绪队列,
     * pick_next_task() 不会选中它, 所以这里不会返回。
     * 即使意外返回, 也只是在这个已死的栈上空转, 不会再碰用户态。
     */
    schedule();

    while (1) {
        __asm__ volatile("WFI");
    }
}
