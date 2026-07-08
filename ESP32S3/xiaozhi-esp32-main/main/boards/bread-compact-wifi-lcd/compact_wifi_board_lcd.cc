#include "wifi_board.h"
#include "audio_codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "led/single_led.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include <driver/uart.h>
#include <esp_timer.h>
#include <cstring>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "CompactWifiBoardLCD"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);

// 自定义LcdDisplay类，用于监听情绪变化
class EmotionAwareLcdDisplay : public SpiLcdDisplay
{
private:
    std::function<void(const char *)> emotion_callback_;

public:
    EmotionAwareLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy,
                           DisplayFonts fonts)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy, fonts) {}

    void SetEmotionCallback(std::function<void(const char *)> callback)
    {
        emotion_callback_ = callback;
    }

    virtual void SetEmotion(const char *emotion) override
    {
        SpiLcdDisplay::SetEmotion(emotion);
        // 通知板子情绪发生了变化
        if (emotion_callback_ && emotion)
        {
            emotion_callback_(emotion);
        }
    }
};

class CompactWifiBoardLCD : public WifiBoard
{
private:
    Button boot_button_;
    EmotionAwareLcdDisplay *display_;
    bool is_speaking_ = false;                // 记录当前是否在说话状态
    std::string current_emotion_ = "neutral"; // 记录当前情绪状态
    bool wake_word_detected_ = false;         // 记录是否检测到唤醒词
    bool virtual_human_page_active_ = false;  // 记录是否已切换到虚拟人页面
    bool ai_has_responded_ = false;           // 记录AI是否已经开始回应

