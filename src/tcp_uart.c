/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * SPDX-License-Identifier: Unlicense
 */

#include <rp6502.h>
#include <stdbool.h>
#include <fcntl.h>

// An extremely simple terminal for the Pico RIA W modem.
// Uses the terminal built in to the Pico VGA.

// WiFi credentials
#define WIFI_SSID "Cudy24G"
#define WIFI_PASSWORD "ZAnne19991214"
#define SERVER_IP "192.168.10.250"
#define SERVER_PORT "8080"

// Test message configuration
#define ITEST_MSG_COUNT 50      // Total number of test messages to send
#define TEST_MSG_LENGTH 1       // Number of times to repeat the message template
#define BATCH_SIZE 10           // Messages per batch

#define RESPONSE_BUFFER_SIZE 256
#define COMMAND_TIMEOUT 10000  // milliseconds
#define NULL ((void*)0)

// Response queue for received messages
#define RESPONSE_QUEUE_SIZE 10
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

static ResponseQueue g_response_queue;

// Message tracking for sent/received verification
#define SENT_MESSAGE_TRACKING_SIZE 50
typedef struct {
    int id;
    bool sent;
    bool response_received;
    char timestamp[32];  // For debug output
} SentMessageTracker;

typedef struct {
    SentMessageTracker messages[ITEST_MSG_COUNT];
    int count;
} MessageTracker;

static MessageTracker g_sent_tracker;

// Forward declarations
int parse_number(char *str);
void delay_ms(int ms);
void send_to_modem(int fd, char *cmd);

// Simple string functions
char* my_strstr(const char *haystack, const char *needle)
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

void my_sprintf(char *dest, const char *fmt, const char *s1, const char *s2)
{
    // Simple sprintf for our specific format strings
    while (*fmt)
    {
        if (*fmt == '%' && *(fmt + 1) == 's')
        {
            // Copy first string
            const char *src = s1;
            while (*src)
                *dest++ = *src++;
            s1 = s2;  // Next %s will use s2
            fmt += 2;
        }
        else
        {
            *dest++ = *fmt++;
        }
    }
    *dest = '\0';
}

void print(char *s)
{
    while (*s)
        if (RIA.ready & RIA_READY_TX_BIT)
            RIA.tx = *s++;
}

// Queue operations
void queue_init(ResponseQueue *q)
{
    q->write_idx = 0;
    q->read_idx = 0;
    q->count = 0;
}

// Message tracker operations
void tracker_init(MessageTracker *t)
{
    int i;
    t->count = 0;
    for (i = 0; i < ITEST_MSG_COUNT; i++)
    {
        t->messages[i].id = -1;
        t->messages[i].sent = false;
        t->messages[i].response_received = false;
    }
}

void tracker_mark_sent(MessageTracker *t, int msg_id)
{
    if (t->count < ITEST_MSG_COUNT)
    {
        t->messages[t->count].id = msg_id;
        t->messages[t->count].sent = true;
        t->messages[t->count].response_received = false;
        t->count++;
    }
}

bool tracker_mark_response(MessageTracker *t, int msg_id)
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
    return false;  // Message ID not found in sent list
}

int tracker_get_missing_count(MessageTracker *t)
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

void tracker_print_status(MessageTracker *t)
{
    int i;
    int missing = 0;
    int received = 0;
    
    print("\r\n========== Message Tracking Status ==========\r\n");
    print("Sent Messages: ");
    {
        char count_str[12];
        int idx = 0;
        int temp = t->count;
        if (temp == 0)
            count_str[idx++] = '0';
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
                count_str[idx++] = digits[--d_idx];
        }
        count_str[idx] = '\0';
        print(count_str);
    }
    print("\r\n");
    
    for (i = 0; i < t->count; i++)
    {
        if (t->messages[i].sent)
        {
            if (t->messages[i].response_received)
                received++;
            else
                missing++;
            
            print("  ID ");
            {
                char id_str[12];
                int idx = 0;
                int temp = t->messages[i].id;
                if (temp == 0)
                    id_str[idx++] = '0';
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
                        id_str[idx++] = digits[--d_idx];
                }
                id_str[idx] = '\0';
                print(id_str);
            }
            print(": ");
            print(t->messages[i].response_received ? "RECEIVED" : "PENDING");
            print("\r\n");
        }
    }
    
    print("Total Received: ");
    {
        char recv_str[12];
        int idx = 0;
        int temp = received;
        if (temp == 0)
            recv_str[idx++] = '0';
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
                recv_str[idx++] = digits[--d_idx];
        }
        recv_str[idx] = '\0';
        print(recv_str);
    }
    print(" / ");
    {
        char total_str[12];
        int idx = 0;
        int temp = t->count;
        if (temp == 0)
            total_str[idx++] = '0';
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
                total_str[idx++] = digits[--d_idx];
        }
        total_str[idx] = '\0';
        print(total_str);
    }
    print("\r\nMissing: ");
    {
        char missing_str[12];
        int idx = 0;
        int temp = missing;
        if (temp == 0)
            missing_str[idx++] = '0';
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
                missing_str[idx++] = digits[--d_idx];
        }
        missing_str[idx] = '\0';
        print(missing_str);
    }
    print("\r\n===========================================\r\n");
}

bool queue_put(ResponseQueue *q, ResponseMessage *msg)
{
    if (q->count >= RESPONSE_QUEUE_SIZE)
        return false;
    
    q->messages[q->write_idx] = *msg;
    q->write_idx = (q->write_idx + 1) % RESPONSE_QUEUE_SIZE;
    q->count++;
    return true;
}

bool queue_get(ResponseQueue *q, ResponseMessage *msg)
{
    if (q->count == 0)
    {
        return false;        
    }
    
    *msg = q->messages[q->read_idx];
    q->read_idx = (q->read_idx + 1) % RESPONSE_QUEUE_SIZE;
    q->count--;
    
    // Print the response message being retrieved from queue
    print("[Queue] Retrieved response - ID: ");
    {
        char id_str[12];
        int id_idx = 0;
        int temp = msg->id;
        
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
        print(id_str);
    }
    print(", Category: ");
    print(msg->category);
    print(", Message: ");
    {
        int i;
        for (i = 0; i < 40 && msg->base64_message[i]; i++)
        {
            if (RIA.ready & RIA_READY_TX_BIT)
                RIA.tx = msg->base64_message[i];
        }
        if (msg->base64_message[40])
            print("...");
    }
    print("\r\n");
    
    return true;
}

