#pragma once

/*
 * 应用级功能开关。
 *
 * APP_ENABLE_WIFI: WiFi(板载 ESP32-C6, esp_hosted/SDIO) 联网链路。
 * 置 0 后 wifi_manager 仍参与编译，但不会初始化和运行。
 */
#ifndef APP_ENABLE_WIFI
#define APP_ENABLE_WIFI 1
#endif
