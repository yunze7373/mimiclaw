# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# ADF compatibility fallback for boards without SD 4-bit/CD/WP pin definitions.
# -1 means "unused pin" in ESP-IDF SDMMC context.
add_compile_definitions(
    ESP_SD_PIN_D4=-1
    ESP_SD_PIN_D5=-1
    ESP_SD_PIN_D6=-1
    ESP_SD_PIN_D7=-1
    ESP_SD_PIN_CD=-1
    ESP_SD_PIN_WP=-1
)

# Optional local SDK paths (set in shell):
#   export MIMI_ENABLE_ADF=1
#   export ADF_PATH=~/esp/esp-adf
#   export ESP_ZIGBEE_SDK_PATH=~/esp/esp-zigbee-sdk
if(DEFINED ENV{MIMI_ENABLE_ADF} AND "$ENV{MIMI_ENABLE_ADF}" STREQUAL "1" AND DEFINED ENV{ADF_PATH} AND EXISTS "$ENV{ADF_PATH}")
    if(EXISTS "$ENV{ADF_PATH}/components")
        list(APPEND EXTRA_COMPONENT_DIRS "$ENV{ADF_PATH}/components")
    endif()
    if(EXISTS "$ENV{ADF_PATH}/extra_components")
        list(APPEND EXTRA_COMPONENT_DIRS "$ENV{ADF_PATH}/extra_components")
    endif()
endif()

if(DEFINED ENV{ESP_ZIGBEE_SDK_PATH} AND EXISTS "$ENV{ESP_ZIGBEE_SDK_PATH}")
    list(APPEND EXTRA_COMPONENT_DIRS
        "$ENV{ESP_ZIGBEE_SDK_PATH}/components"
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mimiclaw)

spiffs_create_partition_image(spiffs spiffs_data FLASH_IN_PROJECT)
