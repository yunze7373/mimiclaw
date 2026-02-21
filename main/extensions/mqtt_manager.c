#include "mqtt_manager.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "mimi_config.h"
#include "mimi_secrets.h"
#include "skills/skill_engine.h"
#include "skills/skill_types.h"
#include "tools/tool_registry.h"
#include "component/component_mgr.h" // For getting device ID if available (or use mac)
#include "esp_mac.h"
#include <string.h>

static const char *TAG = "mqtt_mgr";

static esp_mqtt_client_handle_t s_client = NULL;
static char s_device_id[13]; // MAC address hex string
static char s_topic_prefix[64] = "mimiclaw";

// Defines for HA Discovery
#define HA_DISCOVERY_PREFIX "homeassistant"

// --- Helper Functions ---

static void get_device_id(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void sanitize_entity_id(const char *name, char *out, size_t size)
{
    size_t i = 0;
    while (name[i] && i < size - 1) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            out[i] = c;
        } else if (c >= 'A' && c <= 'Z') {
            out[i] = c + 32;
        } else {
            out[i] = '_';
        }
        i++;
    }
    out[i] = '\0';
}

// --- HA Discovery ---

static void publish_ha_discovery(void)
{
    ESP_LOGI(TAG, "Publishing HA Discovery payloads...");

    for (int i = 0; i < SKILL_MAX_SLOTS; i++) {
        const skill_slot_t *slot = skill_engine_get_slot(i);
        if (!slot) continue;

        char safe_name[32];
        sanitize_entity_id(slot->name, safe_name, sizeof(safe_name));

        const char *component = NULL;
        if (slot->category == SKILL_CAT_SENSOR) component = "sensor";
        else if (slot->category == SKILL_CAT_ACTUATOR) component = "switch";
        
        if (!component) continue;

        // Topic: homeassistant/<component>/mimiclaw_<deviceid>/<skill_name>/config
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/%s/mimiclaw_%s/%s/config", 
                 HA_DISCOVERY_PREFIX, component, s_device_id, safe_name);

        // Payload
        cJSON *root = cJSON_CreateObject();
        
        char name_buf[64];
        snprintf(name_buf, sizeof(name_buf), "Esp32Claw %s %s", s_device_id, safe_name);
        cJSON_AddStringToObject(root, "name", name_buf);
        
        char uniq_id[64];
        snprintf(uniq_id, sizeof(uniq_id), "mimiclaw_%s_%s", s_device_id, safe_name);
        cJSON_AddStringToObject(root, "unique_id", uniq_id);
        
        // Device Info
        cJSON *dev = cJSON_CreateObject();
        cJSON_AddStringToObject(dev, "identifiers", s_device_id);
        cJSON_AddStringToObject(dev, "name", "Esp32Claw S3");
        cJSON_AddStringToObject(dev, "manufacturer", "Esp32Claw");
        cJSON_AddItemToObject(root, "device", dev);

        // State / Command Topics
        char state_topic[128];
        snprintf(state_topic, sizeof(state_topic), "%s/%s/%s/state", s_topic_prefix, s_device_id, safe_name);
        cJSON_AddStringToObject(root, "state_topic", state_topic);

        if (strcmp(component, "switch") == 0) {
            char cmd_topic[128];
            snprintf(cmd_topic, sizeof(cmd_topic), "%s/%s/%s/set", s_topic_prefix, s_device_id, safe_name);
            cJSON_AddStringToObject(root, "command_topic", cmd_topic);
        }

        char *json = cJSON_PrintUnformatted(root);
        esp_mqtt_client_publish(s_client, topic, json, 0, 1, 1);
        cJSON_Delete(root);
        free(json);
    }
}

// --- Command Handling ---

