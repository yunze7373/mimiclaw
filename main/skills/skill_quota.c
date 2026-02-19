#include "skills/skill_quota.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "skill_quota";

static skill_quota_entry_t s_entries[SKILL_QUOTA_MAX_ENTRIES];
static int s_entry_count = 0;
static int32_t s_total_disk_used = 0;

/* ── Internal helpers ─────────────────────────────────────────────── */

static skill_quota_entry_t *find_entry(const char *name)
{
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].name, name) == 0) return &s_entries[i];
    }
    return NULL;
}

static skill_quota_entry_t *find_or_create_entry(const char *name)
{
    skill_quota_entry_t *e = find_entry(name);
    if (e) return e;
    if (s_entry_count >= SKILL_QUOTA_MAX_ENTRIES) return NULL;

    e = &s_entries[s_entry_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->disk_limit  = SKILL_QUOTA_DEFAULT_DISK_LIMIT;
    e->heap_limit  = SKILL_QUOTA_DEFAULT_HEAP_LIMIT;
    e->instr_limit = SKILL_QUOTA_DEFAULT_INSTR_LIMIT;
    return e;
}

static int32_t clamp_i32(int32_t val, int32_t lo, int32_t hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static void recalc_total_disk(void)
{
    s_total_disk_used = 0;
    for (int i = 0; i < s_entry_count; i++) {
        s_total_disk_used += s_entries[i].disk_used;
    }
}

/* ── JSON persistence ─────────────────────────────────────────────── */

static esp_err_t load_from_file(void)
{
    FILE *f = fopen(SKILL_QUOTA_FILE, "r");
    if (!f) {
        ESP_LOGI(TAG, "No quota file, using defaults");
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 4096) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse quota JSON");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *skills = cJSON_GetObjectItem(root, "skills");
    if (cJSON_IsObject(skills)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, skills) {
            if (!cJSON_IsObject(item) || !item->string) continue;
            if (s_entry_count >= SKILL_QUOTA_MAX_ENTRIES) break;

            skill_quota_entry_t *e = find_or_create_entry(item->string);
            if (!e) continue;

            cJSON *v;
            v = cJSON_GetObjectItem(item, "disk_limit");
            if (cJSON_IsNumber(v)) e->disk_limit = clamp_i32((int32_t)v->valuedouble, 0, SKILL_QUOTA_MAX_DISK_LIMIT);
            v = cJSON_GetObjectItem(item, "disk_used");
            if (cJSON_IsNumber(v)) e->disk_used = (int32_t)v->valuedouble;
            v = cJSON_GetObjectItem(item, "heap_limit");
            if (cJSON_IsNumber(v)) e->heap_limit = clamp_i32((int32_t)v->valuedouble, 0, SKILL_QUOTA_MAX_HEAP_LIMIT);
            v = cJSON_GetObjectItem(item, "heap_peak");
            if (cJSON_IsNumber(v)) e->heap_peak = (int32_t)v->valuedouble;
            v = cJSON_GetObjectItem(item, "instr_limit");
            if (cJSON_IsNumber(v)) e->instr_limit = clamp_i32((int32_t)v->valuedouble, 0, SKILL_QUOTA_MAX_INSTR_LIMIT);
            v = cJSON_GetObjectItem(item, "instr_last");
            if (cJSON_IsNumber(v)) e->instr_last = (int32_t)v->valuedouble;
        }
    }

    cJSON_Delete(root);
    recalc_total_disk();
    ESP_LOGI(TAG, "Loaded %d quota entries, total disk used: %d bytes",
             s_entry_count, (int)s_total_disk_used);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t skill_quota_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_entry_count = 0;
    s_total_disk_used = 0;

    load_from_file();  /* OK if file doesn't exist yet */
    return ESP_OK;
}

esp_err_t skill_quota_check_disk(const char *skill_name, int32_t required_bytes)
{
    if (!skill_name || required_bytes <= 0) return ESP_ERR_INVALID_ARG;

    /* Check per-skill limit */
    const skill_quota_entry_t *e = find_entry(skill_name);
    int32_t per_skill_limit = e ? e->disk_limit : SKILL_QUOTA_DEFAULT_DISK_LIMIT;

    if (required_bytes > per_skill_limit) {
        ESP_LOGW(TAG, "Skill '%s' needs %d bytes but limit is %d",
                 skill_name, (int)required_bytes, (int)per_skill_limit);
        return ESP_ERR_NO_MEM;
    }

    /* Check total limit (subtract current skill usage for re-installs) */
    int32_t current_usage = e ? e->disk_used : 0;
    int32_t projected_total = s_total_disk_used - current_usage + required_bytes;
    if (projected_total > SKILL_QUOTA_TOTAL_DISK_LIMIT) {
        ESP_LOGW(TAG, "Total disk quota would exceed: %d > %d",
                 (int)projected_total, (int)SKILL_QUOTA_TOTAL_DISK_LIMIT);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void skill_quota_track_disk(const char *skill_name, int32_t bytes_used)
{
    if (!skill_name) return;
    skill_quota_entry_t *e = find_or_create_entry(skill_name);
    if (!e) return;

    e->disk_used = bytes_used;
    recalc_total_disk();

    /* Auto-save (best effort) */
    skill_quota_save();
}

int32_t skill_quota_get_instr_limit(const char *skill_name)
{
    const skill_quota_entry_t *e = find_entry(skill_name);
    return e ? e->instr_limit : SKILL_QUOTA_DEFAULT_INSTR_LIMIT;
}

int32_t skill_quota_get_heap_limit(const char *skill_name)
{
    const skill_quota_entry_t *e = find_entry(skill_name);
    return e ? e->heap_limit : SKILL_QUOTA_DEFAULT_HEAP_LIMIT;
}

void skill_quota_update_heap_peak(const char *skill_name, int32_t heap_used)
{
    if (!skill_name) return;
    skill_quota_entry_t *e = find_entry(skill_name);
    if (!e) return;
    if (heap_used > e->heap_peak) {
        e->heap_peak = heap_used;
    }
}

void skill_quota_update_instr(const char *skill_name, int32_t instr_used)
{
    if (!skill_name) return;
    skill_quota_entry_t *e = find_entry(skill_name);
    if (!e) return;
    e->instr_last = instr_used;
}

esp_err_t skill_quota_set_limits(const char *skill_name, int32_t disk_limit,
                                 int32_t heap_limit, int32_t instr_limit)
{
    if (!skill_name) return ESP_ERR_INVALID_ARG;
    skill_quota_entry_t *e = find_or_create_entry(skill_name);
    if (!e) return ESP_ERR_NO_MEM;

    if (disk_limit > 0) {
        e->disk_limit = clamp_i32(disk_limit, 1024, SKILL_QUOTA_MAX_DISK_LIMIT);
    }
    if (heap_limit > 0) {
        e->heap_limit = clamp_i32(heap_limit, 1024, SKILL_QUOTA_MAX_HEAP_LIMIT);
    }
    if (instr_limit > 0) {
        e->instr_limit = clamp_i32(instr_limit, 1000, SKILL_QUOTA_MAX_INSTR_LIMIT);
    }

    return skill_quota_save();
}

void skill_quota_remove(const char *skill_name)
{
    if (!skill_name) return;
    for (int i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].name, skill_name) == 0) {
            /* Shift remaining entries down */
            for (int j = i; j < s_entry_count - 1; j++) {
                s_entries[j] = s_entries[j + 1];
            }
            s_entry_count--;
            memset(&s_entries[s_entry_count], 0, sizeof(skill_quota_entry_t));
            recalc_total_disk();
            skill_quota_save();
            return;
        }
    }
}

