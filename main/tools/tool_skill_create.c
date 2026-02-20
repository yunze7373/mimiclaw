#include "tools/tool_skill_create.h"
#include "mimi_config.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "esp_log.h"
#include "skills/skill_rollback.h"

#if CONFIG_MIMI_ENABLE_SKILLS
#include "skills/skill_engine.h"
#endif

static const char *TAG = "tool_skill_create";
/* SPIFFS object name limit is 32, and "/skills/<name>/manifest.json" must fit. */
#define SKILL_FS_NAME_MAX 9

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Validate skill name: lowercase alpha, digits, underscores only */
static bool is_valid_name(const char *name)
{
    if (!name || !name[0] || strlen(name) > 32) return false;
    for (int i = 0; name[i]; i++) {
        if (!isalnum((unsigned char)name[i]) && name[i] != '_') return false;
    }
    return true;
}

static uint8_t name_hash8(const char *s)
{
    uint8_t h = 0x5a;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h = (uint8_t)((h << 1) | (h >> 7));
    }
    return h;
}

/* Keep names compatible with SPIFFS object-name limit constraints. */
static void to_fs_skill_name(const char *in, char out[SKILL_FS_NAME_MAX + 1])
{
    size_t len = strlen(in);
    if (len <= SKILL_FS_NAME_MAX) {
        snprintf(out, SKILL_FS_NAME_MAX + 1, "%s", in);
        return;
    }

    uint8_t h = name_hash8(in);
    /* 6 chars + '_' + 2 hex chars = 9 chars */
    snprintf(out, SKILL_FS_NAME_MAX + 1, "%.6s_%02x", in, (unsigned)h);
}

static bool has_unqualified_call(const char *code, const char *func_name)
{
    if (!code || !func_name || !func_name[0]) return false;
    const char *p = code;
    size_t name_len = strlen(func_name);
    while ((p = strstr(p, func_name)) != NULL) {
        char prev = (p == code) ? '\0' : *(p - 1);
        /* treat identifier chars and '.' as qualified/embedded usage */
        if (!(isalnum((unsigned char)prev) || prev == '_' || prev == '.')) {
            const char *q = p + name_len;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            if (*q == '(') return true;
        }
        p += name_len;
    }
    return false;
}

/* Reject legacy/unsupported Lua APIs to prevent crash-loop skill generation. */
static const char *detect_unsupported_api(const char *code)
{
    if (!code) return "empty skill code";
    if (strstr(code, "mimi.")) return "unsupported namespace 'mimi'";
    if (strstr(code, "rgb.")) return "unsupported namespace 'rgb'";
    if (strstr(code, "timer_start(") || strstr(code, ".timer_start(")) return "unsupported API 'timer_start'";
    if (has_unqualified_call(code, "i2s_read")) return "unsupported global call 'i2s_read' (use hw.i2s_read)";
    if (has_unqualified_call(code, "i2s_write")) return "unsupported global call 'i2s_write'";
    if (has_unqualified_call(code, "rgb_control")) return "unsupported global call 'rgb_control'";
    if (has_unqualified_call(code, "gpio_control")) return "unsupported global call 'gpio_control'";
    return NULL;
}

/* Write a file to SPIFFS, creating parent dir if needed */
static esp_err_t write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    fputs(content, f);
    fclose(f);
    return ESP_OK;
}

/* Generate manifest.json from metadata */
static char *generate_manifest(const char *name, const char *desc,
                               const char *category, const char *type,
                               const char *bus)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "version", "1.0.0");
    cJSON_AddStringToObject(root, "description", desc ? desc : "");
    cJSON_AddStringToObject(root, "author", "agent");
    cJSON_AddStringToObject(root, "entry", "main.lua");

    cJSON *cls = cJSON_CreateObject();
    cJSON_AddStringToObject(cls, "category", category ? category : "software");
    cJSON_AddStringToObject(cls, "type", type ? type : "utility");
    cJSON_AddStringToObject(cls, "bus", bus ? bus : "none");
    cJSON_AddItemToObject(root, "classification", cls);

    cJSON *perms = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "permissions", perms);

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    return json;
}

/* ── skill_create tool ───────────────────────────────────────────── */

