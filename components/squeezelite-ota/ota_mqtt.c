#include "ota_check.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "nvs_utilities.h"
#include "platform_config.h"
#include "messaging.h"
#include "tools.h"

static const char *TAG = "ota_mqtt";
static esp_mqtt_client_handle_t s_cli = NULL;
static bool s_connected = false;
static char s_set_topic[160] = {0};
static char s_status_topic[160] = {0};
static char s_state_topic[160] = {0};
static char s_log_topic[160] = {0};
static char s_host[64] = {0};
static char s_last_state[1024] = {0};
static RingbufHandle_t s_msg_sub = NULL;
#define OTA_MQTT_CONNECTED_BIT BIT0
static EventGroupHandle_t s_evt = NULL;

static void ota_mqtt_bridge_task(void *arg);
static void publish_discovery(esp_mqtt_client_handle_t c);
static void publish_state(esp_mqtt_client_handle_t c, const char *json);
// Client config keeps string pointers: everything cfg points at must be static.
static char s_broker[128] = {0};
static char s_user[64] = {0};
static char s_pass[64] = {0};

static char * s_nvs_or(const char *key, const char *dflt) {    char *v = config_alloc_get(NVS_TYPE_STR, key);
    if (!v && dflt) v = strdup(dflt);
    return v;
}

static void s_sanitize_host(char *host) {
    for (char *p = host; *p; p++) {
        if (*p == ' ' || *p == ',' || *p == '/') *p = '_';
    }
}

// Worker so long commands never block the MQTT event-loop task.
static void ota_mqtt_worker(void *arg) {
    char *payload = arg;
    cJSON *root = cJSON_Parse(payload ? payload : "");
    if (!root) {
        ESP_LOGW(TAG, "Ignoring malformed set payload");
    } else {
        cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
        const char *cmd = (cmd_item && cJSON_IsString(cmd_item)) ? cmd_item->valuestring : "";
        if (!strcmp(cmd, "check")) {
            ESP_LOGI(TAG, "Remote command: check");
            ota_check_notify(NULL);
        } else if (!strcmp(cmd, "flash")) {
            cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
            const char *url = (url_item && cJSON_IsString(url_item)) ? url_item->valuestring : "";
            ESP_LOGI(TAG, "Remote command: flash %s", url);
            esp_err_t err = ota_flash_deferred(url);
            if (err == ESP_ERR_INVALID_STATE) {
                messaging_post_message(MESSAGING_WARNING, MESSAGING_CLASS_OTA,
                    "Remote flash refused: notify-only (ota_allow_flash=0)");
            }
        } else {
            ESP_LOGW(TAG, "Unknown set command: %s", cmd);
        }
        cJSON_Delete(root);
    }
    free(payload);
    vTaskDelete(NULL);
}

