#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_NAMESPACE "wifi"
#define CFG_NAMESPACE "sendercfg"
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define MAX_POST_BODY 1024
#define CAPTIVE_DNS_PORT 53
#define CAPTIVE_IP "192.168.4.1"
#define DEFAULT_PI_PORT 3005
#define DEFAULT_PI_API_PATH "/espdata"
#define DEFAULT_UDP_PORT 55000
#define MIN_PACKET_RATE_HZ 1
#define MAX_PACKET_RATE_HZ 200
#define MIN_PAYLOAD_SIZE 16
#define MAX_PAYLOAD_SIZE 512
#define MIN_PI_POST_MS 1000
#define MAX_PI_POST_MS 30000
#define SENDER_LED_GPIO GPIO_NUM_2
#define SENDER_LED_ON_LEVEL 1
#define SENDER_LED_OFF_LEVEL 0
#define HTTP_TASK_STACK 4096
#define SENDER_TASK_STACK 4096
#define TELEMETRY_TASK_STACK 4096

typedef struct {
    int32_t device_id;
    char name[32];
    char pi_ip[16];
    uint16_t pi_port;
    char pi_api_path[64];
    uint32_t pi_post_interval_ms;
    bool enabled;
    uint16_t packet_rate_hz;
    uint16_t udp_port;
    uint16_t payload_size;
    char broadcast_ip[16];
} sender_config_t;

typedef struct {
    char ip[16];
    char sta_mac[18];
    bool wifi_provisioned;
    bool sta_connected;
    bool enabled;
    uint32_t packets_sent;
    uint32_t send_errors;
    uint32_t last_send_ms;
    uint8_t channel;
    int8_t secondary_channel;
    int64_t boot_us;
} sender_status_t;

static const char *TAG = "csi-sender";
static SemaphoreHandle_t state_lock;
static EventGroupHandle_t wifi_event_group;
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;
static httpd_handle_t http_server;
static sender_config_t g_config;
static sender_status_t g_status;
static bool setup_mode_active;
static int wifi_retry_count;

