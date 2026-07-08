#include "radar_r60abd1.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

/*
 * R60ABD1 60GHz 呼吸睡眠雷达 UART 协议解析。
 *
 * 帧格式: 53 59 | 控制字 | 命令字 | 长度(2B 大端) | 数据(nB) | 校验和 | 54 43
 * 校验和 = 帧头..数据 逐字节求和取低 8 位
 */

#define RADAR_UART_NUM UART_NUM_1
#define RADAR_UART_TX_GPIO 20 /* ESP 发 -> 雷达 RX */
#define RADAR_UART_RX_GPIO 21 /* ESP 收 <- 雷达 TX */
#define RADAR_UART_BAUD_RATE 115200
#define RADAR_UART_RX_BUFFER_SIZE 1024

#define RADAR_TASK_STACK_SIZE 4096
#define RADAR_TASK_PRIORITY 4
#define RADAR_TASK_CORE_ID 1

#define RADAR_FRAME_HEADER0 0x53
#define RADAR_FRAME_HEADER1 0x59
#define RADAR_FRAME_TAIL0 0x54
#define RADAR_FRAME_TAIL1 0x43
/* 目前上报帧数据段最长为睡眠综合帧(十几字节)，64 字节余量充足 */
#define RADAR_FRAME_MAX_DATA_LEN 64
/* 帧头2 + 控制字 + 命令字 + 长度2 + 数据n + 校验 + 帧尾2 */
#define RADAR_FRAME_OVERHEAD 9

#define RADAR_CTRL_HEARTBEAT 0x01
#define RADAR_CTRL_WORK_STATUS 0x05
#define RADAR_CTRL_PRESENCE 0x80
#define RADAR_CTRL_BREATH 0x81
#define RADAR_CTRL_SLEEP 0x84
#define RADAR_CTRL_HEART 0x85

#define RADAR_CMD_BREATH_STATE 0x01
#define RADAR_CMD_BREATH_VALUE 0x02
#define RADAR_CMD_HEART_VALUE 0x02
#define RADAR_CMD_INIT_DONE 0x01

typedef enum {
    RADAR_PARSE_HEADER0,
    RADAR_PARSE_HEADER1,
    RADAR_PARSE_CONTROL,
    RADAR_PARSE_COMMAND,
    RADAR_PARSE_LEN_H,
    RADAR_PARSE_LEN_L,
    RADAR_PARSE_DATA,
    RADAR_PARSE_CHECKSUM,
    RADAR_PARSE_TAIL0,
    RADAR_PARSE_TAIL1,
} radar_parse_state_t;

typedef struct {
    radar_parse_state_t state;
    uint8_t control;
    uint8_t command;
    uint16_t data_len;
    uint16_t data_pos;
    uint8_t data[RADAR_FRAME_MAX_DATA_LEN];
    uint8_t checksum; /* 累加中的校验和（低 8 位） */
    uint32_t frame_count;
    uint32_t error_count;
} radar_parser_t;

static const char *TAG = "radar_r60abd1";

static TaskHandle_t s_task_handle;
static portMUX_TYPE s_vital_lock = portMUX_INITIALIZER_UNLOCKED;
static radar_vital_signs_t s_vital_signs;

static const char *radar_breath_state_str(radar_breath_state_t state)
{
    switch (state) {
    case RADAR_BREATH_STATE_NORMAL:
        return "normal";
    case RADAR_BREATH_STATE_HIGH:
        return "too-high";
    case RADAR_BREATH_STATE_LOW:
        return "too-low";
    case RADAR_BREATH_STATE_NONE:
        return "none";
    default:
        return "unknown";
    }
}

static void radar_handle_frame(radar_parser_t *parser)
{
    const uint8_t *data = parser->data;

    switch (parser->control) {
    case RADAR_CTRL_HEART:
        if (parser->command == RADAR_CMD_HEART_VALUE && parser->data_len == 1) {
            portENTER_CRITICAL(&s_vital_lock);
            s_vital_signs.heart_rate = data[0];
            s_vital_signs.heart_rate_valid = true;
            portEXIT_CRITICAL(&s_vital_lock);
            ESP_LOGI(TAG, "heart rate: %u bpm", data[0]);
        }
        break;
    case RADAR_CTRL_BREATH:
        if (parser->command == RADAR_CMD_BREATH_VALUE && parser->data_len == 1) {
            portENTER_CRITICAL(&s_vital_lock);
            s_vital_signs.breath_rate = data[0];
            s_vital_signs.breath_rate_valid = true;
            portEXIT_CRITICAL(&s_vital_lock);
            ESP_LOGI(TAG, "breath rate: %u rpm", data[0]);
        } else if (parser->command == RADAR_CMD_BREATH_STATE && parser->data_len == 1) {
            portENTER_CRITICAL(&s_vital_lock);
            s_vital_signs.breath_state = (radar_breath_state_t)data[0];
            s_vital_signs.breath_state_valid = true;
            portEXIT_CRITICAL(&s_vital_lock);
            ESP_LOGI(TAG, "breath state: %s (0x%02x)",
                     radar_breath_state_str((radar_breath_state_t)data[0]), data[0]);
        }
        break;
    case RADAR_CTRL_WORK_STATUS:
        if (parser->command == RADAR_CMD_INIT_DONE) {
            ESP_LOGI(TAG, "radar module init done");
        }
        break;
    case RADAR_CTRL_HEARTBEAT:
        ESP_LOGD(TAG, "radar heartbeat");
        break;
    default:
        ESP_LOGD(TAG, "skip frame: control=0x%02x command=0x%02x len=%u",
                 parser->control, parser->command, parser->data_len);
        break;
    }
}