// Parse JSON response from server
// Expects format: {"Id":<num>,"Category":"<str>","Base64Message":"<str>","Base64MessageHash":"<str>",...}
bool parse_json_response(char *json, ResponseMessage *msg)
{
    char *p = json;
    char *start;
    int len;
    
    msg->valid = false;
    msg->id = -1;
    msg->category[0] = '\0';
    msg->base64_message[0] = '\0';
    msg->base64_hash[0] = '\0';
    
    // Find "Id":
    p = my_strstr(json, "\"Id\":");
    if (p)
    {
        p += 5; // skip "Id":
        while (*p == ' ' || *p == '\t') p++;
        msg->id = parse_number(p);
    }
    
    // Find "Category":"
    p = my_strstr(json, "\"Category\":\"");
    if (p)
    {
        p += 12; // skip "Category":"
        start = p;
        len = 0;
        while (*p && *p != '"' && len < 15)
        {
            msg->category[len++] = *p++;
        }
        msg->category[len] = '\0';
    }
    
    // Find "Base64Message":"
    p = my_strstr(json, "\"Base64Message\":\"");
    if (p)
    {
        p += 17; // skip "Base64Message":"
        start = p;
        len = 0;
        while (*p && *p != '"' && len < 511)
        {
            msg->base64_message[len++] = *p++;
        }
        msg->base64_message[len] = '\0';
    }
    
    // Find "Base64MessageHash":"
    p = my_strstr(json, "\"Base64MessageHash\":\"");
    if (p)
    {
        p += 21; // skip "Base64MessageHash":"
        start = p;
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

// UART reader loop - process incoming data and extract responses
// Returns number of characters processed
int uart_reader_loop(int fd, char *buffer, int buf_size)
{
    static char read_buffer[1024];
    static char chunk_buf[256];
    static int buf_pos = 0;
    static ResponseMessage msg;
    static int payload_remaining = 0;  // bytes expected after +DATA
    static int pending_recv_len = 0;   // length announced by +RECV
    static int recv_retry_count = 0;   // how many times we re-request remaining bytes
    static int data_response_seen = 0;  // flag to detect +DATA: only once
    char ch;
    int chars_read;
    int i;
    char *json_start;
    int chunk_idx;
    int idle_loops;
    char *p;
    char *len_start;
    char *colon;
    
    chars_read = 0;
    chunk_idx = 0;
    idle_loops = 0;
    (void)buffer;  // unused
    (void)buf_size;  // unused
    
    // Read available characters
    while (chars_read < 500)  // Limit iterations to prevent blocking
    {
        ria_push_char(1);
        ria_set_ax(fd);
        if (ria_call_int(RIA_OP_READ_XSTACK))
        {
            ch = ria_pop_char();
            
            // Echo to console
            // if (RIA.ready & RIA_READY_TX_BIT)
            //     RIA.tx = ch;
            
            // If we saw a +RECV header earlier, consume exactly payload_remaining bytes
            if (payload_remaining > 0)
            {
                if (buf_pos < 1023)
                {
                    read_buffer[buf_pos++] = ch;
                    read_buffer[buf_pos] = '\0';
                }
                if (chunk_idx < 240)
                {
                    chunk_buf[chunk_idx++] = ch;
                    chunk_buf[chunk_idx] = '\0';
                }
                payload_remaining--;
                
                // Debug: show progress
                if (payload_remaining == 0)
                {
                    print("[Payload complete - received all bytes]\r\n");
                        pending_recv_len = 0;
                }
            }
            else
            {
                // Add to buffer (header or general data)
                if (buf_pos < 1023)
                {
                    read_buffer[buf_pos++] = ch;
                    read_buffer[buf_pos] = '\0';
                }
                
                // Record into chunk buffer for debug print
                if (chunk_idx < 240)
                {
                    chunk_buf[chunk_idx++] = ch;
                    chunk_buf[chunk_idx] = '\0';
                }
                
                // Detect +IPD header for incoming data (ESP8266 single connection mode)
                // Format: "+IPD,<len>:<data>" 
                if (buf_pos >= 10)
                {
                    char *ipd_start = my_strstr(read_buffer, "+IPD,");
                    if (ipd_start)
                    {
                        // Find the colon that separates length from data
                        colon = ipd_start + 5;  // skip "+IPD,"
                        while (*colon && *colon != ':')
                            colon++;
                        
                        // Only parse if we found the colon
                        if (*colon == ':')
                        {
                            int len = parse_number(ipd_start + 5);
                            if (len > 0)
                            {
                                print("[Detected +IPD with ");
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
                                    print(len_str);
                                }
                                print(" bytes expected]\r\n");
                                // Don't reset chunk_idx - we want to see everything for debug
                                // Reset read buffer to store only payload (starts after the ':')
                                buf_pos = 0;
                                read_buffer[0] = '\0';
                                payload_remaining = len;
                                // Don't break - continue reading to get the payload
                                chars_read++;
                                idle_loops = 0;
                                continue;
                            }
                        }
                    }
                }
                
                // Also check for +RECV (notification only - need AT+CIPRECVDATA to retrieve)
                // ESP8266 sends: "+RECV:nnn\r\n" as notification
                if (buf_pos >= 10)
                {
                    char *recv_start = my_strstr(read_buffer, "+RECV:");
                    if (recv_start)
                    {
                        // Find the \r or \n that ends the +RECV line
                        char *line_end = recv_start + 6;  // skip "+RECV:"
                        while (*line_end && *line_end != '\r' && *line_end != '\n')
                            line_end++;
                        
                        // Only parse if we found the line terminator
                        if (*line_end == '\r' || *line_end == '\n')
                        {
                            int len = parse_number(recv_start + 6);
                            if (len > 0)
                            {
                                static char recv_cmd[32];
                                int cmd_idx = 0;

                                // Remember how many bytes the modem announced
                                pending_recv_len = len;
                                
                                print("[Detected +RECV with ");
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
                                    print(len_str);
                                }
                                print(" bytes - sending AT+CIPRECVDATA]\r\n");

                                recv_retry_count = 0;  // reset retries for this payload
                                
                                // Build AT+CIPRECVDATA=<len> command
                                recv_cmd[cmd_idx++] = 'A';
                                recv_cmd[cmd_idx++] = 'T';
                                recv_cmd[cmd_idx++] = '+';
                                recv_cmd[cmd_idx++] = 'C';
                                recv_cmd[cmd_idx++] = 'I';
                                recv_cmd[cmd_idx++] = 'P';
                                recv_cmd[cmd_idx++] = 'R';
                                recv_cmd[cmd_idx++] = 'E';
                                recv_cmd[cmd_idx++] = 'C';
                                recv_cmd[cmd_idx++] = 'V';
                                recv_cmd[cmd_idx++] = 'D';
                                recv_cmd[cmd_idx++] = 'A';
                                recv_cmd[cmd_idx++] = 'T';
                                recv_cmd[cmd_idx++] = 'A';
                                recv_cmd[cmd_idx++] = '=';
                                
                                // Add length
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
                                    
                                    // Copy to command
                                    {
                                        int li = 0;
                                        while (len_str[li])
                                            recv_cmd[cmd_idx++] = len_str[li++];
                                    }
                                }
                                recv_cmd[cmd_idx] = '\0';
                                
                                // Send the command to retrieve data
                                send_to_modem(fd, recv_cmd);
                                print(recv_cmd);
                                
                                // Clear buffer to receive +DATA response
                                // DON'T set payload_remaining yet - wait for +DATA response
                                buf_pos = 0;
                                read_buffer[0] = '\0';
                                chars_read++;
                                idle_loops = 0;
                                continue;
                            }
                        }
                    }
                }
                
                // Check for +DATA: (response to AT+CIPRECVDATA)
                // Format: "+DATA,<len>:<actual_data>" OR just "+DATA:<actual_data>"
                if (buf_pos >= 6)
                {
                    char *data_start = my_strstr(read_buffer, "+DATA");
                    if (data_start)
                    {
                        char *after_data = data_start + 5;  // skip "+DATA"
                        
                        // Check if there's a comma (format: +DATA,<len>:data)
                        if (*after_data == ',')
                        {
                            // Find the colon that separates length from data
                            char *colon = after_data + 1;  // skip ","
                            while (*colon && *colon != ':')
                                colon++;
                            
                            // Only parse if we found the colon
                            if (*colon == ':')
                            {
                                int len = parse_number(after_data + 1);
                                if (len > 0)
                                {
                                    print("[Detected +DATA, with ");
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
                                        print(len_str);
                                    }
                                    print(" bytes]\r\n");
                                    
                                    // Move buffer content to start at the data (after colon)
                                    colon++;  // skip the ':'
                                    {
                                        int src = colon - read_buffer;
                                        int dst = 0;
                                        while (src < buf_pos)
                                            read_buffer[dst++] = read_buffer[src++];
                                        buf_pos = dst;
                                        read_buffer[buf_pos] = '\0';
                                    }
                                    
                                    // Calculate how many more bytes we need
                                    payload_remaining = len - buf_pos;
                                    chars_read++;
                                    idle_loops = 0;
                                    
                                    // If complete payload already in buffer, process immediately
                                    if (payload_remaining <= 0)
                                    {
                                        print("[+DATA payload complete]\r\n");
                                        payload_remaining = 0;
                                    }
                                    continue;
                                }
                            }
                        }
                        // Check if there's a colon directly (format: +DATA:data without length)
                        else if (*after_data == ':' && !data_response_seen)
                        {
                            // Try to use pending_recv_len to know expected bytes
                            if (pending_recv_len > 0)
                            {
                                // Strip the "+DATA:" prefix
                                char *colon = after_data + 1; // points at ':'
                                colon++; // move past ':'
                                {
                                    int src = colon - read_buffer;
                                    int dst = 0;
                                                pending_recv_len = 0;
                                                recv_retry_count = 0;
                                                data_response_seen = 0;
                                        read_buffer[dst++] = read_buffer[src++];
                                    buf_pos = dst;
                                    read_buffer[buf_pos] = '\0';
                                }
                                // Expect pending_recv_len bytes total
                                payload_remaining = pending_recv_len - buf_pos;
                                if (payload_remaining < 0)
                                    payload_remaining = 0;

                                print("[Detected +DATA: - using pending length]");
                                print("\r\n");
                            }
                            else
                            {
                                print("[Detected +DATA: - no length info]\r\n");
                            }
                            data_response_seen = 1;  // Set flag so we don't detect it again
                            // Don't continue - let the character processing continue normally
                        }
                    }
                }
            }
            
            chars_read++;
            idle_loops = 0;  // reset idle counter on successful read
            
            // Look for complete JSON objects
            if (ch == '}')
            {
//                print("[Found }]\r\n");
                // Search backwards for opening brace
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
                    // Try to parse JSON
                    if (parse_json_response(json_start, &msg))
                    {
                        print("\r\n[Parsed response ID:");
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
                            print(id_str);
                        }
                        print("]\r\n");
                        
                        // Add to queue
                        if (!queue_put(&g_response_queue, &msg))
                        {
                            print("[Warning: Response queue full]\r\n");
                        }
                        
                        // Reset the +DATA flag for next response
                        data_response_seen = 0;
                    }
                    
                    // Remove processed JSON from buffer
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
            
            // Prevent buffer overflow - keep only last 512 chars
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
            // No data this call; handle based on whether we're expecting payload
            if (payload_remaining > 0)
            {
                int rem_before;
                int temp;
                char rem_str[12];
                int rem_idx;
                static char cmd[32];
                int cmd_idx;
                char len_str[12];
                int len_idx;
                int d_idx;
                char digits[12];
                
                // We're waiting for payload bytes - keep trying with small delay
                idle_loops++;
                if (idle_loops > 2000)
                {
                    // Try re-requesting remaining bytes up to 3 times
                    if (recv_retry_count < 3 && payload_remaining > 0)
                    {
                        recv_retry_count++;
                        print("[Re-requesting remaining bytes: ");
                        rem_idx = 0;
                        temp = payload_remaining;
                        if (temp == 0)
                            rem_str[rem_idx++] = '0';
                        else
                        {
                            while (temp > 0)
                            {
                                digits[rem_idx++] = '0' + (temp % 10);
                                temp /= 10;
                            }
                            while (rem_idx > 0)
                                rem_str[rem_idx - 1] = digits[--rem_idx];
                            rem_idx = 0;
                            temp = payload_remaining;
                            while (temp > 0)
                            {
                                digits[rem_idx++] = '0' + (temp % 10);
                                temp /= 10;
                            }
                            len_idx = 0;
                            while (rem_idx > 0)
                                rem_str[len_idx++] = digits[--rem_idx];
                            rem_str[len_idx] = '\0';
                        }
                        rem_str[rem_idx == 0 ? 1 : rem_idx] = '\0';
                        print(rem_str);
                        print("]\r\n");
                        
                        // Build AT+CIPRECVDATA=<remaining>
                        cmd_idx = 0;
                        cmd[cmd_idx++] = 'A';
                        cmd[cmd_idx++] = 'T';
                        cmd[cmd_idx++] = '+';
                        cmd[cmd_idx++] = 'C';
                        cmd[cmd_idx++] = 'I';
                        cmd[cmd_idx++] = 'P';
                        cmd[cmd_idx++] = 'R';
                        cmd[cmd_idx++] = 'E';
                        cmd[cmd_idx++] = 'C';
                        cmd[cmd_idx++] = 'V';
                        cmd[cmd_idx++] = 'D';
                        cmd[cmd_idx++] = 'A';
                        cmd[cmd_idx++] = 'T';
                        cmd[cmd_idx++] = 'A';
                        cmd[cmd_idx++] = '=';
                        
                        // remaining length as string
                        len_idx = 0;
                        temp = payload_remaining;
                        if (temp == 0)
                            len_str[len_idx++] = '0';
                        else
                        {
                            while (temp > 0)
                            {
                                digits[len_idx++] = '0' + (temp % 10);
                                temp /= 10;
                            }
                            while (len_idx > 0)
                                len_str[len_idx - 1] = digits[--len_idx];
                            len_idx = 0;
                            temp = payload_remaining;
                            while (temp > 0)
                            {
                                digits[len_idx++] = '0' + (temp % 10);
                                temp /= 10;
                            }
                            while (len_idx > 0)
                                len_str[len_idx - 1] = digits[--len_idx];
                        }
                        len_str[len_idx] = '\0';
                        {
                            int li = 0;
                            while (len_str[li])
                                cmd[cmd_idx++] = len_str[li++];
                        }
                        cmd[cmd_idx] = '\0';
                        send_to_modem(fd, cmd);
                        idle_loops = 0;
                        continue;
                    }
                    
                    print("[Timeout waiting for payload - expected ");
                    rem_idx = 0;
                    temp = payload_remaining;
                    if (temp == 0)
                        rem_str[rem_idx++] = '0';
                    else
                    {
                        while (temp > 0)
                        {
                            digits[rem_idx++] = '0' + (temp % 10);
                            temp /= 10;
                        }
                        len_idx = 0;
                        while (rem_idx > 0)
                            rem_str[len_idx++] = digits[--rem_idx];
                    }
                    rem_str[len_idx] = '\0';
                    print(rem_str);
                    print(" more bytes]\r\n");
                    
                    /* Try to salvage: search for complete JSON in buffer */
                    print("[Searching buffer for JSON, buf_pos=");
                    {
                        char pos_str[12];
                        int pos_idx = 0;
                        temp = buf_pos;
                        if (temp == 0)
                            pos_str[pos_idx++] = '0';
                        else
                        {
                            while (temp > 0)
                            {
                                digits[pos_idx++] = '0' + (temp % 10);
                                temp /= 10;
                            }
                            len_idx = 0;
                            while (pos_idx > 0)
                                pos_str[len_idx++] = digits[--pos_idx];
                            pos_str[len_idx] = '\0';
                        }
                        print(pos_str);
                    }
                    print("]\r\n");
                    
                    if (buf_pos > 10)
                    {
                        int found_brace = 0;
                        char *json_start_ptr = NULL;
                        int j;
                        for (i = buf_pos - 1; i >= 0; i--)
                        {
                            if (read_buffer[i] == '}')
                            {
                                found_brace = 1;
                                print("[Found closing brace at position ");
                                {
                                    char brace_str[12];
                                    int brace_idx = 0;
                                    temp = i;
                                    if (temp == 0)
                                        brace_str[brace_idx++] = '0';
                                    else
                                    {
                                        while (temp > 0)
                                        {
                                            digits[brace_idx++] = '0' + (temp % 10);
                                            temp /= 10;
                                        }
                                        len_idx = 0;
                                        while (brace_idx > 0)
                                            brace_str[len_idx++] = digits[--brace_idx];
                                        brace_str[len_idx] = '\0';
                                    }
                                    print(brace_str);
                                }
                                print("]\r\n");
                                
                                for (j = i - 1; j >= 0; j--)
                                {
                                    if (read_buffer[j] == '{')
                                    {
                                        json_start_ptr = &read_buffer[j];
                                        print("[Found opening brace at position ");
                                        {
                                            char open_str[12];
                                            int open_idx = 0;
                                            temp = j;
                                            if (temp == 0)
                                                open_str[open_idx++] = '0';
                                            else
                                            {
                                                while (temp > 0)
                                                {
                                                    digits[open_idx++] = '0' + (temp % 10);
                                                    temp /= 10;
                                                }
                                                len_idx = 0;
                                                while (open_idx > 0)
                                                    open_str[len_idx++] = digits[--open_idx];
                                                open_str[len_idx] = '\0';
                                            }
                                            print(open_str);
                                        }
                                        print("]\r\n");
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                        
                        if (!found_brace)
                        {
                            print("[No closing brace - trying to parse incomplete JSON]\r\n");
                            /* Search for opening brace even without closing */
                            for (j = 0; j < buf_pos; j++)
                            {
                                if (read_buffer[j] == '{')
                                {
                                    json_start_ptr = &read_buffer[j];
                                    break;
                                }
                            }
                        }
                        
                        if (json_start_ptr)
                        {
                            print("[Attempting to parse incomplete JSON]\r\n");
                            if (parse_json_response(json_start_ptr, &msg))
                            {
                                print("[Parsed partial response ID:");
                                {
                                    char id_str[12];
                                    int id_idx = 0;
                                    temp = msg.id;
                                    if (temp == 0)
                                        id_str[id_idx++] = '0';
                                    else
                                    {
                                        while (temp > 0)
                                        {
                                            digits[id_idx++] = '0' + (temp % 10);
                                            temp /= 10;
                                        }
                                        len_idx = 0;
                                        while (id_idx > 0)
                                            id_str[len_idx++] = digits[--id_idx];
                                        id_str[len_idx] = '\0';
                                    }
                                    print(id_str);
                                }
                                print("]\r\n");
                                
                                if (!queue_put(&g_response_queue, &msg))
                                {
                                    print("[Queue full]\r\n");
                                }
                            }
                        }
                    }
                    
                    payload_remaining = 0;  // reset
                    pending_recv_len = 0;
                    recv_retry_count = 0;
                    data_response_seen = 0;
                    break;
                }
                delay_ms(5);
            }
            else if (chars_read > 0)
            {
                // We read header/data but not expecting payload - short idle wait
                idle_loops++;
                if (idle_loops > 100)
                    break;  // stop after ~500ms idle gap once we read something
                delay_ms(5);
            }
            else
            {
                break;  // nothing read yet, exit immediately
            }
        }
    }
    
    // Debug: print chars received in this iteration
    if (chunk_idx > 0)
    {
        print("[uart_reader_loop recv: ");
        {
            int dbg_i;
            for (dbg_i = 0; dbg_i < chunk_idx; dbg_i++)
            {
                if (RIA.ready & RIA_READY_TX_BIT)
                    RIA.tx = chunk_buf[dbg_i];
            }
        }
        print("]\r\n");
    }

    // Debug: show buffer length after processing
    // print("[Buffer length: ");
    // {
    //     char len_str[12];
    //     int len_idx = 0;
    //     int temp = buf_pos;
    //     if (temp == 0)
    //         len_str[len_idx++] = '0';
    //     else
    //     {
    //         char digits[12];
    //         int d_idx = 0;
    //         while (temp > 0)
    //         {
    //             digits[d_idx++] = '0' + (temp % 10);
    //             temp /= 10;
    //         }
    //         while (d_idx > 0)
    //             len_str[len_idx++] = digits[--d_idx];
    //     }
    //     len_str[len_idx] = '\0';
    //     print(len_str);
    // }
    // print("]\r\n");
    
    return chars_read;
}

// Send a string to the modem
void send_to_modem(int fd, char *cmd)
{
    char *p = cmd;
    while (*p)
    {
        ria_push_char(*p++);
        ria_set_ax(fd);
        while (!ria_call_int(RIA_OP_WRITE_XSTACK))
            ;
    }
    // Send CR+LF for AT commands
    ria_push_char('\r');
    ria_set_ax(fd);
    while (!ria_call_int(RIA_OP_WRITE_XSTACK))
        ;
    ria_push_char('\n');
    ria_set_ax(fd);
    while (!ria_call_int(RIA_OP_WRITE_XSTACK))
        ;
}

// Send raw data without line terminators (for CIPSEND payload)
// Send entire message as fast as possible
void send_raw_data(int fd, char *data, int length)
{
    int i;
    
    // Send all bytes as quickly as possible without delays
    for (i = 0; i < length; i++)
    {
        ria_push_char(data[i]);
        ria_set_ax(fd);
        while (!ria_call_int(RIA_OP_WRITE_XSTACK))
            ;
    }
}

// Simple delay function
void delay_ms(int ms)
{
    int i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 100; j++)
            ;
}

// Read from modem with timeout
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

// Check if response contains any of the expected strings
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

// Forward declaration
int parse_number(char *str);


// Send AT command and wait for expected response
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

// Send AT command with longer timeout (for WiFi connect)
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

// Initialize WiFi connection
bool init_wifi(int fd)
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
    
    // AT+CIFSR - Get IP address
    if (!send_at_command(fd, "AT+CIFSR", ok_resp, 1))
        return false;
    
    // AT+CIPMUX=0 - Single connection mode
    if (!send_at_command(fd, "AT+CIPMUX=0", ok_resp, 1))
        return false;
    
    // AT+CIPRECVMODE=0 - Active receive mode (data pushed via +IPD)
    // This may fail on some firmware - continue even if it errors
    send_at_command(fd, "AT+CIPRECVMODE=0", ok_resp, 1);
    
    // AT+CIPSTART - Start TCP connection
    my_sprintf(cmd, "AT+CIPSTART=\"TCP\",\"%s\",%s", SERVER_IP, SERVER_PORT);
    if (!send_at_command(fd, cmd, tcp_resp, 3))
        return false;
    delay_ms(2000);  // Wait for connection to establish and stabilize
    
    // In normal mode, we use AT+CIPSEND for reliable message delivery
    print("Normal mode active. Ready to send/receive with AT+CIPSEND.\\r\\n");
    
    print("WiFi initialized successfully!\r\n");
    return true;
}