static void ota_mqtt_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_mqtt_event_handle_t evt = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Connected, subscribing %s", s_set_topic);
        // Only the persistent client drives connection state; a one-shot
        // fallback client shares this handler but must not flip it (or its
        // disconnect later clear a live persistent connection).
        bool mine = (evt->client == s_cli);
        if (mine) s_connected = true;
        if (s_evt) xEventGroupSetBits(s_evt, OTA_MQTT_CONNECTED_BIT);
        esp_mqtt_client_subscribe(evt->client, s_set_topic, 1);
        esp_mqtt_client_publish(evt->client, s_status_topic, "online", 0, 1, 1);
        // (Re)publish discovery + last known state so Home Assistant has the
        // sensor even if no OTA check has run since boot (retained).
        publish_discovery(evt->client);
        if (s_last_state[0]) {
            publish_state(evt->client, s_last_state);
        } else if (mine) {
            // No check yet: run one now so the sensor gets real content.
            // Persistent client only: a one-shot must never spawn this or
            // checks would recurse (check -> oneshot -> check -> ...).
            char *ping = strdup("{\"cmd\":\"check\"}");
            if (ping) {
                if (xTaskCreate(&ota_mqtt_worker, "ota_mqtw", 8192,
                                ping, 5, NULL) != pdPASS) free(ping);
            }
        }
        messaging_post_message(MESSAGING_INFO, MESSAGING_CLASS_OTA, "MQTT connected");
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected (auto-reconnect active)");
        if (evt->client == s_cli) s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        if (evt->topic_len == (int)strlen(s_set_topic) &&
            !strncmp(evt->topic, s_set_topic, evt->topic_len)) {
            char *payload = malloc((size_t)evt->data_len + 1);
            if (!payload) break;
            memcpy(payload, evt->data, evt->data_len);
            payload[evt->data_len] = 0;
            if (xTaskCreate(&ota_mqtt_worker, "ota_mqtw", 8192,
                            payload, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Failed to spawn set-command worker");
                free(payload);
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error (wrong credentials/host?)");
        break;
    default:
        break;
    }
}

// HA discovery payload (retained): state template reads .available, full
// check JSON doubles as attributes. Availability tracks the LWT status topic.
static char * build_discovery_json(void) {
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddStringToObject(d, "name", "Squeezelite OTA");
    cJSON_AddStringToObject(d, "stat_t", s_state_topic);
    cJSON_AddStringToObject(d, "val_tpl", "{{ value_json.available }}");
    cJSON_AddStringToObject(d, "json_attr_t", s_state_topic);
    cJSON_AddStringToObject(d, "avty_t", s_status_topic);
    cJSON_AddStringToObject(d, "pl_avail", "online");
    cJSON_AddStringToObject(d, "pl_not_avail", "offline");
    char ids[80];
    snprintf(ids, sizeof(ids), "%s_ota", s_host);
    cJSON_AddStringToObject(d, "uniq_id", ids);
    cJSON *dev = cJSON_CreateObject();
    if (dev) {
        cJSON_AddStringToObject(dev, "name", s_host);
        cJSON_AddItemToObject(d, "dev", dev);
        cJSON *devids = cJSON_CreateStringArray((const char *[]){ s_host }, 1);
        if (devids) cJSON_AddItemToObject(dev, "ids", devids);
        cJSON_AddStringToObject(dev, "mf", "squeezelite-esp32");
    }
    char *out = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    return out;
}

static void publish_discovery(esp_mqtt_client_handle_t c) {
    char disc_topic[180];
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/sensor/%s_ota/config", s_host);
    char *ds = build_discovery_json();
    if (ds) {
        esp_mqtt_client_publish(c, disc_topic, ds, 0, 1, 1);
        free(ds);
    }
}

static void publish_state(esp_mqtt_client_handle_t c, const char *json) {
    if (!json) json = "{}";
    snprintf(s_last_state, sizeof(s_last_state), "%s", json);
    esp_mqtt_client_publish(c, s_state_topic, json, 0, 1, 1);
}

// One-shot fallback when the persistent client isn't connected: distinct
// client id (no takeover fight with the persistent client), waits for the
// CONNECTED event instead of a blind delay, then stops.
static void publish_result_oneshot(const char *json) {
    char *broker = ota_mqtt_broker_resolve();
    if (!broker) return;
    char *user = config_alloc_get(NVS_TYPE_STR, "mqtt_user");
    char *pass = config_alloc_get(NVS_TYPE_STR, "mqtt_pass");
    static EventGroupHandle_t once_evt = NULL;
    if (!once_evt) once_evt = xEventGroupCreate();
    esp_mqtt_client_config_t cfg = { 0 };
    cfg.uri = broker;
    if (user && *user) cfg.username = user;
    if (pass && *pass) cfg.password = pass;
    static char once_id[80];
    snprintf(once_id, sizeof(once_id), "%s-check", s_host[0] ? s_host : "squeezelite");
    cfg.client_id = once_id;
    cfg.keepalive = 30;
    esp_mqtt_client_handle_t c = esp_mqtt_client_init(&cfg);
    if (!c) goto out;
    // Reuse the shared event handler: sets CONNECTED_BIT on our group too.
    EventGroupHandle_t saved = s_evt;
    s_evt = once_evt;
    xEventGroupClearBits(once_evt, OTA_MQTT_CONNECTED_BIT);
    esp_mqtt_client_register_event(c, ESP_EVENT_ANY_ID, ota_mqtt_evt, NULL);
    if (esp_mqtt_client_start(c) != ESP_OK) {
        esp_mqtt_client_destroy(c);
        s_evt = saved;
        goto out;
    }
    EventBits_t bits = xEventGroupWaitBits(once_evt, OTA_MQTT_CONNECTED_BIT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
    if (bits & OTA_MQTT_CONNECTED_BIT) {
        publish_discovery(c);
        // NOTE: one-shot CONNECTED also ran ota_mqtt_evt, which published
        // status/discovery/last-state on this same client; publish_state here
        // is the fresh result. Harmless duplication, same retained values.
        publish_state(c, json);
        vTaskDelay(pdMS_TO_TICKS(500)); // flush before stop
    } else {
        ESP_LOGW(TAG, "One-shot MQTT connect timed out (%s)", broker);
    }
    esp_mqtt_client_stop(c);
    esp_mqtt_client_destroy(c);
    s_evt = saved;
out:
    FREE_AND_NULL(broker);
    FREE_AND_NULL(user); FREE_AND_NULL(pass);
}

void ota_mqtt_publish_result(const char *payload_json) {
    if (!payload_json) payload_json = "{}";
    if (s_cli && s_connected) {
        publish_discovery(s_cli);
        publish_state(s_cli, payload_json);
    } else {
        publish_result_oneshot(payload_json);
    }
}

void ota_mqtt_start(void) {
    if (s_cli) return; // already running
    char *en = config_alloc_get(NVS_TYPE_STR, "mqtt_enable");
    bool enabled = !en || strcmp(en, "0") != 0; // default ON; no broker == no-op
    FREE_AND_NULL(en);
    if (!enabled) {
        ESP_LOGD(TAG, "MQTT disabled via mqtt_enable=0");
        return;
    }
    char *broker = ota_mqtt_broker_resolve();
    if (!broker) return; // retry on next call (periodic task loops)
    char *prefix = s_nvs_or("mqtt_prefix", "squeezelite");
    char *host = s_nvs_or("host_name", "squeezelite");
    s_sanitize_host(host);
    snprintf(s_set_topic, sizeof(s_set_topic), "%s/%s/ota/set", prefix, host);
    snprintf(s_status_topic, sizeof(s_status_topic), "%s/%s/status", prefix, host);
    snprintf(s_state_topic, sizeof(s_state_topic), "%s/%s/ota", prefix, host);
    snprintf(s_log_topic, sizeof(s_log_topic), "%s/%s/ota/log", prefix, host);
    snprintf(s_host, sizeof(s_host), "%s", host);
    if (!s_evt) s_evt = xEventGroupCreate();
    char *user = config_alloc_get(NVS_TYPE_STR, "mqtt_user");
    char *pass = config_alloc_get(NVS_TYPE_STR, "mqtt_pass");
    // IDF v4.4 flat config fields. Copy into static storage: init keeps pointers.
    snprintf(s_broker, sizeof(s_broker), "%s", broker);
    if (user && *user) snprintf(s_user, sizeof(s_user), "%s", user);
    if (pass && *pass) snprintf(s_pass, sizeof(s_pass), "%s", pass);
    esp_mqtt_client_config_t cfg = { 0 };
    cfg.uri = s_broker;
    if (s_user[0]) cfg.username = s_user;
    if (s_pass[0]) cfg.password = s_pass;
    cfg.lwt_topic = s_status_topic;
    cfg.lwt_msg = "offline";
    cfg.lwt_qos = 1;
    cfg.lwt_retain = 1;
    cfg.keepalive = 30;
    s_cli = esp_mqtt_client_init(&cfg);
    FREE_AND_NULL(broker); FREE_AND_NULL(prefix); FREE_AND_NULL(host);
    FREE_AND_NULL(user); FREE_AND_NULL(pass);
    if (!s_cli) return;
    esp_mqtt_client_register_event(s_cli, ESP_EVENT_ANY_ID, ota_mqtt_evt, NULL);
    if (esp_mqtt_client_start(s_cli) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
        esp_mqtt_client_destroy(s_cli);
        s_cli = NULL;
    } else {
        ESP_LOGI(TAG, "MQTT client started: set=%s status=%s", s_set_topic, s_status_topic);
    }
    // Bridge LMS CLASS_OTA messages into the MQTT state topic so the portal
    // message feed and Home Assistant stay in sync.
    if (!s_msg_sub) {
        s_msg_sub = messaging_register_subscriber(8, "ota_mqtt");
    }
    if (s_msg_sub) {
        static bool bridge_started = false;
        if (!bridge_started) {
            bridge_started = true;
            if (xTaskCreate(&ota_mqtt_bridge_task, "ota_mqttb", 4096,
                            NULL, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Failed to start MQTT bridge task");
                bridge_started = false;
            }
        }
    }
}

// Drain our messaging subscription; forward OTA-class messages to the state
// topic. Runs in its own task, publishes only while connected.
static void ota_mqtt_bridge_task(void *arg) {
    (void)arg;
    for (;;) {
        if (s_msg_sub && s_cli && s_connected) {
            // Cap per cycle so a message burst can't starve the task.
            for (int i = 0; i < 8; i++) {
                single_message_t *msg = messaging_retrieve_message(s_msg_sub);
                if (!msg) break;
                if (msg->msg_class == MESSAGING_CLASS_OTA && msg->message[0]) {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddStringToObject(o, "type",
                        msg->type == MESSAGING_ERROR ? "error" :
                        msg->type == MESSAGING_WARNING ? "warning" : "info");
                    cJSON_AddStringToObject(o, "class", "ota");
                    cJSON_AddStringToObject(o, "message", msg->message);
                    cJSON_AddNumberToObject(o, "sent_time", (double)msg->sent_time);
                    char *js = cJSON_PrintUnformatted(o);
                    if (js) {
                        // Log feed lives on its own topic: the state topic
                        // must stay clean check-result JSON or HA's
                        // value_template ({{ value_json.available }}) breaks.
                        esp_mqtt_client_publish(s_cli, s_log_topic, js, 0, 0, 0);
                        free(js);
                    }
                    cJSON_Delete(o);
                }
                free(msg);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
