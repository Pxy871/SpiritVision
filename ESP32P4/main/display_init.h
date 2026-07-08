#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_init(void);
esp_err_t display_show_color_bar(void);
esp_lcd_panel_handle_t display_get_panel_handle(void);
int display_get_h_res(void);
int display_get_v_res(void);
esp_err_t display_scan_control_i2c(void);
esp_err_t display_deinit(void);

#ifdef __cplusplus
}
#endif
