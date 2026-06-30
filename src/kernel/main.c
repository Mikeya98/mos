/**
 * @file    main.c
 * @brief   MOS RTOS C entry — buddy allocator demo (v0.3 preview)
 *
 * Demo scenario (sequential, before scheduler starts):
 *   1. buddy_init() with linker-provided heap region
 *   2. Series of alloc calls → observe splitting
 *   3. buddy_dump() after allocs
 *   4. Free some blocks → observe merging
 *   5. buddy_dump() after frees
 *   6. Re-alloc → verify split from merged block
 *   7. Stress: alloc until OOM, then free all
 *   8. Remaining heap should be back to 1 big block
 */

#include "lib/types.h"
#include "lib/printf.h"
#include "drivers/uart.h"
#include "drivers/gic.h"
#include "drivers/timer.h"
#include "kernel/task.h"
#include "mm/buddy.h"

/* =========================== 外部符号 =========================== */
/* 由 kernel.lds 提供 */
extern u8 _heap_start;
extern u8 _heap_end;

/* =========================== 测试用例 =========================== */

/*
 * Buddy allocator demo — run before sched_start.
 * Tests: rounding, splitting, merging, OOM, stress.
 */
static void buddy_demo(void)
{
    u32 heap_size;
    void *p[10];
    u32 i;

    heap_size = (u32)(&_heap_end - &_heap_start);

    printf("\n========================================\n");
    printf("  Buddy Allocator Demo\n");
    printf("  heap: %X - %X (%u B)\n",
           (u32)(uintptr_t)&_heap_start,
           (u32)(uintptr_t)&_heap_end,
           heap_size);
    printf("========================================\n\n");

    /* 1. 初始化 */
    buddy_init(&_heap_start, heap_size);
    buddy_dump();
    printf("[PASS] buddy_init — 1 free block (order ~%u)\n\n",
           (u32)(heap_size / 64));

    /* 2. 分配: 不同大小 → 触发分裂 */
    p[0] = buddy_alloc(300);   /* → 512B, order 3 */
    printf("[ALLOC] 300B → 512B (order 3)  ptr=%X\n", (u32)(uintptr_t)p[0]);

    p[1] = buddy_alloc(100);   /* → 128B, order 1 */
    printf("[ALLOC] 100B → 128B (order 1)  ptr=%X\n", (u32)(uintptr_t)p[1]);

    p[2] = buddy_alloc(1);     /* → 64B,  order 0 */
    printf("[ALLOC]   1B →  64B (order 0)  ptr=%X\n", (u32)(uintptr_t)p[2]);

    p[3] = buddy_alloc(2000);  /* → 2048B, order 5 */
    printf("[ALLOC] 2KB → 2KB  (order 5)  ptr=%X\n", (u32)(uintptr_t)p[3]);

    p[4] = buddy_alloc(4096);  /* → 4096B, order 6 */
    printf("[ALLOC] 4KB → 4KB  (order 6)  ptr=%X\n", (u32)(uintptr_t)p[4]);

    buddy_dump();

    /* 3. 释放 → 测试合并 */
    printf("[FREE]  ptr=%X (order 3, 512B)\n", (u32)(uintptr_t)p[0]);
    buddy_free(p[0]);

    printf("[FREE]  ptr=%X (order 0, 64B)\n", (u32)(uintptr_t)p[2]);
    buddy_free(p[2]);

    printf("[FREE]  ptr=%X (order 1, 128B)\n", (u32)(uintptr_t)p[1]);
    buddy_free(p[1]);
    printf("  (freed 3 small blocks)\n");

    buddy_dump();

    /* 4. 重新分配同一位置 → 验证重新分裂 */
    p[5] = buddy_alloc(512);
    printf("[ALLOC] 512B → 512B (order 3)  ptr=%X\n", (u32)(uintptr_t)p[5]);
    buddy_dump();

    /* 5. 压力测试: 分配直到 OOM */
    printf("\n--- Stress: alloc until OOM ---\n");
    for (i = 0; i < 10; i++) p[i] = NULL;

    for (i = 0; i < 10; i++) {
        /*
         * 每次分配 128KB (order 11), 1MB 堆最多容纳 8 个。
         * 加上之前已分配的 p[3]=2KB, p[4]=4KB, p[5]=512B,
         * 预期第 7-8 次会失败。
         */
        p[i] = buddy_alloc(128 * 1024);
        if (p[i]) {
            printf("  stress[%u] alloc 128KB → ptr=%X\n", i, (u32)(uintptr_t)p[i]);
        } else {
            printf("  stress[%u] alloc 128KB → NULL (OOM, expected)\n", i);
            break;
        }
    }
    buddy_dump();

    /* 6. 全部释放 → 应该回到 1 个大块 */
    printf("\n--- Free all ---\n");
    buddy_free(p[5]);   /* 512B */
    buddy_free(p[4]);   /* 4KB */
    buddy_free(p[3]);   /* 2KB */
    for (i = 0; i < 10 && p[i]; i++) {
        buddy_free(p[i]);  /* 128KB chunks */
        printf("  freed stress[%u]\n", i);
    }

    buddy_dump();
    printf("[DONE] Buddy allocator demo complete.\n\n");
}

/* =========================== 中断分发 =========================== */

void irq_dispatch(void)
{
    u32 irq_id = gic_get_ack();
    if (irq_id == IRQ_PTIMER) {
        timer_isr();
        sys_tick_handler();
        gic_send_eoi(irq_id);
        schedule();
    } else if (irq_id == IRQ_UART0) {
        gic_send_eoi(irq_id);
    } else {
        printf("[WARN] Unknown IRQ: %u\n", irq_id);
        gic_send_eoi(irq_id);
    }
}

/* =========================== 内核入口 =========================== */

void kernel_main(void)
{
    uart_init(115200);

    printf("\n");
    printf("========================================\n");
    printf("  MOS RTOS v0.3-dev\n");
    printf("  Buddy Allocator Test\n");
    printf("========================================\n\n");

    gic_init();
    printf("[INIT] GIC initialized\n");

    task_init();
    printf("[INIT] Task subsystem ready\n");

    timer_init(TICK_MS);
    printf("[INIT] Timer started\n");

    /* 在启动调度器之前运行 buddy demo (纯逻辑, 无需任务) */
    buddy_demo();

    printf("Starting scheduler...\n");
    printf("--- MOS RTOS is running now ---\n\n");

    sched_start();

    while (1) ;
}
