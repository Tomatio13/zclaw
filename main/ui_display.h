#ifndef UI_DISPLAY_H
#define UI_DISPLAY_H

#include "esp_err.h"
#include <stdbool.h>

// Initialize on-device display widgets (clock + thinking indicator).
esp_err_t ui_display_init(void);

// Toggle "Discord conversation in progress" indicator.
void ui_display_set_thinking(bool active);

#endif // UI_DISPLAY_H
