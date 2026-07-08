#include "display_init.h"

#include "esp_check.h"
#include "esp_lcd_jd9365.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#define DISPLAY_H_RES 800
#define DISPLAY_V_RES 1280
#define DISPLAY_BITS_PER_PIXEL 16
#define DISPLAY_RESET_GPIO GPIO_NUM_NC
#define DISPLAY_MIPI_DSI_LANE_NUM 2
#define DISPLAY_MIPI_DSI_PHY_LDO_CHAN 3
#define DISPLAY_MIPI_DSI_PHY_LDO_VOLTAGE_MV 2500
#define DISPLAY_BACKLIGHT_I2C_PORT I2C_NUM_1
#define DISPLAY_BACKLIGHT_I2C_SDA_GPIO GPIO_NUM_7
#define DISPLAY_BACKLIGHT_I2C_SCL_GPIO GPIO_NUM_8
#define DISPLAY_BACKLIGHT_I2C_FREQ_HZ 100000
#define DISPLAY_BACKLIGHT_I2C_ADDR 0x45
#define DISPLAY_BACKLIGHT_I2C_TIMEOUT_MS 100

static const char *TAG = "display_init";

static esp_ldo_channel_handle_t s_mipi_phy_ldo;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_io_handle_t s_dbi_io;
static esp_lcd_panel_handle_t s_panel;
static bool s_display_initialized;

esp_err_t display_init(void)
{
    esp_err_t ret = ESP_OK;

    if (s_display_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Powering MIPI-DSI PHY LDO channel %d at %d mV",
             DISPLAY_MIPI_DSI_PHY_LDO_CHAN,
             DISPLAY_MIPI_DSI_PHY_LDO_VOLTAGE_MV);
    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = DISPLAY_MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = DISPLAY_MIPI_DSI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &s_mipi_phy_ldo),
                        TAG, "failed to power MIPI-DSI PHY");

    ESP_LOGI(TAG, "Creating MIPI-DSI bus");
    const esp_lcd_dsi_bus_config_t bus_config = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus),
                      fail, TAG, "failed to create MIPI-DSI bus");

    ESP_LOGI(TAG, "Creating DBI panel IO");
    const esp_lcd_dbi_io_config_t dbi_config = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &s_dbi_io),
                      fail, TAG, "failed to create DBI panel IO");

    ESP_LOGI(TAG, "Installing JD9365 10.1-inch DSI panel");
    const esp_lcd_dpi_panel_config_t dpi_config =
        JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    const jd9365_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = DISPLAY_MIPI_DSI_LANE_NUM,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = DISPLAY_BITS_PER_PIXEL,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_jd9365(s_dbi_io, &panel_config, &s_panel),
                      fail, TAG, "failed to install JD9365 panel");

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(s_panel), fail, TAG, "failed to reset panel");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(s_panel), fail, TAG, "failed to initialize panel");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),
                      fail, TAG, "failed to turn display on");
    ESP_GOTO_ON_ERROR(esp_lcd_dpi_panel_set_pattern(s_panel, MIPI_DSI_PATTERN_NONE),
                      fail, TAG, "failed to enable DPI frame buffer output");

    s_display_initialized = true;
    ESP_LOGI(TAG, "Display initialized: %dx%d RGB565, %d-lane MIPI-DSI",
             DISPLAY_H_RES,
             DISPLAY_V_RES,
             DISPLAY_MIPI_DSI_LANE_NUM);
    return ESP_OK;

fail:
    display_deinit();
    return ret;
}

esp_err_t display_show_color_bar(void)
{
    ESP_RETURN_ON_FALSE(s_display_initialized, ESP_ERR_INVALID_STATE,
                        TAG, "display is not initialized");

    ESP_LOGI(TAG, "Showing hardware vertical color bar pattern");
    return esp_lcd_dpi_panel_set_pattern(s_panel, MIPI_DSI_PATTERN_BAR_VERTICAL);
}

esp_lcd_panel_handle_t display_get_panel_handle(void)
{
    return s_panel;
}

int display_get_h_res(void)
{
    return DISPLAY_H_RES;
}

int display_get_v_res(void)
{
    return DISPLAY_V_RES;
}

esp_err_t display_scan_control_i2c(void)
{
    esp_err_t ret = ESP_OK;
    esp_err_t cleanup_ret = ESP_OK;
    i2c_master_bus_handle_t bus_handle = NULL;
    bool found = false;

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = DISPLAY_BACKLIGHT_I2C_PORT,
        .sda_io_num = DISPLAY_BACKLIGHT_I2C_SDA_GPIO,
        .scl_io_num = DISPLAY_BACKLIGHT_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_GOTO_ON_ERROR(i2c_new_master_bus(&bus_config, &bus_handle),
                      cleanup, TAG, "create display I2C scan bus failed");

    ESP_LOGI(TAG, "Scanning display control I2C bus: port=%d, scl=%d, sda=%d",
             DISPLAY_BACKLIGHT_I2C_PORT,
             DISPLAY_BACKLIGHT_I2C_SCL_GPIO,
             DISPLAY_BACKLIGHT_I2C_SDA_GPIO);

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t probe_ret = i2c_master_probe(bus_handle, addr,
                                               DISPLAY_BACKLIGHT_I2C_TIMEOUT_MS);
        if (probe_ret == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02x", addr);
            found = true;
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "No I2C device found on display control bus");
    }

cleanup:
    if (bus_handle) {
        esp_err_t err = i2c_del_master_bus(bus_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "delete display I2C scan bus failed: %s", esp_err_to_name(err));
            cleanup_ret = err;
        }
    }

    return (ret != ESP_OK) ? ret : cleanup_ret;
}

esp_err_t display_deinit(void)
{
    esp_err_t ret = ESP_OK;

    if (s_panel) {
        esp_err_t err = esp_lcd_panel_del(s_panel);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to delete panel: %s", esp_err_to_name(err));
            ret = err;
        }
        s_panel = NULL;
    }

    if (s_dbi_io) {
        esp_err_t err = esp_lcd_panel_io_del(s_dbi_io);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to delete DBI IO: %s", esp_err_to_name(err));
            ret = err;
        }
        s_dbi_io = NULL;
    }

    if (s_dsi_bus) {
        esp_err_t err = esp_lcd_del_dsi_bus(s_dsi_bus);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to delete DSI bus: %s", esp_err_to_name(err));
            ret = err;
        }
        s_dsi_bus = NULL;
    }

    if (s_mipi_phy_ldo) {
        esp_err_t err = esp_ldo_release_channel(s_mipi_phy_ldo);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to release MIPI PHY LDO: %s", esp_err_to_name(err));
            ret = err;
        }
        s_mipi_phy_ldo = NULL;
    }

    s_display_initialized = false;
    return ret;
}