static uint32_t clamp_u32(uint32_t value, uint32_t min, uint32_t max)
{
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static bool valid_ipv4_or_empty(const char *value)
{
    struct in_addr parsed;
    return value == NULL || value[0] == '\0' || inet_pton(AF_INET, value, &parsed) == 1;
}

static void sanitize_api_path(char *path, size_t path_len)
{
    if (path == NULL || path_len == 0) return;
    path[path_len - 1] = '\0';
    if (path[0] == '\0') {
        strlcpy(path, DEFAULT_PI_API_PATH, path_len);
        return;
    }
    if (path[0] != '/') {
        char tmp[64];
        strlcpy(tmp, path, sizeof(tmp));
        snprintf(path, path_len, "/%s", tmp);
    }
}

static void format_mac(const uint8_t mac[6], char *out, size_t out_len)
{
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void default_config(sender_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->device_id = 240;
    strlcpy(cfg->name, "CsiSenderF0", sizeof(cfg->name));
    cfg->pi_port = DEFAULT_PI_PORT;
    strlcpy(cfg->pi_api_path, DEFAULT_PI_API_PATH, sizeof(cfg->pi_api_path));
    cfg->pi_post_interval_ms = 5000;
    cfg->enabled = false;
    cfg->packet_rate_hz = 20;
    cfg->udp_port = DEFAULT_UDP_PORT;
    cfg->payload_size = 64;
    strlcpy(cfg->broadcast_ip, "255.255.255.255", sizeof(cfg->broadcast_ip));
}

static void sanitize_config(sender_config_t *cfg)
{
    if (cfg->device_id < 0) cfg->device_id = 240;
    if (cfg->device_id > 255) cfg->device_id = 255;
    cfg->packet_rate_hz = clamp_u32(cfg->packet_rate_hz, MIN_PACKET_RATE_HZ, MAX_PACKET_RATE_HZ);
    cfg->payload_size = clamp_u32(cfg->payload_size, MIN_PAYLOAD_SIZE, MAX_PAYLOAD_SIZE);
    cfg->pi_post_interval_ms = clamp_u32(cfg->pi_post_interval_ms, MIN_PI_POST_MS, MAX_PI_POST_MS);
    if (cfg->pi_port == 0) cfg->pi_port = DEFAULT_PI_PORT;
    if (cfg->udp_port == 0) cfg->udp_port = DEFAULT_UDP_PORT;
    cfg->name[sizeof(cfg->name) - 1] = '\0';
    cfg->pi_ip[sizeof(cfg->pi_ip) - 1] = '\0';
    cfg->broadcast_ip[sizeof(cfg->broadcast_ip) - 1] = '\0';
    if (!valid_ipv4_or_empty(cfg->pi_ip)) cfg->pi_ip[0] = '\0';
    if (!valid_ipv4_or_empty(cfg->broadcast_ip) || cfg->broadcast_ip[0] == '\0') {
        strlcpy(cfg->broadcast_ip, "255.255.255.255", sizeof(cfg->broadcast_ip));
    }
    sanitize_api_path(cfg->pi_api_path, sizeof(cfg->pi_api_path));
}

static esp_err_t load_config(sender_config_t *cfg)
{
    default_config(cfg);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    size_t name_len = sizeof(cfg->name);
    size_t pi_ip_len = sizeof(cfg->pi_ip);
    size_t pi_path_len = sizeof(cfg->pi_api_path);
    size_t broadcast_len = sizeof(cfg->broadcast_ip);
    nvs_get_i32(nvs, "device_id", &cfg->device_id);
    nvs_get_str(nvs, "name", cfg->name, &name_len);
    nvs_get_str(nvs, "pi_ip", cfg->pi_ip, &pi_ip_len);
    nvs_get_u16(nvs, "pi_port", &cfg->pi_port);
    nvs_get_str(nvs, "pi_path", cfg->pi_api_path, &pi_path_len);
    nvs_get_u32(nvs, "pi_post_ms", &cfg->pi_post_interval_ms);
    uint8_t enabled = 0;
    nvs_get_u8(nvs, "enabled", &enabled);
    cfg->enabled = enabled != 0;
    nvs_get_u16(nvs, "rate_hz", &cfg->packet_rate_hz);
    nvs_get_u16(nvs, "udp_port", &cfg->udp_port);
    nvs_get_u16(nvs, "payload", &cfg->payload_size);
    nvs_get_str(nvs, "broadcast", cfg->broadcast_ip, &broadcast_len);
    nvs_close(nvs);
    sanitize_config(cfg);
    return ESP_OK;
}

static esp_err_t save_config(const sender_config_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    if ((err = nvs_set_i32(nvs, "device_id", cfg->device_id)) != ESP_OK ||
        (err = nvs_set_str(nvs, "name", cfg->name)) != ESP_OK ||
        (err = nvs_set_str(nvs, "pi_ip", cfg->pi_ip)) != ESP_OK ||
        (err = nvs_set_u16(nvs, "pi_port", cfg->pi_port)) != ESP_OK ||
        (err = nvs_set_str(nvs, "pi_path", cfg->pi_api_path)) != ESP_OK ||
        (err = nvs_set_u32(nvs, "pi_post_ms", cfg->pi_post_interval_ms)) != ESP_OK ||
        (err = nvs_set_u8(nvs, "enabled", cfg->enabled ? 1 : 0)) != ESP_OK ||
        (err = nvs_set_u16(nvs, "rate_hz", cfg->packet_rate_hz)) != ESP_OK ||
        (err = nvs_set_u16(nvs, "udp_port", cfg->udp_port)) != ESP_OK ||
        (err = nvs_set_u16(nvs, "payload", cfg->payload_size)) != ESP_OK ||
        (err = nvs_set_str(nvs, "broadcast", cfg->broadcast_ip)) != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static bool load_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return false;
    esp_err_t ssid_err = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
    esp_err_t pass_err = nvs_get_str(nvs, "password", password, &password_len);
    nvs_close(nvs);
    return ssid_err == ESP_OK && pass_err == ESP_OK && ssid[0] != '\0';
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(nvs, "ssid", ssid));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(nvs, "password", password));
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void clear_wifi_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *src = s;
    char *dst = s;
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            *dst++ = (char)((hex_value(src[1]) << 4) | hex_value(src[2]));
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool form_value(const char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    const char *p = body;
    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            const char *end = strchr(p, '&');
            size_t len = end == NULL ? strlen(p) : (size_t)(end - p);
            if (len >= out_len) len = out_len - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p != NULL) p++;
    }
    return false;
}

