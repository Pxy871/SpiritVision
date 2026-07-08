#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 NVS/netif/默认事件循环并启动 WiFi STA（经 esp_hosted 走板载 ESP32-C6）。
 * 断线后内部事件驱动自动无限重连（指数退避 1s 起、封顶 30s，拿到 IP 后复位）。
 * 重复调用返回 ESP_OK。
 */
esp_err_t wifi_manager_start(void);

/**
 * 阻塞等待 WiFi 连接成功（拿到 IP）。
 *
 * @param timeout 最长等待节拍数，portMAX_DELAY 表示一直等。
 * @return ESP_OK 已连接；ESP_ERR_TIMEOUT 超时；ESP_ERR_INVALID_STATE 尚未 start。
 */
esp_err_t wifi_manager_wait_connected(TickType_t timeout);

/**
 * 获取当前 STA 的 IPv4 地址字符串（如 "192.168.1.100"）。
 *
 * @param buf 输出缓冲区（建议 >=16 字节）。
 * @param buf_len 缓冲区长度。
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未连接或未 start。
 */
esp_err_t wifi_manager_get_ip_str(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
