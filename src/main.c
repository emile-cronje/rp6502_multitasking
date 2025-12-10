#include "scheduler.h"
#include <stddef.h> /* For NULL */
#include <stdlib.h> /* For malloc and free */
#include <rp6502.h>
#include <stdbool.h>
#include <fcntl.h>

const unsigned int BATCH_SIZE = 500;    
const unsigned int MAX_ITEM_COUNT = 2000;    
const unsigned int use_monitor = 1u;

/* TCP UART Configuration */
#define WIFI_SSID "Cudy24G"
#define WIFI_PASSWORD "ZAnne19991214"
#define SERVER_IP "192.168.10.250"
#define SERVER_PORT "8080"
#define TCP_TEST_MSG_COUNT 1
#define TCP_TEST_MSG_LENGTH 1
#define TCP_BATCH_SIZE 10
#define RESPONSE_BUFFER_SIZE 256
#define COMMAND_TIMEOUT 10000
#define RESPONSE_QUEUE_SIZE 10
#define SENT_MESSAGE_TRACKING_SIZE 50    
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

/* TCP UART Data Structures */
typedef struct {
    int id;
    char category[16];
    char base64_message[512];
    char base64_hash[64];
    bool valid;
} ResponseMessage;

typedef struct {
    ResponseMessage messages[RESPONSE_QUEUE_SIZE];
    int write_idx;
    int read_idx;
    int count;
} ResponseQueue;

typedef struct {
    int id;
    bool sent;
    bool response_received;
    char timestamp[32];
} SentMessageTracker;

typedef struct {
    SentMessageTracker messages[TCP_TEST_MSG_COUNT];
    int count;
} MessageTracker;

/* TCP UART Global State */
static ResponseQueue g_tcp_response_queue;
static MessageTracker g_tcp_sent_tracker;
static int g_tcp_fd = -1;
static int g_tcp_msgId = 1;
static volatile bool g_tcp_initialized = false;
static volatile unsigned int g_tcp_messages_sent = 0;
static volatile unsigned int g_tcp_responses_received = 0;

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

/* ========== TCP UART Helper Functions ========== */

/* Simple string search function */
static char* my_strstr(const char *haystack, const char *needle)
{
    const char *h, *n;
    
    if (!*needle)
        return (char*)haystack;
    
    while (*haystack)
    {
        h = haystack;
        n = needle;
        
        while (*h && *n && (*h == *n))
        {
            h++;
            n++;
        }
        
        if (!*n)
            return (char*)haystack;
        
        haystack++;
    }
    
    return NULL;
}

/* Simple sprintf for specific format strings */
static void my_sprintf(char *dest, const char *fmt, const char *s1, const char *s2)
{
    while (*fmt)
    {
        if (*fmt == '%' && *(fmt + 1) == 's')
        {
            const char *src = s1;
            while (*src)
                *dest++ = *src++;
            s1 = s2;
            fmt += 2;
        }
        else
        {
            *dest++ = *fmt++;
        }
    }
    *dest = '\0';
}

/* Simple print function */
static void tcp_print(char *s)
{
    while (*s)
        if (RIA.ready & RIA_READY_TX_BIT)
            RIA.tx = *s++;
}

/* Simple delay function */
static void delay_ms(int ms)
{
    int i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 100; j++)
            ;
}

