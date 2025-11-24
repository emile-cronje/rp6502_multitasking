#include "scheduler.h"
#include <stddef.h> /* For NULL */
#include <stdlib.h> /* For malloc and free */

const unsigned int BATCH_SIZE = 500;    
const unsigned int MAX_ITEM_COUNT = 2000;    
const unsigned int use_monitor = 1u;    
void scheduler_sleep(unsigned short ticks);
void scheduler_yield(void);

unsigned int pseudo_random(unsigned int min_val, unsigned int max_val);

/* Move declarations to the top of the file */
static unsigned char mem_fluctuate_active = 0;
static unsigned char mem_fluctuate_buffer[1024];

/* Task to simulate memory usage fluctuation using malloc */
static void mem_fluctuate_task(void *arg)
{
    unsigned int counter;
    unsigned char *dynamic_buffer;
    unsigned int buffer_size;

    counter = 0;
    dynamic_buffer = NULL;
    buffer_size = 0;

    (void)arg;

    for (;;) {
        counter++;

        /* Randomly decide to allocate or free memory every 10 cycles */
        if (counter % 10 == 0) {
            if (dynamic_buffer) {
                /* Free the allocated memory */
                free(dynamic_buffer);
                dynamic_buffer = NULL;
                buffer_size = 0;
            } else {
                buffer_size = pseudo_random(256, 1024);
                dynamic_buffer = (unsigned char *)malloc(buffer_size);

                /* Simulate usage if allocation succeeds */
                if (dynamic_buffer) {
                    unsigned int i;
                    for (i = 0; i < buffer_size; i++) {
                        dynamic_buffer[i] = (unsigned char)(counter + i);
                    }
                }
            }
        }

        scheduler_sleep(50);
        scheduler_yield();
    }
}
#include "scheduler.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "string_helpers.h"
#include "ringq.h"

volatile unsigned int sleep_ms = 250;
volatile unsigned int sleep_ms_short = 10;
volatile unsigned int sleep_ms_producer = 50;
volatile unsigned int sleep_ms_consumer = 200;
volatile unsigned int consumed_item_1 = 0;
volatile unsigned int consumed_item_2 = 0;
volatile unsigned int produced_item = 0;
volatile uint16_t task_a_counter = 1;
volatile uint16_t task_b_counter = 2;

/* Throttling parameters (tunable) */
static const unsigned int PROD_SLEEP_BASE = 50;
static const unsigned int CONS_SLEEP_BASE = 200;
static const unsigned int PROD_SLEEP_MIN = 5;
static const unsigned int PROD_SLEEP_MAX = 1000;
static const unsigned int CONS_SLEEP_MIN = 5;
static const unsigned int CONS_SLEEP_MAX = 1000;

static const unsigned int Q_HIGH_WATERMARK = (Q_CAP * 3) / 4; /* 75% full */
static const unsigned int Q_LOW_WATERMARK = (Q_CAP) / 4;      /* 25% full */

/* Approximate total RAM available for the OS (matches rp6502.cfg:
    RAM start = $0200, size = $FD00 - __STACKSIZE__ where __STACKSIZE__ is $0800
    So total bytes = 0xFD00 - 0x0800 = 62464
*/
static const unsigned int RAM_TOTAL_BYTES = 62464u;

static RingQ test_q_1;

/* forward from scheduler.c */
unsigned int scheduler_memory_usage(void);

/* Simple pseudo-random generator (linear congruential method) */
static unsigned int random_seed = 42u;

void seed_random(unsigned int val)
{
    random_seed = val ? val : 42u;
}

unsigned int pseudo_random(unsigned int min_val, unsigned int max_val)
{
    /* LCG: next = (a * seed + c) mod m */
    random_seed = (1103515245u * random_seed + 12345u) & 0x7FFFFFFFu;
    if (max_val <= min_val) return min_val;
    return min_val + (random_seed % (max_val - min_val + 1u));
}

/* string_helper functions moved to src/string_helpers.c */

void printValue(unsigned int value, char *description)
{
    char buf[16];        

    itoa_new(value, buf, sizeof(buf));
    puts(description);
    puts(buf);
}

/* Render a compact ASCII bar for a percentage (0-100).
   buf must be large enough for width+3 characters ( '[' + width + ']' + '\0' ).
   returns buf. */
