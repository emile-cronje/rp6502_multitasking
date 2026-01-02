# Multi-Topic Pub/Sub System - Usage Guide

## Overview

A lightweight publish/subscribe system for the rp6502 multitasking project that supports multiple topics with multiple publishers and subscribers. The system uses spinlocks for thread-safe operations and queue-based message delivery.

## Features

- **Multiple Topics**: Create and manage up to 16 different topics
- **Multiple Subscribers**: Support up to 32 concurrent subscribers across all topics
- **Message Queuing**: Each topic maintains a 64-message circular queue
- **Thread-Safe**: Spinlock-based locking for safe concurrent access
- **Lightweight**: Minimal memory footprint suitable for embedded systems
- **Callback-Based**: Subscribers use callback functions to process messages

## Files

- `pubsub.h` - Header file with API declarations
- `pubsub.c` - Implementation of the pub/sub system
- Integration examples in `main.c`

## Core Concepts

### Topics
Topics are named channels for publishing and subscribing. Examples:
- `sensors/temperature` - Temperature sensor readings
- `sensors/pressure` - Pressure sensor readings
- `system/status` - System status updates

### Subscribers
A subscriber is a callback function that gets invoked when messages are published to a topic. Multiple subscribers can listen to the same topic.

### Messages
Messages are 32-bit unsigned integers (`unsigned int`). You can encode application-specific data within these integers.

### Processing
Messages published to a topic are queued and processed when `pubsub_process_topic()` or `pubsub_process_all()` is called, typically in a consumer task.

## API Reference

### Initialization

```c
void pubsub_init(PubSubManager *mgr);
```
Initialize the pub/sub manager. Must be called once at startup.

```c
int pubsub_create_topic(PubSubManager *mgr, const char *topic_name);
```
Create a new topic. Returns topic index on success, -1 on failure.

### Publishing

```c
bool pubsub_publish(PubSubManager *mgr, const char *topic, unsigned int message);
```
Publish a message to a topic. Returns true on success, false if queue is full.

### Subscribing

```c
int pubsub_subscribe(PubSubManager *mgr, const char *topic, 
                     pubsub_callback_t callback, void *user_data);
```
Subscribe to a topic with a callback function. Returns subscriber ID on success, -1 on failure.

```c
typedef void (*pubsub_callback_t)(const char *topic, unsigned int message, void *user_data);
```

```c
bool pubsub_unsubscribe(PubSubManager *mgr, int subscriber_id);
```
Unsubscribe a previously registered subscriber.

### Message Processing

```c
void pubsub_process_all(PubSubManager *mgr);
```
Process all pending messages for all topics.

```c
void pubsub_process_topic(PubSubManager *mgr, const char *topic);
```
Process pending messages for a specific topic.

### Utilities

```c
PubSubTopic* pubsub_get_topic(PubSubManager *mgr, const char *topic);
```
Get a topic by name.

```c
unsigned int pubsub_subscriber_count(PubSubManager *mgr, const char *topic);
```
Get number of active subscribers for a topic.

```c
unsigned int pubsub_queue_size(PubSubManager *mgr, const char *topic);
```
Get number of queued messages for a topic.

```c
void pubsub_clear_queue(PubSubManager *mgr, const char *topic);
```
Clear all messages in a topic's queue.

```c
void pubsub_lock(PubSubManager *mgr);
void pubsub_unlock(PubSubManager *mgr);
```
Manual lock/unlock for multi-step operations.

## Usage Example

### 1. Define Callback Functions

```c
static void on_temperature_update(const char *topic, unsigned int message, void *user_data)
{
    printf("Temperature: %u°C\n", message);
}

static void on_pressure_update(const char *topic, unsigned int message, void *user_data)
{
    printf("Pressure: %u kPa\n", message);
}
```

### 2. Initialize in main()

```c
// Global pub/sub manager
static PubSubManager g_pubsub_mgr;

// In main():
pubsub_init(&g_pubsub_mgr);

// Create topics
pubsub_create_topic(&g_pubsub_mgr, "sensors/temperature");
pubsub_create_topic(&g_pubsub_mgr, "sensors/pressure");

// Subscribe with callbacks
pubsub_subscribe(&g_pubsub_mgr, "sensors/temperature", on_temperature_update, NULL);
pubsub_subscribe(&g_pubsub_mgr, "sensors/pressure", on_pressure_update, NULL);
```

### 3. Create Publisher Task

```c
static void sensor_publisher(void *arg)
{
    (void)arg;
    
    for (;;) {
        unsigned int temp = read_temperature();
        unsigned int pressure = read_pressure();
        
        pubsub_publish(&g_pubsub_mgr, "sensors/temperature", temp);
        pubsub_publish(&g_pubsub_mgr, "sensors/pressure", pressure);
        
        scheduler_sleep(1000);  // Publish every 1 second
    }
}
```