    void InitializeSpi()
    {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeUartScreen()
    {
        uart_config_t uart_config = {
            .baud_rate = UART_SCREEN_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_ERROR_CHECK(uart_driver_install(UART_SCREEN_PORT_NUM, UART_SCREEN_BUF_SIZE * 2, 0, 0, NULL, 0));
        ESP_ERROR_CHECK(uart_param_config(UART_SCREEN_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(UART_SCREEN_PORT_NUM, UART_SCREEN_TXD, UART_SCREEN_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_LOGI(TAG, "UART screen initialized on port %d, TX: GPIO%d, RX: GPIO%d",
                 UART_SCREEN_PORT_NUM, UART_SCREEN_TXD, UART_SCREEN_RXD);
    }

    void InitializeUartMcu()
    {
        uart_config_t uart_config = {
            .baud_rate = UART_MCU_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_ERROR_CHECK(uart_driver_install(UART_MCU_PORT_NUM, UART_MCU_BUF_SIZE * 2, 0, 0, NULL, 0));
        ESP_ERROR_CHECK(uart_param_config(UART_MCU_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(UART_MCU_PORT_NUM, UART_MCU_TXD, UART_MCU_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_LOGI(TAG, "UART MCU initialized on port %d, TX: GPIO%d, RX: GPIO%d",
                 UART_MCU_PORT_NUM, UART_MCU_TXD, UART_MCU_RXD);
    }

    void SendScreenCommand(const char *command)
    {
        if (command != nullptr)
        {
            // 使用uart_write_bytes向串口屏发送命令
            int len = strlen(command);
            int written = uart_write_bytes(UART_SCREEN_PORT_NUM, command, len);
            if (written == len)
            {
                ESP_LOGI(TAG, "Sent screen command: %s", command);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to send screen command: %s (written: %d/%d)", command, written, len);
            }
        }
    }

    void SendMcuCommand(const char *command)
    {
        if (command != nullptr)
        {
            // 使用uart_write_bytes向单片机发送命令
            int len = strlen(command);
            int written = uart_write_bytes(UART_MCU_PORT_NUM, command, len);
            if (written == len)
            {
                ESP_LOGI(TAG, "Sent MCU command: %s", command);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to send MCU command: %s (written: %d/%d)", command, written, len);
            }
        }
    }

    int ReadMcuData(char *buffer, int max_len, int timeout_ms = 1000)
    {
        if (buffer == nullptr || max_len <= 0)
        {
            return -1;
        }

        int len = uart_read_bytes(UART_MCU_PORT_NUM, buffer, max_len - 1, pdMS_TO_TICKS(timeout_ms));
        if (len > 0)
        {
            buffer[len] = '\0'; // 添加字符串结束符
            ESP_LOGI(TAG, "Received MCU data (%d bytes): %s", len, buffer);
        }
        else if (len == 0)
        {
            ESP_LOGD(TAG, "MCU read timeout");
        }
        else
        {
            ESP_LOGE(TAG, "MCU read error: %d", len);
        }
        return len;
    }

    void ProcessMcuToScreenForward()
    {
        static char mcu_buffer[512] = {0};  // 静态缓冲区用于累积数据
        static int buffer_pos = 0;          // 当前缓冲区位置
        static uint32_t last_data_time = 0; // 上次接收数据的时间

        // 检查缓冲区超时（2秒没有新数据就清空）
        uint32_t current_time = esp_timer_get_time() / 1000; // 毫秒
        if (buffer_pos > 0 && (current_time - last_data_time) > 2000)
        {
            ESP_LOGW(TAG, "MCU buffer timeout, clearing %d bytes", buffer_pos);
            buffer_pos = 0;
            mcu_buffer[0] = '\0';
        }

        char temp_buffer[256];
        int len = uart_read_bytes(UART_MCU_PORT_NUM, temp_buffer, sizeof(temp_buffer) - 1, pdMS_TO_TICKS(10)); // 短超时，非阻塞读取

        if (len > 0)
        {
            last_data_time = current_time; // 更新最后接收时间
            ESP_LOGI(TAG, "MCU raw data received (%d bytes)", len);

            // 显示接收到的原始数据（十六进制）
            char hex_str[512] = {0};
            for (int i = 0; i < len && i < 50; i++)
            {
                sprintf(hex_str + strlen(hex_str), "%02X ", (unsigned char)temp_buffer[i]);
            }
            ESP_LOGI(TAG, "Raw hex data: %s", hex_str);

            // 将新数据追加到缓冲区
            if (buffer_pos + len < sizeof(mcu_buffer) - 1)
            {
                memcpy(mcu_buffer + buffer_pos, temp_buffer, len);
                buffer_pos += len;

                // 移除末尾的回车换行符
                while (buffer_pos > 0 && (mcu_buffer[buffer_pos - 1] == '\r' || mcu_buffer[buffer_pos - 1] == '\n'))
                {
                    buffer_pos--;
                }
                mcu_buffer[buffer_pos] = '\0';

                ESP_LOGI(TAG, "MCU buffer content (%d bytes): %.*s", buffer_pos, buffer_pos, mcu_buffer);

                // 查找完整的命令（支持两种格式的结尾）
                bool found_end = false;
                int cmd_len = 0;

                // 方法1：查找真正的\xff\xff\xff字节
                for (int i = 0; i <= buffer_pos - 3; i++)
                {
                    if ((unsigned char)mcu_buffer[i] == 0xFF &&
                        (unsigned char)mcu_buffer[i + 1] == 0xFF &&
                        (unsigned char)mcu_buffer[i + 2] == 0xFF)
                    {
                        found_end = true;
                        cmd_len = i + 3;
                        ESP_LOGI(TAG, "Found binary \\xff\\xff\\xff at position %d", i);
                        break;
                    }
                }

                // 方法2：查找文本格式的\xff\xff\xff并转换
                if (!found_end)
                {
                    char *text_end = strstr(mcu_buffer, "\\xff\\xff\\xff");
                    if (text_end != nullptr)
                    {
                        found_end = true;
                        ESP_LOGI(TAG, "Found text \\xff\\xff\\xff at position %d", (int)(text_end - mcu_buffer));

                        // 将文本格式的\xff\xff\xff替换为真正的二进制字节
                        text_end[0] = 0xFF;
                        text_end[1] = 0xFF;
                        text_end[2] = 0xFF;

                        // 移除多余的字符（原来是12个字符，现在是3个字节）
                        int remaining_after_marker = buffer_pos - (text_end - mcu_buffer) - 12;
                        if (remaining_after_marker > 0)
                        {
                            memmove(text_end + 3, text_end + 12, remaining_after_marker);
                            buffer_pos = buffer_pos - 9; // 减少9个字节（12-3=9）
                        }

                        cmd_len = text_end - mcu_buffer + 3;
                        ESP_LOGI(TAG, "Converted text format to binary, new buffer size: %d", buffer_pos);
                    }
                }

                if (found_end)
                {
                    // 提取完整命令
                    char command[512];
                    memcpy(command, mcu_buffer, cmd_len);
                    command[cmd_len] = '\0';

                    // 检查是否是parameters命令格式
                    if (strstr(command, "parameters.") != nullptr)
                    {
                        ESP_LOGI(TAG, "MCU->Screen forward: %s", command);
                        // 原封不动转发给串口屏
                        SendScreenCommand(command);
                    }
                    else
                    {
                        ESP_LOGI(TAG, "Received MCU data (not parameters command): %s", command);
                    }

                    // 移除已处理的命令，保留剩余数据
                    int remaining = buffer_pos - cmd_len;
                    if (remaining > 0)
                    {
                        memmove(mcu_buffer, mcu_buffer + cmd_len, remaining);
                        buffer_pos = remaining;
                    }
                    else
                    {
                        buffer_pos = 0;
                    }
                }

                // 如果没有找到结束标记，但缓冲区有完整的命令，尝试强制处理
                if (!found_end && buffer_pos > 20)
                {
                    // 检查是否有parameters命令但没有正确的结束标记
                    if (strstr(mcu_buffer, "parameters.") != nullptr)
                    {
                        ESP_LOGI(TAG, "Found parameters command without proper end marker, processing anyway");

                        // 查找最后一个引号，在其后添加结束标记
                        char *last_quote = strrchr(mcu_buffer, '"');
                        if (last_quote != nullptr)
                        {
                            // 创建完整命令
                            char command[300];
                            int base_len = last_quote - mcu_buffer + 1;
                            memcpy(command, mcu_buffer, base_len);
                            command[base_len] = 0xFF;
                            command[base_len + 1] = 0xFF;
                            command[base_len + 2] = 0xFF;
                            command[base_len + 3] = '\0';

                            ESP_LOGI(TAG, "Auto-fixed command: %.*s", base_len, command);
                            SendScreenCommand(command);

                            // 清空缓冲区
                            buffer_pos = 0;
                            mcu_buffer[0] = '\0';
                            found_end = true;
                        }
                    }
                }

                if (!found_end && buffer_pos > 100)
                {
                    ESP_LOGW(TAG, "No end marker found in %d bytes, checking for timeout", buffer_pos);
                }
            }
            else
            {
                // 缓冲区溢出，重置
                ESP_LOGW(TAG, "MCU buffer overflow, resetting");
                buffer_pos = 0;
            }
        }
    }

    int GetEmotionVideoId(const std::string &emotion)
    {
        // 情绪到视频ID的映射 - 只区分平和和高兴
        if (emotion == "happy" || emotion == "laughing" || emotion == "funny" || emotion == "loving" || emotion == "winking" || emotion == "cool" || emotion == "confident" || emotion == "kissy")
        {
            return 2; // 高兴
        }
        else
        {
            return 1; // 平和（默认）
        }
    }

    void UpdateVirtualHumanScreen(const std::string &emotion, bool speaking)
    {
        // 检查是否需要切换到虚拟人页面（AI开始说话时）
        if (speaking && !virtual_human_page_active_)
        {
            SendScreenCommand("page virtual_human\xff\xff\xff");
            virtual_human_page_active_ = true;
            ai_has_responded_ = true;
            ESP_LOGI(TAG, "AI started speaking, switched to virtual_human page");
        }

        // 更新视频状态
        if (emotion != current_emotion_ || speaking != is_speaking_)
        {
            current_emotion_ = emotion;
            is_speaking_ = speaking;

            if (speaking && virtual_human_page_active_)
            {
                int video_id = GetEmotionVideoId(emotion);
                char command[64];
                snprintf(command, sizeof(command), "virtual_human.v0.vid=%d\xff\xff\xff", video_id);
                SendScreenCommand(command);
                ESP_LOGI(TAG, "Set virtual human video: %s -> vid=%d", emotion.c_str(), video_id);
            }
            else if (!speaking && virtual_human_page_active_)
            {
                SendScreenCommand("virtual_human.v0.vid=0\xff\xff\xff"); // 停止播放
                ESP_LOGI(TAG, "Stop virtual human video");
            }
        }
    }

    void OnWakeWordDetected()
    {
        wake_word_detected_ = true;
        ai_has_responded_ = false;          // 重置AI回应状态
        virtual_human_page_active_ = false; // 重置页面状态
        ESP_LOGI(TAG, "Wake word detected, waiting for AI response (page_active=%s)",
                 virtual_human_page_active_ ? "true" : "false");
        // 不立即切换页面，等待AI开始说话时再切换
    }

    void OnConversationEnd()
    {
        if (virtual_human_page_active_)
        {
            SendScreenCommand("page home\xff\xff\xff"); // 切换回主页
            virtual_human_page_active_ = false;
            ai_has_responded_ = false;
            ESP_LOGI(TAG, "Conversation ended, switched to home page");
        }
    }

public:
    void OnEmotionChanged(const char *emotion)
    {
        if (emotion != nullptr)
        {
            current_emotion_ = emotion;
            ESP_LOGI(TAG, "Emotion changed to: %s", emotion);

            // 如果已经检测到唤醒词，立即更新虚拟人视频
            if (wake_word_detected_)
            {
                auto &app = Application::GetInstance();
                bool speaking = (app.GetDeviceState() == kDeviceStateSpeaking);
                ESP_LOGI(TAG, "OnEmotionChanged: wake_word_detected=%s, speaking=%s, virtual_human_page_active=%s",
                         wake_word_detected_ ? "true" : "false",
                         speaking ? "true" : "false",
                         virtual_human_page_active_ ? "true" : "false");
                UpdateVirtualHumanScreen(current_emotion_, speaking);
            }
        }
    }

private:
    void InitializeLcdDisplay()
    {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new EmotionAwareLcdDisplay(panel_io, panel,
                                              DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                              {
                                                  .text_font = &font_puhui_16_4,
                                                  .icon_font = &font_awesome_16_4,
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
                                                  .emoji_font = font_emoji_32_init(),
#else
                                                  .emoji_font = DISPLAY_HEIGHT >= 240 ? font_emoji_64_init() : font_emoji_32_init(),
#endif
                                              });

        // 设置情绪变化回调
        display_->SetEmotionCallback([this](const char *emotion)
                                     { this->OnEmotionChanged(emotion); });
    }

    void InitializeButtons()
    {
        boot_button_.OnClick([this]()
                             {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState(); });

        // 添加长按BOOT键进行功能测试
        boot_button_.OnLongPress([this]()
                                 {
            ESP_LOGI(TAG, "Manual test: Testing MCU to Screen forwarding");

            // 直接测试串口屏命令
            ESP_LOGI(TAG, "Direct screen test...");
            SendScreenCommand("parameters.t1.txt=\"Direct Test\"\xff\xff\xff");

            vTaskDelay(pdMS_TO_TICKS(2000));

            // 模拟单片机发送parameters命令
            ESP_LOGI(TAG, "Simulating MCU sending parameters command...");

            // 模拟几种不同的parameters命令
            const char* test_commands[] = {
                "parameters.t0.txt=\"Hello Screen\"\xff\xff\xff",
                "parameters.t1.txt=\"Test Message\"\xff\xff\xff",
                "parameters.t2.txt=\"MCU Data\"\xff\xff\xff",
                "parameters.t3.txt=\"转发测试\"\xff\xff\xff"
            };

            for (int i = 0; i < 4; i++) {
                ESP_LOGI(TAG, "Test %d: Simulating MCU command: %s", i+1, test_commands[i]);
                // 模拟从MCU接收到数据并转发
                SendScreenCommand(test_commands[i]);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            ESP_LOGI(TAG, "MCU to Screen forwarding test completed"); });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot()
    {
        auto &thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
        thing_manager.AddThing(iot::CreateThing("Lamp"));
    }

    void SetupStateMonitoring()
    {
        // 创建一个定时器来定期检查设备状态和情绪变化
        esp_timer_create_args_t timer_args = {
            .callback = [](void *arg)
            {
                auto *board = static_cast<CompactWifiBoardLCD *>(arg);
                auto &app = Application::GetInstance();
                bool speaking = (app.GetDeviceState() == kDeviceStateSpeaking);
                auto device_state = app.GetDeviceState();

                // 检测唤醒词：当设备从idle状态变为connecting/listening状态时
                static DeviceState last_state = kDeviceStateIdle;
                if (!board->wake_word_detected_ &&
                    last_state == kDeviceStateIdle &&
                    (device_state == kDeviceStateConnecting || device_state == kDeviceStateListening))
                {
                    board->OnWakeWordDetected();
                }
                last_state = device_state;

                // 只有检测到唤醒词后才控制虚拟人视频
                if (board->wake_word_detected_)
                {
                    // 添加调试日志
                    static bool last_speaking_state = false;
                    if (speaking != last_speaking_state)
                    {
                        ESP_LOGI(TAG, "Speaking state changed: %s -> %s",
                                 last_speaking_state ? "true" : "false",
                                 speaking ? "true" : "false");
                        last_speaking_state = speaking;
                    }

                    // 使用虚拟人控制模式
                    board->UpdateVirtualHumanScreen(board->current_emotion_, speaking);
                }

                // 当设备回到idle状态时，结束对话并重置标志
                if (device_state == kDeviceStateIdle && board->wake_word_detected_)
                {
                    board->OnConversationEnd();
                    board->wake_word_detected_ = false;
                    ESP_LOGI(TAG, "Reset wake word detection flag");
                }

                // 处理MCU到串口屏的数据转发
                board->ProcessMcuToScreenForward();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "screen_state_monitor"};
        esp_timer_handle_t timer_handle;
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle, 100000)); // 每100ms检查一次
        ESP_LOGI(TAG, "Screen state monitoring started");
    }

public:
    CompactWifiBoardLCD() : boot_button_(BOOT_BUTTON_GPIO)
    {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeUartScreen();
        InitializeUartMcu(); // 初始化单片机UART2
        InitializeButtons();
        InitializeIot();
        SetupStateMonitoring();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC)
        {
            GetBacklight()->RestoreBrightness();
        }
    }

    virtual Led *GetLed() override
    {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec *GetAudioCodec() override
    {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                               AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                              AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display *GetDisplay() override
    {
        return display_;
    }

    virtual Backlight *GetBacklight() override
    {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC)
        {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }
};

DECLARE_BOARD(CompactWifiBoardLCD);
