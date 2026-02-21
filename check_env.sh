#!/bin/bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
echo "ADF Path Configuration: \$MIMI_ENABLE_ADF / \$ADF_PATH"
echo "Zigbee Configuration: \$MIMI_ENABLE_ZIGBEE_SDK / \$ESP_ZIGBEE_SDK_PATH"
env | grep -iE 'adf|zigbee'
