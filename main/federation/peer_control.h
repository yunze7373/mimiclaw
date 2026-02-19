#pragma once

#include "esp_err.h"

/**
 * Execute a tool on a remote peer via HTTP POST /api/tools/exec.
 *
 * @param target_ip   IP address of the remote peer (e.g. "192.168.1.105")
 * @param tool_name   Name of the tool to execute (e.g. "speak")
 * @param json_args   JSON string of arguments (e.g. "{\"text\":\"hello\"}")
 * @param output      Buffer to store the response/output
 * @param output_len  Size of the output buffer
 * @return ESP_OK on success, ESP_FAIL on HTTP error or connection failure
 */
esp_err_t peer_control_execute_tool(const char *target_ip, const char *tool_name, 
                                    const char *json_args, char *output, size_t output_len);