/* Simple number parser */
static int parse_number(char *str)
{
    int result = 0;
    while (*str >= '0' && *str <= '9')
    {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result;
}

/* Send string to modem */
static void send_to_modem(int fd, char *cmd)
{
    char *p = cmd;
    while (*p)
    {
        ria_push_char(*p++);
        ria_set_ax(fd);
        while (!ria_call_int(RIA_OP_WRITE_XSTACK))
            ;
    }
    ria_push_char('\r');
    ria_set_ax(fd);
    while (!ria_call_int(RIA_OP_WRITE_XSTACK))
        ;
    ria_push_char('\n');
    ria_set_ax(fd);
    while (!ria_call_int(RIA_OP_WRITE_XSTACK))
        ;
}

/* Send raw data without line terminators */
static void send_raw_data(int fd, char *data, int length)
{
    int i;
    for (i = 0; i < length; i++)
    {
        ria_push_char(data[i]);
        ria_set_ax(fd);
        while (!ria_call_int(RIA_OP_WRITE_XSTACK))
            ;
    }
}

/* Response queue operations */
static void tcp_queue_init(ResponseQueue *q)
{
    q->write_idx = 0;
    q->read_idx = 0;
    q->count = 0;
}

static bool tcp_queue_put(ResponseQueue *q, ResponseMessage *msg)
{
    if (q->count >= RESPONSE_QUEUE_SIZE)
        return false;
    
    q->messages[q->write_idx] = *msg;
    q->write_idx = (q->write_idx + 1) % RESPONSE_QUEUE_SIZE;
    q->count++;
    return true;
}

static bool tcp_queue_get(ResponseQueue *q, ResponseMessage *msg)
{
    if (q->count == 0)
        return false;
    
    *msg = q->messages[q->read_idx];
    q->read_idx = (q->read_idx + 1) % RESPONSE_QUEUE_SIZE;
    q->count--;
    return true;
}

/* Message tracker operations */
static void tcp_tracker_init(MessageTracker *t)
{
    int i;
    t->count = 0;
    for (i = 0; i < TCP_TEST_MSG_COUNT; i++)
    {
        t->messages[i].id = -1;
        t->messages[i].sent = false;
        t->messages[i].response_received = false;
    }
}

static void tcp_tracker_mark_sent(MessageTracker *t, int msg_id)
{
    if (t->count < TCP_TEST_MSG_COUNT)
    {
        t->messages[t->count].id = msg_id;
        t->messages[t->count].sent = true;
        t->messages[t->count].response_received = false;
        t->count++;
    }
}

static bool tcp_tracker_mark_response(MessageTracker *t, int msg_id)
{
    int i;
    for (i = 0; i < t->count; i++)
    {
        if (t->messages[i].id == msg_id)
        {
            t->messages[i].response_received = true;
            return true;
        }
    }
    return false;
}

static int tcp_tracker_get_missing_count(MessageTracker *t)
{
    int i;
    int missing = 0;
    for (i = 0; i < t->count; i++)
    {
        if (t->messages[i].sent && !t->messages[i].response_received)
            missing++;
    }
    return missing;
}

/* Parse JSON response */
static bool parse_json_response(char *json, ResponseMessage *msg)
{
    char *p;
    char *start;
    int len;
    
    msg->valid = false;
    msg->id = -1;
    msg->category[0] = '\0';
    msg->base64_message[0] = '\0';
    msg->base64_hash[0] = '\0';
    
    p = my_strstr(json, "\"Id\":");
    if (p)
    {
        p += 5;
        while (*p == ' ' || *p == '\t') p++;
        msg->id = parse_number(p);
    }
    
    p = my_strstr(json, "\"Category\":\"");
    if (p)
    {
        p += 12;
        len = 0;
        while (*p && *p != '"' && len < 15)
        {
            msg->category[len++] = *p++;
        }
        msg->category[len] = '\0';
    }
    
    p = my_strstr(json, "\"Base64Message\":\"");
    if (p)
    {
        p += 17;
        len = 0;
        while (*p && *p != '"' && len < 511)
        {
            msg->base64_message[len++] = *p++;
        }
        msg->base64_message[len] = '\0';
    }
    
    p = my_strstr(json, "\"Base64MessageHash\":\"");
    if (p)
    {
        p += 21;
        len = 0;
        while (*p && *p != '"' && len < 63)
        {
            msg->base64_hash[len++] = *p++;
        }
        msg->base64_hash[len] = '\0';
    }
    
    msg->valid = (msg->id >= 0);
    return msg->valid;
}

/* Base64 encoding */
static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const char *input, int input_len, char *output)
{
    int i;
    unsigned char a, b, c;
    int out_idx = 0;
    
    for (i = 0; i < input_len; i += 3)
    {
        a = input[i];
        b = (i + 1 < input_len) ? input[i + 1] : 0;
        c = (i + 2 < input_len) ? input[i + 2] : 0;
        
        output[out_idx++] = base64_table[a >> 2];
        output[out_idx++] = base64_table[((a & 0x03) << 4) | (b >> 4)];
        output[out_idx++] = (i + 1 < input_len) ? base64_table[((b & 0x0F) << 2) | (c >> 6)] : '=';
        output[out_idx++] = (i + 2 < input_len) ? base64_table[c & 0x3F] : '=';
    }
    
    output[out_idx] = '\0';
}

/* Simple hash function (simplified SHA256 substitute) */
static void sha256_simple(const char *input, int input_len, unsigned char *hash)
{
    int i;
    unsigned long sum = 0x5A5A5A5AL;
    
    for (i = 0; i < input_len; i++)
    {
        sum = ((sum << 5) + sum) + (unsigned char)input[i];
        sum ^= (sum >> 16);
    }
    
    for (i = 0; i < 32; i++)
    {
        hash[i] = (unsigned char)((sum >> ((i % 4) * 8)) & 0xFF);
        if (i % 4 == 3)
            sum = ((sum << 7) ^ (sum >> 11)) + input[i % input_len];
    }
}

