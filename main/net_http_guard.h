#ifndef NET_HTTP_GUARD_H
#define NET_HTTP_GUARD_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>

esp_err_t net_http_guard_init(void);
bool net_http_guard_lock(TickType_t timeout_ticks);
void net_http_guard_unlock(void);

#endif // NET_HTTP_GUARD_H
