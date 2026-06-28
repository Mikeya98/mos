/**
 * @file    main.c
 * @brief   MOS RTOS C entry — PI verification (yield-driven, no timer)
 */

#include "lib/types.h"
#include "lib/printf.h"
#include "drivers/uart.h"
#include "drivers/gic.h"
#include "drivers/timer.h"
#include "kernel/task.h"
#include "ipc/semaphore.h"
#include "ipc/mutex.h"

static sem_t   print_sem;
static mutex_t res_mutex;

/*
 * PI verification (yield-driven, no timer needed):
 *
 *   1. L runs first (only user task), locks mutex
 *   2. L dynamically creates H (prio=1)
 *   3. L yields -> H runs -> H tries lock -> BLOCKED
 *   4. boost_owner_chain: L boosted to prio=1
 *   5. L runs again, prints prio=1 (PI works!)
 *   6. M never runs during this because L/H dominate
 */

static void task_h(void)
{
    sem_wait(&print_sem);
    printf("[H] prio=%u try lock...\n", get_current()->priority);
    sem_post(&print_sem);

    mutex_lock(&res_mutex);

    sem_wait(&print_sem);
    printf("[H] prio=%u GOT LOCK!\n", get_current()->priority);
    sem_post(&print_sem);

    mutex_unlock(&res_mutex);

    sem_wait(&print_sem);
    printf("[H] done, yielding forever\n");
    sem_post(&print_sem);

    while (1) task_yield();
}

static void task_m(void)
{
    u32 count = 0;
    while (1) {
        sem_wait(&print_sem);
        printf("[M] prio=%u working... (%u)\n",
               get_current()->priority, count++);
        sem_post(&print_sem);
        task_yield();
    }
}

static void task_l(void)
{
    mutex_lock(&res_mutex);

    sem_wait(&print_sem);
    printf("[L] prio=%u HOLDING mutex\n", get_current()->priority);
    sem_post(&print_sem);

    /* Dynamically create H — higher priority, will preempt */
    sem_wait(&print_sem);
    printf("[L] creating task_h (prio=1)...\n");
    sem_post(&print_sem);

    task_create(task_h, "task_h", 1, STACK_4K);

    /*
     * Yield -> H runs, tries mutex_lock -> BLOCKED
     * -> boost_owner_chain -> L boosted to prio=1
     * -> L resumes
     */
    task_yield();

    /* KEY CHECK: if PI works, priority should be 1 now */
    sem_wait(&print_sem);
    printf("[L] prio=%u after yield (expect 1!)\n",
           get_current()->priority);
    sem_post(&print_sem);

    mutex_unlock(&res_mutex);

    sem_wait(&print_sem);
    printf("[L] prio=%u after unlock (expect 2)\n",
           get_current()->priority);
    sem_post(&print_sem);

    while (1) task_yield();
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
    printf("  Priority Inheritance Test\n");
    printf("========================================\n\n");

    gic_init();
    printf("[INIT] GIC initialized\n");

    timer_init(TICK_MS);
    printf("[INIT] Timer initialized\n");

    task_init();
    printf("[INIT] Task subsystem ready\n");

    sem_init(&print_sem, 1);
    mutex_init(&res_mutex);
    printf("[INIT] Semaphore + Mutex ready\n\n");

    /*
     * Create L and M only. H is created dynamically by L
     * AFTER L has locked the mutex — this guarantees
     * H hits the blocking path.
     */
    printf("Creating initial tasks...\n");
    task_create(task_l, "task_l", 2, STACK_4K);
    printf("  [CREATED] task_l (prio=2)\n");
    task_create(task_m, "task_m", 3, STACK_4K);
    printf("  [CREATED] task_m (prio=3)\n\n");

    printf("Starting scheduler...\n");
    printf("--- MOS RTOS is running now ---\n\n");

    sched_start();

    while (1) ;
}