char *make_bar(unsigned int pct, char *buf, int width)
{
    int filled = (pct * width) / 100;
    int i;
    if (width <= 0) {
        buf[0] = '\0';
        return buf;
    }
    buf[0] = '[';
    for (i = 0; i < width; ++i) {
        buf[1 + i] = (i < filled) ? '#' : '.';
    }
    buf[1 + width] = ']';
    buf[2 + width] = 0;
    return buf;
}

/* forward declare fail_halt (defined after test globals so it can print them) */
static void fail_halt(const char *msg, unsigned int a, unsigned int b);

static void task_a(void *arg)
{
    while (1) {
//        puts("Task A:");
//        print_u16(task_a_counter++);
        (void)arg;
        task_a_counter++;        
        scheduler_yield();
    }
}

static void task_b(void *arg)
{
    while (1) {
//        puts("Task B:");
        (void)arg;
        task_b_counter++;        
        scheduler_yield();
    }
}

static void task_once(void *arg)
{
    (void)arg;
    puts("One-shot task: running once");
}

/* Deep-stack test task: recursive variant that allocates a small buffer on
   each recursion frame so the total stack in-use is the sum of frames.
   We use depth=3 with 64 bytes per frame (≈192B total) to stay inside the
   per-task 256-byte stack window while making stack usage visible. The
   bottom recursion frame loops forever so the stack remains allocated. */
static void deep_stack_recursive(int depth, int max_depth)
{
    unsigned char buffer[64]; /* 64B per frame */
    int i;

    /* touch buffer so compiler won't optimize it away */
    for (i = 0; i < (int)sizeof(buffer); ++i) {
        buffer[i] = (unsigned char)(depth + i);
    }

    if (depth < max_depth) {
        deep_stack_recursive(depth + 1, max_depth);
        /* never returns from bottom loop, but if it did we yield once */
        scheduler_yield();
    } else {
        /* Bottom-most frame: loop forever so earlier frames stay on stack. */
        unsigned int counter = 0;
        for (;;) {
            counter++;
            scheduler_yield();
        }
    }
}

static void deep_stack_test(void *arg)
{
    (void)arg;
    /* Start recursion with depth 0, max depth 2 => frames: 0,1,2 (3 frames) */
    deep_stack_recursive(0, 2);
    /* Should never reach here */
    for (;;) scheduler_yield();
}

static void producer_task(void *arg)
{
    unsigned int val = 0;

    (void)arg;

    for (;;)
    {
        scheduler_sleep(sleep_ms_producer);        

        if (q_push(&test_q_1, val))
        {
            produced_item = val;            
            val++;
        }

        scheduler_yield();
    }
}

/* Queue test: producer/consumer pair that use their own RingQ instance to
   send a random number of items (1000-10000 per run) and verify that all
   items were received. */
static RingQ test_q_2;
volatile unsigned int test_sent_count = 0;
volatile unsigned int test_recv_count = 0;
volatile unsigned int test_producer_done = 0;
volatile unsigned int test_total_item_count = 0;
volatile unsigned int test_start_ticks = 0;
volatile unsigned int test_consumer_ready = 0;
volatile unsigned int test_end_ticks = 0;
static unsigned int test_time_elapsed_ticks = 0;
volatile unsigned long test_sent_sum = 0UL;
volatile unsigned long test_recv_sum = 0UL;
volatile unsigned int test_run_count = 0;

/* Instrumentation totals for ringq operations (incremented in ringq.c). */
volatile unsigned long ringq_total_pushed = 0UL;
volatile unsigned long ringq_total_popped = 0UL;

/* Debug hook invoked from ringq.c on invariant failures. */
void ringq_debug_fail(const char *msg, unsigned int a, unsigned int b)
{
    /* Reuse fail_halt to print extended state and halt */
    fail_halt(msg, a, b);
}

/* Sequence instrumentation: monotonic sequence written by producer and
   recorded by consumer to help detect extra/duplicate pops. */
volatile unsigned long test_seq = 0UL;
volatile unsigned int recv_log[64];
volatile unsigned int recv_log_pos = 0u;