// Simple JSON parser - extract Id field
int parse_msg_id(char *json_str)
{
    char *p;
    p = json_str;
    while (*p)
    {
        // Look for "Id"
        if (p[0] == '"' && p[1] == 'I' && p[2] == 'd' && p[3] == '"')
        {
            // Skip to colon and value
            p += 4;
            while (*p && *p != ':')
                p++;
            if (*p == ':')
            {
                p++;
                while (*p && (*p == ' ' || *p == '\t'))
                    p++;
                // Parse number
                return parse_number(p);
            }
        }
        p++;
    }
    return -1;
}

// Simple number parser
int parse_number(char *str)
{
    int result = 0;
    while (*str >= '0' && *str <= '9')
    {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result;
}

// Read a message from modem and parse it
bool receive_message(int fd, char *buffer, int max_len)
{
    static char response[RESPONSE_BUFFER_SIZE];
    char *json_start;
    char *json_end;
    int len;
    int i;
    int msg_id;
    char id_str[16];
    int temp;
    int idx;
    
    if (read_modem_response(fd, response, RESPONSE_BUFFER_SIZE, 3000))
    {
        // Look for JSON object in response
        json_start = my_strstr(response, "{");
        json_end = my_strstr(response, "}");
        
        if (json_start && json_end && json_end > json_start)
        {
            len = (int)(json_end - json_start) + 1;
            if (len > max_len)
                len = max_len;
            
            for (i = 0; i < len; i++)
                buffer[i] = json_start[i];
            buffer[len] = '\0';
            
            print("Received message (ID: ");
            msg_id = parse_msg_id(buffer);
            if (msg_id >= 0)
            {
                // Print message ID (simplified - just print number)
                temp = msg_id;
                idx = 0;
                if (temp == 0)
                    id_str[idx++] = '0';
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
                        id_str[idx++] = digits[--d_idx];
                }
                id_str[idx] = '\0';
                print(id_str);
            }
            print(")\r\n");
            return true;
        }
    }
    
    return false;
}