/* Build test message in JSON format */
static int build_test_msg(char *msg_text, char *json_output, int max_json_len)
{
    static char base64_msg[512];
    static char base64_hash[64];
    static unsigned char hash[32];
    int msg_len;
    int msgId;
    char *p;
    int i;
    char id_str[12];
    int id_idx;
    
    msg_len = 0;
    p = msg_text;
    while (*p)
    {
        msg_len++;
        p++;
    }
    
    sha256_simple(msg_text, msg_len, hash);
    base64_encode(msg_text, msg_len, base64_msg);
    base64_encode((char*)hash, 32, base64_hash);
    
    msgId = g_tcp_msgId++;
    
    id_idx = 0;
    if (msgId == 0)
    {
        id_str[id_idx++] = '0';
    }
    else
    {
        char digits[12];
        int d_idx = 0;
        int temp = msgId;
        
        while (temp > 0)
        {
            digits[d_idx++] = '0' + (temp % 10);
            temp /= 10;
        }
        while (d_idx > 0)
            id_str[id_idx++] = digits[--d_idx];
    }
    id_str[id_idx] = '\0';
    
    p = json_output;
    *p++ = '{'; *p++ = '"'; *p++ = 'I'; *p++ = 'd'; *p++ = '"'; *p++ = ':';
    
    i = 0;
    while (id_str[i])
        *p++ = id_str[i++];
    
    *p++ = ','; *p++ = '"'; *p++ = 'C'; *p++ = 'a'; *p++ = 't'; *p++ = 'e';
    *p++ = 'g'; *p++ = 'o'; *p++ = 'r'; *p++ = 'y'; *p++ = '"'; *p++ = ':';
    *p++ = '"'; *p++ = 'T'; *p++ = 'e'; *p++ = 's'; *p++ = 't'; *p++ = '"';
    
    *p++ = ','; *p++ = '"'; *p++ = 'B'; *p++ = 'a'; *p++ = 's'; *p++ = 'e';
    *p++ = '6'; *p++ = '4'; *p++ = 'M'; *p++ = 'e'; *p++ = 's'; *p++ = 's';
    *p++ = 'a'; *p++ = 'g'; *p++ = 'e'; *p++ = '"'; *p++ = ':'; *p++ = '"';
    
    i = 0;
    while (base64_msg[i] && (p - json_output) < max_json_len - 200)
        *p++ = base64_msg[i++];
    
    *p++ = '"'; *p++ = ','; *p++ = '"'; *p++ = 'B'; *p++ = 'a'; *p++ = 's';
    *p++ = 'e'; *p++ = '6'; *p++ = '4'; *p++ = 'M'; *p++ = 'e'; *p++ = 's';
    *p++ = 's'; *p++ = 'a'; *p++ = 'g'; *p++ = 'e'; *p++ = 'H'; *p++ = 'a';
    *p++ = 's'; *p++ = 'h'; *p++ = '"'; *p++ = ':'; *p++ = '"';
    
    i = 0;
    while (base64_hash[i])
        *p++ = base64_hash[i++];
    
    *p++ = '"'; *p++ = ','; *p++ = '"'; *p++ = 'R'; *p++ = 's'; *p++ = 'p';
    *p++ = 'R'; *p++ = 'e'; *p++ = 'c'; *p++ = 'e'; *p++ = 'i'; *p++ = 'v';
    *p++ = 'e'; *p++ = 'd'; *p++ = 'O'; *p++ = 'K'; *p++ = '"'; *p++ = ':';
    *p++ = 'f'; *p++ = 'a'; *p++ = 'l'; *p++ = 's'; *p++ = 'e'; *p++ = '}';
    
    *p = '\0';
    
    return msgId;
}

/* Build formatted test message */
static void build_formatted_msg(int i, int testMsgLength, char *output, int max_len)
{
    static const char template[] = "Hello World !!! ";
    int repeat;
    char *p;
    char num_str[12];
    int n_idx;
    int temp;
    
    p = output;
    
    for (repeat = 0; repeat < testMsgLength && (p - output) < max_len - 30; repeat++)
    {
        const char *t = template;
        while (*t)
            *p++ = *t++;
        
        n_idx = 0;
        temp = i;
        if (temp == 0)
        {
            num_str[n_idx++] = '0';
        }
        else
        {
            char digits[12];
            int d_idx = 0;
            
            while (temp > 0)
            {
                digits[d_idx++] = '0' + (temp % 10);
                temp /= 10;
            }
            while (d_idx > 0)
                num_str[n_idx++] = digits[--d_idx];
        }
        num_str[n_idx] = '\0';
        
        n_idx = 0;
        while (num_str[n_idx])
            *p++ = num_str[n_idx++];
        
        *p++ = '\r';
        *p++ = '\n';
    }
    
    *p = '\0';
}