// Topic: mimiclaw/<device_id>/<skill_safe_name>/set
static void handle_mqtt_command(const char *topic, int topic_len, const char *data, int data_len)
{
    // Parse topic to get skill name
    // Format: mimiclaw/DEVICE_ID/SKILL_NAME/set
    
    char topic_buf[128];
    if (topic_len >= sizeof(topic_buf)) return;
    memcpy(topic_buf, topic, topic_len);
    topic_buf[topic_len] = '\0';

    // Crude parsing
    // Skip prefix + device_id
    // "mimiclaw/aabbccddeeff/" -> 9 + 12 + 1 = 22 chars
    char prefix_check[32];
    snprintf(prefix_check, sizeof(prefix_check), "%s/%s/", s_topic_prefix, s_device_id);
    
    char *p = strstr(topic_buf, prefix_check);
    if (!p) return;
    
    char *skill_part = p + strlen(prefix_check); // "skill_name/set"
    char *slash = strchr(skill_part, '/');
    if (!slash) return;
    
    *slash = '\0'; // skill_part is now just "skill_name"
    char *suffix = slash + 1; // "set"
    
    if (strcmp(suffix, "set") != 0) return;

    ESP_LOGI(TAG, "Command for skill: %s", skill_part);

    // Find the skill slot
    const skill_slot_t *target_slot = NULL;
    for (int i = 0; i < SKILL_MAX_SLOTS; i++) {
        const skill_slot_t *slot = skill_engine_get_slot(i);
        if (slot) {
            char safe_name[32];
            sanitize_entity_id(slot->name, safe_name, sizeof(safe_name));
            if (strcmp(safe_name, skill_part) == 0) {
                target_slot = slot;
                break;
            }
        }
    }

    if (!target_slot || target_slot->tool_count == 0) {
        ESP_LOGW(TAG, "Skill not found or has no tools: %s", skill_part);
        return;
    }

    // Prepare Arguments
    char payload_str[32];
    int copy_len = data_len < sizeof(payload_str)-1 ? data_len : sizeof(payload_str)-1;
    memcpy(payload_str, data, copy_len);
    payload_str[copy_len] = '\0';

    // Construct JSON args for tool
    // If payload is "ON" -> {"state": "ON"} ? 
    // Or just pass as raw string if tool expects it?
    // tool_registry_execute takes a JSON string usually.
    // Let's assume the tool takes {"state": "ON"} or similar.
    
    char args_json[64];
    snprintf(args_json, sizeof(args_json), "{\"state\":\"%s\"}", payload_str);
    
    // Execute first tool
    // TODO: Better tool selection
    const char *tool_name = target_slot->tool_names[0];
    char output[256];
    
    ESP_LOGI(TAG, "Executing %s with %s", tool_name, args_json);
    tool_registry_execute(tool_name, args_json, output, sizeof(output));
}

// --- MQTT Events ---

static void __attribute__((unused)) mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected");
        // Subscribe to commands: mimiclaw/DEVICE_ID/+/set
        char sub_topic[128];
        snprintf(sub_topic, sizeof(sub_topic), "%s/%s/+/set", s_topic_prefix, s_device_id);
        esp_mqtt_client_subscribe(s_client, sub_topic, 0);

        // Publish Discovery Configs
        publish_ha_discovery();
        
        // Publish Online Status
        char status_topic[128];
        snprintf(status_topic, sizeof(status_topic), "%s/%s/status", s_topic_prefix, s_device_id);
        esp_mqtt_client_publish(s_client, status_topic, "online", 0, 1, 1);
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT Disconnected");
        break;

    case MQTT_EVENT_DATA:
        handle_mqtt_command(event->topic, event->topic_len, event->data, event->data_len);
        break;

    default:
        break;
    }
}

// --- Public API ---

bool mqtt_manager_is_configured(void)
{
#ifdef MIMI_SECRET_MQTT_URL
    return (strlen(MIMI_SECRET_MQTT_URL) > 0);
#else
    return false;
#endif
}

esp_err_t mqtt_manager_init(void)
{
    get_device_id();
    return ESP_OK;
}

esp_err_t mqtt_manager_start(void)
{
    if (!mqtt_manager_is_configured()) {
        ESP_LOGW(TAG, "MQTT URL not configured, skipping.");
        return ESP_ERR_INVALID_STATE;
    }

#ifdef MIMI_SECRET_MQTT_URL
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MIMI_SECRET_MQTT_URL,
    };
    
    // Config LWT
    char lwt_topic[128];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/%s/status", s_topic_prefix, s_device_id);
    mqtt_cfg.session.last_will.topic = lwt_topic;
    mqtt_cfg.session.last_will.msg = "offline";
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 1;

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_client);
#else
    return ESP_FAIL;
#endif
}

esp_err_t mqtt_manager_publish(const char *topic, const char *payload, int qos, int retain)
{
    if (s_client) {
        esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
        return ESP_OK;
    }
    return ESP_FAIL;
}
