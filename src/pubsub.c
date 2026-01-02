/* pubsub.c - Publish/Subscribe system implementation
 * 
 * Simple, efficient pub/sub for embedded systems with multiple topics support.
 */

#include "pubsub.h"
#include <string.h>
#include <stdlib.h>

/* Simple spinlock implementation */
static void _lock(volatile unsigned int *lock)
{
    while (*lock) {
        /* Busy wait */
    }
    *lock = 1;
}

static void _unlock(volatile unsigned int *lock)
{
    *lock = 0;
}

/* Initialize the pub/sub manager */
void pubsub_init(PubSubManager *mgr)
{
    unsigned int i, j;
    
    if (!mgr) return;
    
    mgr->topic_count = 0;
    mgr->subscriber_count = 0;
    mgr->lock = 0;
    
    /* Initialize topics */
    for (i = 0; i < PUBSUB_MAX_TOPICS; i++) {
        mgr->topics[i].name[0] = '\0';
        mgr->topics[i].queue_head = 0;
        mgr->topics[i].queue_tail = 0;
        mgr->topics[i].lock = 0;
    }
    
    /* Initialize subscribers */
    for (i = 0; i < PUBSUB_MAX_SUBSCRIBERS; i++) {
        mgr->subscribers[i].topic_name[0] = '\0';
        mgr->subscribers[i].callback = NULL;
        mgr->subscribers[i].user_data = NULL;
        mgr->subscribers[i].active = false;
    }
}

/* Create a new topic */
int pubsub_create_topic(PubSubManager *mgr, const char *topic_name)
{
    unsigned int i;
    
    if (!mgr || !topic_name || mgr->topic_count >= PUBSUB_MAX_TOPICS)
        return -1;
    
    /* Check if topic already exists */
    for (i = 0; i < mgr->topic_count; i++) {
        if (strcmp(mgr->topics[i].name, topic_name) == 0)
            return (int)i;  /* Topic already exists */
    }
    
    /* Add new topic */
    _lock(&mgr->lock);
    
    if (mgr->topic_count >= PUBSUB_MAX_TOPICS) {
        _unlock(&mgr->lock);
        return -1;
    }
    
    i = mgr->topic_count;
    strncpy(mgr->topics[i].name, topic_name, PUBSUB_MAX_TOPIC_NAME - 1);
    mgr->topics[i].name[PUBSUB_MAX_TOPIC_NAME - 1] = '\0';
    mgr->topics[i].queue_head = 0;
    mgr->topics[i].queue_tail = 0;
    mgr->topics[i].lock = 0;
    
    mgr->topic_count++;
    
    _unlock(&mgr->lock);
    return (int)i;
}

/* Get topic by name */
PubSubTopic* pubsub_get_topic(PubSubManager *mgr, const char *topic)
{
    unsigned int i;
    
    if (!mgr || !topic)
        return NULL;
    
    for (i = 0; i < mgr->topic_count; i++) {
        if (strcmp(mgr->topics[i].name, topic) == 0)
            return &mgr->topics[i];
    }
    
    return NULL;
}

/* Publish a message to a topic */
bool pubsub_publish(PubSubManager *mgr, const char *topic, unsigned int message)
{
    PubSubTopic *t;
    unsigned int next_head;
    
    if (!mgr || !topic)
        return false;
    
    t = pubsub_get_topic(mgr, topic);
    if (!t)
        return false;  /* Topic doesn't exist */
    
    _lock(&t->lock);
    
    /* Calculate next head position */
    next_head = (t->queue_head + 1) % PUBSUB_MESSAGE_QUEUE_SIZE;
    
    /* Check if queue is full */
    if (next_head == t->queue_tail) {
        _unlock(&t->lock);
        return false;  /* Queue is full */
    }
    
    /* Add message to queue */
    t->message_queue[t->queue_head] = message;
    t->queue_head = next_head;
    
    _unlock(&t->lock);
    return true;
}

/* Subscribe to a topic with a callback function */
int pubsub_subscribe(PubSubManager *mgr, const char *topic, 
                     pubsub_callback_t callback, void *user_data)
{
    unsigned int i;
    
    if (!mgr || !topic || !callback || mgr->subscriber_count >= PUBSUB_MAX_SUBSCRIBERS)
        return -1;
    
    /* Ensure topic exists, create if it doesn't */
    if (!pubsub_get_topic(mgr, topic)) {
        if (pubsub_create_topic(mgr, topic) < 0)
            return -1;
    }
    
    _lock(&mgr->lock);
    
    /* Find first available subscriber slot */
    for (i = 0; i < PUBSUB_MAX_SUBSCRIBERS; i++) {
        if (!mgr->subscribers[i].active) {
            strncpy(mgr->subscribers[i].topic_name, topic, PUBSUB_MAX_TOPIC_NAME - 1);
            mgr->subscribers[i].topic_name[PUBSUB_MAX_TOPIC_NAME - 1] = '\0';
            mgr->subscribers[i].callback = callback;
            mgr->subscribers[i].user_data = user_data;
            mgr->subscribers[i].active = true;
            
            if (i >= mgr->subscriber_count)
                mgr->subscriber_count = i + 1;
            
            _unlock(&mgr->lock);
            return (int)i;
        }
    }
    
    _unlock(&mgr->lock);
    return -1;  /* No available subscriber slots */
}

