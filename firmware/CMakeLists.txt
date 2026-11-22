# Wired Switchboard firmware - ESP32-C6 + LAN8651 10BASE-T1S, Matter + MQTT
#
# Requires: ESP-IDF v6.0.2, esp-matter release/v1.6.1 (ESP_MATTER_PATH) with
# patches/0001-esp32-ethernet-app-init-hook.patch applied to connectedhomeip.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Please set ESP_MATTER_PATH to the path of the esp-matter repo")
endif()

# We drive LEDs/buttons ourselves; use the empty device HAL on every target.
if(NOT DEFINED ENV{ESP_MATTER_DEVICE_PATH})
    set(ENV{ESP_MATTER_DEVICE_PATH} $ENV{ESP_MATTER_PATH}/device_hal/device/hollow)
endif()

set(PROJECT_VER "0.2.0")
set(PROJECT_VER_NUMBER 2)

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

# Fail early if the Ethernet hook patch is missing (see README).
file(READ ${MATTER_SDK_PATH}/src/platform/ESP32/NetworkCommissioningDriver_Ethernet.cpp _eth_drv)
string(FIND "${_eth_drv}" "esp_matter_app_ethernet_init" _hook)
if(_hook EQUAL -1)
    message(FATAL_ERROR "connectedhomeip is missing the Ethernet app-init hook. Run:\n"
        "  git -C ${MATTER_SDK_PATH} apply ${CMAKE_CURRENT_LIST_DIR}/patches/0001-esp32-ethernet-app-init-hook.patch")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)

set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/examples/common"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    "${ESP_MATTER_PATH}/device_hal/device"
    ${extra_components_dirs_append})

project(wired_switchboard)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
