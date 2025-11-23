#include "ringq.h"

/* Lock implementation: use C11 atomic_flag when available, otherwise fall
 * back to the previous volatile-spin approach. The fallback is not atomic
 * and should be used only on single-core targets or when higher-level
 * synchronization (e.g. disabling interrupts) is present. */

#ifdef RQ_HAVE_STDATOMIC
void q_lock(RingQ *q)
{
    while (atomic_flag_test_and_set_explicit(&q->lock, memory_order_acquire)) {
        /* spin until unlocked */
    }
}

void q_unlock(RingQ *q)
{
    atomic_flag_clear_explicit(&q->lock, memory_order_release);
}

void q_init(RingQ *q)
{
    q->head = 0;
    q->tail = 0;
    atomic_flag_clear_explicit(&q->lock, memory_order_relaxed);
}
#else
/* Fallback implementation for toolchains without C11 atomics.
 * Use an interrupt-disable critical section on 6502 (cc65) to make the
 * operations atomic with respect to interrupt handlers. On other hosts
 * this falls back to a simple volatile flag (best-effort). */

#if defined(__CC65__)
static inline void irq_disable(void)
{
    __asm__("sei");
}

static inline void irq_enable(void)
{
    __asm__("cli");
}
#else
static inline void irq_disable(void) { /* no-op */ }
static inline void irq_enable(void) { /* no-op */ }
#endif

void q_lock(RingQ *q)
{
    irq_disable();
    q->lock = 1;
}

void q_unlock(RingQ *q)
{
    q->lock = 0;
    irq_enable();
}

void q_init(RingQ *q)
{
    q->head = 0;
    q->tail = 0;
    q->lock = 0;
}
#endif

unsigned int q_is_full(const RingQ *q)
{
    unsigned int next = (unsigned int)((q->head + 1) & (Q_CAP - 1));
    return (next == q->tail);
}

/* Instrumentation hooks (defined in main.c) to help debug corruption. */
extern void ringq_debug_fail(const char *msg, unsigned int a, unsigned int b);
extern volatile unsigned long ringq_total_pushed;
extern volatile unsigned long ringq_total_popped;

unsigned int q_is_empty(const RingQ *q)
{
    return (q->head == q->tail);
}

unsigned int q_push(RingQ *q, unsigned int v)
{
    unsigned int next;
    unsigned int before_count, after_count;

    q_lock(q);
    before_count = q_count(q);
    next = (unsigned int)((q->head + 1) & (Q_CAP - 1));
    if (next == q->tail) {
        q_unlock(q);
        return 0;     /* full */
    }
    q->buf[q->head] = v;
    q->head = next;
    after_count = q_count(q);

    /* Sanity check: count should increase by 1 */
    if (after_count != (unsigned int)((before_count + 1) & (Q_CAP - 1))) {
        ringq_debug_fail("ringq: q_push count mismatch", before_count, after_count);
    }

    /* Instrumentation: increment global pushed counter if present */
    ringq_total_pushed++;

    q_unlock(q);
    return 1;
}

unsigned int q_pop(RingQ *q, unsigned int *out)
{
    unsigned int before_count, after_count;

    q_lock(q);

    before_count = q_count(q);
    if (q->head == q->tail) {
        q_unlock(q);
        return 0; /* empty */
    }
    
    *out = q->buf[q->tail];
    q->tail = (unsigned int)((q->tail + 1) & (Q_CAP - 1));
    after_count = q_count(q);

    /* Sanity check: count should decrease by 1 */
    if (after_count != (unsigned int)((before_count - 1) & (Q_CAP - 1))) {
        ringq_debug_fail("ringq: q_pop count mismatch", before_count, after_count);
    }

    /* Instrumentation: increment global popped counter if present */
    ringq_total_popped++;

    q_unlock(q);
    return 1;
}

unsigned int q_count(const RingQ *q)
{
    unsigned int head = q->head;
    unsigned int tail = q->tail;

    return (unsigned int)((head - tail) & (Q_CAP - 1));
}

unsigned int q_space_free(const RingQ *q)
{
    return (unsigned int)((Q_CAP - 1) - q_count(q));
}
