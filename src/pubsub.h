/* pubsub.h - Publish/Subscribe system for multiple topics
 * 
 * Provides a simple, memory-efficient pub/sub system for embedded systems.
 * Supports multiple topics with multiple subscribers and publishers.
 */

#ifndef PUBSUB_H
#define PUBSUB_H

#include <stdint.h>
#include <stdbool.h>

/* Configuration */
#define PUBSUB_MAX_TOPICS 16
#define PUBSUB_MAX_SUBSCRIBERS 32
#define PUBSUB_MAX_TOPIC_NAME 32
#define PUBSUB_MESSAGE_QUEUE_SIZE 64

/* Subscriber callback function type */
typedef void (*pubsub_callback_t)(const char *topic, unsigned int message, void *user_data);

/* Topic structure */
typedef struct {
    char name[PUBSUB_MAX_TOPIC_NAME];
    unsigned int message_queue[PUBSUB_MESSAGE_QUEUE_SIZE];
    unsigned int queue_head;
    unsigned int queue_tail;
    volatile unsigned int lock;
} PubSubTopic;

/* Subscriber structure */
typedef struct {
    char topic_name[PUBSUB_MAX_TOPIC_NAME];
    pubsub_callback_t callback;
    void *user_data;
    bool active;
} PubSubSubscriber;

/* Pub/Sub manager structure */
typedef struct {
    PubSubTopic topics[PUBSUB_MAX_TOPICS];
    unsigned int topic_count;
    
    PubSubSubscriber subscribers[PUBSUB_MAX_SUBSCRIBERS];
    unsigned int subscriber_count;
    
    volatile unsigned int lock;
} PubSubManager;

/* Initialize the pub/sub system */
void pubsub_init(PubSubManager *mgr);

/* Create a new topic */
int pubsub_create_topic(PubSubManager *mgr, const char *topic_name);

/* Publish a message to a topic */
bool pubsub_publish(PubSubManager *mgr, const char *topic, unsigned int message);

/* Subscribe to a topic with a callback function */
int pubsub_subscribe(PubSubManager *mgr, const char *topic, 
                     pubsub_callback_t callback, void *user_data);

/* Unsubscribe from a topic */
bool pubsub_unsubscribe(PubSubManager *mgr, int subscriber_id);

/* Process all pending messages for all topics */
void pubsub_process_all(PubSubManager *mgr);

/* Process pending messages for a specific topic */
void pubsub_process_topic(PubSubManager *mgr, const char *topic);

/* Get topic by name */
PubSubTopic* pubsub_get_topic(PubSubManager *mgr, const char *topic);

/* Get number of active subscribers for a topic */
unsigned int pubsub_subscriber_count(PubSubManager *mgr, const char *topic);

/* Get the number of queued messages for a topic */
unsigned int pubsub_queue_size(PubSubManager *mgr, const char *topic);

/* Clear all messages in a topic's queue */
void pubsub_clear_queue(PubSubManager *mgr, const char *topic);

/* Lock/unlock for thread-safe operations */
void pubsub_lock(PubSubManager *mgr);
void pubsub_unlock(PubSubManager *mgr);

#endif /* PUBSUB_H */
