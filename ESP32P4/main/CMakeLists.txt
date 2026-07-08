# 注意：main 组件不显式声明 REQUIRES/PRIV_REQUIRES，
# IDF 会自动让 main 依赖构建内全部组件（mqtt/esp-tls/nvs_flash/esp_wifi_remote 等），
# 一旦显式声明将关闭该自动依赖，需补全全部现有依赖。
idf_component_register(
    SRCS "main.c" "radar_r60abd1.c" "wifi_manager.c"
    INCLUDE_DIRS "."
)
