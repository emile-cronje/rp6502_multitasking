/* scheduler.c - stackful cooperative scheduler using stack-copying context switch
 */
#include "scheduler.h"

/* Task structure layout must match offsets used in ctxswitch.s */
typedef struct {
    scheduler_task_fn fn;   /* 0..1 */
    void *arg;              /* 2..3 */
    unsigned char in_use;   /* 4 */
    unsigned char one_shot; /* 5 */
    unsigned char started;  /* 6 */
    unsigned char saved_sp; /* 7 */
    unsigned char a;        /* 8 */
    unsigned char x;        /* 9 */
    unsigned char y;        /* 10 */
    unsigned char p;        /* 11 */
    unsigned short wake_tick; /* 12..13 */
    unsigned char stack[SCHED_TASK_STACK_SIZE]; /* 14.. */
} task_t;

static task_t tasks[SCHED_MAX_TASKS];
static task_t scheduler_ctx; /* used to hold scheduler/main context */

/* Per-task max observed stack usage (bytes) - tracked at runtime */
static unsigned int task_max_stack[SCHED_MAX_TASKS];

/* Pointers referenced by assembly */
void *ctx_cur = 0;
void *ctx_next = 0;

static int current = -1;
static unsigned short ticks = 0;
static unsigned long cpu_active_ticks = 0;
static unsigned long cpu_total_ticks = 0;
static int idle_task_id = -1; /* task index reserved for idle */

/* Assembly function provided in ctxswitch.s */
extern void ctx_switch(void);

void scheduler_init(void)
{
    int i;
    for (i = 0; i < SCHED_MAX_TASKS; ++i) {
        tasks[i].fn = (scheduler_task_fn)0;
        tasks[i].arg = (void*)0;
        tasks[i].in_use = 0;
        tasks[i].one_shot = 0;
        tasks[i].started = 0;
        tasks[i].saved_sp = 0xFF;
        tasks[i].a = tasks[i].x = tasks[i].y = tasks[i].p = 0;
        tasks[i].wake_tick = 0;
        {
            unsigned int j;
            for (j = 0; j < SCHED_TASK_STACK_SIZE; ++j) tasks[i].stack[j] = 0;
        }
    }
    scheduler_ctx.in_use = 1;
    current = -1;
}

static int find_next_task(int from)
{
    int i, start;
    if (from < 0) start = 0; else start = (from + 1) % SCHED_MAX_TASKS;
    /* First, try to find a non-idle runnable task */
    for (i = 0; i < SCHED_MAX_TASKS; ++i) {
        int idx = (start + i) % SCHED_MAX_TASKS;
        if (!tasks[idx].in_use) continue;
        if (idx == idle_task_id) continue; /* prefer non-idle tasks */
        /* skip sleeping tasks (wake_tick==0 means ready) */
        if (tasks[idx].wake_tick != 0) {
            unsigned short diff = (unsigned short)(ticks - tasks[idx].wake_tick);
            if (diff >= 0x8000u) continue; /* wake time not reached */
        }
        return idx;
    }
    /* If none found, fall back to idle task if it's runnable */
    if (idle_task_id >= 0 && idle_task_id < SCHED_MAX_TASKS && tasks[idle_task_id].in_use) {
        /* check wake_tick */
        if (tasks[idle_task_id].wake_tick == 0 || (unsigned short)(ticks - tasks[idle_task_id].wake_tick) < 0x8000u) {
            return idle_task_id;
        }
    }
    /* Otherwise, try any runnable task as a last resort (including idle) */
    for (i = 0; i < SCHED_MAX_TASKS; ++i) {
        int idx = (start + i) % SCHED_MAX_TASKS;
        if (!tasks[idx].in_use) continue;
        if (tasks[idx].wake_tick != 0) {
            unsigned short diff = (unsigned short)(ticks - tasks[idx].wake_tick);
            if (diff >= 0x8000u) continue;
        }
        return idx;
    }
    return -1;
}

int scheduler_add(scheduler_task_fn fn, void *arg)
{
    int i;
    for (i = 0; i < SCHED_MAX_TASKS; ++i) {
        if (!tasks[i].in_use) {
            tasks[i].fn = fn;
            tasks[i].arg = arg;
            tasks[i].in_use = 1;
            tasks[i].one_shot = 0;
            tasks[i].started = 0;
            tasks[i].saved_sp = 0xFF;
            tasks[i].wake_tick = 0;
            return i;
        }
    }
    return -1;
}

int scheduler_add_once(scheduler_task_fn fn, void *arg)
{
    int id = scheduler_add(fn,arg);
    if (id >= 0) tasks[id].one_shot = 1;
    return id;
}

int scheduler_remove(int id)
{
    if (id < 0 || id >= SCHED_MAX_TASKS) return -1;
    tasks[id].in_use = 0;
    tasks[id].fn = (scheduler_task_fn)0;
    tasks[id].arg = (void*)0;
    tasks[id].started = 0;
    return 0;
}

void scheduler_task_return(void)
{
    /* called when a task function returns: remove it and yield */
    if (current >= 0 && current < SCHED_MAX_TASKS) {
        tasks[current].in_use = 0;
    }
    /* yield to next task (will not return here) */
    scheduler_yield();
}

void scheduler_sleep(unsigned short delta)
{
    if (current < 0 || current >= SCHED_MAX_TASKS) return;
    if (delta == 0) delta = 1;
    tasks[current].wake_tick = (unsigned short)(ticks + delta);
    scheduler_yield();
}

