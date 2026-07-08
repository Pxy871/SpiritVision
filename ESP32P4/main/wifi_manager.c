#include "wifi_manager.h"

#include <inttypes.h>
#include <stdio.h>

#include "app_secrets.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

/*
 * ESP32-P4 无 WiFi 射频，esp_wifi 调用经 esp_wifi_remote + esp_hosted(SDIO)
 * 透明转发到板载 ESP32-C6，标准 esp_wifi API 原样可用。
 */

#define WIFI_MANAGER_CONNECTED_BIT BIT0

/* 断线重连指数退避：1s 起步，每次翻倍，封顶 30s，拿到 IP 后复位 */
#define WIFI_MANAGER_BACKOFF_MIN_MS 1000
#define WIFI_MANAGER_BACKOFF_MAX_MS 30000

static const char *TAG = "wifi_manager";

static EventGroupHandle_t s_event_group;
static esp_netif_t *s_sta_netif;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_backoff_ms = WIFI_MANAGER_BACKOFF_MIN_MS;
static bool s_started;

static void wifi_manager_reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* 同步失败（如 esp_hosted 转发层瞬时故障）不会再产生 DISCONNECTED 事件，
         * 必须重新武装定时器，否则重连链条永久断掉 */
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s, retry in %" PRIu32 " ms",
                 esp_err_to_name(err), s_backoff_ms);
        err = esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to re-arm reconnect timer: %s", esp_err_to_name(err));
        }
    }
}

static void wifi_manager_event_handler(void *arg,
                                       esp_event_base_t event_base,
                                       int32_t event_id,
                                       void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            /* 同步失败无后续 DISCONNECTED 事件，交给定时器兜底重试 */
            ESP_LOGW(TAG, "initial esp_wifi_connect failed: %s, retry in %" PRIu32 " ms",
                     esp_err_to_name(err), s_backoff_ms);
            err = esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "failed to arm reconnect timer: %s", esp_err_to_name(err));
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
        xEventGroupClearBits(s_event_group, WIFI_MANAGER_CONNECTED_BIT);
        ESP_LOGW(TAG, "disconnected (reason=%u), reconnect in %" PRIu32 " ms",
                 event->reason, s_backoff_ms);
        /* 常驻设备不放弃：定时器到期后重连，避免阻塞默认事件循环任务 */
        esp_timer_stop(s_reconnect_timer); /* 未在计时则返回错误，忽略 */
        esp_err_t err = esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to arm reconnect timer: %s", esp_err_to_name(err));
        }
        s_backoff_ms *= 2;
        if (s_backoff_ms > WIFI_MANAGER_BACKOFF_MAX_MS) {
            s_backoff_ms = WIFI_MANAGER_BACKOFF_MAX_MS;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_backoff_ms = WIFI_MANAGER_BACKOFF_MIN_MS;
        xEventGroupSetBits(s_event_group, WIFI_MANAGER_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* WiFi 需要 NVS 存校准数据；无空页/版本不符时擦除后重试 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (%s)", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed");

    s_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_event_group, ESP_ERR_NO_MEM, TAG, "failed to create event group");

    const esp_timer_create_args_t timer_args = {
        .callback = wifi_manager_reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer),
                        TAG, "failed to create reconnect timer");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");

    err = esp_event_loop_create_default();
    /* 默认事件循环可能已由其他模块创建，已存在时复用 */
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err,
                        TAG, "esp_event_loop_create_default failed");

    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta_netif, ESP_FAIL, TAG, "failed to create default STA netif");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "esp_wifi_init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_manager_event_handler,
                                                            NULL,
                                                            NULL),
                        TAG, "failed to register WIFI_EVENT handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_manager_event_handler,
                                                            NULL,
                                                            NULL),
                        TAG, "failed to register IP_EVENT handler");

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = APP_WIFI_SSID,
            .password = APP_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH, /* WPA3 兼容 */
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config),
                        TAG, "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    s_started = true;
    ESP_LOGI(TAG, "WiFi STA started, ssid=%s", APP_WIFI_SSID);
    return ESP_OK;
}

esp_err_t wifi_manager_wait_connected(TickType_t timeout)
{
    if (!s_started || !s_event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           WIFI_MANAGER_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           timeout);
    return (bits & WIFI_MANAGER_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_get_ip_str(char *buf, size_t buf_len)
{
    ESP_RETURN_ON_FALSE(buf && buf_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid buffer");

    if (!s_sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info = {0};
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_sta_netif, &ip_info),
                        TAG, "esp_netif_get_ip_info failed");
    if (ip_info.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(buf, buf_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}
