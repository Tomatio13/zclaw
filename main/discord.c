#include "discord.h"
#include "nvs_keys.h"
#include "memory.h"
#include "messages.h"
#include "config.h"
#include "net_http_guard.h"
#include "utf8_utils.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "discord";

// Discord state
static char s_bot_token[128] = {0};
static char s_channel_id[64] = {0};
static QueueHandle_t s_input_queue = NULL;
static QueueHandle_t s_output_queue = NULL;

// Long polling state
static char s_last_message_id[64] = {0};
static bool s_running = true;
static bool s_first_poll_done = false;

// Forward declarations
static void discord_task(void *pvParameters);
static esp_err_t discord_send_http(const char *text);
static esp_err_t discord_poll_messages(void);
static void process_message(cJSON *message_json);

esp_err_t discord_init(void)
{
    // Load bot token from NVS
    if (!memory_get(NVS_KEY_DISCORD_BOT_TOKEN, s_bot_token, sizeof(s_bot_token))) {
        ESP_LOGW(TAG, "Discord bot token not found in NVS");
        return ESP_ERR_NOT_FOUND;
    }

    // Load channel ID from NVS
    if (!memory_get(NVS_KEY_DISCORD_CHANNEL_ID, s_channel_id, sizeof(s_channel_id))) {
        ESP_LOGW(TAG, "Discord channel ID not found in NVS");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Discord initialized (channel: %s)", s_channel_id);
    return ESP_OK;
}

bool discord_is_configured(void)
{
    return s_bot_token[0] != '\0' && s_channel_id[0] != '\0';
}

const char *discord_get_channel_id(void)
{
    return s_channel_id;
}

esp_err_t discord_start(QueueHandle_t input_queue, QueueHandle_t output_queue)
{
    if (!discord_is_configured()) {
        ESP_LOGE(TAG, "Cannot start Discord: not configured");
        return ESP_ERR_INVALID_STATE;
    }

    s_input_queue = input_queue;
    s_output_queue = output_queue;
    s_running = true;

    // Start Discord task
    BaseType_t ret = xTaskCreate(discord_task, "discord",
                                   8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Discord task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Discord task started");
    return ESP_OK;
}

esp_err_t discord_send(const char *text)
{
    if (!s_output_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    discord_msg_t msg;
    utf8_safe_strlcpy(msg.text, sizeof(msg.text), text);
    strncpy(msg.channel_id, s_channel_id, sizeof(msg.channel_id) - 1);
    msg.channel_id[sizeof(msg.channel_id) - 1] = '\0';

    if (xQueueSend(s_output_queue, &msg, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send message to Discord queue");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t discord_send_startup(void)
{
    const char *msg = "🤖 zclaw is ready! M5Stack Core online.";
    return discord_send(msg);
}

// HTTP POST to send message to Discord
static esp_err_t discord_send_http(const char *text)
{
    if (!s_bot_token[0] || !s_channel_id[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    // Build URL: https://discord.com/api/v10/channels/{channel.id}/messages
    char url[256];
    snprintf(url, sizeof(url), "%s/channels/%s/messages",
             DISCORD_API_URL, s_channel_id);

    // Build JSON payload
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "content", text);

    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    // HTTP client configuration
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = DISCORD_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = DISCORD_HTTP_BUFFER_SIZE,
    };

    // Add authorization header
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bot %s", s_bot_token);

    TickType_t lock_ticks = pdMS_TO_TICKS(HTTP_GUARD_LOCK_TIMEOUT_MS);
    if (lock_ticks == 0) {
        lock_ticks = pdMS_TO_TICKS(1);
    }
    if (!net_http_guard_lock(lock_ticks)) {
        ESP_LOGW(TAG, "Skipping Discord send: shared HTTP lock timeout");
        free(json_str);
        return ESP_ERR_TIMEOUT;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        net_http_guard_unlock();
        free(json_str);
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    // Execute request
    ESP_LOGI(TAG, "Sending Discord message to %s", s_channel_id);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    // Read response for debugging (especially useful for 4xx error payloads).
    int content_length = esp_http_client_get_content_length(client);
    int response_cap = 1024;
    if (content_length > 0 && content_length < response_cap) {
        response_cap = content_length;
    }
    if (response_cap < 128) {
        response_cap = 128;
    }
    char *response = malloc((size_t)response_cap + 1);
    if (response) {
        int read_len = esp_http_client_read(client, response, response_cap);
        if (read_len < 0) {
            read_len = 0;
        }
        response[read_len] = '\0';
        if (read_len > 0) {
            if (status >= 200 && status < 300) {
                ESP_LOGI(TAG, "Discord response: status=%d, body=%s", status, response);
            } else {
                ESP_LOGW(TAG, "Discord error response: status=%d, body=%s", status, response);
            }
        }
        free(response);
    }

    esp_http_client_cleanup(client);
    net_http_guard_unlock();
    free(json_str);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Discord HTTP send failed: status=%d, err=0x%x", status, err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Discord message sent successfully");
    return ESP_OK;
}

// Poll for new messages from Discord channel
static esp_err_t discord_poll_messages(void)
{
    char url[256];
    if (s_last_message_id[0] != '\0') {
        // Get messages after the last processed one
        snprintf(url, sizeof(url), "%s/channels/%s/messages?after=%s&limit=%d",
                 DISCORD_API_URL, s_channel_id, s_last_message_id, DISCORD_POLL_LIMIT);
    } else {
        // First poll - get only the latest message to set cursor, don't process it
        snprintf(url, sizeof(url), "%s/channels/%s/messages?limit=%d",
                 DISCORD_API_URL, s_channel_id, DISCORD_POLL_LIMIT);
    }

    TickType_t lock_ticks = pdMS_TO_TICKS(150);
    if (lock_ticks == 0) {
        lock_ticks = pdMS_TO_TICKS(1);
    }
    if (!net_http_guard_lock(lock_ticks)) {
        ESP_LOGW(TAG, "Skipping Discord poll: shared HTTP lock busy");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Polling Discord for messages...");

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = DISCORD_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = DISCORD_HTTP_BUFFER_SIZE,
    };

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bot %s", s_bot_token);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        net_http_guard_unlock();
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Authorization", auth_header);

    // Open connection and fetch headers
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        net_http_guard_unlock();
        ESP_LOGW(TAG, "Failed to open HTTP connection: 0x%x", err);
        return ESP_FAIL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "Poll response: status=%d, content_len=%d", status, content_length);

    // Read response body
    char *response_data = NULL;
    int read_len = 0;
    int total_read = 0;
    bool truncated = false;

    // Bound response allocation to protect low-heap targets.
    int desired_size = (content_length > 0) ? content_length : DISCORD_MAX_POLL_RESPONSE_BYTES;
    int buffer_size = desired_size;
    if (buffer_size > DISCORD_MAX_POLL_RESPONSE_BYTES) {
        buffer_size = DISCORD_MAX_POLL_RESPONSE_BYTES;
    }
    if (buffer_size <= 0) {
        buffer_size = DISCORD_MAX_POLL_RESPONSE_BYTES;
    }

    response_data = malloc((size_t)buffer_size + 1);
    if (!response_data) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        net_http_guard_unlock();
        ESP_LOGW(TAG, "Failed to allocate response buffer");
        return ESP_FAIL;
    }

    // Read data in chunks
    int remaining = buffer_size;
    while (remaining > 0) {
        int len = esp_http_client_read(client, response_data + total_read,
                                       (remaining > 2048) ? 2048 : remaining);
        if (len <= 0) {
            break;
        }
        total_read += len;
        remaining -= len;
    }
    if (content_length < 0 || (content_length > buffer_size && total_read == buffer_size)) {
        truncated = true;
    }
    response_data[total_read] = '\0';
    read_len = total_read;

    ESP_LOGI(TAG, "Read %d bytes from Discord", read_len);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    net_http_guard_unlock();

    if (truncated) {
        ESP_LOGW(TAG, "Discord poll response capped at %d bytes", DISCORD_MAX_POLL_RESPONSE_BYTES);
    }

    if (status == 200 && read_len > 0) {
        cJSON *json = cJSON_Parse(response_data);
        if (json) {
            // Discord API returns array directly (newest first)
            if (cJSON_IsArray(json)) {
                int array_size = cJSON_GetArraySize(json);
                ESP_LOGI(TAG, "Discord JSON parsed: %d messages (%d bytes)", array_size, read_len);

                if (!s_first_poll_done) {
                    // On startup, only set the cursor to the newest seen message ID and skip processing
                    // the entire backlog to avoid executing stale commands.
                    if (array_size > 0) {
                        cJSON *newest = cJSON_GetArrayItem(json, 0);
                        cJSON *id_json = newest ? cJSON_GetObjectItem(newest, "id") : NULL;
                        if (id_json && cJSON_IsString(id_json) && id_json->valuestring) {
                            strncpy(s_last_message_id, id_json->valuestring, sizeof(s_last_message_id) - 1);
                            s_last_message_id[sizeof(s_last_message_id) - 1] = '\0';
                            ESP_LOGI(TAG, "First poll cursor initialized to latest message ID: %s",
                                     s_last_message_id);
                        }
                    }
                    s_first_poll_done = true;
                    ESP_LOGI(TAG, "First poll: backlog skipped");
                } else {
                    ESP_LOGI(TAG, "Processing %d messages from Discord", array_size);
                    // Process messages in reverse order (oldest first)
                    for (int i = array_size - 1; i >= 0; i--) {
                        cJSON *message = cJSON_GetArrayItem(json, i);
                        process_message(message);
                    }
                }
            } else {
                ESP_LOGW(TAG, "No array in Discord response");
            }
            cJSON_Delete(json);
        } else {
            ESP_LOGE(TAG, "Failed to parse Discord JSON (bytes=%d)", read_len);
        }
    } else if (status == 200 && !s_first_poll_done) {
        s_first_poll_done = true;
        ESP_LOGI(TAG, "First poll completed with empty body");
    }

    if (response_data) {
        free(response_data);
    }

    if (status != 200) {
        ESP_LOGW(TAG, "Poll failed: status=%d", status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

// Process a single Discord message
static void process_message(cJSON *message_json)
{
    if (!message_json || !s_input_queue) {
        ESP_LOGW(TAG, "process_message: invalid input");
        return;
    }

    ESP_LOGI(TAG, "Processing Discord message...");

    // Extract message data
    cJSON *content_json = cJSON_GetObjectItem(message_json, "content");
    cJSON *author_json = cJSON_GetObjectItem(message_json, "author");
    cJSON *bot_json = author_json ? cJSON_GetObjectItem(author_json, "bot") : NULL;
    cJSON *id_json = cJSON_GetObjectItem(message_json, "id");
    cJSON *username_json = author_json ? cJSON_GetObjectItem(author_json, "username") : NULL;

    // Step 1: Check if already processed
    if (id_json && id_json->valuestring) {
        if (s_last_message_id[0] != '\0' &&
            strcmp(id_json->valuestring, s_last_message_id) == 0) {
            ESP_LOGI(TAG, "Already processed message ID: %s", id_json->valuestring);
            return;
        }
    }

    // Step 2: Update last message ID (for both bot and user messages)
    if (id_json && id_json->valuestring) {
        strncpy(s_last_message_id, id_json->valuestring,
                sizeof(s_last_message_id) - 1);
        s_last_message_id[sizeof(s_last_message_id) - 1] = '\0';
        ESP_LOGI(TAG, "Updated last_message_id: %s", s_last_message_id);
    }

    // Step 3: Ignore bot messages (but ID is already updated)
    if (bot_json && cJSON_IsTrue(bot_json)) {
        ESP_LOGI(TAG, "Ignoring bot message");
        return;
    }

    // Log author info (for user messages)
    if (username_json && username_json->valuestring) {
        ESP_LOGI(TAG, "Discord message from user: %s", username_json->valuestring);
    }

    // Log content with more detail
    if (content_json) {
        if (content_json->valuestring) {
            ESP_LOGI(TAG, "Content: '%.80s' (len=%d)",
                     content_json->valuestring,
                     (int)strlen(content_json->valuestring));
        } else {
            ESP_LOGW(TAG, "Content field is null");
        }
    } else {
        ESP_LOGW(TAG, "Content field not found in JSON");
    }

    // Step 6: Skip empty messages
    if (!content_json || !content_json->valuestring || content_json->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "Skipping empty Discord message");
        return;
    }

    // Step 7: Process valid user message

    // Extract and queue the message content
    if (content_json && content_json->valuestring) {
        channel_msg_t msg = {0};
        strncpy(msg.text, content_json->valuestring, CHANNEL_RX_BUF_SIZE - 1);
        msg.text[CHANNEL_RX_BUF_SIZE - 1] = '\0';
        msg.source = MSG_SOURCE_DISCORD;
        msg.chat_id = 0;

        if (xQueueSend(s_input_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to queue message");
        } else {
            ESP_LOGI(TAG, "Queued Discord message: %.50s", msg.text);
        }
    }
}

// Discord task - handles polling and message processing
static void discord_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Discord task running (HTTP long polling mode)");

    // Initial delay to let WiFi connect
    vTaskDelay(pdMS_TO_TICKS(5000));

    TickType_t last_poll = 0;
    const TickType_t poll_interval = pdMS_TO_TICKS(DISCORD_POLL_INTERVAL_MS);

    while (s_running) {
        // Send pending messages from output queue
        discord_msg_t out_msg;
        while (xQueueReceive(s_output_queue, &out_msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "Sending to Discord: %s", out_msg.text);
            discord_send_http(out_msg.text);
        }

        // Poll for new messages
        TickType_t now = xTaskGetTickCount();
        if (now - last_poll >= poll_interval) {
            discord_poll_messages();
            last_poll = now;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    vTaskDelete(NULL);
}
