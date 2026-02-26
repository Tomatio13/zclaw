#ifndef DISCORD_H
#define DISCORD_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>

// Initialize Discord client
esp_err_t discord_init(void);

// Start Discord Gateway and message handler task
esp_err_t discord_start(QueueHandle_t input_queue, QueueHandle_t output_queue);

// Send a message to the configured channel
esp_err_t discord_send(const char *text);

// Send startup notification
esp_err_t discord_send_startup(void);

// Check if Discord is configured (token exists)
bool discord_is_configured(void);

// Get the configured channel ID
const char *discord_get_channel_id(void);

#endif // DISCORD_H
