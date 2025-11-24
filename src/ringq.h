#ifndef RINGQ_H
#define RINGQ_H

#include <stdint.h>

/* Prefer C11 atomics when available; fall back to a volatile spin variable
 * for toolchains that don't provide <stdatomic.h> (e.g. cc65). Use the
 * preprocessor to detect availability when possible. */
#if defined(__has_include)
#  if __has_include(<stdatomic.h>)
#    include <stdatomic.h>
#    define RQ_HAVE_STDATOMIC 1
#  endif
#endif

#ifndef RQ_HAVE_STDATOMIC
#  if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112) && !defined(__STDC_NO_ATOMICS__)
#    include <stdatomic.h>
#    define RQ_HAVE_STDATOMIC 1
#  endif
#endif

/* Power-of-two capacity for cheap masking; keep in sync with main if changed. */
#define Q_CAP 2048

typedef struct {
    unsigned int buf[Q_CAP];
    volatile unsigned int head; /* next write */
    volatile unsigned int tail; /* next read  */
#ifdef RQ_HAVE_STDATOMIC
    atomic_flag lock; /* atomic spinlock */
#else
    /* Fallback for non-C11 toolchains: simple volatile flag. This is not
     * atomic; it relies on single-core / interrupt-disabled usage or higher
     * level synchronization. */
    volatile unsigned int lock; /* 0 = unlocked, 1 = locked */
#endif
    /* Lightweight debug invariants: running checksum of contents and
     * last pushed sequence value (useful when producer writes monotonic
     * sequence numbers). Kept small to reduce memory impact. */
    unsigned long debug_sum;
    unsigned int debug_last_seq;
    /* Per-slot guard pattern to detect overwrites/zeroing of buffer slots. */
    unsigned int guard[Q_CAP];
} RingQ;

void q_init(RingQ *q);
unsigned int q_is_full(const RingQ *q);
unsigned int q_is_empty(const RingQ *q);
unsigned int q_push(RingQ *q, unsigned int v);
unsigned int q_pop(RingQ *q, unsigned int *out);
unsigned int q_count(const RingQ *q);
unsigned int q_space_free(const RingQ *q);
void q_lock(RingQ *q);
void q_unlock(RingQ *q);

#endif /* RINGQ_H */
