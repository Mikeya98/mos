/**
 * @file    main.c
 * @brief   MOS RTOS C entry — message queue producer-consumer demo
 *
 * v0.2: mutex (PI) + message queue
 *
 * Demo scenario (yield-driven, no timer needed):
 *   1. msgq capacity=4, msg_size=4 (u32)
 *   2. Producer (prio=2) sends 6 values into a 4-slot queue
 *   3. First 4 sends succeed, 5th blocks (queue full)
 *   4. Consumer (prio=3) receives, each recv wakes producer
 *   5. Producer sends one more, blocks again → cycle repeats
 */

#include "lib/types.h"
#include "lib/printf.h"
#include "drivers/uart.h"
#include "drivers/gic.h"
#include "drivers/timer.h"
#include "kernel/task.h"
#include "ipc/semaphore.h"
#include "ipc/mutex.h"
#include "ipc/msgq.h"

static sem_t   print_sem;
static msgq_t  test_msgq;
static u8      msgq_buf[4 * 4];  /* 4 messages × 4 bytes each */

/*
 * Producer (prio=2): send 6 values into a 4-slot queue.
 * No yield — blocking/waking drives the alternation.
 *
 * Flow:
 *   1. Sends 100,200,300,400 → queue full
 *   2. Tries 500 → BLOCKS (no space)
 *   3. Consumer wakes, receives 100 → wakes producer
 *   4. Producer preempts consumer (prio=2 > 3), sends 500 → full, blocks again
 *   5. Cycle repeats for 600
 */
static void producer(void)
{
    u32 vals[] = {100, 200, 300, 400, 500, 600};
    int i;

    for (i = 0; i < 6; i++) {
        sem_wait(&print_sem);
        printf("[PROD] send %u (count=%u)\n", vals[i], test_msgq.count);
        sem_post(&print_sem);

        msgq_send(&test_msgq, &vals[i], 1);  /* may block here */
    }

    sem_wait(&print_sem);
    printf("[PROD] all sent, sleeping\n");
    sem_post(&print_sem);

    task_sleep(999999);  /* sleep "forever" — removed from ready queue */
}

/*
 * Consumer (prio=3): receive 6 values, blocking when empty.
 *
 * Flow:
 *   1. Starts first, tries recv → BLOCKS (queue empty)
 *   2. Producer fills queue, each send wakes consumer but
 *      consumer is lower priority → producer keeps going
 *   3. Producer blocks on 5th send → consumer finally runs
 *   4. Consumer recv → wakes producer (higher prio) → producer preempts
 *   5. Cycle repeats
 */
static void consumer(void)
{
    u32 val;
    int i;

    for (i = 0; i < 6; i++) {
        sem_wait(&print_sem);
        printf("[CONS] waiting... (count=%u)\n", test_msgq.count);
        sem_post(&print_sem);

        msgq_recv(&test_msgq, &val, 1);  /* may block here */

        sem_wait(&print_sem);
        printf("[CONS] recv %u (count=%u)\n", val, test_msgq.count);
        sem_post(&print_sem);
    }

    sem_wait(&print_sem);
    printf("[CONS] all received, sleeping\n");
    sem_post(&print_sem);

    task_sleep(999999);  /* sleep "forever" — removed from ready queue */
}

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

void kernel_main(void)
{
    uart_init(115200);

    printf("\n");
    printf("========================================\n");
    printf("  MOS RTOS v0.2\n");
    printf("  Message Queue Demo\n");
    printf("========================================\n\n");

    gic_init();
    printf("[INIT] GIC initialized\n");

    task_init();
    printf("[INIT] Task subsystem ready\n");

    sem_init(&print_sem, 1);
    msgq_init(&test_msgq, msgq_buf, 4, 4);
    printf("[INIT] Semaphore + MsgQ(4x4B) ready\n\n");

    printf("Creating tasks...\n");
    task_create(producer, "producer", 2, STACK_4K);
    printf("  [CREATED] producer (prio=2)\n");
    task_create(consumer, "consumer", 3, STACK_4K);
    printf("  [CREATED] consumer (prio=3)\n\n");

    timer_init(TICK_MS);
    printf("[INIT] Timer started\n");

    printf("Starting scheduler...\n");
    printf("--- MOS RTOS is running now ---\n\n");

    sched_start();

    while (1) ;
}
