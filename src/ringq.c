#include "ringq.h"

// void q_lock(RingQ *q)
// {
//     while (q->lock) {
//         /* spin until unlocked */
//     }
//     q->lock = 1;
// }

// void q_unlock(RingQ *q)
// {
//     q->lock = 0;
// }

void q_init(RingQ *q)
{
    q->head = 0;
    q->tail = 0;
//    q->lock = 0; --- IGNORE ---}
}

unsigned int q_is_full(const RingQ *q)
{
    unsigned int next = (unsigned int)((q->head + 1) & (Q_CAP - 1));
    return (next == q->tail);
}

unsigned int q_is_empty(const RingQ *q)
{
    return (q->head == q->tail);
}

unsigned int q_push(RingQ *q, unsigned int v)
{
    unsigned int next;
//    q_lock(q);
    next = (unsigned int)((q->head + 1) & (Q_CAP - 1));
    if (next == q->tail) {
  //      q_unlock(q);
        return 0;     /* full */
    }
    q->buf[q->head] = v;
    q->head = next;
    //q_unlock(q);
    return 1;
}

unsigned int q_pop(RingQ *q, unsigned int *out)
{
//    q_lock(q);
    if (q->head == q->tail) {
//        q_unlock(q);
        return 0; /* empty */
    }
    *out = q->buf[q->tail];
    q->tail = (unsigned int)((q->tail + 1) & (Q_CAP - 1));
    //q_unlock(q);
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