/* Full fail_halt implementation prints extended runtime state then halts. */
static void fail_halt(const char *msg, unsigned int a, unsigned int b)
{
    char numbuf[32];
    unsigned int tail_idx;
    unsigned int val_tail;
    unsigned int prev_idx;

    puts("*** FATAL: "); puts(msg); puts("\r\n");

    puts("args: ");
    itoa_new(a, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    itoa_new(b, numbuf, sizeof(numbuf)); puts(numbuf); puts("\r\n");

    puts("run:"); itoa_new(test_run_count, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("ticks:"); itoa_new(scheduler_get_ticks(), numbuf, sizeof(numbuf)); puts(numbuf); puts("\r\n");

    puts("total_items:"); itoa_new(test_total_item_count, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("sent_count:"); itoa_new(test_sent_count, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("recv_count:"); itoa_new(test_recv_count, numbuf, sizeof(numbuf)); puts(numbuf); puts("\r\n");

    puts("sent_sum_lo:"); itoa_new((unsigned int)(test_sent_sum & 0xFFFFu), numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("recv_sum_lo:"); itoa_new((unsigned int)(test_recv_sum & 0xFFFFu), numbuf, sizeof(numbuf)); puts(numbuf); puts("\r\n");

    /* Test queue state */
    puts("test_q_2 head:"); itoa_new(test_q_2.head, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("tail:"); itoa_new(test_q_2.tail, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("count:"); itoa_new(q_count(&test_q_2), numbuf, sizeof(numbuf)); puts(numbuf); puts(" free:"); itoa_new(q_space_free(&test_q_2), numbuf, sizeof(numbuf)); puts(numbuf); puts("\r\n");

    if (!q_is_empty(&test_q_2)) {
        tail_idx = test_q_2.tail;
        val_tail = test_q_2.buf[tail_idx];
        itoa_new(val_tail, numbuf, sizeof(numbuf)); puts("q2.buf[tail]:"); puts(numbuf); puts(" ");
        prev_idx = (test_q_2.head - 1) & (Q_CAP - 1);
        itoa_new(test_q_2.buf[prev_idx], numbuf, sizeof(numbuf)); puts("q2.buf[head-1]:"); puts(numbuf); puts("\r\n");
    }

    /* Print a small window of consecutive queue entries starting at tail. */
    {
        unsigned int idx = test_q_2.tail;
        unsigned int i;
        puts("q2.buf[window 8]:");
        for (i = 0; i < 8u; ++i) {
            unsigned int v = test_q_2.buf[idx];
            itoa_new(v, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
            idx = (idx + 1) & (Q_CAP - 1);
        }
        puts("\r\n");
    }

    /* Print recent received values from consumer log */
    {
        unsigned int rp = recv_log_pos;
        unsigned int i;
        puts("recent_recv[8]:");
        for (i = 0; i < 8u; ++i) {
            unsigned int idx = (unsigned int)((rp - 8u + i) & 63u);
            unsigned int v = recv_log[idx];
            itoa_new(v, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
        }
        puts("\r\n");
    }

    /* Print global instrumentation totals */
    puts("ringq_total_pushed:"); itoa_new((unsigned int)(ringq_total_pushed & 0xFFFFFFFFu), numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("ringq_total_popped:"); itoa_new((unsigned int)(ringq_total_popped & 0xFFFFFFFFu), numbuf, sizeof(numbuf)); puts(numbuf); puts("\r\n");

#if defined(__CC65__)
    /* disable interrupts to keep system halted */
    __asm__("sei");
#endif

    for (;;) { /* halt */ }
}

static void queue_test_producer(void *arg)
{
    unsigned int i;
    static char buf[32];
    static char summary_buffer[128];    
    size_t sb_pos = 0;
    unsigned int y;    
    unsigned int short_pct;
    (void)arg;

    for (;;) {
        q_init(&test_q_2);

        test_run_count++;                 

        if (use_monitor == 0) {
            itoa_new(test_run_count, buf, sizeof(buf));
            puts("Test run starting:");
            puts(buf);
        }        

        /* Quick runtime sanity check: ensure test_q and q buffers don't overlap */
        {
            unsigned int *a_start = (unsigned int *)&test_q_2.buf[0];
            unsigned int *a_end = (unsigned int *)&test_q_2.buf[Q_CAP - 1];
            unsigned int *b_start = (unsigned int *)&test_q_1.buf[0];
            unsigned int *b_end = (unsigned int *)&test_q_1.buf[Q_CAP - 1];

            if (!((b_end < a_start) || (b_start > a_end))) {
                puts("Warning: RingQ buffers overlap or are adjacent - check memory layout\r\n");
            }
        }

        /* Wait for consumer to be ready for the next run */
        test_consumer_ready = 0;
        test_producer_done = 0;
        test_sent_count = 0;
        test_recv_count = 0;
        test_seq = 0UL;
        test_sent_sum = 0UL;
        test_recv_sum = 0UL;
        while (test_consumer_ready == 0) {
            scheduler_yield();
        }

        test_total_item_count = pseudo_random(100u, MAX_ITEM_COUNT);

        // if (use_monitor == 1)
        // {
        //     itoa_new(test_total_item_count, buf, sizeof(buf));
        //     puts("Test Queue: total items:");
        //     puts(buf);
        // }        

        test_start_ticks = scheduler_get_ticks();

        i = 0;
        while (i < test_total_item_count)
        {
            unsigned int seq = (unsigned int)(++test_seq);
            while (!q_push(&test_q_2, seq)) {
                /* let consumer run */
                scheduler_yield();
            }

            test_sent_count = i + 1;
            test_sent_sum += (unsigned long)seq;
            i++;
        }

        /* Signal consumer that producer has finished enqueuing for this run,
         * then wait for the consumer to drain the remaining items. */
        test_producer_done = 1;

        /* Wait for consumer to finish draining the queue before restarting */
        while (!q_is_empty(&test_q_2))
        {
            scheduler_yield();
        }

        /* Perform final validation in the producer, which is the only task
         * that can safely determine when a run is complete. */
        if (test_total_item_count != 0) {
            if (test_recv_count > test_total_item_count) {
                fail_halt("Queue test: FATAL - recv_count > total_items", test_recv_count, test_total_item_count);
            }

            if ((unsigned int)(test_recv_sum & 0xFFFFu) > (unsigned int)(test_sent_sum & 0xFFFFu)) {
                fail_halt("Queue test: FATAL - recv_sum_low16 > sent_sum_low16",
                          (unsigned int)(test_recv_sum & 0xFFFFu),
                          (unsigned int)(test_sent_sum & 0xFFFFu));
            }
        }

        if (use_monitor == 0)
        {
                itoa_new(test_sent_count, buf, sizeof(buf));
            puts("\r\nQueue test: producer sent:");
            puts(buf);
                itoa_new(test_recv_count, buf, sizeof(buf));
            puts("Queue test: consumer received:");
            puts(buf);

            if (test_sent_count == test_recv_count)
                puts("Queue test: PASS (count match)");

            if (test_sent_sum == test_recv_sum)
                puts("Queue test: PASS (sum match)");
            else {
                puts("Queue test: FAIL\r\n");
                puts("Details: sent_sum = ");
                itoa_new((unsigned int)(test_sent_sum & 0xFFFF), buf, sizeof(buf));
                puts(buf);
                puts(" recv_sum = ");
                itoa_new((unsigned int)(test_recv_sum & 0xFFFF), buf, sizeof(buf));
                puts(buf);
            }

            /* Record elapsed ticks for this run and print when sums match */
            // test_end_ticks = scheduler_get_ticks();
            // test_time_elapsed_ticks = (unsigned int)(test_end_ticks - test_start_ticks);
            // if (test_sent_sum == test_recv_sum) {
            //     puts("Elapsed ticks:");
            //     itoa_new(test_time_elapsed_ticks, buf);
            //     puts(buf);
            //     puts("\r\n");
            // }

            if (test_sent_count == test_total_item_count
                && test_recv_count == test_total_item_count
                && q_is_empty(&test_q_2))
            {
                sb_pos = 0;
                itoa_new(test_total_item_count, buf, sizeof(buf));
                append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Test total:%s ", buf);

                itoa_new(test_sent_count, buf, sizeof(buf));
                append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Test sent:%s ", buf);
                
                itoa_new(test_recv_count, buf, sizeof(buf));
                append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Test recv:%s ", buf);

                /* Completion percentage (sent/total) */
                {
                    unsigned int pct = 0u;
                    if (test_total_item_count) {
                        pct = (unsigned int)(((unsigned long)test_sent_count * 100UL) / (unsigned long)test_total_item_count);
                    }
                    itoa_new(pct, buf, sizeof(buf));
                    append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Completed:%s%%\r\n", buf);
                }

                append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Test:PASS\r\n");
            }
        }

        for (i = 0; i < 1000u; ++i)
            scheduler_yield();
    }
}

static void queue_test_consumer(void *arg)
{
    unsigned int v;
    static char buf[32];
    (void)arg;

    for (;;) {
        /* Wait for the producer to signal the start of a new run. */
        while (test_producer_done != 0) {
            scheduler_yield();
        }
        /* Now that a new run has begun, signal that we are ready. */
        test_consumer_ready = 1;
        for (;;) {
            /* Try to pop, if empty yield so producer can run */
            while (q_pop(&test_q_2, &v)) {
                /* record received value into a small circular log for diagnostics */
                recv_log[recv_log_pos & 63u] = v;
                recv_log_pos++;

                test_recv_count++;
                test_recv_sum += (unsigned long)v;
                /* Debug: print first few pops and periodic progress */
                // if (test_recv_count <= 5 || (test_recv_count % 500) == 0) {
                //     itoa_new(test_recv_count, buf);
                //     puts("Consumer: popped count:");
                //     puts(buf);
                //     itoa_new(v, buf);
                //     puts("Consumer: popped val:");
                //     puts(buf);
                // }
                /* After each pop, yield to allow producer to run */
                scheduler_yield();
            }
            /* Check if done: producer finished AND queue is empty */
            if (test_producer_done && q_is_empty(&test_q_2)) {
                //puts("Consumer: detected producer done and empty queue");
                break;
            }
            scheduler_yield();
        }

    }
}

static void consumer_task_1(void *arg)
{
    unsigned int v;

    (void)arg;
    for (;;)
    {
        scheduler_sleep(sleep_ms_consumer);                

        if (q_pop(&test_q_1, &v)) {
            consumed_item_1 = v;
        }

        scheduler_yield();
    }
}

static void consumer_task_2(void *arg)
{
    unsigned int v;

    (void)arg;
    for (;;)
    {
        scheduler_sleep(sleep_ms_consumer);                

        if (q_pop(&test_q_1, &v)) {
            consumed_item_2 = v;
        }

        scheduler_yield();
    }
}

/* Idle task used to represent CPU idle time for accounting. */
static void idle_task(void *arg)
{
    (void)arg;
    for (;;) {
        scheduler_yield();
    }
}

// static void consumer_task_1_old(void *arg)
// {
//     unsigned int v;
//     volatile unsigned int *p = (volatile unsigned int *)arg;

//     for (;;)
//     {
//         scheduler_sleep(sleep_ms_consumer);                

//         if (q_pop(&q, &v)) {
//             if (p) {
//                 //printValue(v, "popped...: ");
//                 *p = v;
//                 printValue(consumed_item_1, "popped...: ");                
//             }
//         }

//         scheduler_yield();
//     }
// }

void task_monitor(void *arg)
{
    char num_buffer[8];
    static char summary_buffer[1024];
    char bar[32];
    size_t sb_pos = 0;
    unsigned int fill;
    unsigned int mem_used;
    unsigned long pct32;
    unsigned int cpu_pct;
    unsigned long cpu_active;
    unsigned long cpu_total;
    char cpu_buf[16];
    int ti;
    unsigned int used;
    unsigned int used_arr[SCHED_MAX_TASKS];
    unsigned int total_stack = 0;
    char tnum[4];
    unsigned long active_pool = 0UL;
    unsigned int active_count = 0u;
    unsigned long pct_tenths = 0UL;
    unsigned int int_part = 0u;
    unsigned int dec = 0u;
    char pctbuf[8];
    char decch[2];
    unsigned int bar_pct = 0u;
    static unsigned int toggle_counter = 0;    
    (void)arg;

    puts("main: Monitor task started. Reporting in 5s...\r\n");

    for (;;)
    {
        // if (test_run_count == 0)
        // {
        //     continue;
        // }

        scheduler_sleep(1000);
        toggle_counter++;
        /* Toggle mem_fluctuate_active every 10 monitor cycles */
        if (toggle_counter % 10 == 0) {
            mem_fluctuate_active = !mem_fluctuate_active;
        }
        puts("------------------------\r\n");
        sb_pos = 0;
        append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "--- Task Summary ---\r\n");

        itoa_new(task_a_counter, num_buffer, sizeof(num_buffer));
        append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "A:%s ", num_buffer);

        itoa_new(task_b_counter, num_buffer, sizeof(num_buffer));
        append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "\r\nB:%s ", num_buffer);

        itoa_new(produced_item, num_buffer, sizeof(num_buffer));
        append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "\r\nProduced item:%s ", num_buffer);

        itoa_new(consumed_item_1, num_buffer, sizeof(num_buffer));
        append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "\r\nConsumed item 1:%s ", num_buffer);

        itoa_new(consumed_item_2, num_buffer, sizeof(num_buffer));
        append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "\r\nConsumed item 2:%s \r\n", num_buffer);

        /* Throttle producer/consumer based on queue fill level. */
        fill = q_count(&test_q_1);

        if (fill >= Q_HIGH_WATERMARK) {
            if (sleep_ms_producer + 50u < PROD_SLEEP_MAX)
                sleep_ms_producer += 50u;
            else
                sleep_ms_producer = PROD_SLEEP_MAX;

            if (sleep_ms_consumer > CONS_SLEEP_MIN + 10u)
                sleep_ms_consumer -= 10u;
            else
                sleep_ms_consumer = CONS_SLEEP_MIN;

        } else if (fill <= Q_LOW_WATERMARK) {
            if (sleep_ms_producer > PROD_SLEEP_MIN + 10u)
                sleep_ms_producer -= 10u;
            else
                sleep_ms_producer = PROD_SLEEP_MIN;

            if (sleep_ms_consumer + 50u < CONS_SLEEP_MAX)
                sleep_ms_consumer += 50u;
            else
                sleep_ms_consumer = CONS_SLEEP_MAX;

        } else {
            if (sleep_ms_producer > PROD_SLEEP_BASE) sleep_ms_producer--;
            else if (sleep_ms_producer < PROD_SLEEP_BASE) sleep_ms_producer++;

            if (sleep_ms_consumer > CONS_SLEEP_BASE) sleep_ms_consumer--;
            else if (sleep_ms_consumer < CONS_SLEEP_BASE) sleep_ms_consumer++;
        }

        itoa_new(sleep_ms_producer, num_buffer, sizeof(num_buffer));
        itoa_new(sleep_ms_consumer, pctbuf, sizeof(pctbuf));
        append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Prod sleep_ms:%s Cons sleep_ms:%s \r\n", num_buffer, pctbuf);

        /* Memory usage indicator (approximate): scheduler + queue footprint + simulated fluctuation */
        mem_used = (unsigned int)(sizeof(test_q_1) + scheduler_memory_usage());
        mem_used += (unsigned int)(sizeof(test_q_2));        

        if (mem_fluctuate_active) {
            mem_used += sizeof(mem_fluctuate_buffer);
        } else {
            if (mem_used > sizeof(mem_fluctuate_buffer)) {
                mem_used -= sizeof(mem_fluctuate_buffer);
            }
        }
        pct32 = ((unsigned long)mem_used * 100UL) / ((unsigned long)(RAM_TOTAL_BYTES ? RAM_TOTAL_BYTES : 1u));
        itoa_new(mem_used, num_buffer, sizeof(num_buffer));
        itoa_new((unsigned int)pct32, pctbuf, sizeof(pctbuf));
        make_bar((unsigned int)pct32, bar, 20);
        append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Mem used:%s (%s%% ) %s\r\n", num_buffer, pctbuf, bar);

        /* CPU usage metric: percent and raw tick counters */
        cpu_pct = scheduler_cpu_usage_percent();
        cpu_active = scheduler_cpu_active_ticks();
        cpu_total = scheduler_cpu_total_ticks();
        // itoa_new(cpu_pct, cpu_buf);
        // strcat(summary_buffer, "CPU usage:");
        // strcat(summary_buffer, cpu_buf);
        // strcat(summary_buffer, "% ");
        // make_bar(cpu_pct, bar, 20);
        // strcat(summary_buffer, bar);
        // strcat(summary_buffer, "\r\n");
        /* Show raw tick counters for diagnostics */
        itoa_new((unsigned int)(cpu_active & 0xFFFF), cpu_buf, sizeof(cpu_buf)); /* low 16 bits */
        itoa_new((unsigned int)(cpu_total & 0xFFFF), pctbuf, sizeof(pctbuf)); /* low 16 bits */
        append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "CPU active ticks:%s CPU total ticks:%s\r\n", cpu_buf, pctbuf);

        /* Runtime stack usage (per-task + total) */
        {
            int ti;
            unsigned int used;
            unsigned int used_arr[SCHED_MAX_TASKS];
            unsigned int total_stack = 0;
            char tnum[4];
            unsigned long active_pool = 0UL;
            unsigned int active_count = 0u;
            unsigned long pct_tenths = 0UL;
            unsigned int int_part = 0u;
            unsigned int dec = 0u;
            char pctbuf[8];
            char decch[2];
            unsigned int bar_pct = 0u;

            /* Gather per-task used values and compute total_stack */
            for (ti = 0; ti < SCHED_MAX_TASKS; ++ti) {
                used = scheduler_task_stack_used(ti);
                used_arr[ti] = used;
                total_stack += used;
            }

            // itoa_new(total_stack, num_buffer);
            // strcat(summary_buffer, "Stack used:");
            // strcat(summary_buffer, num_buffer);
            // strcat(summary_buffer, " ");

            /* Compute percent relative to active tasks only (sum of stacks for tasks that are in use)
               and display with one decimal place (e.g. 2.4%). */
            for (ti = 0; ti < SCHED_MAX_TASKS; ++ti) {
                if (used_arr[ti] > 0) {
                    active_pool += (unsigned long)SCHED_TASK_STACK_SIZE;
                    active_count++;
                }
            }

            /* If no active tasks, fall back to full pool to avoid division by zero */
            if (active_count == 0) {
                active_pool = (unsigned long)SCHED_TASK_STACK_SIZE * (unsigned long)SCHED_MAX_TASKS;
            }

            /* pct_tenths = percent * 10 (one decimal), computed in 32-bit */
            pct_tenths = active_pool ? ((unsigned long)total_stack * 1000UL) / active_pool : 0UL;
            int_part = (unsigned int)(pct_tenths / 10UL);
            dec = (unsigned int)(pct_tenths % 10UL);

            /* format X.Y% */
            // itoa_new(int_part, pctbuf);
            // strcat(summary_buffer, "(");
            // strcat(summary_buffer, pctbuf);
            // strcat(summary_buffer, ".");
            // decch[0] = '0' + (char)dec; decch[1] = 0;
            // strcat(summary_buffer, decch);
            // strcat(summary_buffer, "%) ");

            /* bar uses integer percent (rounded) */
            // bar_pct = (unsigned int)((pct_tenths + 5UL) / 10UL);
            // if (bar_pct > 100U) bar_pct = 100U;
            // make_bar(bar_pct, bar, 20);
            // strcat(summary_buffer, bar);
            // strcat(summary_buffer, "\r\n");

            /* Per-task brief: show used bytes for each active task + high-water mark */
            for (ti = 0; ti < SCHED_MAX_TASKS; ++ti) {
                used = used_arr[ti];
                if (used == 0) continue;
                itoa_new(used, num_buffer, sizeof(num_buffer));
                itoa_new(scheduler_task_max_used(ti), pctbuf, sizeof(pctbuf));
                append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "T%d:%s(%s) ", ti, num_buffer, pctbuf);
            }
            append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "\r\n");
        }

    /* Append queue metrics into the summary and print everything as one block */
    {
        unsigned int qc = q_count(&test_q_1);
        unsigned int qf = q_space_free(&test_q_1);
        itoa_new(qc, num_buffer, sizeof(num_buffer));
        itoa_new(qf, pctbuf, sizeof(pctbuf));
        append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Q count:%s\r\nQ free:%s\r\n", num_buffer, pctbuf);

        /* Also show test_q (self-test) counts */
        itoa_new(q_count(&test_q_2), num_buffer, sizeof(num_buffer));
        itoa_new(q_space_free(&test_q_2), pctbuf, sizeof(pctbuf));
        append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "TestQ count:%s TestQ free:%s\r\n", num_buffer, pctbuf);
    }

    /* Append queue-test results (producer/consumer self-test) */
    {
        unsigned int elapsed = test_time_elapsed_ticks;
        unsigned int completion_pct = 0u;
        unsigned int throughput = 0u;
        
        /* Show test summary only when test is complete */
        /* Also always append a short completed-percentage progress field when a test is active */
        if (test_total_item_count) {
            unsigned int prod_q_items_count;
            unsigned int cons_q_received_count;
            unsigned int short_pct;
            /* Show items remaining on producer and consumer queues */
            prod_q_items_count = q_count(&test_q_2);
            cons_q_received_count = test_recv_count;
            itoa_new(prod_q_items_count, num_buffer, sizeof(num_buffer));
            itoa_new(cons_q_received_count, pctbuf, sizeof(pctbuf));
            append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Producer Q items remaining:%s Consumer items received:%s\r\n", num_buffer, pctbuf);
            itoa_new(test_total_item_count, num_buffer, sizeof(num_buffer));
            append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Total items:%s\r\n", num_buffer);
            itoa_new(cons_q_received_count, num_buffer, sizeof(num_buffer));
            append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Received count:%s\r\n", num_buffer);

            short_pct = (unsigned int)(((unsigned long)cons_q_received_count * 100UL) / (unsigned long)test_total_item_count);
            itoa_new(short_pct, num_buffer, sizeof(num_buffer));
            append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Completed:%s%%\r\n", num_buffer);
            itoa_new(test_run_count, num_buffer, sizeof(num_buffer));
            append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Test run:%s\r\n", num_buffer);
        }

        if (test_sent_count == test_total_item_count
            && test_recv_count == test_total_item_count
            && q_is_empty(&test_q_2))
        {
            itoa_new(test_total_item_count, num_buffer, sizeof(num_buffer));
            itoa_new(test_sent_count, pctbuf, sizeof(pctbuf));
            itoa_new(test_recv_count, cpu_buf, sizeof(cpu_buf));
            append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Test total:%s Test sent:%s Test recv:%s ", num_buffer, pctbuf, cpu_buf);

            /* Completion percentage (sent/total) */
            {
                unsigned int pct = 0u;
                if (test_total_item_count) {
                    pct = (unsigned int)(((unsigned long)test_sent_count * 100UL) / (unsigned long)test_total_item_count);
                }
                itoa_new(pct, num_buffer, sizeof(num_buffer));
                append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Completed:%s%%\r\n", num_buffer);
            }

            /* Elapsed time in ticks and estimated throughput (items per 100 ticks) */
            itoa_new(elapsed, num_buffer, sizeof(num_buffer));
            append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Ticks:%s ", num_buffer);
            
            if (elapsed > 0) {
                throughput = (test_recv_count * 100u) / elapsed;
            }
            itoa_new(throughput, num_buffer, sizeof(num_buffer));
            append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Items/100tk:%s ", num_buffer);

            append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Test:PASS\r\n");
        }
        else
        {
            append_fmt(summary_buffer, sizeof(summary_buffer), &sb_pos, "Test:RUNNING\r\n");
        }
    }

    /* print assembled summary (includes memory/stack and queue lines appended above) */
    puts(summary_buffer);
  //      puts("\r\n");                        
    }
}

