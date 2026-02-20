#ifndef SYSTEM_MANAGER_H
#define SYSTEM_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

// Initialize system manager (NVS, Safe Mode check)
// Should be called early in app_main
void system_manager_init(void);

// Check if system is in Safe Mode
bool system_is_safe_mode(void);

// Report successful boot (clears crash counter)
// automatically called after N seconds by system_manager_init? 
// Or manually called? Let's make it automatic internal task/timer.
void system_mark_boot_successful(void);

// Get system health info (JSON)
char *system_get_health_json(void);

#endif // SYSTEM_MANAGER_H