// Global message ID counter
static int g_msgId = 1;

// Base64 encoding table
static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Simple Base64 encoder
void base64_encode(const char *input, int input_len, char *output)
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

// Simple SHA256 implementation (simplified for embedded use)
// Note: This is a basic implementation. For production, use a proper crypto library.
void sha256_simple(const char *input, int input_len, unsigned char *hash)
{
    // For simplicity, we'll use a basic checksum instead of full SHA256
    // A full SHA256 implementation would be quite large for cc65
    int i;
    unsigned long sum = 0x5A5A5A5AL;  // seed
    
    for (i = 0; i < input_len; i++)
    {
        sum = ((sum << 5) + sum) + (unsigned char)input[i];
        sum ^= (sum >> 16);
    }
    
    // Generate 32 bytes of "hash" from the checksum
    for (i = 0; i < 32; i++)
    {
        hash[i] = (unsigned char)((sum >> ((i % 4) * 8)) & 0xFF);
        if (i % 4 == 3)
            sum = ((sum << 7) ^ (sum >> 11)) + input[i % input_len];
    }
}

// Build test message in JSON format with Base64 encoding
// Returns the message ID
int build_test_msg(char *msg_text, char *json_output, int max_json_len)
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
    
    // ,"Category":"Test"
    *p++ = ','; *p++ = '"'; *p++ = 'C'; *p++ = 'a'; *p++ = 't'; *p++ = 'e';
    *p++ = 'g'; *p++ = 'o'; *p++ = 'r'; *p++ = 'y'; *p++ = '"'; *p++ = ':';
    *p++ = '"'; *p++ = 'T'; *p++ = 'e'; *p++ = 's'; *p++ = 't'; *p++ = '"';
    
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
    
    return msgId;
}

