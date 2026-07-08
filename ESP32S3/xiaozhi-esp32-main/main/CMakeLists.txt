set(BOARD_TYPE "bread-compact-wifi-lcd")

set(SOURCES
    "audio_codecs/audio_codec.cc"
    "audio_codecs/no_audio_codec.cc"
    "led/single_led.cc"
    "display/display.cc"
    "display/lcd_display.cc"
    "protocols/protocol.cc"
    "protocols/mqtt_protocol.cc"
    "protocols/websocket_protocol.cc"
    "iot/thing.cc"
    "iot/thing_manager.cc"
    "iot/things/speaker.cc"
    "iot/things/screen.cc"
    "iot/things/lamp.cc"
    "boards/common/board.cc"
    "boards/common/wifi_board.cc"
    "boards/common/button.cc"
    "boards/common/backlight.cc"
    "boards/common/system_reset.cc"
    "boards/${BOARD_TYPE}/compact_wifi_board_lcd.cc"
    "system_info.cc"
    "application.cc"
    "ota.cc"
    "settings.cc"
    "background_task.cc"
    "main.cc"
)

set(INCLUDE_DIRS
    "."
    "display"
    "audio_codecs"
    "protocols"
    "audio_processing"
    "boards/common"
)

if(CONFIG_USE_AUDIO_PROCESSOR)
    list(APPEND SOURCES "audio_processing/afe_audio_processor.cc")
else()
    list(APPEND SOURCES "audio_processing/dummy_audio_processor.cc")
endif()

if(CONFIG_USE_WAKE_WORD_DETECT)
    list(APPEND SOURCES "audio_processing/wake_word_detect.cc")
endif()

if(CONFIG_LANGUAGE_ZH_CN)
    set(LANG_DIR "zh-CN")
elseif(CONFIG_LANGUAGE_ZH_TW)
    set(LANG_DIR "zh-TW")
elseif(CONFIG_LANGUAGE_EN_US)
    set(LANG_DIR "en-US")
elseif(CONFIG_LANGUAGE_JA_JP)
    set(LANG_DIR "ja-JP")
endif()

set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")
file(GLOB LANG_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/${LANG_DIR}/*.p3)
file(GLOB COMMON_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.p3)

idf_component_register(SRCS ${SOURCES}
                    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    )

set(BOARD_NAME ${BOARD_TYPE})
target_compile_definitions(${COMPONENT_LIB}
                    PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
                    )

add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND python ${PROJECT_DIR}/scripts/gen_lang.py
            --input "${LANG_JSON}"
            --output "${LANG_HEADER}"
    DEPENDS
        ${LANG_JSON}
        ${PROJECT_DIR}/scripts/gen_lang.py
    COMMENT "Generating ${LANG_DIR} language config"
)

add_custom_target(lang_header ALL
    DEPENDS ${LANG_HEADER}
)