/* ========== End TCP UART Helper Functions ========== */

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

/* ========== TCP UART Tasks ========== */

/* TCP Initialization Task - sets up WiFi and TCP connection */
static void tcp_init_task(void *arg)
{
    static char cmd[128];
    static char response[256];
    int idx;
    char ch;
    int timeout;
    bool got_ok;
    
    (void)arg;
    
    tcp_print("TCP Init: Opening modem...\r\n");
    
    g_tcp_fd = open("AT:", 0);
    if (g_tcp_fd < 0)
    {
        tcp_print("TCP Init: Modem not found!\r\n");
        for (;;) {
            scheduler_sleep(1000);
            scheduler_yield();
        }
    }
    
    tcp_print("TCP Init: Modem online\r\n");
    
    /* Initialize queues and tracker */
    tcp_queue_init(&g_tcp_response_queue);
    tcp_tracker_init(&g_tcp_sent_tracker);
    
    /* Send AT test */
    tcp_print("TCP Init: Testing modem...\r\n");
    send_to_modem(g_tcp_fd, "AT");
    delay_ms(1000);
    
    /* Disable echo */
    tcp_print("TCP Init: Disabling echo...\r\n");
    send_to_modem(g_tcp_fd, "ATE0");
    delay_ms(1000);
    
    /* Connect to WiFi */
    tcp_print("TCP Init: Connecting to WiFi...\r\n");
    my_sprintf(cmd, "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
    send_to_modem(g_tcp_fd, cmd);
    delay_ms(15000); /* Wait for connection */
    
    /* Flush response */
    idx = 0;
    timeout = 0;
    while (timeout < 2000)
    {
        ria_push_char(1);
        ria_set_ax(g_tcp_fd);
        if (ria_call_int(RIA_OP_READ_XSTACK))
        {
            ch = ria_pop_char();
            if (idx < 255)
                response[idx++] = ch;
            timeout = 0;
        }
        else
        {
            delay_ms(10);
            timeout += 10;
        }
    }
    response[idx] = '\0';
    
    /* Get IP address */
    tcp_print("TCP Init: Getting IP...\r\n");
    send_to_modem(g_tcp_fd, "AT+CIFSR");
    delay_ms(2000);
    
    /* Set single connection mode */
    tcp_print("TCP Init: Setting connection mode...\r\n");
    send_to_modem(g_tcp_fd, "AT+CIPMUX=0");
    delay_ms(1000);
    
    /* Set active receive mode */
    send_to_modem(g_tcp_fd, "AT+CIPRECVMODE=0");
    delay_ms(1000);
    
    /* Start TCP connection */
    tcp_print("TCP Init: Connecting to server...\r\n");
    my_sprintf(cmd, "AT+CIPSTART=\"TCP\",\"%s\",%s", SERVER_IP, SERVER_PORT);
    send_to_modem(g_tcp_fd, cmd);
    delay_ms(3000);
    
    /* Flush any pending data */
    idx = 0;
    timeout = 0;
    while (timeout < 1000 && idx < 1000)
    {
        ria_push_char(1);
        ria_set_ax(g_tcp_fd);
        if (ria_call_int(RIA_OP_READ_XSTACK))
        {
            ch = ria_pop_char();
            timeout = 0;
            idx++;
        }
        else
        {
            delay_ms(10);
            timeout += 10;
        }
    }
    
    tcp_print("TCP Init: Connection established!\r\n");
    
    /* Signal that initialization is complete */
    g_tcp_initialized = true;
    
    /* Keep task alive */
    for (;;)
    {
        scheduler_sleep(1000);
        scheduler_yield();
    }
}

/* TCP Sender Task - sends messages to server */
static void tcp_sender_task(void *arg)
{
    static char test_message[512];
    static char json_message[1024];
    static char cmd[128];
    int i;
    int msgId;
    char *p;
    int msg_len;
    int total_len;
    int idx;
    int temp;
    
    (void)arg;
    
    /* Wait for TCP initialization */
    while (!g_tcp_initialized || g_tcp_fd < 0)
    {
        scheduler_sleep(500);
        scheduler_yield();
    }
    
    tcp_print("TCP Sender: Starting message transmission\r\n");
    
    for (i = 1; i <= TCP_TEST_MSG_COUNT; i++)
    {
        tcp_print("\r\n=== Loop iteration, i=");
        {
            char loop_str[12];
            int loop_idx = 0;
            int loop_temp = i;
            if (loop_temp == 0)
                loop_str[loop_idx++] = '0';
            else if (loop_temp < 0)
            {
                loop_str[loop_idx++] = '-';
                loop_temp = -loop_temp;
            }
            if (loop_temp > 0)
            {
                char digits[12];
                int d_idx = 0;
                while (loop_temp > 0)
                {
                    digits[d_idx++] = '0' + (loop_temp % 10);
                    loop_temp /= 10;
                }
                while (d_idx > 0)
                    loop_str[loop_idx++] = digits[--d_idx];
            }
            loop_str[loop_idx] = '\0';
            tcp_print(loop_str);
        }
        tcp_print(", COUNT=");
        {
            char count_str[12];
            int count_idx = 0;
            int count_temp = TCP_TEST_MSG_COUNT;
            if (count_temp == 0)
                count_str[count_idx++] = '0';
            else
            {
                char digits[12];
                int d_idx = 0;
                while (count_temp > 0)
                {
                    digits[d_idx++] = '0' + (count_temp % 10);
                    count_temp /= 10;
                }
                while (d_idx > 0)
                    count_str[count_idx++] = digits[--d_idx];
            }
            count_str[count_idx] = '\0';
            tcp_print(count_str);
        }
        tcp_print(" ===\r\n");
        
        /* Build message */
        build_formatted_msg(i, TCP_TEST_MSG_LENGTH, test_message, 512);
        msgId = build_test_msg(test_message, json_message, 1024);
        
        /* Calculate message length */
        msg_len = 0;
        p = json_message;
        while (*p++)
            msg_len++;
        
        total_len = msg_len + 2; /* Include \r\n */
        
        /* Build AT+CIPSEND command */
        cmd[0] = 'A'; cmd[1] = 'T'; cmd[2] = '+';
        cmd[3] = 'C'; cmd[4] = 'I'; cmd[5] = 'P';
        cmd[6] = 'S'; cmd[7] = 'E'; cmd[8] = 'N';
        cmd[9] = 'D'; cmd[10] = '=';
        
        idx = 11;
        temp = total_len;
        if (temp == 0)
        {
            cmd[idx++] = '0';
        }
        else
        {
            char digits[16];
            int d_idx = 0;
            while (temp > 0)
            {
                digits[d_idx++] = '0' + (temp % 10);
                temp /= 10;
            }
            while (d_idx > 0)
                cmd[idx++] = digits[--d_idx];
        }
        cmd[idx] = '\0';
        
        tcp_print("Sender: Msg ");
        {
            char id_str[12];
            int id_idx = 0;
            temp = i;
            if (temp == 0)
                id_str[id_idx++] = '0';
            else
            {
                char digits[12];
                int d_idx = 0;
                while (temp > 0)
                {
                    digits[d_idx++] = '0' + (temp % 10);
                    temp /= 10;
                }
                while (d_idx > 0)
                    id_str[id_idx++] = digits[--d_idx];
            }
            id_str[id_idx] = '\0';
            tcp_print(id_str);
        }
        tcp_print(" - ");
        
        /* Send AT+CIPSEND */
        tcp_print("[Sending CIPSEND cmd]\r\n");
        send_to_modem(g_tcp_fd, cmd);
        
        /* Small delay for modem to process */
        scheduler_sleep(100);
        scheduler_yield();
        
        tcp_print("[Waiting for >]\r\n");
        
        /* Wait for > prompt */
        {
            char ch;
            int attempts = 0;
            bool got_prompt = false;
            
            /* Wait up to 3 seconds (300 attempts × 10ms) */
            while (attempts < 300 && !got_prompt)
            {
                ria_push_char(1);
                ria_set_ax(g_tcp_fd);

                if (ria_call_int(RIA_OP_READ_XSTACK))
                {
                    ch = ria_pop_char();
                    
                    /* Echo what we got while waiting */
                    if (RIA.ready & RIA_READY_TX_BIT)
                        RIA.tx = ch;
                    
                    /* Only look for >, ignore everything else */
                    if (ch == '>')
                    {
                        got_prompt = true;
                        tcp_print("\r\n[Got >]\r\n");
                        break;
                    }
                }
                else
                {
                    scheduler_sleep(10);
                    scheduler_yield();
                    attempts++;
                }
            }
            
            tcp_print("[After wait loop - got_prompt=");
            if (got_prompt)
                tcp_print("YES]\r\n");
            else
                tcp_print("NO]\r\n");
            
            if (!got_prompt)
            {
                tcp_print("ERROR: TIMEOUT waiting for > - skipping this message\r\n");
                continue;
            }
        }
        
        tcp_print("[About to send data]\r\n");
        
        scheduler_sleep(10);
        scheduler_yield();
        
        /* Send message data */
        send_raw_data(g_tcp_fd, json_message, msg_len);
        
        /* Send \r\n terminator */
        ria_push_char('\r');
        ria_set_ax(g_tcp_fd);
        while (!ria_call_int(RIA_OP_WRITE_XSTACK))
            ;
        ria_push_char('\n');
        ria_set_ax(g_tcp_fd);
        while (!ria_call_int(RIA_OP_WRITE_XSTACK))
            ;
        
        tcp_print("[Data sent, waiting for SEND OK]\r\n");
        
        /* Wait for SEND OK confirmation */
        {
            char ch;
            int attempts = 0;
            bool got_send_ok = false;
            static char ok_buffer[32];
            int ok_idx = 0;
            
            ok_buffer[0] = '\0';
            
            /* Wait up to 5 seconds for SEND OK */
            while (attempts < 500 && !got_send_ok)
            {
                ria_push_char(1);
                ria_set_ax(g_tcp_fd);
                
                if (ria_call_int(RIA_OP_READ_XSTACK))
                {
                    ch = ria_pop_char();
                    
                    /* Echo */
                    if (RIA.ready & RIA_READY_TX_BIT)
                        RIA.tx = ch;
                    
                    /* Add to buffer */
                    if (ok_idx < 31)
                    {
                        ok_buffer[ok_idx++] = ch;
                        ok_buffer[ok_idx] = '\0';
                    }
                    
                    /* Check for "OK" */
                    if (my_strstr(ok_buffer, "OK"))
                    {
                        got_send_ok = true;
                        tcp_print("\r\n[Got SEND OK]\r\n");
                        break;
                    }
                    
                    /* Keep buffer small */
                    if (ok_idx > 20)
                    {
                        int i;
                        for (i = 0; i < 10; i++)
                            ok_buffer[i] = ok_buffer[ok_idx - 10 + i];
                        ok_idx = 10;
                        ok_buffer[ok_idx] = '\0';
                    }
                }
                else
                {
                    scheduler_sleep(10);
                    scheduler_yield();
                    attempts++;
                }
            }
            
            if (!got_send_ok)
            {
                tcp_print("[WARNING: No SEND OK confirmation]\r\n");
            }
        }
        
        /* Track as sent */
        tcp_tracker_mark_sent(&g_tcp_sent_tracker, msgId);
        g_tcp_messages_sent++;
        
        tcp_print("SENT\r\n");
        
        /* Give receiver task time to process incoming response
           Server typically responds within 100-500ms */
        scheduler_sleep(3000);
        scheduler_yield();
    }
    
    tcp_print("TCP Sender: All messages sent\r\n");
    
    /* Keep task alive */
    for (;;)
    {
        scheduler_sleep(1000);
        scheduler_yield();
    }
}

/* TCP Receiver Task - receives responses from server */
static void tcp_receiver_task(void *arg)
{
    static char read_buffer[1024];
    static int buf_pos = 0;
    static ResponseMessage msg;
    static int payload_remaining = 0;
    static int idle_count = 0;
    char ch;
    char *json_start;
    char *ipd_start;
    char *colon;
    int i;
    int len;
    
    (void)arg;
    
    /* Wait for TCP initialization */
    while (!g_tcp_initialized || g_tcp_fd < 0)
    {
        scheduler_sleep(500);
        scheduler_yield();
    }
    
    tcp_print("TCP Receiver: Starting\r\n");
    
    buf_pos = 0;
    read_buffer[0] = '\0';
    payload_remaining = 0;
    idle_count = 0;
    
    for (;;)
    {
        /* Heartbeat every 100 idle reads to show receiver is active */
        if (idle_count > 0 && idle_count % 100 == 0)
        {
            tcp_print("[R");
            {
                char idle_str[12];
                int idle_idx = 0;
                int idle_temp = idle_count;
                if (idle_temp == 0)
                    idle_str[idle_idx++] = '0';
                else
                {
                    char digits[12];
                    int d_idx = 0;
                    while (idle_temp > 0)
                    {
                        digits[d_idx++] = '0' + (idle_temp % 10);
                        idle_temp /= 10;
                    }
                    while (d_idx > 0)
                        idle_str[idle_idx++] = digits[--d_idx];
                }
                idle_str[idle_idx] = '\0';
                tcp_print(idle_str);
            }
            tcp_print("]");
        }
        
        /* Try to read available characters */
        ria_push_char(1);
        ria_set_ax(g_tcp_fd);
        if (ria_call_int(RIA_OP_READ_XSTACK))
        {
            ch = ria_pop_char();
            
            /* Echo character for debugging */
            if (RIA.ready & RIA_READY_TX_BIT)
                RIA.tx = ch;
            
            idle_count = 0;
            
            /* If we're collecting payload bytes after +IPD header */
            if (payload_remaining > 0)
            {
                if (buf_pos < 1023)
                {
                    read_buffer[buf_pos++] = ch;
                    read_buffer[buf_pos] = '\0';
                }
                payload_remaining--;
                
                if (payload_remaining == 0)
                {
                    tcp_print("\r\n[Receiver: Payload complete]\r\n");
                }
            }
            else
            {
                /* Add to buffer (header or general data) */
                if (buf_pos < 1023)
                {
                    read_buffer[buf_pos++] = ch;
                    read_buffer[buf_pos] = '\0';
                }
                
                /* Detect +IPD header for incoming data
                   Format: "+IPD,<len>:<data>" */
                if (buf_pos >= 10)
                {
                    ipd_start = my_strstr(read_buffer, "+IPD,");
                    if (ipd_start)
                    {
                        /* Find the colon that separates length from data */
                        colon = ipd_start + 5;  /* skip "+IPD," */
                        while (*colon && *colon != ':')
                            colon++;
                        
                        /* Only parse if we found the colon */
                        if (*colon == ':')
                        {
                            len = parse_number(ipd_start + 5);
                            if (len > 0)
                            {
                                tcp_print("\r\n[Receiver: Detected +IPD with ");
                                {
                                    char len_str[12];
                                    int len_idx = 0;
                                    int temp = len;
                                    if (temp == 0)
                                        len_str[len_idx++] = '0';
                                    else
                                    {
                                        char digits[12];
                                        int d_idx = 0;
                                        while (temp > 0)
                                        {
                                            digits[d_idx++] = '0' + (temp % 10);
                                            temp /= 10;
                                        }
                                        while (d_idx > 0)
                                            len_str[len_idx++] = digits[--d_idx];
                                    }
                                    len_str[len_idx] = '\0';
                                    tcp_print(len_str);
                                }
                                tcp_print(" bytes]\\r\\n");
                                
                                /* Reset buffer to store only payload (starts after ':') */
                                buf_pos = 0;
                                read_buffer[0] = '\0';
                                payload_remaining = len;
                                continue;
                            }
                        }
                    }
                }
            }
            
            /* Look for complete JSON objects */
            if (ch == '}')
            {
                /* Search backwards for opening brace */
                json_start = NULL;
                for (i = buf_pos - 1; i >= 0; i--)
                {
                    if (read_buffer[i] == '{')
                    {
                        json_start = &read_buffer[i];
                        break;
                    }
                }
                
                if (json_start)
                {
                    /* Try to parse JSON */
                    tcp_print("\r\n[Receiver: Found JSON, attempting parse]\r\n");
                    if (parse_json_response(json_start, &msg))
                    {
                        tcp_print("Receiver: Parsed ID ");
                        {
                            char id_str[12];
                            int id_idx = 0;
                            int temp = msg.id;
                            
                            if (temp == 0)
                                id_str[id_idx++] = '0';
                            else
                            {
                                char digits[12];
                                int d_idx = 0;
                                while (temp > 0)
                                {
                                    digits[d_idx++] = '0' + (temp % 10);
                                    temp /= 10;
                                }
                                while (d_idx > 0)
                                    id_str[id_idx++] = digits[--d_idx];
                            }
                            id_str[id_idx] = '\0';
                            tcp_print(id_str);
                        }
                        tcp_print("\r\n");
                        
                        /* Add to queue */
                        if (!tcp_queue_put(&g_tcp_response_queue, &msg))
                        {
                            tcp_print("Receiver: Queue full!\r\n");
                        }
                        else
                        {
                            g_tcp_responses_received++;
                        }
                    }
                    
                    /* Remove processed JSON from buffer */
                    {
                        int json_len = (buf_pos - (json_start - read_buffer));
                        int remaining = buf_pos - (json_start - read_buffer) - json_len;
                        if (remaining > 0)
                        {
                            for (i = 0; i < remaining; i++)
                                read_buffer[i] = read_buffer[(json_start - read_buffer) + json_len + i];
                        }
                        buf_pos = remaining;
                        read_buffer[buf_pos] = '\0';
                    }
                }
            }
            
            /* Prevent buffer overflow */
            if (buf_pos > 800)
            {
                int keep = 512;
                for (i = 0; i < keep; i++)
                    read_buffer[i] = read_buffer[buf_pos - keep + i];
                buf_pos = keep;
                read_buffer[buf_pos] = '\0';
            }
        }
        else
        {
            /* No data available */
            idle_count++;
            
            /* If we're waiting for payload and it's taking too long, reset */
            if (payload_remaining > 0 && idle_count > 500)
            {
                tcp_print("\r\n[Receiver: Timeout waiting for payload, resetting]\r\n");
                payload_remaining = 0;
                buf_pos = 0;
                read_buffer[0] = '\0';
                idle_count = 0;
            }
            
            /* Sleep briefly then yield to other tasks */
            scheduler_sleep(10);
            scheduler_yield();
        }
    }
}

/* TCP Comparator Task - validates received responses */
static void tcp_comparator_task(void *arg)
{
    ResponseMessage response;
    int missing;
    
    (void)arg;
    
    /* Wait for TCP initialization */
    while (!g_tcp_initialized || g_tcp_fd < 0)
    {
        scheduler_sleep(500);
        scheduler_yield();
    }
    
    tcp_print("TCP Comparator: Starting\r\n");
    
    for (;;)
    {
        /* Check if there are responses to process */
        while (tcp_queue_get(&g_tcp_response_queue, &response))
        {
            tcp_print("Comparator: Validating ID ");
            {
                char id_str[12];
                int id_idx = 0;
                int temp = response.id;
                
                if (temp == 0)
                    id_str[id_idx++] = '0';
                else
                {
                    char digits[12];
                    int d_idx = 0;
                    while (temp > 0)
                    {
                        digits[d_idx++] = '0' + (temp % 10);
                        temp /= 10;
                    }
                    while (d_idx > 0)
                        id_str[id_idx++] = digits[--d_idx];
                }
                id_str[id_idx] = '\0';
                tcp_print(id_str);
            }
            
            /* Check if this message was actually sent */
            if (tcp_tracker_mark_response(&g_tcp_sent_tracker, response.id))
            {
                tcp_print(" - VALID\r\n");
            }
            else
            {
                tcp_print(" - ERROR: Not in sent list!\r\n");
            }
        }
        
        /* Periodically report status */
        scheduler_sleep(5000);
        
        missing = tcp_tracker_get_missing_count(&g_tcp_sent_tracker);
        
        tcp_print("TCP Status: Sent=");
        {
            char buf[12];
            int idx = 0;
            int temp = g_tcp_messages_sent;
            
            if (temp == 0)
                buf[idx++] = '0';
            else
            {
                char digits[12];
                int d_idx = 0;
                while (temp > 0)
                {
                    digits[d_idx++] = '0' + (temp % 10);
                    temp /= 10;
                }
                while (d_idx > 0)
                    buf[idx++] = digits[--d_idx];
            }
            buf[idx] = '\0';
            tcp_print(buf);
        }
        
        tcp_print(" Recv=");
        {
            char buf[12];
            int idx = 0;
            int temp = g_tcp_responses_received;
            
            if (temp == 0)
                buf[idx++] = '0';
            else
            {
                char digits[12];
                int d_idx = 0;
                while (temp > 0)
                {
                    digits[d_idx++] = '0' + (temp % 10);
                    temp /= 10;
                }
                while (d_idx > 0)
                    buf[idx++] = digits[--d_idx];
            }
            buf[idx] = '\0';
            tcp_print(buf);
        }
        
        tcp_print(" Missing=");
        {
            char buf[12];
            int idx = 0;
            int temp = missing;
            
            if (temp == 0)
                buf[idx++] = '0';
            else
            {
                char digits[12];
                int d_idx = 0;
                while (temp > 0)
                {
                    digits[d_idx++] = '0' + (temp % 10);
                    temp /= 10;
                }
                while (d_idx > 0)
                    buf[idx++] = digits[--d_idx];
            }
            buf[idx] = '\0';
            tcp_print(buf);
        }
        tcp_print("\r\n");
        
        if (missing == 0 && g_tcp_sent_tracker.count > 0 && g_tcp_messages_sent >= TCP_TEST_MSG_COUNT)
        {
            tcp_print("*** ALL MESSAGES RECEIVED AND VALIDATED! ***\r\n");
        }
        
        scheduler_yield();
    }
}

/* ========== End TCP UART Tasks ========== */

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

    /* Initialize TCP UART tasks */
    // tcp_print("Main: Initializing TCP UART tasks...\r\n");
    // scheduler_add(tcp_init_task, NULL);
    // scheduler_add(tcp_sender_task, NULL);
    // scheduler_add(tcp_receiver_task, NULL);
  //  scheduler_add(tcp_comparator_task, NULL);

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
    // else
    // {
    //     scheduler_add(queue_test_producer, NULL);
    //     scheduler_add(queue_test_consumer, NULL);
    //     scheduler_add(queue_test_consumer, NULL);        
    // }

//    scheduler_add_once(task_once, NULL);
//    scheduler_add(mem_fluctuate_task, NULL);
    scheduler_run();
}