void scheduler_start_task(void)
{
    /* Called from assembly trampoline after stack/registers/SP restored for the task.
     * Read ctx_next to find fn/arg and invoke fn(arg). When fn returns, call scheduler_task_return.
     */
    task_t *t = (task_t*)ctx_next;
    if (!t || !t->fn) return;
    /* Call the task function normally; when it returns, call scheduler_task_return */
    t->fn(t->arg);
    scheduler_task_return();
}

void scheduler_yield(void)
{
    int prev = current;
    int next;
    /* advance tick (one tick per yield/context switch) */
    ++ticks;
    ++cpu_total_ticks;
    next = find_next_task(prev);
    if (next < 0) return; /* nothing to run */

    if (prev < 0) {
        ctx_cur = &scheduler_ctx;
    } else {
        ctx_cur = &tasks[prev];
    }
    ctx_next = &tasks[next];
    current = next;
    /* Count as active tick if switching to a non-idle real task (not idle/scheduler) */
    if (next >= 0 && next < SCHED_MAX_TASKS && tasks[next].in_use && next != idle_task_id) {
        ++cpu_active_ticks;
    }
    /* call assembly context switch; this will switch contexts and return when resumed */
    ctx_switch();
}

/* Set/get idle task id used for CPU accounting */
void scheduler_set_idle_task(int id)
{
    if (id >= 0 && id < SCHED_MAX_TASKS) idle_task_id = id;
    else idle_task_id = -1;
}

int scheduler_get_idle_task(void)
{
    return idle_task_id;
}

/* C helper called from assembly to copy stacks between hardware stack and task buffers.
 * It expects ctx_cur and ctx_next to be set to task_t* pointers.
 */
void scheduler_copy_stacks(void)
{
    task_t *cur = (task_t*)ctx_cur;
    task_t *next = (task_t*)ctx_next;
    unsigned int i;
    /* copy only the used portion of the hardware stack into cur->stack
     * saved_sp holds the X register value at the time of save (0..255).
     * used bytes are from saved_sp+1 .. PAGE_STACK_SIZE-1 (inclusive).
     * For safety cap the copy size to the actual 6502 hardware stack page
     * size (256 bytes) so builds with a larger SCHED_TASK_STACK_SIZE do not
     * read/write beyond $01FF and corrupt memory.
     */
    {
        const unsigned int PAGE_STACK_SIZE = 256u;
        const unsigned int max_copy = (SCHED_TASK_STACK_SIZE < PAGE_STACK_SIZE) ? SCHED_TASK_STACK_SIZE : PAGE_STACK_SIZE;
        unsigned int start = (unsigned int)cur->saved_sp + 1u;
        if (start < max_copy) {
            for (i = start; i < max_copy; ++i) {
                cur->stack[i] = *((unsigned char*)(0x0100 + i));
            }
        }
    }
    /* restore only the used portion of next->stack back into hardware stack */
    {
        const unsigned int PAGE_STACK_SIZE = 256u;
        const unsigned int max_copy = (SCHED_TASK_STACK_SIZE < PAGE_STACK_SIZE) ? SCHED_TASK_STACK_SIZE : PAGE_STACK_SIZE;
        unsigned int start = (unsigned int)next->saved_sp + 1u;
        if (start < max_copy) {
            for (i = start; i < max_copy; ++i) {
                *((unsigned char*)(0x0100 + i)) = next->stack[i];
            }
        }
    }
}

void scheduler_run(void)
{
    int first = find_next_task(-1);
    if (first < 0) return;
    ctx_cur = &scheduler_ctx;
    ctx_next = &tasks[first];
    current = first;
    ctx_switch();
}

unsigned int scheduler_memory_usage(void)
{
    /* Return the size of the tasks array + scheduler_ctx as an approximation */
    return (unsigned int)(sizeof(tasks) + sizeof(scheduler_ctx));
}

unsigned int scheduler_task_stack_used(int id)
{
    unsigned char sp;
    unsigned int used;
    if (id < 0 || id >= SCHED_MAX_TASKS) return 0;
    /* saved_sp holds the SP value at time of save (0..255); 0xFF means unused/empty */
    sp = tasks[id].saved_sp;
    if (sp == (unsigned char)0xFF) return 0;
    if ((unsigned int)sp + 1 >= SCHED_TASK_STACK_SIZE) return 0;
    used = (unsigned int)(SCHED_TASK_STACK_SIZE - ((unsigned int)sp + 1));
    /* update high-water mark */
    if (used > task_max_stack[id]) task_max_stack[id] = used;
    return used;
}

unsigned int scheduler_total_stack_used(void)
{
    unsigned int sum = 0;
    int i;
    for (i = 0; i < SCHED_MAX_TASKS; ++i) {
        sum += scheduler_task_stack_used(i);
    }
    return sum;
}

unsigned int scheduler_task_max_used(int id)
{
    if (id < 0 || id >= SCHED_MAX_TASKS) return 0;
    return task_max_stack[id];
}

unsigned int scheduler_get_ticks(void)
{
    return (unsigned int)ticks;
}

/* Return CPU usage percent (0-100), computed as (active_ticks * 100) / total_ticks. */
unsigned int scheduler_cpu_usage_percent(void)
{
    unsigned long pct;
    if (cpu_total_ticks == 0) return 0;
    pct = (cpu_active_ticks * 100UL) / cpu_total_ticks;
    if (pct > 100UL) pct = 100UL;
    return (unsigned int)pct;
}

/* Optionally expose raw tick counters for diagnostics */
unsigned long scheduler_cpu_active_ticks(void) { return cpu_active_ticks; }
unsigned long scheduler_cpu_total_ticks(void) { return cpu_total_ticks; }