esp_err_t tool_skill_create_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: Invalid JSON input");
        return ESP_FAIL;
    }

    cJSON *j_name = cJSON_GetObjectItem(root, "name");
    cJSON *j_code = cJSON_GetObjectItem(root, "code");
    cJSON *j_desc = cJSON_GetObjectItem(root, "description");
    cJSON *j_cat  = cJSON_GetObjectItem(root, "category");
    cJSON *j_type = cJSON_GetObjectItem(root, "type");
    cJSON *j_bus  = cJSON_GetObjectItem(root, "bus");

    if (!cJSON_IsString(j_name) || !cJSON_IsString(j_code)) {
        snprintf(output, output_size, "Error: 'name' and 'code' are required strings");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const char *name = j_name->valuestring;
    const char *code = j_code->valuestring;
    char skill_name[SKILL_FS_NAME_MAX + 1] = {0};

    /* 1. Validate name */
    if (!is_valid_name(name)) {
        snprintf(output, output_size,
                 "Error: Invalid name '%s'. Use lowercase letters, digits, underscores (max 32 chars).",
                 name);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    to_fs_skill_name(name, skill_name);
    if (strcmp(name, skill_name) != 0) {
        ESP_LOGW(TAG, "Skill name '%s' normalized to '%s' due to SPIFFS path length limit", name, skill_name);
    }

    const char *unsupported = detect_unsupported_api(code);
    if (unsupported) {
        snprintf(output, output_size,
                 "Error: %s. Use only sandbox APIs: hw.*, console.log, agent.emit_event, struct.* "
                 "(timers: hw.timer_every/hw.timer_once/hw.timer_cancel).",
                 unsupported);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 2. Create skill directory */
    char dir_path[80];
    snprintf(dir_path, sizeof(dir_path), "%s/skills/%s", MIMI_SPIFFS_BASE, skill_name);
    mkdir(dir_path, 0755);  /* May fail if exists, that's OK on SPIFFS */

    /* 2b. Backup existing skill before overwriting */
    char existing_lua[96];
    snprintf(existing_lua, sizeof(existing_lua), "%s/main.lua", dir_path);
    struct stat st;
    if (stat(existing_lua, &st) == 0) {
        ESP_LOGI(TAG, "Backing up existing skill '%s' before overwrite", skill_name);
        skill_rollback_backup(skill_name);
    }

    /* 3. Write main.lua */
    char lua_path[96];
    snprintf(lua_path, sizeof(lua_path), "%s/main.lua", dir_path);
    if (write_file(lua_path, code) != ESP_OK) {
        snprintf(output, output_size, "Error: Failed to write %s", lua_path);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* 4. Generate and write manifest.json */
    char *manifest = generate_manifest(
        skill_name,
        cJSON_IsString(j_desc) ? j_desc->valuestring : "",
        cJSON_IsString(j_cat)  ? j_cat->valuestring  : NULL,
        cJSON_IsString(j_type) ? j_type->valuestring  : NULL,
        cJSON_IsString(j_bus)  ? j_bus->valuestring   : NULL
    );
    if (manifest) {
        char manifest_path[96];
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", dir_path);
        if (write_file(manifest_path, manifest) != ESP_OK) {
            free(manifest);
            snprintf(output, output_size, "Error: Failed to write %s (skill name may be too long)", manifest_path);
            cJSON_Delete(root);
            return ESP_FAIL;
        }
        free(manifest);
    }

    ESP_LOGI(TAG, "Skill '%s' written to %s", skill_name, dir_path);

    /* 5. Hot-reload: re-init skill engine to pick up new skill */
#if CONFIG_MIMI_ENABLE_SKILLS
    esp_err_t err = skill_engine_init();
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "Skill '%s' files saved but reload failed: %s. "
                 "Check Lua syntax and try 'skill_reload' from CLI.",
                 skill_name, esp_err_to_name(err));
        cJSON_Delete(root);
        return ESP_OK;  /* Files saved, just reload issue */
    }
#endif

    snprintf(output, output_size,
             "Skill '%s' created and loaded successfully. "
             "Files: %s/main.lua, %s/manifest.json",
             skill_name, dir_path, dir_path);

    cJSON_Delete(root);
    return ESP_OK;
}

/* ── skill_list_templates tool ───────────────────────────────────── */

typedef struct {
    const char *name;
    const char *desc;
    const char *category;
    const char *type;
    const char *bus;
} template_info_t;

static const char *s_template_content[] = {
    /* i2c_sensor */
    "SKILL = {\n"
    "    name = \"i2c_sensor_demo\",\n"
    "    version = \"1.0.0\",\n"
    "    author = \"agent\",\n"
    "    description = \"Reads generic I2C register\",\n"
    "    classification = { category=\"hardware\", type=\"sensor\", bus=\"i2c\" },\n"
    "    permissions = { i2c={\"i2c0\"} }\n"
    "}\n\n"
    "function read_reg(reg)\n"
    "    -- Assuming i2c0 is configured. addr=0x40 example\n"
    "    local dev_addr = 0x40\n"
    "    local i2c_num = 0\n"
    "    -- Write register address\n"
    "    local ok = hw.i2c_init(\"i2c0\")\n"
    "    if not ok then return nil, \"i2c init failed\" end\n"
    "    -- Read 1 byte from register\n"
    "    local data = hw.i2c_read(\"i2c0\", dev_addr, reg, 1)\n"
    "    if not data then return nil, \"read failed\" end\n"
    "    return string.byte(data, 1)\n"
    "end\n\n"
    "TOOLS = {\n"
    "    {\n"
    "        name = \"read_value\",\n"
    "        description = \"Read sensor value from register\",\n"
    "        parameters = { type=\"object\", properties={ reg={type=\"integer\"} }, required={\"reg\"} },\n"
    "        handler = function(args)\n"
    "            local val, err = read_reg(args.reg)\n"
    "            if err then return { error=err } end\n"
    "            return { value=val }\n"
    "        end\n"
    "    }\n"
    "}",

    /* gpio_control */
    "SKILL = {\n"
    "    name = \"gpio_toggle\",\n"
    "    version = \"1.0.0\",\n"
    "    author = \"agent\",\n"
    "    description = \"Control GPIO pin\",\n"
    "    classification = { category=\"hardware\", type=\"actuator\", bus=\"gpio\" },\n"
    "    permissions = { gpio={\"18\"} } -- Example pin\n"
    "}\n\n"
    "local PIN = 18\n\n"
    "function set_state(on)\n"
    "    hw.gpio_set_mode(PIN, \"output\")\n"
    "    hw.gpio_write(PIN, on and 1 or 0)\n"
    "    return true\n"
    "end\n\n"
    "TOOLS = {\n"
    "    {\n"
    "        name = \"set_led\",\n"
    "        description = \"Turn LED on or off\",\n"
    "        parameters = { type=\"object\", properties={ on={type=\"boolean\"} }, required={\"on\"} },\n"
    "        handler = function(args)\n"
    "            set_state(args.on)\n"
    "            return { ok=true, state=args.on }\n"
    "        end\n"
    "    }\n"
    "}",

    /* timer_service */
    "SKILL = {\n"
    "    name = \"timer_demo\",\n"
    "    version = \"1.0.0\",\n"
    "    author = \"agent\",\n"
    "    description = \"Runs a task every 5 seconds\",\n"
    "    classification = { category=\"software\", type=\"service\", bus=\"none\" }\n"
    "}\n\n"
    "local count = 0\n\n"
    "function on_timer()\n"
    "    count = count + 1\n"
    "    print(\"Timer tick: \" .. count)\n"
    "end\n\n"
    "-- Start periodic timer every 5000ms\n"
    "local timer_id = hw.timer_every(5000, on_timer)\n\n"
    "TOOLS = {\n"
    "    {\n"
    "        name = \"get_count\",\n"
    "        description = \"Get current timer tick count\",\n"
    "        parameters = { type=\"object\", properties={}, required={} },\n"
    "        handler = function(args)\n"
    "            return { count=count }\n"
    "        end\n"
    "    }\n"
    "}"
};

static const template_info_t s_templates[] = {
    {
        .name     = "i2c_sensor",
        .desc     = "I2C sensor driver template. Reads registers from an I2C device.",
        .category = "hardware",
        .type     = "sensor",
        .bus      = "i2c",
    },
    {
        .name     = "gpio_control",
        .desc     = "GPIO input/output template. Read a pin and write to another.",
        .category = "hardware",
        .type     = "actuator",
        .bus      = "gpio",
    },
    {
        .name     = "timer_service",
        .desc     = "Software timer/service template. Runs periodic tasks.",
        .category = "software",
        .type     = "service",
        .bus      = "none",
    },
};

esp_err_t tool_skill_list_templates_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *arr = cJSON_CreateArray();
    int count = sizeof(s_templates) / sizeof(s_templates[0]);

    for (int i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", s_templates[i].name);
        cJSON_AddStringToObject(obj, "description", s_templates[i].desc);
        cJSON_AddStringToObject(obj, "category", s_templates[i].category);
        cJSON_AddStringToObject(obj, "type", s_templates[i].type);
        cJSON_AddStringToObject(obj, "bus", s_templates[i].bus);
        cJSON_AddItemToArray(arr, obj);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json) {
        snprintf(output, output_size, "%s", json);
        free(json);
    } else {
        snprintf(output, output_size, "[]");
    }

    return ESP_OK;
}

