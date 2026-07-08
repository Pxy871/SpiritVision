#include "app_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "radar_r60abd1.h"

#if APP_ENABLE_WIFI
#include "wifi_manager.h"
#endif

static const char *TAG = "app_main";

void app_main(void)
{
    /* 初始化顺序：WiFi(含 NVS) -> 雷达 */
#if APP_ENABLE_WIFI
    ESP_LOGI(TAG, "Starting WiFi STA (esp_hosted, onboard ESP32-C6)");
    ESP_ERROR_CHECK(wifi_manager_start());
#else
    ESP_LOGI(TAG, "WiFi pipeline disabled (APP_ENABLE_WIFI=0)");
#endif

    ESP_LOGI(TAG, "Starting R60ABD1 mmWave radar vital signs monitor");
    ESP_ERROR_CHECK(radar_r60abd1_start());
}