// Build formatted test message: "Hello World !!! <i>\r\n" repeated testMsgLength times
void build_formatted_msg(int i, int testMsgLength, char *output, int max_len)
{
    static const char template[] = "Hello World !!! ";
    int repeat;
    char *p;
    int num_idx;
    char num_str[12];
    int n_idx;
    int temp;
    
    p = output;
    
    for (repeat = 0; repeat < testMsgLength && (p - output) < max_len - 30; repeat++)
    {
        // Copy "Hello World !!! "
        const char *t = template;
        while (*t)
            *p++ = *t++;
        
        // Add number
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
        
        // Copy number
        num_idx = 0;
        while (num_str[num_idx])
            *p++ = num_str[num_idx++];
        
        // Add \r\n
        *p++ = '\r';
        *p++ = '\n';
    }
    
    *p = '\0';
}

// Print a response message
// Check if a message ID was sent and verify it matches
bool validate_received_msg_id(MessageTracker *t, int received_id, int *sent_index)
{
    int i;
    for (i = 0; i < t->count; i++)
    {
        if (t->messages[i].id == received_id && t->messages[i].sent)
        {
            if (sent_index)
                *sent_index = i;
            return true;
        }
    }
    return false;  // Received ID not in sent list
}

void print_response(ResponseMessage *response)
{
    int i;
    int sent_idx = -1;
    bool id_valid = validate_received_msg_id(&g_sent_tracker, response->id, &sent_idx);
    
    print("\r\n>>> RESPONSE ARRIVED <<<\r\n");
    print("Received Message ID: ");
    {
        char id_str[12];
        int id_idx = 0;
        int temp = response->id;
        
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
        print(id_str);
    }
    print("\r\nID Comparison: ");
    if (id_valid)
    {
        print("PASS - Message ID matches sent message\r\n");
    }
    else
    {
        print("FAIL - Message ID does NOT match any sent message!\r\n");
    }
    print("Category: ");
    print(response->category);
    print("\r\nBase64 Message: ");
    for (i = 0; i < 60 && response->base64_message[i]; i++)
    {
        if (RIA.ready & RIA_READY_TX_BIT)
            RIA.tx = response->base64_message[i];
    }
    if (response->base64_message[60])
        print("...");
    print("\r\nBase64 Hash: ");
    for (i = 0; i < 40 && response->base64_hash[i]; i++)
    {
        if (RIA.ready & RIA_READY_TX_BIT)
            RIA.tx = response->base64_hash[i];
    }
    if (response->base64_hash[40])
        print("...");
    print("\r\n<<< END RESPONSE >>>\r\n");
}

