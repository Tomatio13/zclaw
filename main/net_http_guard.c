#include "net_http_guard.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_http_mutex = NULL;

esp_err_t net_http_guard_init(void)
{
    if (s_http_mutex) {
        return ESP_OK;
    }

    s_http_mutex = xSemaphoreCreateMutex();
    if (!s_http_mutex) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool net_http_guard_lock(TickType_t timeout_ticks)
{
    if (!s_http_mutex) {
        return false;
    }

    return xSemaphoreTake(s_http_mutex, timeout_ticks) == pdTRUE;
}

void net_http_guard_unlock(void)
{
    if (s_http_mutex) {
        xSemaphoreGive(s_http_mutex);
    }
}
