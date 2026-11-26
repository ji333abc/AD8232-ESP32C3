set(sources main.c ecg_display.c ecg_filter.c ecg_qrs.c st7789.c)
set(dependencies esp_app_format esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_spi esp_driver_uart esp_timer)
idf_build_get_property(wifi_enabled ECG_WIFI)
if(wifi_enabled)
    list(APPEND sources ecg_web.c)
    list(APPEND dependencies esp_event esp_http_server esp_netif esp_wifi nvs_flash)
    set(web_asset EMBED_TXTFILES ecg.html)
endif()

idf_component_register(SRCS ${sources} PRIV_REQUIRES ${dependencies} ${web_asset})

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ECG_SIMULATE=$<BOOL:${ECG_SIMULATE}>
    ECG_DIAGNOSTICS=$<BOOL:${ECG_DIAGNOSTICS}>
    ECG_UART_OUTPUT=$<BOOL:${ECG_UART_OUTPUT}>
    ECG_WIFI=$<BOOL:${ECG_WIFI}>
    ECG_STARTUP_BUZZER=$<BOOL:${ECG_STARTUP_BUZZER}>)