### 4. Create Consumer Task

```c
static void sensor_monitor(void *arg)
{
    (void)arg;
    
    for (;;) {
        // Process all messages from all topics
        pubsub_process_all(&g_pubsub_mgr);
        
        // Or process a specific topic:
        // pubsub_process_topic(&g_pubsub_mgr, "sensors/temperature");
        
        scheduler_sleep(500);  // Check every 500ms
    }
}
```

### 5. Add Tasks to Scheduler

```c
scheduler_add(sensor_publisher, NULL);
scheduler_add(sensor_monitor, NULL);
scheduler_run();
```

## Advanced Usage

### Custom User Data

Pass context-specific data to subscribers:

```c
typedef struct {
    char sensor_name[32];
    unsigned int alarm_threshold;
} SensorContext;

static void on_sensor_alert(const char *topic, unsigned int message, void *user_data)
{
    SensorContext *ctx = (SensorContext *)user_data;
    
    if (message > ctx->alarm_threshold) {
        printf("ALERT: %s exceeded threshold!\n", ctx->sensor_name);
    }
}

// Subscribe with context
SensorContext temp_ctx = {"Temperature", 40};
pubsub_subscribe(&g_pubsub_mgr, "sensors/temperature", on_sensor_alert, &temp_ctx);
```

### Message Encoding

Encode multiple values in a single 32-bit message:

```c
// Encode: 16-bit temperature + 16-bit humidity
unsigned int message = ((temp & 0xFFFF) << 16) | (humidity & 0xFFFF);
pubsub_publish(&g_pubsub_mgr, "env/combined", message);

// Decode in subscriber:
static void on_env_update(const char *topic, unsigned int message, void *user_data)
{
    unsigned int temp = (message >> 16) & 0xFFFF;
    unsigned int humidity = message & 0xFFFF;
    printf("Temp: %u, Humidity: %u\n", temp, humidity);
}
```

### Conditional Processing

```c
// Process only specific topics
for (;;) {
    if (pubsub_queue_size(&g_pubsub_mgr, "sensors/temperature") > 0) {
        pubsub_process_topic(&g_pubsub_mgr, "sensors/temperature");
    }
    
    if (pubsub_queue_size(&g_pubsub_mgr, "system/status") > 0) {
        pubsub_process_topic(&g_pubsub_mgr, "system/status");
    }
    
    scheduler_sleep(100);
}
```

## Configuration

Edit `pubsub.h` to adjust limits:

```c
#define PUBSUB_MAX_TOPICS 16              // Maximum number of topics
#define PUBSUB_MAX_SUBSCRIBERS 32         // Maximum total subscribers
#define PUBSUB_MAX_TOPIC_NAME 32          // Max topic name length
#define PUBSUB_MESSAGE_QUEUE_SIZE 64      // Messages per topic queue
```

## Performance Considerations

1. **Message Queue**: Each topic maintains a 64-message queue. If publishing faster than consuming, the queue will fill and new messages will be dropped.

2. **Callback Processing**: Callbacks are executed sequentially. Long-running callbacks will delay processing of other topics.

3. **Spinlocks**: The system uses spinlocks, which are suitable for cooperative multitasking but will busy-wait on contention.

4. **Memory**: Total memory usage is roughly:
   - Topics: ~1KB per topic
   - Subscribers: ~64 bytes per subscriber
   - Queues: ~256 bytes per topic (64 x 4-byte messages)

## Example from main.c

The project includes a complete working example:

- `pubsub_sensor_publisher()` - Publishes temperature and pressure
- `pubsub_sensor_monitor()` - Subscribes to sensor topics and displays updates
- Callbacks: `on_temperature_update()`, `on_pressure_update()`, `on_system_status()`

Run with: Set `use_monitor = 1` in main() to enable the pub/sub demo.

## Troubleshooting

### Messages Not Received

1. Ensure topic was created before subscribing
2. Check that `pubsub_process_all()` or `pubsub_process_topic()` is called regularly
3. Verify the subscriber callback is registered

### Queue Full Warnings

If you see "Failed to publish" messages:
1. Increase `PUBSUB_MESSAGE_QUEUE_SIZE` in pubsub.h
2. Call `pubsub_process_all()` more frequently
3. Check for slow subscribers blocking message processing

### Memory Issues

If you're running low on memory:
1. Reduce `PUBSUB_MAX_TOPICS` or `PUBSUB_MAX_SUBSCRIBERS`
2. Reduce `PUBSUB_MESSAGE_QUEUE_SIZE`
3. Use shorter topic names

## Thread Safety

The pub/sub system is designed for cooperative multitasking with spinlocks. It's safe to:
- Publish from one task while consuming from another
- Have multiple subscribers for the same topic
- Have the same task publish to multiple topics

However, unsubscribing while messages are being processed may cause issues. For production use, consider adding reference counting to subscribers.
