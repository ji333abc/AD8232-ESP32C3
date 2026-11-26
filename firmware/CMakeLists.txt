cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET esp32c3)
option(ECG_SIMULATE "Generate a test waveform instead of reading ADS1115" OFF)
option(ECG_DIAGNOSTICS "Send periodic diagnostic lines on UART0" OFF)
option(ECG_UART_OUTPUT "Send raw ADC codes on UART0" OFF)
option(ECG_WIFI "Serve a live ECG page over the onboard Wi-Fi hotspot" ON)
option(ECG_STARTUP_BUZZER "Play the power-on beeps" ON)

if(ECG_WIFI)
    # Reconfigure when a local Wi-Fi configuration is added or removed.
    file(GLOB wifi_local_config CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/main/ecg_wifi.local.h")
    if(NOT wifi_local_config)
        set(ECG_WIFI OFF)
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
idf_build_set_property(ECG_WIFI ${ECG_WIFI})
project(ad8232_ecg VERSION 0.4.1)
