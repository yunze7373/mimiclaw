#pragma once

#include "esp_err.h"

/**
 * MCP Tool for Agent
 * Allows Agent to configure MCP sources via natural language
 */

esp_err_t tool_mcp_add(const char *input_json, char *output, size_t output_size);

esp_err_t tool_mcp_list(const char *input_json, char *output, size_t output_size);

esp_err_t tool_mcp_remove(const char *input_json, char *output, size_t output_size);

esp_err_t tool_mcp_action(const char *input_json, char *output, size_t output_size);
