#include "skills/skill_rollback.h"
#include "mimi_config.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "esp_log.h"

#if CONFIG_MIMI_ENABLE_SKILLS
#include "skills/skill_engine.h"
#endif

static const char *TAG = "skill_rollback";

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Copy a file from src to dst. Returns ESP_OK on success. */
static esp_err_t copy_file(const char *src, const char *dst)
{
    FILE *fin = fopen(src, "r");
    if (!fin) return ESP_ERR_NOT_FOUND;

    FILE *fout = fopen(dst, "w");
    if (!fout) {
        fclose(fin);
        return ESP_FAIL;
    }

    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        fwrite(buf, 1, n, fout);
    }

    fclose(fin);
    fclose(fout);
    return ESP_OK;
}

/* Check if a file exists */
static bool file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0);
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t skill_rollback_backup(const char *name)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;

    /* Source: /spiffs/skills/<name>/main.lua */
    char src_lua[80], src_manifest[80];
    snprintf(src_lua, sizeof(src_lua), "%s/skills/%s/main.lua", MIMI_SPIFFS_BASE, name);
    snprintf(src_manifest, sizeof(src_manifest), "%s/skills/%s/manifest.json", MIMI_SPIFFS_BASE, name);

    /* Check source exists */
    if (!file_exists(src_lua)) {
        ESP_LOGW(TAG, "No main.lua for '%s', skip backup", name);
        return ESP_ERR_NOT_FOUND;
    }

    /* Create rollback dir: /spiffs/skills/.rb/<name>/ */
    char rb_dir[80];
    snprintf(rb_dir, sizeof(rb_dir), "%s/skills/.rb", MIMI_SPIFFS_BASE);
    mkdir(rb_dir, 0755);

    char rb_skill_dir[80];
    snprintf(rb_skill_dir, sizeof(rb_skill_dir), "%s/skills/.rb/%s", MIMI_SPIFFS_BASE, name);
    mkdir(rb_skill_dir, 0755);

    /* Copy main.lua */
    char dst_lua[96];
    snprintf(dst_lua, sizeof(dst_lua), "%s/main.lua", rb_skill_dir);
    esp_err_t err = copy_file(src_lua, dst_lua);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to backup main.lua for '%s'", name);
        return err;
    }

    /* Copy manifest.json if exists */
    if (file_exists(src_manifest)) {
        char dst_manifest[96];
        snprintf(dst_manifest, sizeof(dst_manifest), "%s/manifest.json", rb_skill_dir);
        copy_file(src_manifest, dst_manifest);
    }

    ESP_LOGI(TAG, "Backup created for skill '%s'", name);
    return ESP_OK;
}

esp_err_t skill_rollback_restore(const char *name)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;

    /* Source: /spiffs/skills/.rb/<name>/main.lua */
    char rb_lua[96], rb_manifest[96];
    snprintf(rb_lua, sizeof(rb_lua), "%s/skills/.rb/%s/main.lua", MIMI_SPIFFS_BASE, name);
    snprintf(rb_manifest, sizeof(rb_manifest), "%s/skills/.rb/%s/manifest.json", MIMI_SPIFFS_BASE, name);

    if (!file_exists(rb_lua)) {
        ESP_LOGW(TAG, "No rollback backup for '%s'", name);
        return ESP_ERR_NOT_FOUND;
    }

    /* Destination: /spiffs/skills/<name>/ */
    char dst_dir[80];
    snprintf(dst_dir, sizeof(dst_dir), "%s/skills/%s", MIMI_SPIFFS_BASE, name);
    mkdir(dst_dir, 0755);

    char dst_lua[96], dst_manifest[96];
    snprintf(dst_lua, sizeof(dst_lua), "%s/main.lua", dst_dir);
    snprintf(dst_manifest, sizeof(dst_manifest), "%s/manifest.json", dst_dir);

    esp_err_t err = copy_file(rb_lua, dst_lua);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore main.lua for '%s'", name);
        return err;
    }

    if (file_exists(rb_manifest)) {
        copy_file(rb_manifest, dst_manifest);
    }

    ESP_LOGI(TAG, "Skill '%s' restored from backup", name);

    /* Re-initialize skill engine to pick up restored skill */
#if CONFIG_MIMI_ENABLE_SKILLS
    skill_engine_init();
#endif

    return ESP_OK;
}

bool skill_rollback_exists(const char *name)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/skills/.rb/%s/main.lua", MIMI_SPIFFS_BASE, name);
    return file_exists(path);
}

char *skill_rollback_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    char rb_dir[80];
    snprintf(rb_dir, sizeof(rb_dir), "%s/skills/.rb", MIMI_SPIFFS_BASE);

    DIR *dir = opendir(rb_dir);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_DIR && ent->d_name[0] != '.') {
                /* Verify main.lua exists in this backup */
                char check[96];
                snprintf(check, sizeof(check), "%s/%s/main.lua", rb_dir, ent->d_name);
                if (file_exists(check)) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(ent->d_name));
                }
            }
        }
        closedir(dir);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}
