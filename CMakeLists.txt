# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Optional local SDK paths (set in shell):
#   export ADF_PATH=~/esp/esp-adf
#   export ESP_ZIGBEE_SDK_PATH=~/esp/esp-zigbee-sdk
if(DEFINED ENV{ADF_PATH} AND EXISTS "$ENV{ADF_PATH}")
    list(APPEND EXTRA_COMPONENT_DIRS
        "$ENV{ADF_PATH}/components"
        "$ENV{ADF_PATH}/extra_components"
    )
endif()

if(DEFINED ENV{ESP_ZIGBEE_SDK_PATH} AND EXISTS "$ENV{ESP_ZIGBEE_SDK_PATH}")
    list(APPEND EXTRA_COMPONENT_DIRS
        "$ENV{ESP_ZIGBEE_SDK_PATH}/components"
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mimiclaw)

spiffs_create_partition_image(spiffs spiffs_data FLASH_IN_PROJECT)