esp_err_t skill_quota_save(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON *skills = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "skills", skills);

    for (int i = 0; i < s_entry_count; i++) {
        skill_quota_entry_t *e = &s_entries[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "disk_limit",  e->disk_limit);
        cJSON_AddNumberToObject(item, "disk_used",   e->disk_used);
        cJSON_AddNumberToObject(item, "heap_limit",  e->heap_limit);
        cJSON_AddNumberToObject(item, "heap_peak",   e->heap_peak);
        cJSON_AddNumberToObject(item, "instr_limit", e->instr_limit);
        cJSON_AddNumberToObject(item, "instr_last",  e->instr_last);
        cJSON_AddItemToObject(skills, e->name, item);
    }

    cJSON_AddNumberToObject(root, "total_disk_used",  s_total_disk_used);
    cJSON_AddNumberToObject(root, "total_disk_limit", SKILL_QUOTA_TOTAL_DISK_LIMIT);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return ESP_ERR_NO_MEM;

    FILE *f = fopen(SKILL_QUOTA_FILE, "w");
    if (!f) {
        free(str);
        ESP_LOGE(TAG, "Failed to open quota file for writing");
        return ESP_FAIL;
    }

    fputs(str, f);
    fclose(f);
    free(str);

    ESP_LOGD(TAG, "Quota saved (%d entries)", s_entry_count);
    return ESP_OK;
}

const skill_quota_entry_t *skill_quota_get(const char *skill_name)
{
    return find_entry(skill_name);
}

int32_t skill_quota_calc_dir_size(const char *path)
{
    if (!path) return -1;
    DIR *d = opendir(path);
    if (!d) return -1;

    int32_t total = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char full[256];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            int32_t sub = skill_quota_calc_dir_size(full);
            if (sub > 0) total += sub;
        } else {
            total += (int32_t)st.st_size;
        }
    }
    closedir(d);
    return total;
}
