/**
 * @file    main.c
 * @brief   MOS RTOS C 入口 — 初始化一切并启动调度器
 *
 * 初始化顺序有严格依赖:
 *   UART 必须最先 — 否则看不到任何调试输出
 *   GIC  必须第二  — timer_init 需要使能中断
 *   Timer 必须第三 — 之后系统 tick 开始运转
 *   Task  必须最后 — 创建任务时可能用到 semaphore
 *
 * CPU 状态 (从 startup.S 交来):
 *   SVC 模式, IRQ/FIQ 屏蔽, MMU 关闭, Cache 无效, BSS 清零
 */

#include "lib/types.h"
#include "lib/printf.h"
#include "drivers/uart.h"
#include "drivers/gic.h"
#include "drivers/timer.h"
#include "kernel/task.h"
#include "ipc/semaphore.h"

/* =========================== 外部引用 =========================== */
/* startup.S 中的 IRQ 分发 (此处声明, C 中实现) */

/* =========================== 测试任务 =========================== */

/** 共享信号量 (用于演示任务同步) */
static sem_t print_sem;

/**
 * @brief  测试任务 A — 高优先级 (prio=1)
 *
 * 每 1 秒获取信号量并打印, 演示优先级抢占
 */
static void task_a(void)
{
    u32 count = 0;
    while (1) {
        sem_wait(&print_sem);
        printf("[Task A] count=%u\n", count++);
        sem_post(&print_sem);

        /* 睡眠 1000ms */
        task_sleep(1000);
    }
}

/**
 * @brief  测试任务 B — 低优先级 (prio=2)
 *
 * 每 2 秒获取信号量并打印。
 * 当 A 就绪时 B 会被立即抢占。
 */
static void task_b(void)
{
    u32 count = 0;
    while (1) {
        sem_wait(&print_sem);
        printf("[Task B] count=%u\n", count++);
        sem_post(&print_sem);

        /* 睡眠 2000ms — 比 A 睡得更久 */
        task_sleep(2000);
    }
}

/* =========================== IRQ 分发 =========================== */

/**
 * @brief  IRQ 分发函数 (由 startup.S 的 irq_handler 调用)
 *
 * 流程:
 *   1. 读 GIC IAR → 中断 ID
 *   2. 根据 ID 查 ISR 表
 *   3. 写 GIC EOIR
 *   4. 检查是否需要调度
 */
void irq_dispatch(void)
{
    u32 irq_id = gic_get_ack();

    /* 处理已知中断 */
    if (irq_id == IRQ_PTIMER) {
        /*
         * 私有定时器中断 (系统 tick)
         *
         * 处理顺序:
         *   a) 清除定时器中断标志
         *   b) 系统 tick 计数 +1
         *   c) 处理睡眠任务
         *   d) 检查调度
         */
        timer_isr();
        sys_tick_handler();
        gic_send_eoi(irq_id);

        /*
         * tick 处理可能唤醒了更高优先级任务,
         * 检查是否需要切换。
         */
        schedule();

    } else if (irq_id == IRQ_UART0) {
        /* UART0 中断 — v0.1 不使用中断驱动, 仅 ACK */
        gic_send_eoi(irq_id);

    } else {
        /* 未知中断 — 打印警告 (调试用) */
        printf("[WARN] Unknown IRQ: %u\n", irq_id);
        gic_send_eoi(irq_id);
    }
}

/* =========================== 主入口 =========================== */

/**
 * @brief  内核主入口 (从 startup.S 调用)
 *
 * 永不返回 — 启动调度器后 CPU 进入任务循环
 */
void kernel_main(void)
{
    /* ===========================================
     * 1. UART — 最先初始化 (调试刚需)
     * =========================================== */
    uart_init(115200);

    printf("\n");
    printf("========================================\n");
    printf("  MOS RTOS v0.1\n");
    printf("  Priority-Preemptive Scheduler\n");
    printf("  32 priority levels, O(1) bitmap\n");
    printf("========================================\n\n");

    /* ===========================================
     * 2. GIC — 中断控制器
     * =========================================== */
    gic_init();
    printf("[INIT] GIC initialized\n");

    /* ===========================================
     * 3. Timer — 系统 tick (1ms)
     * =========================================== */
    timer_init(TICK_MS);
    printf("[INIT] Timer initialized (tick=%ums)\n", TICK_MS);

    /* ===========================================
     * 4. 任务子系统
     * =========================================== */
    task_init();
    printf("[INIT] Task subsystem ready\n");

    /* ===========================================
     * 5. 信号量 — 演示用
     * =========================================== */
    sem_init(&print_sem, 1);  /* 初始 = 1, 互斥效果 */
    printf("[INIT] Semaphore ready\n\n");

    /* ===========================================
     * 6. 创建测试任务
     * =========================================== */
    printf("Creating tasks...\n");

    /**
     * Task A: prio=1, 每 1s 打印一次
     * Task B: prio=2, 每 2s 打印一次
     *
     * 预期行为:
     *   1. A 先运行 (优先级更高)
     *   2. A sleep(1s) → B 运行
     *   3. 1s 后 A 醒来 → 抢占 B → A 打印
     *   4. A sleep → B 继续之前的 2s sleep
     */
    task_create(task_a, "task_a", 1, STACK_4K);
    printf("  [CREATED] task_a (prio=1, 4KB stack)\n");

    task_create(task_b, "task_b", 2, STACK_4K);
    printf("  [CREATED] task_b (prio=2, 4KB stack)\n\n");

    /* ===========================================
     * 7. 启动调度器 — 永不返回
     * =========================================== */
    printf("Starting scheduler...\n");
    printf("--- MOS RTOS is running now ---\n\n");

    sched_start();

    /* 不会到达这里 */
    while (1)
        ;
}
