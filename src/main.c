#include "scheduler.h"
#include "pubsub.h"
#include <stddef.h> /* For NULL */
#include <stdlib.h> /* For malloc and free */
#include <rp6502.h>
#include <stdbool.h>
#include <fcntl.h>

const unsigned int BATCH_SIZE = 500;    
const unsigned int MAX_ITEM_COUNT = 2000;    
unsigned int use_monitor = 1u;
volatile unsigned char mq_lock = 0;

/* TCP UART Configuration */
#define WIFI_SSID "Cudy24G"
#define WIFI_PASSWORD "ZAnne19991214"
#define SERVER_IP "192.168.10.250"
#define SERVER_PORT "8080"
#define MQTT_BROKER_IP "192.168.10.174"
//#define MQTT_BROKER_IP "192.168.10.135"
#define MQTT_BROKER_PORT "1883"
#define TEST_MSG_LENGTH 1
#define MQTT_PUBLISH_COUNT 10
#define TCP_TEST_MSG_COUNT 1
#define TCP_TEST_MSG_LENGTH 1
#define TCP_BATCH_SIZE 10
#define RESPONSE_BUFFER_SIZE 256
#define COMMAND_TIMEOUT 10000

void scheduler_sleep(unsigned short ticks);
void scheduler_yield(void);

unsigned int pseudo_random(unsigned int min_val, unsigned int max_val);

/* Move declarations to the top of the file */
static unsigned char mem_fluctuate_active = 0;
static unsigned char mem_fluctuate_buffer[64];

/* Task to simulate memory usage fluctuation using malloc */
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

/* Message tracking structure */
typedef struct {
    unsigned int guid_high;  /* High 16 bits of GUID */
    unsigned int guid_low;   /* Low 16 bits of GUID */
    bool sent;
    bool received;
} TrackedMessage;

static TrackedMessage g_message_tracker[MQTT_PUBLISH_COUNT];
static int g_tracked_count = 0;
static int g_expected_message_count = 0;
static unsigned long g_guid_counter = 0;
static int g_msgId = 1;

/* TCP UART Data Structures */
typedef struct {
    int id;
    char category[16];
    char base64_message[512];
    char base64_hash[64];
    bool valid;
} ResponseMessage;

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
static MessageTracker g_tcp_sent_tracker;
static int g_tcp_fd = -1;
static int g_tcp_msgId = 1;
static volatile bool g_tcp_initialized = false;
static volatile bool g_tcp_send_in_progress = false;
static volatile unsigned int g_tcp_messages_sent = 0;
static volatile unsigned int g_tcp_responses_received = 0;

/* Pub/Sub Manager for multi-topic messaging */
static PubSubManager g_pubsub_mgr;

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

static void mq_acquire(void) {
    while (mq_lock) scheduler_yield();
    mq_lock = 1;
}

static void mq_release(void) { mq_lock = 0; }

void generate_guid(unsigned int *high, unsigned int *low)
{
    /* Simple incrementing GUID for embedded system */
    g_guid_counter++;
    *high = (unsigned int)((g_guid_counter >> 16) & 0xFFFF);
    *low = (unsigned int)(g_guid_counter & 0xFFFF);
}

void print(char *s)
{
    while (*s)
        if (RIA.ready & RIA_READY_TX_BIT)
            RIA.tx = *s++;
}

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