/* Unsubscribe from a topic */
bool pubsub_unsubscribe(PubSubManager *mgr, int subscriber_id)
{
    if (!mgr || subscriber_id < 0 || subscriber_id >= PUBSUB_MAX_SUBSCRIBERS)
        return false;
    
    _lock(&mgr->lock);
    
    if (mgr->subscribers[subscriber_id].active) {
        mgr->subscribers[subscriber_id].active = false;
        mgr->subscribers[subscriber_id].callback = NULL;
        mgr->subscribers[subscriber_id].topic_name[0] = '\0';
        
        _unlock(&mgr->lock);
        return true;
    }
    
    _unlock(&mgr->lock);
    return false;
}

/* Process all pending messages for a specific topic */
void pubsub_process_topic(PubSubManager *mgr, const char *topic)
{
    PubSubTopic *t;
    unsigned int i;
    unsigned int message;
    
    if (!mgr || !topic)
        return;
    
    t = pubsub_get_topic(mgr, topic);
    if (!t)
        return;
    
    _lock(&t->lock);
    
    /* Process all messages in the queue */
    while (t->queue_tail != t->queue_head) {
        message = t->message_queue[t->queue_tail];
        t->queue_tail = (t->queue_tail + 1) % PUBSUB_MESSAGE_QUEUE_SIZE;
        
        _unlock(&t->lock);
        
        /* Call all subscribers for this topic */
        for (i = 0; i < PUBSUB_MAX_SUBSCRIBERS; i++) {
            if (mgr->subscribers[i].active && 
                strcmp(mgr->subscribers[i].topic_name, topic) == 0) {
                if (mgr->subscribers[i].callback) {
                    mgr->subscribers[i].callback(topic, message, 
                                                 mgr->subscribers[i].user_data);
                }
            }
        }
        
        _lock(&t->lock);
    }
    
    _unlock(&t->lock);
}

/* Process all pending messages for all topics */
void pubsub_process_all(PubSubManager *mgr)
{
    unsigned int i;
    
    if (!mgr)
        return;
    
    for (i = 0; i < mgr->topic_count; i++) {
        pubsub_process_topic(mgr, mgr->topics[i].name);
    }
}

/* Get the number of queued messages for a topic */
unsigned int pubsub_queue_size(PubSubManager *mgr, const char *topic)
{
    PubSubTopic *t;
    unsigned int size;
    
    if (!mgr || !topic)
        return 0;
    
    t = pubsub_get_topic(mgr, topic);
    if (!t)
        return 0;
    
    _lock(&t->lock);
    
    if (t->queue_head >= t->queue_tail) {
        size = t->queue_head - t->queue_tail;
    } else {
        size = PUBSUB_MESSAGE_QUEUE_SIZE - (t->queue_tail - t->queue_head);
    }
    
    _unlock(&t->lock);
    return size;
}

/* Get number of active subscribers for a topic */
unsigned int pubsub_subscriber_count(PubSubManager *mgr, const char *topic)
{
    unsigned int i, count = 0;
    
    if (!mgr || !topic)
        return 0;
    
    for (i = 0; i < PUBSUB_MAX_SUBSCRIBERS; i++) {
        if (mgr->subscribers[i].active && 
            strcmp(mgr->subscribers[i].topic_name, topic) == 0) {
            count++;
        }
    }
    
    return count;
}

/* Clear all messages in a topic's queue */
void pubsub_clear_queue(PubSubManager *mgr, const char *topic)
{
    PubSubTopic *t;
    
    if (!mgr || !topic)
        return;
    
    t = pubsub_get_topic(mgr, topic);
    if (!t)
        return;
    
    _lock(&t->lock);
    t->queue_head = 0;
    t->queue_tail = 0;
    _unlock(&t->lock);
}

/* Lock the pub/sub manager */
void pubsub_lock(PubSubManager *mgr)
{
    if (mgr)
        _lock(&mgr->lock);
}

/* Unlock the pub/sub manager */
void pubsub_unlock(PubSubManager *mgr)
{
    if (mgr)
        _unlock(&mgr->lock);
}