/* old monitor kept for reference removed to avoid duplicate code and warnings */

void main()
{
    unsigned int i;

    test_run_count = 0;
    scheduler_init();
    /* Create and register an idle task so CPU accounting can exclude idle yields */
    // {
    //     int idle_id = scheduler_add(idle_task, NULL);
    //     scheduler_set_idle_task(idle_id);
    // }
    
    /* Seed RNG with tick count so different runs have different sequences */
    seed_random(scheduler_get_ticks());
    
    /* Warm up the RNG with a few iterations to diverge from fixed seed */
    for (i = 0; i < 10u; ++i) {
        pseudo_random(0u, 1u);
    }

    /* Ensure test queues are initialized regardless of monitor mode */
    q_init(&test_q_1);
    q_init(&test_q_2);

    if (use_monitor == 1)
    {
        scheduler_add(task_a, NULL);
        scheduler_add(task_b, NULL);
        scheduler_add(queue_test_producer, NULL);
        scheduler_add(queue_test_consumer, NULL);
        scheduler_add(queue_test_consumer, NULL);        

        scheduler_add(producer_task, NULL);
        scheduler_add(consumer_task_1, NULL);
        scheduler_add(consumer_task_2, NULL);        
        scheduler_add(task_monitor, NULL);        
        scheduler_add(deep_stack_test, NULL);
    }
    else
    {
        scheduler_add(queue_test_producer, NULL);
        scheduler_add(queue_test_consumer, NULL);
//        scheduler_add(queue_test_consumer, NULL);        
    }

//    scheduler_add_once(task_once, NULL);
//    scheduler_add(mem_fluctuate_task, NULL);
    scheduler_run();
}