static esp_err_t read_request_body(httpd_req_t *req, char *buffer, size_t buffer_len)
{
    if (req->content_len >= buffer_len) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Request body too large");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (ret <= 0) return ESP_FAIL;
        received += ret;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

static void update_status_ip(const char *ip)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    strlcpy(g_status.ip, ip, sizeof(g_status.ip));
    xSemaphoreGive(state_lock);
}

static void sender_led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << SENDER_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(SENDER_LED_GPIO, SENDER_LED_OFF_LEVEL));
}

static cJSON *status_to_json(void)
{
    sender_config_t cfg;
    sender_status_t status;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cfg = g_config;
    status = g_status;
    status.enabled = cfg.enabled;
    xSemaphoreGive(state_lock);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "device_id", cfg.device_id);
    cJSON_AddStringToObject(root, "name", cfg.name);
    cJSON_AddStringToObject(root, "role", "csi_sender");
    cJSON_AddStringToObject(root, "ip", status.ip);
    cJSON_AddStringToObject(root, "sta_mac", status.sta_mac);
    cJSON_AddBoolToObject(root, "enabled", status.enabled);
    cJSON_AddNumberToObject(root, "packet_rate_hz", cfg.packet_rate_hz);
    cJSON_AddNumberToObject(root, "udp_port", cfg.udp_port);
    cJSON_AddNumberToObject(root, "payload_size", cfg.payload_size);
    cJSON_AddStringToObject(root, "broadcast_ip", cfg.broadcast_ip);
    cJSON_AddNumberToObject(root, "packets_sent", status.packets_sent);
    cJSON_AddNumberToObject(root, "send_errors", status.send_errors);
    cJSON_AddNumberToObject(root, "last_send_ms", status.last_send_ms);
    cJSON_AddNumberToObject(root, "channel", status.channel);
    cJSON_AddNumberToObject(root, "secondary_channel", status.secondary_channel);
    cJSON_AddBoolToObject(root, "wifi_provisioned", status.wifi_provisioned);
    cJSON_AddBoolToObject(root, "sta_connected", status.sta_connected);
    cJSON_AddNumberToObject(root, "uptime_ms", (esp_timer_get_time() - status.boot_us) / 1000LL);
    return root;
}

static cJSON *config_to_json(void)
{
    sender_config_t cfg;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cfg = g_config;
    xSemaphoreGive(state_lock);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "device_id", cfg.device_id);
    cJSON_AddStringToObject(root, "name", cfg.name);
    cJSON_AddStringToObject(root, "role", "csi_sender");
    cJSON_AddStringToObject(root, "pi_ip", cfg.pi_ip);
    cJSON_AddNumberToObject(root, "pi_port", cfg.pi_port);
    cJSON_AddStringToObject(root, "pi_api_path", cfg.pi_api_path);
    cJSON_AddNumberToObject(root, "pi_post_interval_ms", cfg.pi_post_interval_ms);
    cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
    cJSON_AddNumberToObject(root, "packet_rate_hz", cfg.packet_rate_hz);
    cJSON_AddNumberToObject(root, "udp_port", cfg.udp_port);
    cJSON_AddNumberToObject(root, "payload_size", cfg.payload_size);
    cJSON_AddStringToObject(root, "broadcast_ip", cfg.broadcast_ip);
    return root;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static void apply_json_number(cJSON *root, const char *name, double *value)
{
    cJSON *item = cJSON_GetObjectItem(root, name);
    if (cJSON_IsNumber(item)) *value = item->valuedouble;
}