static void radar_parser_reset(radar_parser_t *parser)
{
    parser->state = RADAR_PARSE_HEADER0;
    parser->data_len = 0;
    parser->data_pos = 0;
    parser->checksum = 0;
}

static void radar_parser_error(radar_parser_t *parser, const char *reason, uint8_t byte)
{
    parser->error_count++;
    /* 前几次和之后每 64 次打印一次，避免坏连线时刷屏 */
    if (parser->error_count <= 3 || (parser->error_count % 64) == 0) {
        ESP_LOGW(TAG, "frame error (%s, byte=0x%02x, total errors=%lu)",
                 reason, byte, (unsigned long)parser->error_count);
    }
    radar_parser_reset(parser);
}

static void radar_parser_feed(radar_parser_t *parser, uint8_t byte)
{
    switch (parser->state) {
    case RADAR_PARSE_HEADER0:
        if (byte == RADAR_FRAME_HEADER0) {
            parser->checksum = byte;
            parser->state = RADAR_PARSE_HEADER1;
        }
        break;
    case RADAR_PARSE_HEADER1:
        if (byte == RADAR_FRAME_HEADER1) {
            parser->checksum += byte;
            parser->state = RADAR_PARSE_CONTROL;
        } else {
            /* 不算协议错误：可能只是数据流里出现了孤立的 0x53 */
            radar_parser_reset(parser);
            if (byte == RADAR_FRAME_HEADER0) {
                parser->checksum = byte;
                parser->state = RADAR_PARSE_HEADER1;
            }
        }
        break;
    case RADAR_PARSE_CONTROL:
        parser->control = byte;
        parser->checksum += byte;
        parser->state = RADAR_PARSE_COMMAND;
        break;
    case RADAR_PARSE_COMMAND:
        parser->command = byte;
        parser->checksum += byte;
        parser->state = RADAR_PARSE_LEN_H;
        break;
    case RADAR_PARSE_LEN_H:
        parser->data_len = (uint16_t)byte << 8;
        parser->checksum += byte;
        parser->state = RADAR_PARSE_LEN_L;
        break;
    case RADAR_PARSE_LEN_L:
        parser->data_len |= byte;
        parser->checksum += byte;
        if (parser->data_len > RADAR_FRAME_MAX_DATA_LEN) {
            radar_parser_error(parser, "data length too large", byte);
        } else {
            parser->data_pos = 0;
            parser->state = (parser->data_len > 0) ? RADAR_PARSE_DATA : RADAR_PARSE_CHECKSUM;
        }
        break;
    case RADAR_PARSE_DATA:
        parser->data[parser->data_pos++] = byte;
        parser->checksum += byte;
        if (parser->data_pos >= parser->data_len) {
            parser->state = RADAR_PARSE_CHECKSUM;
        }
        break;
    case RADAR_PARSE_CHECKSUM:
        if (byte == parser->checksum) {
            parser->state = RADAR_PARSE_TAIL0;
        } else {
            radar_parser_error(parser, "checksum mismatch", byte);
        }
        break;
    case RADAR_PARSE_TAIL0:
        if (byte == RADAR_FRAME_TAIL0) {
            parser->state = RADAR_PARSE_TAIL1;
        } else {
            radar_parser_error(parser, "bad tail0", byte);
        }
        break;
    case RADAR_PARSE_TAIL1:
        if (byte == RADAR_FRAME_TAIL1) {
            parser->frame_count++;
            radar_handle_frame(parser);
        } else {
            radar_parser_error(parser, "bad tail1", byte);
        }
        radar_parser_reset(parser);
        break;
    default:
        radar_parser_reset(parser);
        break;
    }
}

static void radar_rx_task(void *arg)
{
    static radar_parser_t parser;
    uint8_t chunk[128];

    radar_parser_reset(&parser);
    ESP_LOGI(TAG, "radar rx task started, waiting for frames");

    while (true) {
        const int len = uart_read_bytes(RADAR_UART_NUM, chunk, sizeof(chunk),
                                        pdMS_TO_TICKS(100));
        for (int i = 0; i < len; i++) {
            radar_parser_feed(&parser, chunk[i]);
        }
    }
}

esp_err_t radar_r60abd1_start(void)
{
    if (s_task_handle) {
        return ESP_OK;
    }

    const uart_config_t uart_config = {
        .baud_rate = RADAR_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(RADAR_UART_NUM, RADAR_UART_RX_BUFFER_SIZE,
                                            0, 0, NULL, 0),
                        TAG, "uart_driver_install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(RADAR_UART_NUM, &uart_config),
                        TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(RADAR_UART_NUM, RADAR_UART_TX_GPIO, RADAR_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");

    const BaseType_t created = xTaskCreatePinnedToCore(radar_rx_task,
                                                       "radar_rx",
                                                       RADAR_TASK_STACK_SIZE,
                                                       NULL,
                                                       RADAR_TASK_PRIORITY,
                                                       &s_task_handle,
                                                       RADAR_TASK_CORE_ID);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM,
                        TAG, "failed to create radar rx task");

    ESP_LOGI(TAG, "R60ABD1 radar started: UART%d, TX=GPIO%d, RX=GPIO%d, %d bps",
             RADAR_UART_NUM, RADAR_UART_TX_GPIO, RADAR_UART_RX_GPIO, RADAR_UART_BAUD_RATE);
    return ESP_OK;
}

esp_err_t radar_r60abd1_get_vital_signs(radar_vital_signs_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    portENTER_CRITICAL(&s_vital_lock);
    *out = s_vital_signs;
    portEXIT_CRITICAL(&s_vital_lock);
    return ESP_OK;
}