// Send a message in normal mode using AT+CIPSEND
bool send_message(int fd, char *message)
{
    static char cmd[128];
    int msg_len;
    int total_len;  // msg_len + 2 for \r\n terminator
    char *p;
    int idx;
    int temp;
    
    msg_len = 0;
    p = message;
    
    // Calculate message length
    while (*p)
    {
        msg_len++;
        p++;
    }
    
    // Total length includes \r\n terminator
    total_len = msg_len + 2;
    
    // Build AT+CIPSEND=<length> command (single connection mode)
    cmd[0] = 'A';
    cmd[1] = 'T';
    cmd[2] = '+';
    cmd[3] = 'C';
    cmd[4] = 'I';
    cmd[5] = 'P';
    cmd[6] = 'S';
    cmd[7] = 'E';
    cmd[8] = 'N';
    cmd[9] = 'D';
    cmd[10] = '=';
    
    // Convert length to string (use total_len which includes \r\n)
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
    
    print("\r\n--- Sending: ");
    print(message);
    print(" ---\r\n");
    
    // Send AT+CIPSEND=<length>
    print("Sending: ");
    print(cmd);
    print("\r\n");
    
    send_to_modem(fd, cmd);
    
    // Wait for ">" prompt before sending data
    print("Waiting for > prompt...\r\n");
    {
        static char prompt_buffer[64];
        int prompt_idx = 0;
        char ch;
        int timeout = 0;
        bool got_prompt = false;
        
        prompt_buffer[0] = '\0';
        
        // Wait up to 5 seconds for prompt
        while (timeout < 5000 && !got_prompt)
        {
            ria_push_char(1);
            ria_set_ax(fd);
            if (ria_call_int(RIA_OP_READ_XSTACK))
            {
                ch = ria_pop_char();
                
                // Echo to console
                if (RIA.ready & RIA_READY_TX_BIT)
                    RIA.tx = ch;
                
                // Add to buffer
                if (prompt_idx < 63)
                {
                    prompt_buffer[prompt_idx++] = ch;
                    prompt_buffer[prompt_idx] = '\0';
                }
                
                // Check for ">" character
                if (ch == '>')
                {
                    got_prompt = true;
                    print("\r\nGot > prompt!\r\n");
                    break;
                }
                
                // Keep buffer manageable
                if (prompt_idx > 50)
                {
                    // Shift buffer left
                    int i;
                    for (i = 0; i < 32; i++)
                        prompt_buffer[i] = prompt_buffer[prompt_idx - 32 + i];
                    prompt_idx = 32;
                    prompt_buffer[prompt_idx] = '\0';
                }
                
                timeout = 0;  // Reset timeout when we receive data
            }
            else
            {
                delay_ms(10);
                timeout += 10;
            }
        }
        
        if (!got_prompt)
        {
            print("ERROR: Timeout waiting for > prompt\r\n");
            return false;
        }
    }
    
    // Delay to let modem settle after prompt
    delay_ms(100);
    
    // Now send the actual message data with \r\n terminator
    print("Sending message data...\r\n");
    print("Length: ");
    {
        char len_str[12];
        int len_idx = 0;
        int temp = total_len;
        
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
        print(len_str);
    }
    print(" bytes\r\n");
//    print("Data: ");
    //print(message);    
    //print("\r\n");
    
    // Send message bytes
    send_raw_data(fd, message, msg_len);
    
    // Send \r\n terminator as part of the payload
    ria_push_char('\r');
    ria_set_ax(fd);
    while (!ria_call_int(RIA_OP_WRITE_XSTACK))
        ;
    ria_push_char('\n');
    ria_set_ax(fd);
    while (!ria_call_int(RIA_OP_WRITE_XSTACK))
        ;
    
    // Wait for modem to fully buffer and transmit before asking for SEND OK
    // This prevents the modem from fragmenting the message
    delay_ms(1000);
    
    // Wait for SEND OK response
    print("Waiting for OK...\r\n");
    {
        static char send_buffer[128];
        int send_idx = 0;
        char ch;
        int timeout = 0;
        bool got_send_ok = false;
        
        send_buffer[0] = '\0';
        
        // Wait up to 5 seconds for SEND OK
        while (timeout < 5000 && !got_send_ok)
        {
            ria_push_char(1);
            ria_set_ax(fd);
            if (ria_call_int(RIA_OP_READ_XSTACK))
            {
                ch = ria_pop_char();
                
                // Echo to console
                if (RIA.ready & RIA_READY_TX_BIT)
                    RIA.tx = ch;
                
                // Add to buffer
                if (send_idx < 127)
                {
                    send_buffer[send_idx++] = ch;
                    send_buffer[send_idx] = '\0';
                }
                
                // Check for "SEND OK"
                if (my_strstr(send_buffer, "OK"))
                {
                    got_send_ok = true;
                    print("\r\nGot OK!\r\n");
                    break;
                }
                
                // Keep buffer manageable
                if (send_idx > 100)
                {
                    // Shift buffer left
                    int i;
                    for (i = 0; i < 64; i++)
                        send_buffer[i] = send_buffer[send_idx - 64 + i];
                    send_idx = 64;
                    send_buffer[send_idx] = '\0';
                }
                
                timeout = 0;  // Reset timeout when we receive data
            }
            else
            {
                delay_ms(10);
                timeout += 10;
            }
        }
        
        if (!got_send_ok)
        {
            print("WARNING: Timeout waiting for SEND OK\r\n");
            // Continue anyway - message may have been sent
        }
    }
    
    print("--- Message sent ---\r\n");
    return true;
}

