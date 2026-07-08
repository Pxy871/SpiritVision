#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* R60ABD1 呼吸信息（控制字 0x81 / 命令字 0x01）取值 */
typedef enum {
    RADAR_BREATH_STATE_NORMAL = 0x01, /* 正常 */
    RADAR_BREATH_STATE_HIGH = 0x02,   /* 呼吸过高 */
    RADAR_BREATH_STATE_LOW = 0x03,    /* 呼吸过低 */
    RADAR_BREATH_STATE_NONE = 0x04,   /* 无（未检测到呼吸） */
} radar_breath_state_t;

/* 雷达最近一次上报的生命体征快照 */
typedef struct {
    uint8_t heart_rate;                /* 心率，次/分，0~100，3s 更新 */
    uint8_t breath_rate;               /* 呼吸，次/分，0~25，3s 更新 */
    radar_breath_state_t breath_state; /* 呼吸状态，变化时更新 */
    bool heart_rate_valid;             /* 上电后是否收到过心率帧 */
    bool breath_rate_valid;            /* 上电后是否收到过呼吸帧 */
    bool breath_state_valid;           /* 上电后是否收到过呼吸状态帧 */
} radar_vital_signs_t;

/**
 * 初始化 UART 并启动雷达接收解析任务。
 * 收到心率/呼吸帧时通过 ESP_LOGI 打印。重复调用返回 ESP_OK。
 */
esp_err_t radar_r60abd1_start(void);

/** 获取最近一次解析到的生命体征数据（线程安全的快照拷贝）。 */
esp_err_t radar_r60abd1_get_vital_signs(radar_vital_signs_t *out);

#ifdef __cplusplus
}
#endif