static void apply_config_json(cJSON *root, sender_config_t *cfg)
{
    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(name) && name->valuestring != NULL) strlcpy(cfg->name, name->valuestring, sizeof(cfg->name));
    cJSON *pi_ip = cJSON_GetObjectItem(root, "pi_ip");
    if (cJSON_IsString(pi_ip) && pi_ip->valuestring != NULL) strlcpy(cfg->pi_ip, pi_ip->valuestring, sizeof(cfg->pi_ip));
    cJSON *pi_path = cJSON_GetObjectItem(root, "pi_api_path");
    if (cJSON_IsString(pi_path) && pi_path->valuestring != NULL) strlcpy(cfg->pi_api_path, pi_path->valuestring, sizeof(cfg->pi_api_path));
    cJSON *broadcast = cJSON_GetObjectItem(root, "broadcast_ip");
    if (cJSON_IsString(broadcast) && broadcast->valuestring != NULL) strlcpy(cfg->broadcast_ip, broadcast->valuestring, sizeof(cfg->broadcast_ip));
    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(enabled)) cfg->enabled = cJSON_IsTrue(enabled);

    double value = cfg->device_id;
    apply_json_number(root, "device_id", &value);
    cfg->device_id = (int32_t)value;
    value = cfg->pi_port;
    apply_json_number(root, "pi_port", &value);
    cfg->pi_port = value >= 1.0 && value <= 65535.0 ? (uint16_t)value : DEFAULT_PI_PORT;
    value = cfg->pi_post_interval_ms;
    apply_json_number(root, "pi_post_interval_ms", &value);
    cfg->pi_post_interval_ms = (uint32_t)value;
    value = cfg->packet_rate_hz;
    apply_json_number(root, "packet_rate_hz", &value);
    cfg->packet_rate_hz = (uint16_t)value;
    value = cfg->udp_port;
    apply_json_number(root, "udp_port", &value);
    cfg->udp_port = value >= 1.0 && value <= 65535.0 ? (uint16_t)value : DEFAULT_UDP_PORT;
    value = cfg->payload_size;
    apply_json_number(root, "payload_size", &value);
    cfg->payload_size = (uint16_t)value;
    sanitize_config(cfg);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *json = status_to_json();
    esp_err_t err = send_json(req, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    cJSON *json = config_to_json();
    esp_err_t err = send_json(req, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char body[MAX_POST_BODY];
    if (read_request_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    sender_config_t cfg;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cfg = g_config;
    xSemaphoreGive(state_lock);
    apply_config_json(root, &cfg);
    cJSON_Delete(root);

    esp_err_t save_err = save_config(&cfg);
    if (save_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config");
        return ESP_FAIL;
    }

    xSemaphoreTake(state_lock, portMAX_DELAY);
    g_config = cfg;
    g_status.enabled = cfg.enabled;
    if (sta_netif != NULL) esp_netif_set_hostname(sta_netif, cfg.name);
    xSemaphoreGive(state_lock);

    cJSON *reply = config_to_json();
    esp_err_t err = send_json(req, reply);
    cJSON_Delete(reply);
    return err;
}

static esp_err_t start_stop_handler(httpd_req_t *req)
{
    bool enable = strcmp(req->uri, "/api/start") == 0;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    g_config.enabled = enable;
    g_status.enabled = enable;
    sender_config_t cfg = g_config;
    xSemaphoreGive(state_lock);
    save_config(&cfg);
    cJSON *reply = status_to_json();
    esp_err_t err = send_json(req, reply);
    cJSON_Delete(reply);
    return err;
}

static esp_err_t options_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32 CSI Sender</title><style>body{font-family:system-ui;margin:0;background:#101418;color:#ecf2f8}"
        "main{max-width:920px;margin:auto;padding:24px}.card{border:1px solid #33404b;border-radius:8px;padding:14px;background:#182029;margin:12px 0}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:10px}label{display:grid;gap:5px;color:#9fb0bf;font-size:12px;text-transform:uppercase}"
        "input,button{font:inherit;padding:10px;border-radius:6px;border:1px solid #465562}input{background:#0d1116;color:#ecf2f8}input[type=checkbox]{width:auto;justify-self:start}"
        "button{background:#2e7d5b;color:white;cursor:pointer}.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}pre{white-space:pre-wrap;overflow:auto}.msg{color:#9fb0bf;margin-left:8px}</style></head>"
        "<body><main><h1>ESP32 CSI Sender</h1><div class=card><h2>Status</h2><pre id=s>loading</pre><div class=actions><button id=start type=button>Start</button><button id=stop type=button>Stop</button><span id=msg class=msg></span></div></div>"
        "<form id=cfg class=card><h2>Sender Configuration</h2><div class=grid>"
        "<label>Device ID<input name=device_id type=number min=0 max=255></label><label>Name<input name=name autocomplete=off></label>"
        "<label>Enabled<input name=enabled type=checkbox></label><label>Packet rate Hz<input name=packet_rate_hz type=number min=1 max=200 step=1></label>"
        "<label>UDP port<input name=udp_port type=number min=1 max=65535 step=1></label><label>Payload bytes<input name=payload_size type=number min=16 max=512 step=1></label>"
        "<label>Broadcast IP<input name=broadcast_ip autocomplete=off></label><label>Pi IP<input name=pi_ip autocomplete=off></label>"
        "<label>Pi port<input name=pi_port type=number min=1 max=65535 step=1></label><label>Pi API path<input name=pi_api_path autocomplete=off></label>"
        "<label>Pi post interval seconds<input name=pi_post_interval_s type=number min=1 max=30 step=1></label></div><div class=actions><button type=submit>Save Configuration</button></div></form>"
        "<form method=post action=/api/provision class=card><h2>Wi-Fi Provisioning</h2><div class=grid><label>2.4 GHz SSID<input name=ssid required></label>"
        "<label>Wi-Fi password<input name=password type=password></label></div><div class=actions><button>Save Wi-Fi</button></div></form>"
        "<script>const f=document.querySelector('#cfg'),msg=document.querySelector('#msg'),s=document.querySelector('#s'),start=document.querySelector('#start'),stop=document.querySelector('#stop');"
        "async function json(u,o){const r=await fetch(u,o);if(!r.ok)throw new Error(await r.text());return r.json()}"
        "function fill(c){for(const [k,v] of Object.entries(c)){if(k==='pi_post_interval_ms'){f.elements.pi_post_interval_s.value=Math.round((+v||5000)/1000);continue}const e=f.elements[k];if(!e)continue;if(e.type==='checkbox')e.checked=!!v;else e.value=(v==null?'':v)}}"
        "function payload(){const d=new FormData(f);return{device_id:+d.get('device_id'),name:String(d.get('name')||'').trim(),enabled:d.get('enabled')==='on',packet_rate_hz:+d.get('packet_rate_hz'),udp_port:+d.get('udp_port'),payload_size:+d.get('payload_size'),broadcast_ip:String(d.get('broadcast_ip')||'').trim(),pi_ip:String(d.get('pi_ip')||'').trim(),pi_port:+d.get('pi_port'),pi_api_path:String(d.get('pi_api_path')||'/espdata').trim(),pi_post_interval_ms:+d.get('pi_post_interval_s')*1000}}"
        "async function load(){const c=await json('/api/config');fill(c);await tick()}"
        "async function tick(){const j=await json('/status.json');s.textContent=JSON.stringify(j,null,2)}"
        "start.onclick=async()=>{await json('/api/start',{method:'POST'});msg.textContent='sender started';load()};stop.onclick=async()=>{await json('/api/stop',{method:'POST'});msg.textContent='sender stopped';load()};"
        "f.onsubmit=async e=>{e.preventDefault();await json('/api/config',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(payload())});msg.textContent='configuration saved';load()};"
        "setInterval(tick,2000);load()</script>"
        "</main></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t provision_post_handler(httpd_req_t *req)
{
    char body[MAX_POST_BODY];
    char ssid[33] = {0};
    char password[65] = {0};
    if (read_request_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (root != NULL) {
        cJSON *ssid_json = cJSON_GetObjectItem(root, "ssid");
        cJSON *password_json = cJSON_GetObjectItem(root, "password");
        if (cJSON_IsString(ssid_json)) strlcpy(ssid, ssid_json->valuestring, sizeof(ssid));
        if (cJSON_IsString(password_json)) strlcpy(password, password_json->valuestring, sizeof(password));
        cJSON_Delete(root);
    } else {
        form_value(body, "ssid", ssid, sizeof(ssid));
        form_value(body, "password", password, sizeof(password));
    }
    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }
    save_wifi_credentials(ssid, password);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Wi-Fi saved. Rebooting into station mode.");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t reset_wifi_post_handler(httpd_req_t *req)
{
    clear_wifi_credentials();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Wi-Fi credentials cleared. Rebooting into setup mode.");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static void sender_task(void *arg)
{
    (void)arg;
    uint32_t sequence = 0;
    uint8_t payload[MAX_PAYLOAD_SIZE];
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock >= 0) {
        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    }
    while (true) {
        sender_config_t cfg;
        bool connected;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        cfg = g_config;
        connected = g_status.sta_connected;
        xSemaphoreGive(state_lock);

        if (!cfg.enabled || !connected) {
            gpio_set_level(SENDER_LED_GPIO, SENDER_LED_OFF_LEVEL);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if (sock >= 0) {
            struct sockaddr_in dest = {0};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(cfg.udp_port);
            inet_pton(AF_INET, cfg.broadcast_ip, &dest.sin_addr);

            memset(payload, 0xA5, cfg.payload_size);
            int header = snprintf((char *)payload, cfg.payload_size, "esp32-csi-sender,%ld,%lu", (long)cfg.device_id, (unsigned long)sequence++);
            if (header < 0 || header >= cfg.payload_size) {
                header = cfg.payload_size - 1;
            }
            payload[header] = '\0';

            int sent = sendto(sock, payload, cfg.payload_size, 0, (struct sockaddr *)&dest, sizeof(dest));
            xSemaphoreTake(state_lock, portMAX_DELAY);
            if (sent > 0) {
                g_status.packets_sent++;
                g_status.last_send_ms = (uint32_t)((esp_timer_get_time() - g_status.boot_us) / 1000LL);
                gpio_set_level(SENDER_LED_GPIO, (g_status.packets_sent & 1U) ? SENDER_LED_ON_LEVEL : SENDER_LED_OFF_LEVEL);
            } else {
                g_status.send_errors++;
            }
            xSemaphoreGive(state_lock);
        } else {
            xSemaphoreTake(state_lock, portMAX_DELAY);
            g_status.send_errors++;
            xSemaphoreGive(state_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(1000 / cfg.packet_rate_hz));
    }
}

static void telemetry_task(void *arg)
{
    (void)arg;
    while (true) {
        sender_config_t cfg;
        bool connected;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        cfg = g_config;
        connected = g_status.sta_connected;
        xSemaphoreGive(state_lock);

        if (connected && cfg.pi_ip[0] != '\0') {
            cJSON *json = status_to_json();
            char *body = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            if (body != NULL) {
                int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
                if (sock >= 0) {
                    struct sockaddr_in dest = {0};
                    dest.sin_family = AF_INET;
                    dest.sin_port = htons(cfg.pi_port);
                    inet_pton(AF_INET, cfg.pi_ip, &dest.sin_addr);
                    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) == 0) {
                        char request[1536];
                        int len = snprintf(request, sizeof(request),
                                           "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
                                           cfg.pi_api_path, cfg.pi_ip, (unsigned)strlen(body), body);
                        if (len > 0 && len < (int)sizeof(request)) {
                            send(sock, request, len, 0);
                        }
                    }
                    close(sock);
                }
                free(body);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(cfg.pi_post_interval_ms));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        g_status.sta_connected = false;
        xSemaphoreGive(state_lock);
        if (wifi_retry_count < 10) {
            wifi_retry_count++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        uint8_t primary = 0;
        wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&primary, &secondary);
        xSemaphoreTake(state_lock, portMAX_DELAY);
        strlcpy(g_status.ip, ip, sizeof(g_status.ip));
        g_status.sta_connected = true;
        g_status.channel = primary;
        g_status.secondary_channel = (int8_t)secondary;
        xSemaphoreGive(state_lock);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void start_softap(void)
{
    setup_mode_active = true;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap_config = {0};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "CSI-Sender-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    update_status_ip(CAPTIVE_IP);
}

static void start_station(const char *ssid, const char *password)
{
    setup_mode_active = false;
    if (sta_netif != NULL) esp_netif_set_hostname(sta_netif, g_config.name);
    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
}

static void init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    char ssid[33] = {0};
    char password[65] = {0};
    bool has_credentials = load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password));
    xSemaphoreTake(state_lock, portMAX_DELAY);
    g_status.wifi_provisioned = has_credentials;
    xSemaphoreGive(state_lock);
    if (has_credentials) {
        start_station(ssid, password);
    } else {
        start_softap();
    }
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 16;
    ESP_ERROR_CHECK(httpd_start(&http_server, &config));

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t status = {.uri = "/status.json", .method = HTTP_GET, .handler = status_get_handler};
    httpd_uri_t api_status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler};
    httpd_uri_t config_get = {.uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler};
    httpd_uri_t config_post = {.uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler};
    httpd_uri_t config_options = {.uri = "/api/config", .method = HTTP_OPTIONS, .handler = options_handler};
    httpd_uri_t start = {.uri = "/api/start", .method = HTTP_POST, .handler = start_stop_handler};
    httpd_uri_t stop = {.uri = "/api/stop", .method = HTTP_POST, .handler = start_stop_handler};
    httpd_uri_t provision = {.uri = "/api/provision", .method = HTTP_POST, .handler = provision_post_handler};
    httpd_uri_t reset_wifi = {.uri = "/api/reset-wifi", .method = HTTP_POST, .handler = reset_wifi_post_handler};

    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &api_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &config_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &config_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &config_options));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &start));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &stop));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &provision));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &reset_wifi));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    state_lock = xSemaphoreCreateMutex();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(state_lock == NULL || wifi_event_group == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    load_config(&g_config);
    memset(&g_status, 0, sizeof(g_status));
    g_status.boot_us = esp_timer_get_time();
    strlcpy(g_status.ip, "0.0.0.0", sizeof(g_status.ip));
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    format_mac(mac, g_status.sta_mac, sizeof(g_status.sta_mac));
    g_status.enabled = g_config.enabled;

    sender_led_init();
    init_wifi();
    start_http_server();
    xTaskCreate(sender_task, "csi_sender", SENDER_TASK_STACK, NULL, 5, NULL);
    xTaskCreate(telemetry_task, "sender_telemetry", TELEMETRY_TASK_STACK, NULL, 4, NULL);
    ESP_LOGI(TAG, "ESP32 CSI sender ready, STA MAC %s", g_status.sta_mac);
}