void main()
{
    char rx_char, tx_char;
    int fd, cp;
    static char test_message[512];
    static char json_message[1024];
    int loop_count;
    int flush_count;
    int heartbeat;
    int test_msg_counter;
    int msgId;

    loop_count = 0;
    test_msg_counter = 1;
    
    // Initialize response queue and message tracker
    queue_init(&g_response_queue);
    tracker_init(&g_sent_tracker);

    cp = code_page(437);
    if (cp != 437)
    {
        print("Code page 437 not found.\r\n");
    }

    fd = open("AT:", 0);
    if (fd < 0)
    {
        print("Modem not found.\r\n");
        return;
    }
    print("Modem online.\r\n");
    
    // Initialize WiFi connection
    if (!init_wifi(fd))
    {
        print("WiFi initialization failed.\r\n");
        return;
    }

    print("Connected! Starting message loop...\r\n");
    print("Ready to send/receive data. Loop running...\r\n");
    delay_ms(500);
    
    // Flush any pending data from connection setup
    print("[Flushing initial data...]\r\n");
    ria_push_char(1);
    ria_set_ax(fd);
    flush_count = 0;
    while (ria_call_int(RIA_OP_READ_XSTACK) && flush_count++ < 1000)
    {
        rx_char = ria_pop_char();
        if (RIA.ready & RIA_READY_TX_BIT)
            RIA.tx = rx_char;
        delay_ms(1);
    }
    print("[Flush complete]\r\n");
    
    // Main loop - send messages with interleaved response processing
    {
        int icounter = 1;
        int batch_num = 1;
        int batch_start;
        int batch_end;
        int i;
        
        print("Starting message transmission with interleaved response processing: ");
        {
            char count_str[12];
            int c_idx = 0;
            int temp = ITEST_MSG_COUNT;
            
            if (temp == 0)
                count_str[c_idx++] = '0';
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
                    count_str[c_idx++] = digits[--d_idx];
            }
            count_str[c_idx] = '\0';
            print(count_str);
        }
        print(" messages total\r\n");
        
        loop_count = 0;
        heartbeat = 0;
        
        while (icounter <= ITEST_MSG_COUNT)
        {
            batch_start = icounter;
            batch_end = (icounter + BATCH_SIZE > ITEST_MSG_COUNT) ? ITEST_MSG_COUNT + 1 : icounter + BATCH_SIZE;
            
            print("--- Batch ");
            {
                char batch_str[12];
                int b_idx = 0;
                int temp = batch_num;
                
                if (temp == 0)
                    batch_str[b_idx++] = '0';
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
                        batch_str[b_idx++] = digits[--d_idx];
                }
                batch_str[b_idx] = '\0';
                print(batch_str);
            }
            print(" (messages ");
            {
                char start_str[12];
                int s_idx = 0;
                int temp = batch_start;
                
                if (temp == 0)
                    start_str[s_idx++] = '0';
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
                        start_str[s_idx++] = digits[--d_idx];
                }
                start_str[s_idx] = '\0';
                print(start_str);
            }
            print("-");
            {
                char end_str[12];
                int e_idx = 0;
                int temp = batch_end - 1;
                
                if (temp == 0)
                    end_str[e_idx++] = '0';
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
                        end_str[e_idx++] = digits[--d_idx];
                }
                end_str[e_idx] = '\0';
                print(end_str);
            }
            print(") ---\r\n");
            
            // Send batch of messages with interleaved response processing
            for (i = batch_start; i < batch_end; i++)
            {
                print("Message ");
                {
                    char msg_num_str[12];
                    int m_idx = 0;
                    int temp = i;
                    
                    if (temp == 0)
                        msg_num_str[m_idx++] = '0';
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
                            msg_num_str[m_idx++] = digits[--d_idx];
                    }
                    msg_num_str[m_idx] = '\0';
                    print(msg_num_str);
                }
                print(": Building... ");
                
                // Build formatted message
                build_formatted_msg(i, TEST_MSG_LENGTH, test_message, 512);
                
                // Build JSON message
                msgId = build_test_msg(test_message, json_message, 1024);
                
                print("Sending... ");
                print(test_message);                
                
                // Send message
                if (send_message(fd, json_message))
                {
                    print("OK\r\n");
                    // Track this message as sent
                    tracker_mark_sent(&g_sent_tracker, msgId);
                }
                else
                {
                    print("FAILED\r\n");
                }
                
                // After each message, process any responses that have arrived (poll several times)
                {
                    static char dummy_buf[256];
                    int poll;
                    int idle_empty;
                    idle_empty = 0;
                    for (poll = 0; poll < 50; poll++)
                    {
                        int chars_processed = uart_reader_loop(fd, dummy_buf, 256);
                        // Debug: show how many chars were processed in this iteration
                        if (chars_processed > 0)
                        {
                            print("[uart_reader_loop processed ");
                            {
                                char count_str[12];
                                int idx = 0;
                                int temp = chars_processed;
                                if (temp == 0)
                                    count_str[idx++] = '0';
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
                                        count_str[idx++] = digits[--d_idx];
                                }
                                count_str[idx] = '\0';
                                print(count_str);
                            }
                            print(" chars]\r\n");
                            idle_empty = 0;
                        }
                        else
                        {
                            idle_empty++;
                            if (idle_empty > 20)  // Increased from 5 to wait longer for slow data
                                break;
                            delay_ms(5);
                        }
                        
                        // Process any queued responses
                        {
                            ResponseMessage response;
                            while (queue_get(&g_response_queue, &response))
                            {
                                // Validate that the received message ID was actually sent
                                if (validate_received_msg_id(&g_sent_tracker, response.id, NULL))
                                {
                                    // Mark this message ID as having received a response
                                    tracker_mark_response(&g_sent_tracker, response.id);
                                    
                                    // Print the response
                                    print_response(&response);
                                }
                                else
                                {
                                    // ID mismatch - print error
                                    print("\r\n!!! ERROR: Received response for ID ");
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
                                        print(id_str);
                                    }
                                    print(" but no matching sent message! !!!");
                                }
                            }
                        }
                    }
                }
                
                // Small delay between messages in same batch
                delay_ms(50);
            }
            
            print("Batch ");
            {
                char batch_end_str[12];
                int be_idx = 0;
                int temp = batch_num;
                
                if (temp == 0)
                    batch_end_str[be_idx++] = '0';
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
                        batch_end_str[be_idx++] = digits[--d_idx];
                }
                batch_end_str[be_idx] = '\0';
                print(batch_end_str);
            }
            print(" complete\r\n");
            
            icounter = batch_end;
            
            // Wait before next batch
            if (icounter <= ITEST_MSG_COUNT)
            {
                print("Waiting before next batch...\r\n");
                delay_ms(1000);
                batch_num++;
            }
        }
        
        print("All ");
        {
            char final_str[12];
            int f_idx = 0;
            int temp = ITEST_MSG_COUNT;
            
            if (temp == 0)
                final_str[f_idx++] = '0';
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
                    final_str[f_idx++] = digits[--d_idx];
            }
            final_str[f_idx] = '\0';
            print(final_str);
        }
        print(" messages sent. Processing remaining responses...\r\n");
    }
    
    // After sending all messages, continue processing any remaining responses
    print("Entering continuous response processing mode...\r\n");
    loop_count = 0;
    heartbeat = 0;
    
    while (true)
    {
        // Heartbeat indicator every 50 seconds worth of loops
        if (++heartbeat > 50)
        {
            heartbeat = 0;
            print(".");
        }
        
        // Use uart_reader_loop to process incoming data and parse responses
        {
            static char dummy_buf[256];
            int chars_processed = uart_reader_loop(fd, dummy_buf, 256);
            // Debug: show how many chars were processed in this iteration
            if (chars_processed > 0)
            {
                print("[uart_reader_loop processed ");
                {
                    char count_str[12];
                    int idx = 0;
                    int temp = chars_processed;
                    if (temp == 0)
                        count_str[idx++] = '0';
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
                            count_str[idx++] = digits[--d_idx];
                    }
                    count_str[idx] = '\0';
                    print(count_str);
                }
                print(" chars]\r\n");
            }
            
            // Process any queued responses
            {
                ResponseMessage response;
                while (queue_get(&g_response_queue, &response))
                {
                    // Validate that the received message ID was actually sent
                    if (validate_received_msg_id(&g_sent_tracker, response.id, NULL))
                    {
                        // Mark this message ID as having received a response
                        tracker_mark_response(&g_sent_tracker, response.id);
                        
                        // Print the response
                        print_response(&response);
                    }
                    else
                    {
                        // ID mismatch - print error
                        print("\r\n!!! ERROR: Received response for ID ");
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
                            print(id_str);
                        }
                        print(" but no matching sent message! !!!");
                    }
                }
            }
        }
        
        // Check for console input
        if (RIA.ready & RIA_READY_RX_BIT)
        {
            tx_char = RIA.rx;
            // Echo to console
            if (RIA.ready & RIA_READY_TX_BIT)
                RIA.tx = tx_char;
            
            // Check for 's' key to show tracking status
            if (tx_char == 's' || tx_char == 'S')
            {
                tracker_print_status(&g_sent_tracker);
            }
            
            // Send console input to modem
            ria_push_char(tx_char);
            ria_set_ax(fd);
            ria_call_int(RIA_OP_WRITE_XSTACK);
        }
        
        // Periodically check if all messages have been received
        if (loop_count++ > 500)
        {
            int missing = tracker_get_missing_count(&g_sent_tracker);
            if (missing == 0 && g_sent_tracker.count > 0)
            {
                print("\r\n*** ALL MESSAGES RECEIVED! ***\r\n");
                tracker_print_status(&g_sent_tracker);
                loop_count = 0;  // Reset to avoid repeated printing
            }
            loop_count = 0;
        }
        
        // Small delay to allow data to accumulate and prevent CPU overload
        delay_ms(10);
    }
}
