#include "skills/skill_rollback.h"
#include "skills/skill_engine.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "skill_rollback";

#define SKILL_BASE_DIR "/spiffs/skills"
#define ROLLBACK_DIR   "/spiffs/skills/.rollback"
#define MAX_BACKUPS    3

static void ensure_rollback_dir(const char *skill_name)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", ROLLBACK_DIR, skill_name);
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(ROLLBACK_DIR, 0755);
        mkdir(path, 0755);
    }
}

static esp_err_t copy_file(const char *src, const char *dst)
{
    FILE *f_src = fopen(src, "rb");
    if (!f_src) return ESP_FAIL;

    FILE *f_dst = fopen(dst, "wb");
    if (!f_dst) {
        fclose(f_src);
        return ESP_FAIL;
    }

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f_src)) > 0) {
        fwrite(buf, 1, n, f_dst);
    }

    fclose(f_src);
    fclose(f_dst);
    return ESP_OK;
}

esp_err_t skill_rollback_backup(const char *skill_name)
{
    char src_main[128], src_manifest[128];
    snprintf(src_main, sizeof(src_main), "%s/%s/main.lua", SKILL_BASE_DIR, skill_name);
    snprintf(src_manifest, sizeof(src_manifest), "%s/%s/manifest.json", SKILL_BASE_DIR, skill_name);

    struct stat st;
    if (stat(src_main, &st) != 0) {
        ESP_LOGW(TAG, "No existing skill '%s' to backup", skill_name);
        return ESP_OK; /* Nothing to backup is fine */
    }

    ensure_rollback_dir(skill_name);

    /* Generate timestamp version: YYYYMMDD_HHMMSS */
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char version[32];
    strftime(version, sizeof(version), "%Y%m%d_%H%M%S", &timeinfo);

    char dst_main[128], dst_manifest[128];
    snprintf(dst_main, sizeof(dst_main), "%s/%s/main.lua.%s", ROLLBACK_DIR, skill_name, version);
    snprintf(dst_manifest, sizeof(dst_manifest), "%s/%s/manifest.json.%s", ROLLBACK_DIR, skill_name, version);

    ESP_LOGI(TAG, "Backing up '%s' to version '%s'", skill_name, version);
    
    if (copy_file(src_main, dst_main) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to backup main.lua");
        return ESP_FAIL;
    }
    
    /* Manifest is optional but good to have */
    copy_file(src_manifest, dst_manifest);

    // TODO: implement pruning of old backups (MAX_BACKUPS)

    return ESP_OK;
}

esp_err_t skill_rollback_restore(const char *skill_name, const char *version)
{
    char src_main[128], src_manifest[128];
    snprintf(src_main, sizeof(src_main), "%s/%s/main.lua.%s", ROLLBACK_DIR, skill_name, version);
    snprintf(src_manifest, sizeof(src_manifest), "%s/%s/manifest.json.%s", ROLLBACK_DIR, skill_name, version);

    char dst_main[128], dst_manifest[128];
    snprintf(dst_main, sizeof(dst_main), "%s/%s/main.lua", SKILL_BASE_DIR, skill_name);
    snprintf(dst_manifest, sizeof(dst_manifest), "%s/%s/manifest.json", SKILL_BASE_DIR, skill_name);

    struct stat st;
    if (stat(src_main, &st) != 0) {
        ESP_LOGE(TAG, "Backup version '%s' not found for skill '%s'", version, skill_name);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Restoring '%s' from version '%s'", skill_name, version);

    if (copy_file(src_main, dst_main) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore main.lua");
        return ESP_FAIL;
    }
    
    copy_file(src_manifest, dst_manifest);

    /* Reload engine to pick up changes */
    return skill_engine_init();
}

char *skill_rollback_list_json(const char *skill_name)
{
    char dir_path[128];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", ROLLBACK_DIR, skill_name);

    DIR *d = opendir(dir_path);
    if (!d) return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON *backups = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "backups", backups);

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strstr(entry->d_name, "main.lua.")) {
            /* version is suffix after main.lua. */
            char *ver = entry->d_name + 9; /* strlen("main.lua.") */
            cJSON_AddItemToArray(backups, cJSON_CreateString(ver));
        }
    }
    closedir(d);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