esp_err_t tool_skill_get_template_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: Invalid JSON input");
        return ESP_FAIL;
    }
    
    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(name) || !name->valuestring || !name->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'name' parameter required");
        return ESP_FAIL;
    }

    /* Copy before deleting root to avoid use-after-free on error paths. */
    char template_name[64];
    snprintf(template_name, sizeof(template_name), "%s", name->valuestring);
    
    int idx = -1;
    int count = sizeof(s_templates) / sizeof(s_templates[0]);
    for(int i=0; i<count; i++) {
        if (strcmp(s_templates[i].name, template_name) == 0) {
            idx = i;
            break;
        }
    }
    
    cJSON_Delete(root);
    
    if (idx < 0) {
        snprintf(output, output_size, "Error: Template '%s' not found", template_name);
        return ESP_OK; /* Not a system error, just not found */
    }

    int template_content_count = (int)(sizeof(s_template_content) / sizeof(s_template_content[0]));
    if (idx >= template_content_count || !s_template_content[idx]) {
        snprintf(output, output_size, "{\"error\":\"template content missing\"}");
        return ESP_FAIL;
    }
    
    /* Return JSON object with code */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "name", s_templates[idx].name);
    cJSON_AddStringToObject(resp, "code", s_template_content[idx]);
    
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    
    if (json) {
        snprintf(output, output_size, "%s", json);
        free(json);
    } else {
        snprintf(output, output_size, "{\"error\":\"out of memory\"}");
    }
    
    return ESP_OK;
}
