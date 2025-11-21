/* Set and get CPU frequency in Hz */

/* Set and get CPU frequency in Hz */
void scheduler_set_cpu_frequency(unsigned int hz);
unsigned int scheduler_get_cpu_frequency(void);
/* scheduler.h - stackful cooperative scheduler API
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

/* Config */
#ifndef SCHED_MAX_TASKS
#define SCHED_MAX_TASKS 10
#endif

#ifndef SCHED_TASK_STACK_SIZE
#define SCHED_TASK_STACK_SIZE 256
#endif

/* On the 6502 the hardware stack is fixed to page 1 (addresses $0100..$01FF)
 * which is 256 bytes. The scheduler saves/restores the hardware stack page into
 * per-task buffers. Therefore `SCHED_TASK_STACK_SIZE` must be <= 256 or the
 * scheduler will attempt to read/write beyond the hardware stack page and
 * corrupt memory, leading to hangs. Enforce this at compile time.
 */
#if SCHED_TASK_STACK_SIZE > 256
#error "SCHED_TASK_STACK_SIZE must be <= 256 because the 6502 hardware stack is 256 bytes"
#endif

typedef void (*scheduler_task_fn)(void *);

/* Scheduler API */
void scheduler_init(void);
int scheduler_add(scheduler_task_fn fn, void *arg);
int scheduler_add_once(scheduler_task_fn fn, void *arg);
int scheduler_remove(int id);
void scheduler_run(void);
void scheduler_yield(void);
void scheduler_task_return(void);
void scheduler_start_task(void);
void scheduler_sleep(unsigned short ticks);
unsigned int scheduler_get_ticks(void);

/* Return CPU usage percent (0-100), computed as (active_ticks * 100) / total_ticks. */
unsigned int scheduler_cpu_usage_percent(void);
unsigned long scheduler_cpu_active_ticks(void);
unsigned long scheduler_cpu_total_ticks(void);
/* Optional: mark a task id as the idle task so CPU accounting can exclude it */
void scheduler_set_idle_task(int id);
int scheduler_get_idle_task(void);

/* Return approximate memory used by scheduler data structures (bytes). */
unsigned int scheduler_memory_usage(void);
unsigned int scheduler_task_stack_used(int id);
unsigned int scheduler_total_stack_used(void);
unsigned int scheduler_task_max_used(int id);

/* Assembly context switch entry (implemented in ctxswitch.s) */
extern void ctx_switch(void);

/* Pointers used by assembly (defined in scheduler.c) */
extern void *ctx_cur;
extern void *ctx_next;

#endif /* SCHEDULER_H */
