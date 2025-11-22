#ifndef RINGQ_H
#define RINGQ_H

#include <stdint.h>

/* Power-of-two capacity for cheap masking; keep in sync with main if changed. */
#define Q_CAP 2048u

typedef struct {
    unsigned int buf[Q_CAP];
    volatile unsigned int head; /* next write */
    volatile unsigned int tail; /* next read  */
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