void xram_strcpy(unsigned int addr, const char* str) {
    int i;
    RIA.step0 = 1;  // Enable auto-increment
    RIA.addr0 = addr;
    for (i = 0; str[i]; i++) {
        RIA.rw0 = str[i];
    }
    RIA.rw0 = 0;
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

bool read_modem_response(int fd, char *buffer, int max_len, int timeout_ms)
{
    int idx = 0;
    int elapsed = 0;
    int idle_count = 0;
    char ch;
    
    buffer[0] = '\0';
    
    // Wait a bit for response to start
    delay_ms(100);
    
    while (elapsed < timeout_ms && idx < max_len - 1)
    {
        ria_push_char(1);
        ria_set_ax(fd);
        if (ria_call_int(RIA_OP_READ_XSTACK))
        {
            ch = ria_pop_char();
            buffer[idx++] = ch;
            buffer[idx] = '\0';
            idle_count = 0;
            elapsed = 0;
        }
        else
        {
            idle_count++;
            // If we've received data and then no data for a while, we're done
            if (idx > 0 && idle_count > 50)
                break;
            delay_ms(10);
            elapsed += 10;
        }
    }
    
    return idx > 0;
}

bool check_response(char *response, const char *expected[], int num_expected)
{
    int i;

    /* Treat any ERROR as failure even if OK also appears */
    if (my_strstr(response, "ERROR") != NULL)
        return false;

    for (i = 0; i < num_expected; i++)
    {
        if (my_strstr(response, expected[i]) != NULL)
        {
            return true;
        }
    }
    return false;
}

bool send_at_command(int fd, char *cmd, const char *expected[], int num_expected)
{
    static char response[RESPONSE_BUFFER_SIZE];
    
    print("Sending: ");
    print(cmd);
    print("\r\n");
    
    send_to_modem(fd, cmd);
    
    // Wait for response
    if (read_modem_response(fd, response, RESPONSE_BUFFER_SIZE, COMMAND_TIMEOUT))
    {
        print("Response: ");
        print(response);
        print("\r\n");
        
        /* Special-case bare AT: accept OK even if noise ERROR appears */
        if ((cmd[0] == 'A' && cmd[1] == 'T' && cmd[2] == '\0') && my_strstr(response, "OK"))
        {
            print("OK\r\n");
            return true;
        }

        if (check_response(response, expected, num_expected))
        {
            print("OK\r\n");
            return true;
        }
        else
        {
            print("Unexpected response\r\n");
            return false;
        }
    }
    else
    {
        print("Timeout\r\n");
        return false;
    }
}

bool send_at_command_long(int fd, char *cmd, const char *expected[], int num_expected)
{
    static char response[RESPONSE_BUFFER_SIZE];
    
    print("Sending: ");
    print(cmd);
    print("\r\n");
    
    send_to_modem(fd, cmd);
    
    // Wait for response with longer timeout (20 seconds for WiFi)
    if (read_modem_response(fd, response, RESPONSE_BUFFER_SIZE, 20000))
    {
        print("Response: ");
        print(response);
        print("\r\n");
        
        if (check_response(response, expected, num_expected))
        {
            print("Connected!\r\n");
            return true;
        }
        else
        {
            print("Connection failed\r\n");
            return false;
        }
    }
    else
    {
        print("Connection timeout\r\n");
        return false;
    }
}

bool init_mqtt_wifi(int fd)
{
    static char cmd[128];
    static const char *ok_resp[] = {"OK"};
    static const char *connect_resp[] = {"OK", "WIFI CONNECTED", "WIFI GOT IP"};
    static const char *tcp_resp[] = {"CONNECT", "ALREADY CONNECTED"};
    
    print("Initializing WiFi...\r\n");
    
    // AT - Test command
    if (!send_at_command(fd, "AT", ok_resp, 1))
        return false;

    delay_ms(500);
    
    // ATE0 - Disable echo
    if (!send_at_command(fd, "ATE0", ok_resp, 1))
        return false;
    delay_ms(1000);  // Wait for echo disable to take effect
    
    // AT+CWJAP - Join access point (can take 10-15 seconds)
    print("Connecting to WiFi (may take 15+ seconds)...\r\n");
    my_sprintf(cmd, "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);

    if (!send_at_command_long(fd, cmd, connect_resp, 3))
        return false;

    delay_ms(2000);  // Wait after connection
    
    // AT+CIPSTART - Start TCP connection
    my_sprintf(cmd, "AT+CIPSTART=\"TCP\",\"%s\",%s", MQTT_BROKER_IP, MQTT_BROKER_PORT);

    if (!send_at_command(fd, cmd, tcp_resp, 3))
        return false;

    delay_ms(2000);  // Wait for connection to establish and stabilize
    
    // In normal mode, we use AT+CIPSEND for reliable message delivery
    print("Normal mode active. Ready to send/receive with AT+CIPSEND.\\r\\n");
    
    print("WiFi initialized successfully!\r\n");
    return true;
}

int mqtt_init(void)
{
    char broker[] = MQTT_BROKER_IP;
    char client_id[] = "rp6502_demo";
    unsigned int port = 1883;    
    int i, k;
    char sub_topic[] = "rp6502_sub";
    unsigned int sub_len;
    char pub_topic1[] = "rp6502_pub";
    int msg_count = 0;

    /* STEP 1: Connect to WiFi */
    print("[1/6] Connecting to WiFi...\n");
    g_tcp_fd = open("AT:", 0);    

    print("Waiting for WiFi connection...\n");

    if (!init_mqtt_wifi(g_tcp_fd))
    {
        print("WiFi initialization failed.\r\n");
        return -1;
    }    

    print("WiFi connected!\n");
    
    /* STEP 2: Connect to MQTT Broker */
    print("[2/6] Connecting to MQTT broker...\n");
    
    xram_strcpy(0x0000, broker);
    xram_strcpy(0x0100, client_id);

    printf("Broker: %s:%d\n", broker, port);
    printf("Client: %s\n", client_id);
    
    // Initiate connection
    RIA.xstack = port >> 8;      // port high
    RIA.xstack = port & 0xFF;    // port low

    // Push client_id last (will be popped first with api_pop_uint16)
    RIA.xstack = 0x0100 >> 8;    // client_id high
    RIA.xstack = 0x0100 & 0xFF;  // client_id low

    // Put hostname address in A/X
    RIA.a = 0x0000 & 0xFF;       // low
    RIA.x = 0x0000 >> 8;    

    RIA.op = 0x30;  // mq_connect
    
    if (RIA.a != 0) {
        printf("ERROR: Connection failed: %d\n", RIA.a);
        return -1;
    }
    
    /* Wait for connection */
    print("Waiting for MQTT connection...");

    for (i = 0; i < 50; i++) {
        for (k = 0; k < 10000; k++);
        RIA.op = 0x38;  /* mq_connected */

        if (RIA.a == 1) {
            print(" CONNECTED!\n\n");
            break;
        }

        if (i % 5 == 0) print(".");
    }
    
    RIA.op = 0x38;  /* Check connection status */

    if (RIA.a != 1) {
        print("\nERROR: Connection timeout\n");
        return -1;
    }

    /* STEP 3: Subscribe to Topics */
    print("[3/6] Subscribing to topics...\n");
    
    xram_strcpy(0x0200, sub_topic);
    sub_len = strlen(sub_topic);
    
    printf("Subscribing to: %s\n", sub_topic);

    RIA.xstack = 0x0200 >> 8;    
    RIA.xstack = 0x0200 & 0xFF;    
    RIA.xstack = sub_len >> 8;    
    RIA.xstack = sub_len & 0xFF;
    RIA.xstack = 0;                 // QoS 1    
    RIA.op = 0x33;  // mq_subscribe

    while (RIA.busy) { }    

    if (RIA.a == 0) {
        print("Subscribed successfully!\n\n");
    } else {
        print("ERROR: Subscribe failed\n");
        return -1;
    }

    return 0;
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

int build_test_msg(char *msg_text, char *json_output, int max_json_len, char *out_base64_msg, char *out_base64_hash, unsigned int *guid_high, unsigned int *guid_low)
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
    
    /* Clear buffers to prevent stale data from previous calls */
    for (i = 0; i < 512; i++) base64_msg[i] = 0;
    for (i = 0; i < 64; i++) base64_hash[i] = 0;
    for (i = 0; i < 32; i++) hash[i] = 0;
    
    // Get message length
    msg_len = 0;
    p = msg_text;
    while (*p)
    {
        msg_len++;
        p++;
    }
    
    // Calculate hash
    sha256_simple(msg_text, msg_len, hash);
    
    // Base64 encode message
    base64_encode(msg_text, msg_len, base64_msg);
    
    // Base64 encode hash
    base64_encode((char*)hash, 32, base64_hash);
    
    // Get next message ID
    msgId = g_msgId++;
    
    // Generate GUID
    generate_guid(guid_high, guid_low);
    
    // Convert msgId to string
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
    
    // Build JSON string manually
    p = json_output;
    
    // {"Id":
    *p++ = '{'; *p++ = '"'; *p++ = 'I'; *p++ = 'd'; *p++ = '"'; *p++ = ':';
    
    // <msgId>
    i = 0;
    while (id_str[i])
        *p++ = id_str[i++];
    
    // ,"Guid":
    *p++ = ',';
    *p++ = '"';
    *p++ = 'G';
    *p++ = 'u';
    *p++ = 'i';
    *p++ = 'd';
    *p++ = '"';
    *p++ = ':';
    *p++ = '"';
    
    // Convert GUID to hex string (8 chars)
    for (i = 3; i >= 0; i--)
    {
        unsigned char nibble;
        nibble = ((*guid_high) >> (i * 4)) & 0x0F;
        *p++ = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    for (i = 3; i >= 0; i--)
    {
        unsigned char nibble;
        nibble = ((*guid_low) >> (i * 4)) & 0x0F;
        *p++ = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    
    *p++ = '"';
    
    // ,"Category":"Test"
    *p++ = ','; *p++ = '"'; *p++ = 'C'; *p++ = 'a'; *p++ = 't'; *p++ = 'e';
    *p++ = 'g'; *p++ = 'o'; *p++ = 'r'; *p++ = 'y'; *p++ = '"'; *p++ = ':';
    *p++ = '"'; *p++ = 'T'; *p++ = 'e'; *p++ = 's'; *p++ = 't'; *p++ = '"';
    
    // ,"Message":"
    *p++ = ','; *p++ = '"'; *p++ = 'M'; *p++ = 'e'; *p++ = 's'; *p++ = 's';
    *p++ = 'a'; *p++ = 'g'; *p++ = 'e'; *p++ = '"'; *p++ = ':'; *p++ = '"';
    
    // <msg_text>
    i = 0;
    while (msg_text[i] && (p - json_output) < max_json_len - 300)
        *p++ = msg_text[i++];
    
    // "
    *p++ = '"';
    
    // ,"Base64Message":"
    *p++ = ','; *p++ = '"'; *p++ = 'B'; *p++ = 'a'; *p++ = 's'; *p++ = 'e';
    *p++ = '6'; *p++ = '4'; *p++ = 'M'; *p++ = 'e'; *p++ = 's'; *p++ = 's';
    *p++ = 'a'; *p++ = 'g'; *p++ = 'e'; *p++ = '"'; *p++ = ':'; *p++ = '"';
    
    // <base64_msg>
    i = 0;
    while (base64_msg[i] && (p - json_output) < max_json_len - 200)
        *p++ = base64_msg[i++];
    
    // ","Base64MessageHash":"
    *p++ = '"'; *p++ = ','; *p++ = '"'; *p++ = 'B'; *p++ = 'a'; *p++ = 's';
    *p++ = 'e'; *p++ = '6'; *p++ = '4'; *p++ = 'M'; *p++ = 'e'; *p++ = 's';
    *p++ = 's'; *p++ = 'a'; *p++ = 'g'; *p++ = 'e'; *p++ = 'H'; *p++ = 'a';
    *p++ = 's'; *p++ = 'h'; *p++ = '"'; *p++ = ':'; *p++ = '"';
    
    // <base64_hash>
    i = 0;
    while (base64_hash[i])
        *p++ = base64_hash[i++];
    
    // ","RspReceivedOK":false}
    *p++ = '"'; *p++ = ','; *p++ = '"'; *p++ = 'R'; *p++ = 's'; *p++ = 'p';
    *p++ = 'R'; *p++ = 'e'; *p++ = 'c'; *p++ = 'e'; *p++ = 'i'; *p++ = 'v';
    *p++ = 'e'; *p++ = 'd'; *p++ = 'O'; *p++ = 'K'; *p++ = '"'; *p++ = ':';
    *p++ = 'f'; *p++ = 'a'; *p++ = 'l'; *p++ = 's'; *p++ = 'e'; *p++ = '}';
    
    *p = '\0';
    
    /* Copy base64 values to output buffers if provided */
    if (out_base64_msg)
    {
        for (i = 0; i < 511 && base64_msg[i]; i++)
            out_base64_msg[i] = base64_msg[i];
        out_base64_msg[i] = '\0';
    }
    
    if (out_base64_hash)
    {
        for (i = 0; i < 63 && base64_hash[i]; i++)
            out_base64_hash[i] = base64_hash[i];
        out_base64_hash[i] = '\0';
    }
    
    return msgId;
}

/* Build test message in JSON format */
static int build_test_msg_old(char *msg_text, char *json_output, int max_json_len)
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

/* Idle task to keep ticks advancing when all other tasks are sleeping. */
static void idle_task(void *arg)
{
    (void)arg;
    for (;;) {
        scheduler_yield();
    }
}

/* ==================== PUB/SUB EXAMPLE TASKS ==================== */

/* Callback for temperature topic */
static void on_temperature_update(const char *topic, unsigned int message, void *user_data)
{
    printf("[TEMP_SUBSCRIBER] Received on topic '%s': temp=%u°C\n", topic, message);
    (void)user_data;
}

/* Callback for pressure topic */
static void on_pressure_update(const char *topic, unsigned int message, void *user_data)
{
    printf("[PRESSURE_SUBSCRIBER] Received on topic '%s': pressure=%u kPa\n", topic, message);
    (void)user_data;
}

/* Callback for system status topic */
static void on_system_status(const char *topic, unsigned int message, void *user_data)
{
    printf("[STATUS_SUBSCRIBER] Received on topic '%s': status=0x%04X\n", topic, message);
    (void)user_data;
}

/* Publisher task that sends temperature and pressure readings */
static void pubsub_sensor_publisher(void *arg)
{
    unsigned int temp_value = 20;
    unsigned int pressure_value = 1013;
    unsigned int iteration = 0;
    
    (void)arg;
    
    printf("[PUBLISHER] Starting sensor publisher task\n");
    
    for (;;) {
        iteration++;
        
        /* Simulate temperature change */
        temp_value = 20 + (iteration % 10);
        
        /* Simulate pressure change */
        pressure_value = 1010 + (iteration % 20);
        
        /* Publish to temperature topic */
        if (pubsub_publish(&g_pubsub_mgr, "sensors/temperature", temp_value)) {
            printf("[PUBLISHER] Published temp=%u to 'sensors/temperature'\n", temp_value);
        } else {
            printf("[PUBLISHER] FAILED to publish temperature\n");
        }
        
        /* Publish to pressure topic */
        if (pubsub_publish(&g_pubsub_mgr, "sensors/pressure", pressure_value)) {
            printf("[PUBLISHER] Published pressure=%u to 'sensors/pressure'\n", pressure_value);
        } else {
            printf("[PUBLISHER] FAILED to publish pressure\n");
        }
        
        /* Publish system status every 5 iterations */
        if (iteration % 5 == 0) {
            unsigned int status = 0x0001 | (iteration << 8);
            if (pubsub_publish(&g_pubsub_mgr, "system/status", status)) {
                printf("[PUBLISHER] Published status=0x%04X to 'system/status'\n", status);
            }
        }
        
        scheduler_sleep(500);
    }
}

/* Subscriber task that processes sensor messages */
static void pubsub_sensor_monitor(void *arg)
{
    (void)arg;
    
    printf("[MONITOR] Starting sensor monitor task\n");
    
    for (;;) {
        /* Process all pending messages from all topics */
        pubsub_process_all(&g_pubsub_mgr);
        
        printf("[MONITOR] Queue sizes: temp=%u, pressure=%u, status=%u\n",
               pubsub_queue_size(&g_pubsub_mgr, "sensors/temperature"),
               pubsub_queue_size(&g_pubsub_mgr, "sensors/pressure"),
               pubsub_queue_size(&g_pubsub_mgr, "system/status"));
        
        scheduler_sleep(300);
    }
}

/* ==================== END PUB/SUB EXAMPLE TASKS ==================== */

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

        //for (i = 0; i < 1000u; ++i)
        scheduler_yield();
    }
}

static void queue_test_consumer(void *arg)
{
    unsigned int v;
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
    char pctbuf[8];
    static unsigned int toggle_counter = 0;    
    (void)arg;

    puts("main: Monitor task started. Reporting in 5s...\r\n");

    for (;;)
    {
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
            unsigned long active_pool = 0UL;
            unsigned int active_count = 0u;
            unsigned long pct_tenths = 0UL;
            unsigned int int_part = 0u;
            unsigned int dec = 0u;
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

    puts(summary_buffer);
  //      puts("\r\n");                        
    }
}

int track_message(unsigned int guid_high, unsigned int guid_low)
{
    if (g_tracked_count >= MQTT_PUBLISH_COUNT)
        return -1;
    
    g_message_tracker[g_tracked_count].guid_high = guid_high;
    g_message_tracker[g_tracked_count].guid_low = guid_low;
    g_message_tracker[g_tracked_count].sent = true;
    g_message_tracker[g_tracked_count].received = false;
    g_tracked_count++;
    
    return g_tracked_count - 1;
}

/* Mark message as received */
bool mark_received(unsigned int guid_high, unsigned int guid_low)
{
    int i;
    for (i = 0; i < g_tracked_count; i++)
    {
        if (g_message_tracker[i].guid_high == guid_high &&
            g_message_tracker[i].guid_low == guid_low)
        {
            g_message_tracker[i].received = true;
            return true;
        }
    }
    return false;
}

/* Count received messages */
int count_received_messages(void)
{
    int i;
    int count = 0;
    for (i = 0; i < g_tracked_count; i++)
    {
        if (g_message_tracker[i].received)
            count++;
    }
    return count;
}

/* Check if all messages received */
bool all_messages_received(void)
{
    int received = count_received_messages();
    return (received >= g_expected_message_count);
}

static void mqtt_producer_task(void *arg)
{
    /* ALL state must be static to avoid 256-byte stack overflow */
    static char test_message[512];
    static char json_message[1024];
    static char sent_base64_msg[512];
    static char sent_base64_hash[64];
    static char pub_topic1[] = "rp6502_pub";
    static int publish_total;
    static int i;
    static unsigned int msg_guid_high, msg_guid_low;
    
    (void)arg;    

    print("[4/6] Publishing messages...\n");

    publish_total = MQTT_PUBLISH_COUNT;
    g_expected_message_count = publish_total;

    for (i = 0; i < publish_total; i++) {
        printf("Producer: iteration %d, acquiring lock...\n", i + 1);
        mq_acquire();
        printf("Producer: lock acquired\n");

        build_formatted_msg(i + 1, TEST_MSG_LENGTH, test_message, 512);    
        build_test_msg(test_message, json_message, 1024, sent_base64_msg, sent_base64_hash, &msg_guid_high, &msg_guid_low);
        
        /* Track this message */
        track_message(msg_guid_high, msg_guid_low);
        printf("\r\nTracking message with GUID: %04X%04X\n", msg_guid_high, msg_guid_low);

        xram_strcpy(0x0300, pub_topic1);
        xram_strcpy(0x0400, json_message);
        
        printf("Publishing (%d/%d): %s -> %s\n", i + 1, publish_total, pub_topic1, json_message);
        
        /* Debug: print message length for last few messages */
        if (i + 1 >= publish_total - 2) {
            printf("  Message length: %d bytes\n", (int)strlen(json_message));
            printf("  Topic length: %d bytes\n", (int)strlen(pub_topic1));
        }
        //printf("Topic and message len: %zu -> %zu\n", strlen(pub_topic1), strlen(json_message));    
        
        /* payload address */   
        RIA.xstack = 0x0400 >> 8;
        RIA.xstack = 0x0400 & 0xFF;

        /* payload length */   
        RIA.xstack = strlen(json_message) >> 8;    
        RIA.xstack = strlen(json_message) & 0xFF;

        /* topic address */   
        RIA.xstack = 0x0300 >> 8;    /* topic addr high */
        RIA.xstack = 0x0300 & 0xFF;  /* topic addr low */

        /* topic length */
        RIA.xstack = strlen(pub_topic1) >> 8;    
        RIA.xstack = strlen(pub_topic1) & 0xFF;

        RIA.xstack = 1;                              /* retain = false */    
        RIA.xstack = 0;                              /* QoS 0 */    

        // Kick off publish
        RIA.op = 0x32;  // mq_publish

        while (RIA.busy) { }

        if (RIA.mq_publish_done)
        {
            print("Message published!\n");
        }
        else
        {
            print("ERROR: Publish failed\n");
        }

        mq_release();
        printf("Producer: lock released, sleeping...\n");
        scheduler_sleep(500);
        printf("Producer: woke up from sleep\n");
    }
    printf("Producer: completed all %d publishes\n", publish_total);
}

static void mqtt_consumer_task(void *arg)
{
    /* ALL state must be static to avoid 256-byte stack overflow */
    static char broker[] = MQTT_BROKER_IP;
    static char sub_topic[] = "rp6502_sub";
    static unsigned int msg_len, topic_len, bytes_read;
    static int msg_count = 0;
    static int i, j;
    static int poll_attempts = 0;
    static const int MAX_POLL_ATTEMPTS = 100;  /* ~50 seconds at 500ms sleep */
    
    (void)arg;    

    print("[5/6] Listening for incoming messages...\n");
    printf("Waiting for %d messages to be received\n", g_expected_message_count);
    
    while (!all_messages_received() && poll_attempts < MAX_POLL_ATTEMPTS) {
        printf("Consumer: checking for messages, acquiring lock...\n");
        mq_acquire();
        printf("Consumer: lock acquired\n");
        RIA.op = 0x35;  /* mq_poll */

        while (RIA.busy) { }            

        msg_len = RIA.a | (RIA.x << 8);

        /* If no message, release lock immediately and sleep */
        if (msg_len == 0) {
            printf("Consumer: no message, releasing lock and sleeping...\n");
            mq_release();
            poll_attempts++;
            scheduler_sleep(500);
            printf("Consumer: woke up from sleep\n");
            continue;
        }
        
        /* Reset poll attempts when we get a message */
        poll_attempts = 0;

        /* Drain all pending messages before delaying again */
        while (msg_len > 0) {
            msg_count++;
            printf("\n=== Message %d (Payload: %d bytes) ===\n", msg_count, msg_len);
            
            /* topic address */   
            RIA.xstack = 0x0500 >> 8;    /* topic addr high */
            RIA.xstack = 0x0500 & 0xFF;  /* topic addr low */
            RIA.xstack = 128 >> 8;                                    
            RIA.xstack = 128 & 0xFF; // buf len

            RIA.op = 0x37;  /* mq_get_topic */
            
            while (RIA.busy) { }                

            topic_len = RIA.a | (RIA.x << 8);
            
            printf("Topic: ");
            RIA.addr0 = 0x0500;

            for (j = 0; j < topic_len; j++) {
                putchar(RIA.rw0);
            }

            printf("\n");
            
            /* Read message */
            RIA.xstack = 0x0600 >> 8;    /* topic addr high */
            RIA.xstack = 0x0600 & 0xFF;  /* topic addr low */
            RIA.xstack = 255 >> 8;            
            RIA.xstack = 255 & 0xFF; // buf len

            RIA.op = 0x36;  /* mq_read_message */
            
            while (RIA.busy) { }                

            bytes_read = RIA.a | (RIA.x << 8);
            
            printf("Payload: ");
            RIA.addr0 = 0x0600;
            RIA.step0 = 1;  // Enable auto-increment

            printf("\n");
            
            /* Parse GUID from received message */
            {
                static char payload_buf[512];
                static char guid_str[9];
                char *guid_start;
                unsigned int recv_guid_high = 0;
                unsigned int recv_guid_low = 0;
                int k;
                int start_idx;
                
                /* Copy payload to buffer for parsing */
                RIA.addr0 = 0x0600;
                RIA.step0 = 1;

                for (j = 0; j < bytes_read && j < 511; j++) {
                    payload_buf[j] = RIA.rw0;
                    putchar(payload_buf[j]);                    
                }
                
                payload_buf[j] = '\0';
                
                /* Skip any leading control bytes (e.g., 0x00/0x0A) before JSON */
                start_idx = 0;

                while (start_idx < bytes_read && payload_buf[start_idx] != '{')
                    start_idx++;

                guid_start = my_strstr(&payload_buf[start_idx], "\"Guid\"");

                if (guid_start)
                {
                    /* Move past "Guid" */
                    guid_start += 6; /* length of "Guid" */

                    /* Skip optional spaces and colon */
                    while (*guid_start == ' ')
                        guid_start++;
                    if (*guid_start == ':')
                        guid_start++;
                    while (*guid_start == ' ')
                        guid_start++;

                    /* Expect opening quote */
                    if (*guid_start == '"')
                        guid_start++;
                    
                    /* Extract 8 hex characters */
                    for (k = 0; k < 8 && guid_start[k] != '\0' && guid_start[k] != '\"'; k++) {
                        guid_str[k] = guid_start[k];
                    }
                    guid_str[k] = '\0';
                    
                    /* Parse hex string to GUID values */
                    if (k == 8) {
                        for (k = 0; k < 4; k++) {
                            char c = guid_str[k];
                            recv_guid_high = (recv_guid_high << 4);
                            if (c >= '0' && c <= '9')
                                recv_guid_high |= (c - '0');
                            else if (c >= 'A' && c <= 'F')
                                recv_guid_high |= (c - 'A' + 10);
                            else if (c >= 'a' && c <= 'f')
                                recv_guid_high |= (c - 'a' + 10);
                        }
                        
                        for (k = 4; k < 8; k++) {
                            char c = guid_str[k];
                            recv_guid_low = (recv_guid_low << 4);
                            if (c >= '0' && c <= '9')
                                recv_guid_low |= (c - '0');
                            else if (c >= 'A' && c <= 'F')
                                recv_guid_low |= (c - 'A' + 10);
                            else if (c >= 'a' && c <= 'f')
                                recv_guid_low |= (c - 'a' + 10);
                        }
                        
                        printf("Received message GUID: %04X%04X\n", recv_guid_high, recv_guid_low);
                        
                        /* Mark as received */
                        if (mark_received(recv_guid_high, recv_guid_low)) {
                            printf("Message matched and marked received! (%d/%d)\n", 
                                   count_received_messages(), g_tracked_count);
                        } else {
                            printf("Message GUID not in tracking list\n");
                        }
                    }
                }
                else
                {
                    printf("GUID not found in message payload\n");
                }
            }

            /* Poll again immediately to see if more messages are queued */
            RIA.op = 0x35;  /* mq_poll */
            while (RIA.busy) { }
            msg_len = RIA.a | (RIA.x << 8);
        }
        
        /* Progress indicator */
        if (i % 20 == 0 && i > 0) {
            printf(".");
            fflush(stdout);
        }
   
        if (all_messages_received())
        {
            printf("\n\nAll messages received! Ending gracefully.\n");
        }

        mq_release();        
        scheduler_sleep(500);
    }
    
    if (all_messages_received()) {
        printf("\n\nAll messages received! Ending gracefully.\n");
    } else {
        printf("\n\nTimeout: Only received %d/%d messages after %d poll attempts.\n", 
               count_received_messages(), g_expected_message_count, MAX_POLL_ATTEMPTS);
    }
    
    printf("\n\nReceived %d message%s total\n", 
           msg_count, msg_count == 1 ? "" : "s");
    printf("Tracked/Matched: %d/%d messages\n\n", 
           count_received_messages(), g_tracked_count);
    
    /* STEP 6: Disconnect */
    printf("[6/6] Disconnecting from broker...\n");
    RIA.op = 0x31;  /* mq_disconnect */
    
    if (RIA.a == 0) {
        printf("Disconnected successfully!\n");
    }
    
    printf("\n=== EXAMPLE COMPLETE ===\n");
    printf("Summary:\n");
    printf("  - Connected to %s\n", broker);
    printf("  - Subscribed to: %s\n", sub_topic);
    printf("  - Published %d messages\n", g_tracked_count);
    printf("  - Received %d messages\n", msg_count);
    printf("  - Disconnected cleanly\n");
}

/* ========== End TCP UART Tasks ========== */

void main()
{
    unsigned int i;
    int mqtt_ok;

    test_run_count = 0;
    scheduler_init();

    /* Seed RNG with tick count so different runs have different sequences */
    seed_random(scheduler_get_ticks());
    
    /* Warm up the RNG with a few iterations to diverge from fixed seed */
    for (i = 0; i < 10u; ++i) {
        pseudo_random(0u, 1u);
    }

    use_monitor = 1;

    if (use_monitor == -1)
    {
        /* Ensure test queues are initialized regardless of monitor mode */
        q_init(&test_q_1);
        q_init(&test_q_2);

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
    else
    {
        /* Initialize pub/sub system for multi-topic messaging */
        printf("\n[MAIN] Initializing pub/sub system...\n");
        pubsub_init(&g_pubsub_mgr);
        
        /* Create topics */
//        pubsub_create_topic(&g_pubsub_mgr, "sensors/temperature");
        //pubsub_create_topic(&g_pubsub_mgr, "sensors/pressure");
        pubsub_create_topic(&g_pubsub_mgr, "rp6502_pub");
        
        printf("[MAIN] Subscribing to topics...\n");
        
        /* Subscribe to temperature topic */
        //pubsub_subscribe(&g_pubsub_mgr, "sensors/temperature", 
                        //on_temperature_update, NULL);
        
        /* Subscribe to pressure topic */
        //pubsub_subscribe(&g_pubsub_mgr, "sensors/pressure", 
                        //on_pressure_update, NULL);
        
        /* Subscribe to system status topic */
        pubsub_subscribe(&g_pubsub_mgr, "rp6502_pub", 
                        on_system_status, NULL);
        
        printf("[MAIN] Pub/Sub initialized with 3 topics and 3 subscribers\n\n");
        
        /* Add pub/sub demonstration tasks */
        scheduler_add(pubsub_sensor_publisher, NULL);
        scheduler_add(pubsub_sensor_monitor, NULL);
        scheduler_add(idle_task, NULL);
        
        printf("[MAIN] Pub/Sub tasks added to scheduler. Starting...\n\n");
    }

    scheduler_run();
}
