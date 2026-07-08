#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_NAMESPACE "wifi"
#define CFG_NAMESPACE "nodecfg"
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define CSI_QUEUE_LEN 256
#define MAX_POST_BODY 1024
#define MIN_IDLE_RATE_HZ 1
#define MAX_IDLE_RATE_HZ 10
#define MIN_BOOST_RATE_HZ 5
#define MAX_BOOST_RATE_HZ 250
#define MIN_WINDOW_MS 100
#define MAX_WINDOW_MS 2000
#define MIN_BOOST_MS 1000
#define MAX_BOOST_MS 60000
#define MIN_COOLDOWN_MS 1000
#define MAX_COOLDOWN_MS 120000

typedef enum {
    SENSE_IDLE = 0,
    SENSE_BOOST,
    SENSE_COOLDOWN,
} sense_state_t;

typedef struct {
    int64_t timestamp_us;
    int16_t rssi;
    int16_t noise_floor;
    uint16_t csi_len;
    float energy;
} csi_sample_t;

typedef struct {
    int32_t device_id;
    char name[32];
    uint16_t idle_rate_hz;
    uint16_t boost_rate_hz;
    float movement_threshold;
    float settle_threshold;
    uint32_t boost_duration_ms;
    uint32_t cooldown_ms;
    uint32_t feature_window_ms;
} node_config_t;

typedef struct {
    sense_state_t state;
    char ip[16];
    uint32_t sample_rate_hz;
    int16_t rssi;
    int16_t noise_floor;
    float amplitude_variance;
    float subcarrier_energy_delta;
    float movement_score;
    bool movement_detected;
    uint32_t packet_count;
    uint32_t last_window_packets;
    uint32_t baseline_age_s;
    uint32_t last_packet_ms;
    int64_t boot_us;
    int64_t baseline_started_us;
    int64_t last_packet_us;
    int64_t state_until_us;
    bool wifi_provisioned;
    bool sta_connected;
} node_status_t;

static const char *TAG = "csi-node";
static QueueHandle_t csi_queue;
static SemaphoreHandle_t state_lock;
static EventGroupHandle_t wifi_event_group;
static node_config_t g_config;
static node_status_t g_status;
static httpd_handle_t http_server;
static int wifi_retry_count;
static volatile int64_t csi_min_interval_us = 333333;
static volatile int64_t csi_last_sample_us;

/**
 * Introduction:
 * Restricts an unsigned 32-bit value to a known safe range. Configuration
 * values can arrive from persistent storage or HTTP requests, so this helper
 * makes sure later code never has to handle values below the supported minimum
 * or above the supported maximum.
 *
 * Parameters:
 * - value: The value to validate.
 * - min: The lowest acceptable value.
 * - max: The highest acceptable value.
 *
 * Returns the original value when it is in range, otherwise the nearest limit.
 */
static uint32_t clamp_u32(uint32_t value, uint32_t min, uint32_t max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

/**
 * Introduction:
 * Restricts a floating-point value to a safe range. This is mainly used for
 * movement detection thresholds, where unreasonable values would make the node
 * either too sensitive or unable to detect motion at all.
 *
 * Parameters:
 * - value: The value to validate.
 * - min: The lowest acceptable value.
 * - max: The highest acceptable value.
 *
 * Returns the original value when it is in range, otherwise the nearest limit.
 */
static float clamp_f32(float value, float min, float max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

/**
 * Introduction:
 * Converts the internal sensing state enum into the lowercase string exposed by
 * the HTTP status API. Keeping this mapping in one place prevents API responses
 * from depending on enum numeric values.
 *
 * Parameters:
 * - state: The current sensing state.
 *
 * Returns a static string describing the state: "idle", "boost", or
 * "cooldown".
 */
static const char *sense_state_name(sense_state_t state)
{
    switch (state) {
    case SENSE_BOOST:
        return "boost";
    case SENSE_COOLDOWN:
        return "cooldown";
    case SENSE_IDLE:
    default:
        return "idle";
    }
}

/**
 * Introduction:
 * Fills a node_config_t structure with sensible factory defaults. This gives the
 * firmware a complete working configuration before any values are loaded from
 * NVS, and also acts as a fallback when the node has never been configured.
 *
 * Parameters:
 * - cfg: The configuration structure to initialize.
 *
 * Side effects:
 * - Clears the whole structure before assigning default values.
 */
static void default_config(node_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->device_id = 0;
    strlcpy(cfg->name, "esp32-csi-0", sizeof(cfg->name));
    cfg->idle_rate_hz = 3;
    cfg->boost_rate_hz = 80;
    cfg->movement_threshold = 0.35f;
    cfg->settle_threshold = 0.18f;
    cfg->boost_duration_ms = 8000;
    cfg->cooldown_ms = 15000;
    cfg->feature_window_ms = 250;
}

/**
 * Introduction:
 * Normalizes configuration values after loading or receiving them from the API.
 * The function protects the sensing loop from invalid sample rates, thresholds,
 * durations, and unterminated names by clamping every externally controlled
 * field to the firmware-supported range.
 *
 * Parameters:
 * - cfg: The configuration structure to validate and modify in place.
 */
static void sanitize_config(node_config_t *cfg)
{
    cfg->idle_rate_hz = clamp_u32(cfg->idle_rate_hz, MIN_IDLE_RATE_HZ, MAX_IDLE_RATE_HZ);
    cfg->boost_rate_hz = clamp_u32(cfg->boost_rate_hz, MIN_BOOST_RATE_HZ, MAX_BOOST_RATE_HZ);
    cfg->movement_threshold = clamp_f32(cfg->movement_threshold, 0.01f, 5.0f);
    cfg->settle_threshold = clamp_f32(cfg->settle_threshold, 0.005f, cfg->movement_threshold);
    cfg->boost_duration_ms = clamp_u32(cfg->boost_duration_ms, MIN_BOOST_MS, MAX_BOOST_MS);
    cfg->cooldown_ms = clamp_u32(cfg->cooldown_ms, MIN_COOLDOWN_MS, MAX_COOLDOWN_MS);
    cfg->feature_window_ms = clamp_u32(cfg->feature_window_ms, MIN_WINDOW_MS, MAX_WINDOW_MS);
    cfg->name[sizeof(cfg->name) - 1] = '\0';
}

/**
 * Introduction:
 * Loads the node configuration from non-volatile storage. The structure is first
 * populated with defaults so missing NVS keys are harmless, then any stored
 * values are layered over the defaults and sanitized before use.
 *
 * Parameters:
 * - cfg: The configuration structure to populate.
 *
 * Returns:
 * - ESP_OK when the NVS namespace was read successfully.
 * - An NVS error when the configuration namespace could not be opened. The
 *   caller can still use the defaults already written into cfg.
 */
static esp_err_t load_config(node_config_t *cfg)
{
    default_config(cfg);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t name_len = sizeof(cfg->name);
    nvs_get_i32(nvs, "device_id", &cfg->device_id);
    nvs_get_str(nvs, "name", cfg->name, &name_len);
    nvs_get_u16(nvs, "idle_hz", &cfg->idle_rate_hz);
    nvs_get_u16(nvs, "boost_hz", &cfg->boost_rate_hz);
    nvs_get_u32(nvs, "boost_ms", &cfg->boost_duration_ms);
    nvs_get_u32(nvs, "cool_ms", &cfg->cooldown_ms);
    nvs_get_u32(nvs, "window_ms", &cfg->feature_window_ms);

    size_t f_len = sizeof(float);
    nvs_get_blob(nvs, "move_th", &cfg->movement_threshold, &f_len);
    f_len = sizeof(float);
    nvs_get_blob(nvs, "settle_th", &cfg->settle_threshold, &f_len);
    nvs_close(nvs);
    sanitize_config(cfg);
    return ESP_OK;
}

/**
 * Introduction:
 * Persists the current node configuration to non-volatile storage. This is used
 * by the configuration API so user-tuned sensor thresholds and rates survive
 * reboots.
 *
 * Parameters:
 * - cfg: The sanitized configuration to save.
 *
 * Returns:
 * - ESP_OK when all changes were committed.
 * - An ESP-IDF/NVS error if the namespace could not be opened or committed.
 */
static esp_err_t save_config(const node_config_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_i32(nvs, "device_id", cfg->device_id));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(nvs, "name", cfg->name));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u16(nvs, "idle_hz", cfg->idle_rate_hz));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u16(nvs, "boost_hz", cfg->boost_rate_hz));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u32(nvs, "boost_ms", cfg->boost_duration_ms));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u32(nvs, "cool_ms", cfg->cooldown_ms));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u32(nvs, "window_ms", cfg->feature_window_ms));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_blob(nvs, "move_th", &cfg->movement_threshold, sizeof(float)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_blob(nvs, "settle_th", &cfg->settle_threshold, sizeof(float)));
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

/**
 * Introduction:
 * Reads saved Wi-Fi station credentials from non-volatile storage. The boot path
 * uses this to decide whether the node should join an existing network or start
 * its setup access point.
 *
 * Parameters:
 * - ssid: Destination buffer for the saved SSID.
 * - ssid_len: Size of the SSID buffer.
 * - password: Destination buffer for the saved password.
 * - password_len: Size of the password buffer.
 *
 * Returns true only when both values were read and the SSID is not empty.
 */
static bool load_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    esp_err_t ssid_err = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
    esp_err_t pass_err = nvs_get_str(nvs, "password", password, &password_len);
    nvs_close(nvs);
    return ssid_err == ESP_OK && pass_err == ESP_OK && ssid[0] != '\0';
}

/**
 * Introduction:
 * Saves Wi-Fi station credentials to non-volatile storage. After provisioning,
 * these values let the device reboot directly into station mode instead of
 * returning to setup AP mode.
 *
 * Parameters:
 * - ssid: The network name to store.
 * - password: The network password to store. Empty strings are allowed for open
 *   networks.
 *
 * Returns ESP_OK on commit success, otherwise an NVS error.
 */
static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(nvs, "ssid", ssid));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(nvs, "password", password));
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

/**
 * Introduction:
 * Removes all saved Wi-Fi provisioning data from non-volatile storage. This is
 * called by the reset Wi-Fi endpoint so the next boot starts the setup access
 * point again.
 *
 * Side effects:
 * - Erases every key in the Wi-Fi NVS namespace when it can be opened.
 */
static void clear_wifi_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/**
 * Introduction:
 * Updates the public status copy of the node IP address while holding the shared
 * state mutex. Both Wi-Fi event handlers and setup mode use this helper so HTTP
 * responses see a consistent string.
 *
 * Parameters:
 * - ip: Null-terminated IPv4 address text to copy into g_status.
 */
static void update_status_ip(const char *ip)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    strlcpy(g_status.ip, ip, sizeof(g_status.ip));
    xSemaphoreGive(state_lock);
}

/**
 * Introduction:
 * Receives raw CSI packets from the ESP-IDF Wi-Fi driver and converts them into
 * compact samples for the aggregation task. The callback rate-limits packets,
 * extracts RSSI/noise metadata, computes a simple average CSI energy value, and
 * pushes the result into the FreeRTOS queue without blocking the Wi-Fi stack.
 *
 * Parameters:
 * - ctx: Optional callback context, unused by this firmware.
 * - info: CSI packet data and receive metadata supplied by ESP-IDF.
 *
 * Side effects:
 * - Updates csi_last_sample_us for rate limiting.
 * - Enqueues a csi_sample_t when the queue exists and the packet passes checks.
 */
static void csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    if (info == NULL || info->buf == NULL || info->len == 0) {
        return;
    }

    int64_t now = esp_timer_get_time();
    int64_t min_interval_us = csi_min_interval_us;
    if (min_interval_us > 0 && csi_last_sample_us > 0 && now - csi_last_sample_us < min_interval_us) {
        return;
    }
    csi_last_sample_us = now;

    int32_t energy_sum = 0;
    int len = info->len;
    for (int i = 0; i + 1 < len; i += 2) {
        energy_sum += abs((int)info->buf[i]) + abs((int)info->buf[i + 1]);
    }

    csi_sample_t sample = {
        .timestamp_us = now,
        .rssi = info->rx_ctrl.rssi,
        .noise_floor = info->rx_ctrl.noise_floor,
        .csi_len = info->len,
        .energy = len > 0 ? (float)energy_sum / (float)len : 0.0f,
    };

    if (csi_queue != NULL) {
        (void)xQueueSend(csi_queue, &sample, 0);
    }
}

/**
 * Introduction:
 * Enables CSI collection on the Wi-Fi driver. This configures which long
 * training fields are captured, registers csi_rx_cb as the receiver callback,
 * turns CSI on, and enables promiscuous mode so CSI data is delivered.
 *
 * Side effects:
 * - Changes Wi-Fi driver CSI and promiscuous-mode settings.
 */
static void enable_csi_capture(void)
{
    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
        .shift = 0,
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_csi(true));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_promiscuous(true));
}

/**
 * Introduction:
 * Stops CSI collection when the station disconnects or capture should be
 * suspended. Disabling CSI and promiscuous mode avoids collecting stale data
 * while the device is not attached to the monitored Wi-Fi environment.
 *
 * Side effects:
 * - Changes Wi-Fi driver CSI and promiscuous-mode settings.
 */
static void disable_csi_capture(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_csi(false));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_promiscuous(false));
}

/**
 * Introduction:
 * Handles Wi-Fi and IP lifecycle events for both station and setup modes. It
 * starts connection attempts, retries station disconnects, records connection
 * status, updates the node IP address, and starts CSI capture once the station
 * has a valid IP address.
 *
 * Parameters:
 * - arg: Optional event handler argument, unused by this firmware.
 * - event_base: ESP-IDF event group, such as WIFI_EVENT or IP_EVENT.
 * - event_id: Specific event identifier within the event group.
 * - event_data: Event-specific payload supplied by ESP-IDF.
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        disable_csi_capture();
        xSemaphoreTake(state_lock, portMAX_DELAY);
        g_status.sta_connected = false;
        xSemaphoreGive(state_lock);

        if (wifi_retry_count < 10) {
            wifi_retry_count++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        strlcpy(g_status.ip, ip, sizeof(g_status.ip));
        g_status.sta_connected = true;
        xSemaphoreGive(state_lock);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected at %s", ip);
        enable_csi_capture();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ESP_LOGI(TAG, "Setup client received an IP address");
    }
}

/**
 * Introduction:
 * Starts the device in setup access point mode when no saved Wi-Fi credentials
 * are available. The SSID includes the last bytes of the SoftAP MAC address so
 * multiple unprovisioned nodes can be distinguished during setup.
 *
 * Side effects:
 * - Switches Wi-Fi to AP mode.
 * - Starts the Wi-Fi driver.
 * - Updates the status IP to the default SoftAP address.
 */
static void start_softap(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_config_t ap_config = {0};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "ESP32-CSI-SETUP-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    update_status_ip("192.168.4.1");
    ESP_LOGI(TAG, "Setup AP started: %s", ap_config.ap.ssid);
}

/**
 * Introduction:
 * Starts the device in station mode using saved or newly provided credentials.
 * The actual connection result is handled asynchronously by wifi_event_handler
 * through Wi-Fi and IP events.
 *
 * Parameters:
 * - ssid: Network name to join.
 * - password: Network password. This may be empty for open networks.
 *
 * Side effects:
 * - Switches Wi-Fi to station mode.
 * - Starts the Wi-Fi driver and begins the connection process.
 */
static void start_station(const char *ssid, const char *password)
{
    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Joining Wi-Fi SSID %s", ssid);
}

/**
 * Introduction:
 * Initializes the ESP-IDF networking stack, creates default STA/AP interfaces,
 * registers event handlers, and chooses the correct Wi-Fi mode for this boot. If
 * credentials exist, the node joins the configured network; otherwise it exposes
 * a setup access point for provisioning.
 *
 * Side effects:
 * - Initializes esp_netif, the default event loop, and the Wi-Fi driver.
 * - Updates g_status.wifi_provisioned under the state mutex.
 */
static void init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

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

/**
 * Introduction:
 * Publishes one completed CSI feature window into the shared node status. This
 * function compares the current window against an adaptive baseline, computes a
 * movement score, advances the idle/boost/cooldown state machine, retunes the
 * CSI sampling interval, and stores the latest metrics for HTTP clients.
 *
 * Parameters:
 * - mean_energy: Average CSI energy for the completed window.
 * - variance: Sample variance of CSI energy in the window.
 * - window_packets: Number of CSI samples included in the window.
 * - window_ms: Actual elapsed window duration in milliseconds.
 * - last: Most recent CSI sample in the window, used for RSSI/noise/timestamp.
 *
 * Side effects:
 * - Mutates g_status while holding state_lock.
 * - Updates the static adaptive baseline when the environment appears stable.
 * - Updates csi_min_interval_us to match idle or boost sampling rates.
 */
static void publish_window(float mean_energy, float variance, uint32_t window_packets, uint32_t window_ms, const csi_sample_t *last)
{
    static float baseline_energy = 0.0f;
    static float baseline_variance = 0.0f;

    int64_t now = esp_timer_get_time();
    xSemaphoreTake(state_lock, portMAX_DELAY);

    if (baseline_energy <= 0.01f) {
        baseline_energy = mean_energy;
        baseline_variance = variance;
        g_status.baseline_started_us = now;
    }

    float energy_delta = fabsf(mean_energy - baseline_energy) / fmaxf(baseline_energy, 1.0f);
    float variance_delta = fabsf(variance - baseline_variance) / fmaxf(baseline_variance + 1.0f, 1.0f);
    float score = clamp_f32((0.70f * energy_delta) + (0.30f * variance_delta), 0.0f, 10.0f);

    if (g_status.state == SENSE_IDLE && score >= g_config.movement_threshold) {
        g_status.state = SENSE_BOOST;
        g_status.state_until_us = now + ((int64_t)g_config.boost_duration_ms * 1000);
    } else if (g_status.state == SENSE_BOOST && now >= g_status.state_until_us) {
        g_status.state = SENSE_COOLDOWN;
        g_status.state_until_us = now + ((int64_t)g_config.cooldown_ms * 1000);
    } else if (g_status.state == SENSE_COOLDOWN && now >= g_status.state_until_us && score <= g_config.settle_threshold) {
        g_status.state = SENSE_IDLE;
    }

    bool stable = score < g_config.settle_threshold && g_status.state != SENSE_BOOST;
    if (stable) {
        baseline_energy = (baseline_energy * 0.98f) + (mean_energy * 0.02f);
        baseline_variance = (baseline_variance * 0.98f) + (variance * 0.02f);
    }

    uint16_t target_rate = g_status.state == SENSE_BOOST ? g_config.boost_rate_hz : g_config.idle_rate_hz;
    csi_min_interval_us = target_rate > 0 ? 1000000LL / target_rate : 0;
    g_status.sample_rate_hz = window_ms > 0 ? (window_packets * 1000U) / window_ms : 0;
    g_status.rssi = last->rssi;
    g_status.noise_floor = last->noise_floor;
    g_status.amplitude_variance = variance;
    g_status.subcarrier_energy_delta = energy_delta;
    g_status.movement_score = score;
    g_status.movement_detected = g_status.state == SENSE_BOOST || score >= g_config.movement_threshold;
    g_status.packet_count += window_packets;
    g_status.last_window_packets = window_packets;
    g_status.last_packet_us = last->timestamp_us;
    g_status.baseline_age_s = (uint32_t)((now - g_status.baseline_started_us) / 1000000LL);
    g_status.last_packet_ms = (uint32_t)((now - g_status.last_packet_us) / 1000LL);
    xSemaphoreGive(state_lock);
}

/**
 * Introduction:
 * Runs forever as the background CSI feature extraction task. It drains raw
 * samples from csi_queue, calculates a rolling mean and variance for each
 * configured feature window using Welford-style accumulation, and passes each
 * completed window to publish_window for movement detection and status updates.
 *
 * Parameters:
 * - arg: Optional FreeRTOS task argument, unused by this firmware.
 *
 * Side effects:
 * - Consumes samples from csi_queue.
 * - Periodically updates shared status through publish_window or by recording
 *   that no packets were received in the current window.
 */
static void csi_aggregation_task(void *arg)
{
    (void)arg;
    csi_sample_t sample = {0};
    csi_sample_t last = {0};
    float mean = 0.0f;
    float m2 = 0.0f;
    uint32_t count = 0;
    int64_t window_start_us = esp_timer_get_time();

    while (true) {
        uint32_t configured_window_ms;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        configured_window_ms = g_config.feature_window_ms;
        xSemaphoreGive(state_lock);

        TickType_t timeout = pdMS_TO_TICKS(configured_window_ms);
        if (xQueueReceive(csi_queue, &sample, timeout) == pdTRUE) {
            last = sample;
            count++;
            float delta = sample.energy - mean;
            mean += delta / (float)count;
            float delta2 = sample.energy - mean;
            m2 += delta * delta2;
        }

        int64_t now = esp_timer_get_time();
        uint32_t elapsed_ms = (uint32_t)((now - window_start_us) / 1000LL);
        if (elapsed_ms >= configured_window_ms) {
            if (count > 0) {
                float variance = count > 1 ? m2 / (float)(count - 1) : 0.0f;
                publish_window(mean, variance, count, elapsed_ms, &last);
            } else {
                xSemaphoreTake(state_lock, portMAX_DELAY);
                g_status.last_packet_ms = g_status.last_packet_us > 0 ? (uint32_t)((now - g_status.last_packet_us) / 1000LL) : 0;
                g_status.sample_rate_hz = 0;
                g_status.last_window_packets = 0;
                xSemaphoreGive(state_lock);
            }

            mean = 0.0f;
            m2 = 0.0f;
            count = 0;
            window_start_us = now;
        }
    }
}

/**
 * Introduction:
 * Serializes a cJSON object and sends it as an HTTP JSON response. This helper
 * centralizes the response content type and permissive CORS header used by the
 * status and configuration APIs.
 *
 * Parameters:
 * - req: HTTP server request to answer.
 * - json: cJSON object to serialize. Ownership remains with the caller.
 *
 * Returns ESP_OK on successful send, ESP_FAIL on allocation or send failure.
 */
static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

/**
 * Introduction:
 * Builds the JSON representation of the node's live status. It snapshots the
 * shared configuration and status under the mutex, computes current packet age,
 * and exposes both sensor metrics and Wi-Fi/provisioning state to clients.
 *
 * Returns:
 * - A newly allocated cJSON object. The caller must delete it with cJSON_Delete.
 */
static cJSON *status_to_json(void)
{
    node_config_t cfg;
    node_status_t status;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cfg = g_config;
    status = g_status;
    status.last_packet_ms = status.last_packet_us > 0 ? (uint32_t)((esp_timer_get_time() - status.last_packet_us) / 1000LL) : 0;
    xSemaphoreGive(state_lock);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "device_id", cfg.device_id);
    cJSON_AddStringToObject(root, "name", cfg.name);
    cJSON_AddStringToObject(root, "ip", status.ip);
    cJSON_AddNumberToObject(root, "uptime_ms", (esp_timer_get_time() - status.boot_us) / 1000LL);
    cJSON_AddNumberToObject(root, "sample_rate_hz", status.sample_rate_hz);
    cJSON_AddNumberToObject(root, "rssi", status.rssi);
    cJSON_AddNumberToObject(root, "noise_floor", status.noise_floor);
    cJSON_AddNumberToObject(root, "amplitude_variance", status.amplitude_variance);
    cJSON_AddNumberToObject(root, "subcarrier_energy_delta", status.subcarrier_energy_delta);
    cJSON_AddNumberToObject(root, "movement_score", status.movement_score);
    cJSON_AddBoolToObject(root, "movement_detected", status.movement_detected);
    cJSON_AddStringToObject(root, "sensing_state", sense_state_name(status.state));
    cJSON_AddNumberToObject(root, "baseline_age_s", status.baseline_age_s);
    cJSON_AddNumberToObject(root, "last_packet_ms", status.last_packet_ms);
    cJSON_AddNumberToObject(root, "packet_count", status.packet_count);
    cJSON_AddNumberToObject(root, "last_window_packets", status.last_window_packets);
    cJSON_AddBoolToObject(root, "wifi_provisioned", status.wifi_provisioned);
    cJSON_AddBoolToObject(root, "sta_connected", status.sta_connected);
    return root;
}

/**
 * Introduction:
 * Builds the JSON representation of the node's current configuration. The
 * function takes a mutex-protected copy of g_config so the HTTP response is
 * internally consistent even if another task updates configuration later.
 *
 * Returns:
 * - A newly allocated cJSON object. The caller must delete it with cJSON_Delete.
 */
static cJSON *config_to_json(void)
{
    node_config_t cfg;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cfg = g_config;
    xSemaphoreGive(state_lock);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "device_id", cfg.device_id);
    cJSON_AddStringToObject(root, "name", cfg.name);
    cJSON_AddNumberToObject(root, "idle_rate_hz", cfg.idle_rate_hz);
    cJSON_AddNumberToObject(root, "boost_rate_hz", cfg.boost_rate_hz);
    cJSON_AddNumberToObject(root, "movement_threshold", cfg.movement_threshold);
    cJSON_AddNumberToObject(root, "settle_threshold", cfg.settle_threshold);
    cJSON_AddNumberToObject(root, "boost_duration_ms", cfg.boost_duration_ms);
    cJSON_AddNumberToObject(root, "cooldown_ms", cfg.cooldown_ms);
    cJSON_AddNumberToObject(root, "feature_window_ms", cfg.feature_window_ms);
    return root;
}

/**
 * Introduction:
 * Reads the complete HTTP request body into a caller-provided buffer and
 * null-terminates it for JSON or form parsing. The size check reserves one byte
 * for the terminator and rejects oversized requests before reading.
 *
 * Parameters:
 * - req: HTTP request containing the body.
 * - buffer: Destination buffer.
 * - buffer_len: Total size of the destination buffer.
 *
 * Returns ESP_OK on success, otherwise ESP_FAIL after sending an HTTP error when
 * appropriate.
 */
static esp_err_t read_request_body(httpd_req_t *req, char *buffer, size_t buffer_len)
{
    if (req->content_len >= buffer_len) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Request body too large");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

/**
 * Introduction:
 * Converts a single hexadecimal digit into its numeric value. This helper is
 * used by the URL decoder for form submissions from the built-in provisioning
 * page.
 *
 * Parameters:
 * - c: Character to interpret as a hexadecimal digit.
 *
 * Returns 0-15 for valid digits, or -1 for any non-hex character.
 */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/**
 * Introduction:
 * Decodes URL-encoded form text in place. It converts percent-encoded bytes
 * such as "%20" back to characters and maps plus signs to spaces, which matches
 * the encoding used by browser form posts.
 *
 * Parameters:
 * - s: Mutable null-terminated string to decode in place.
 */
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

/**
 * Introduction:
 * Extracts one field from an application/x-www-form-urlencoded request body.
 * This supports the provisioning web page, which posts SSID and password values
 * as standard browser form data instead of JSON.
 *
 * Parameters:
 * - body: Full form body, for example "ssid=name&password=secret".
 * - key: Field name to search for.
 * - out: Destination buffer for the decoded value.
 * - out_len: Size of the destination buffer.
 *
 * Returns true when the requested key was found and copied, otherwise false.
 */
static bool form_value(const char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    const char *p = body;
    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            const char *end = strchr(p, '&');
            size_t len = end == NULL ? strlen(p) : (size_t)(end - p);
            if (len >= out_len) {
                len = out_len - 1;
            }
            memcpy(out, p, len);
            out[len] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p != NULL) {
            p++;
        }
    }
    return false;
}

/**
 * Introduction:
 * Serves the built-in browser interface at the root URL. The page displays live
 * node status tiles, draws a simple movement/sample-rate chart, and includes a
 * Wi-Fi provisioning form for first-time setup or network changes.
 *
 * Parameters:
 * - req: HTTP request for the root page.
 *
 * Returns the result of sending the static HTML response.
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32 CSI Node</title><style>"
        "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:#101418;color:#ecf2f8}"
        "main{max-width:860px;margin:0 auto;padding:24px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}"
        ".tile{border:1px solid #33404b;border-radius:8px;padding:14px;background:#182029}.label{color:#9fb0bf;font-size:12px;text-transform:uppercase}"
        ".value{font-size:26px;font-weight:700;margin-top:6px}input,button{font:inherit;padding:10px;border-radius:6px;border:1px solid #465562}"
        "input{background:#0d1116;color:#ecf2f8}button{background:#2e7d5b;color:white;cursor:pointer}form{display:grid;gap:10px;max-width:420px;margin-top:24px}"
        "canvas{width:100%;height:220px;margin-top:18px;border:1px solid #33404b;border-radius:8px;background:#0d1116}"
        "</style></head><body><main><h1 id=\"title\">ESP32 CSI Node</h1><div class=\"grid\" id=\"grid\"></div><canvas id=\"chart\" width=\"820\" height=\"220\"></canvas>"
        "<form method=\"post\" action=\"/api/provision\"><h2>Wi-Fi Provisioning</h2><input name=\"ssid\" placeholder=\"2.4 GHz SSID\" required>"
        "<input name=\"password\" type=\"password\" placeholder=\"Wi-Fi password\"><button type=\"submit\">Save Wi-Fi</button></form>"
        "<script>const keys=['sensing_state','sample_rate_hz','movement_score','movement_detected','rssi','noise_floor','last_packet_ms','packet_count'];"
        "const hist=[];function line(c,a,color,max){c.strokeStyle=color;c.beginPath();a.forEach((v,i)=>{let x=i*(chart.width/119),y=chart.height-10-Math.min(1,v/max)*(chart.height-20);i?c.lineTo(x,y):c.moveTo(x,y)});c.stroke()}"
        "function draw(){const c=chart.getContext('2d');c.clearRect(0,0,chart.width,chart.height);c.lineWidth=2;line(c,hist.map(x=>x.m),'#5bd19a',1);line(c,hist.map(x=>x.r),'#62a8ff',160)}"
        "async function tick(){const r=await fetch('/status.json');const s=await r.json();title.textContent=s.name+' '+s.ip;"
        "grid.innerHTML=keys.map(k=>'<div class=tile><div class=label>'+k+'</div><div class=value>'+s[k]+'</div></div>').join('');"
        "hist.push({m:s.movement_score,r:s.sample_rate_hz});if(hist.length>120)hist.shift();draw()}"
        "tick();setInterval(tick,1000)</script></main></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

/**
 * Introduction:
 * Handles GET requests for the live status API. It creates a fresh status JSON
 * snapshot, sends it to the client, and releases the cJSON allocation before
 * returning.
 *
 * Parameters:
 * - req: HTTP request to answer.
 *
 * Returns the HTTP send result from send_json.
 */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *json = status_to_json();
    esp_err_t err = send_json(req, json);
    cJSON_Delete(json);
    return err;
}

/**
 * Introduction:
 * Handles GET requests for the configuration API. It serializes the current
 * sensing and device configuration so clients can inspect the active thresholds,
 * rates, durations, and display name.
 *
 * Parameters:
 * - req: HTTP request to answer.
 *
 * Returns the HTTP send result from send_json.
 */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    cJSON *json = config_to_json();
    esp_err_t err = send_json(req, json);
    cJSON_Delete(json);
    return err;
}

/**
 * Introduction:
 * Applies an optional numeric JSON property to an existing numeric value. The
 * configuration POST handler uses this helper to keep current settings when a
 * field is omitted, while still accepting partial updates for any number field.
 *
 * Parameters:
 * - root: JSON object to search.
 * - name: Property name to read.
 * - value: In/out numeric value updated only when the property exists as a
 *   number.
 */
static void apply_json_number(cJSON *root, const char *name, double *value)
{
    cJSON *item = cJSON_GetObjectItem(root, name);
    if (cJSON_IsNumber(item)) {
        *value = item->valuedouble;
    }
}

/**
 * Introduction:
 * Handles POST requests that update the node configuration. The handler reads a
 * JSON body, starts from the existing configuration, overlays any supplied
 * fields, sanitizes the result, stores it in shared state, persists it to NVS,
 * and returns the effective configuration.
 *
 * Parameters:
 * - req: HTTP request containing a JSON configuration patch.
 *
 * Returns ESP_OK when the response is sent, or ESP_FAIL for invalid input/read
 * failures.
 */
static esp_err_t config_post_handler(httpd_req_t *req)
{
    char body[MAX_POST_BODY];
    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    node_config_t cfg;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cfg = g_config;
    xSemaphoreGive(state_lock);

    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(name) && name->valuestring != NULL) {
        strlcpy(cfg.name, name->valuestring, sizeof(cfg.name));
    }

    double value;
    value = cfg.device_id;
    apply_json_number(root, "device_id", &value);
    cfg.device_id = (int32_t)value;
    value = cfg.idle_rate_hz;
    apply_json_number(root, "idle_rate_hz", &value);
    cfg.idle_rate_hz = (uint16_t)value;
    value = cfg.boost_rate_hz;
    apply_json_number(root, "boost_rate_hz", &value);
    cfg.boost_rate_hz = (uint16_t)value;
    value = cfg.movement_threshold;
    apply_json_number(root, "movement_threshold", &value);
    cfg.movement_threshold = (float)value;
    value = cfg.settle_threshold;
    apply_json_number(root, "settle_threshold", &value);
    cfg.settle_threshold = (float)value;
    value = cfg.boost_duration_ms;
    apply_json_number(root, "boost_duration_ms", &value);
    cfg.boost_duration_ms = (uint32_t)value;
    value = cfg.cooldown_ms;
    apply_json_number(root, "cooldown_ms", &value);
    cfg.cooldown_ms = (uint32_t)value;
    value = cfg.feature_window_ms;
    apply_json_number(root, "feature_window_ms", &value);
    cfg.feature_window_ms = (uint32_t)value;
    sanitize_config(&cfg);

    xSemaphoreTake(state_lock, portMAX_DELAY);
    g_config = cfg;
    xSemaphoreGive(state_lock);
    save_config(&cfg);
    cJSON_Delete(root);

    cJSON *reply = config_to_json();
    esp_err_t err = send_json(req, reply);
    cJSON_Delete(reply);
    return err;
}

/**
 * Introduction:
 * Handles Wi-Fi provisioning submissions. The handler accepts either JSON or
 * browser form data, validates that an SSID was provided, stores credentials in
 * NVS, sends a short confirmation, and reboots so station mode can start with
 * the new network settings.
 *
 * Parameters:
 * - req: HTTP request containing SSID and password fields.
 *
 * Returns ESP_OK after scheduling the reboot, or ESP_FAIL for invalid/missing
 * request data.
 */
static esp_err_t provision_post_handler(httpd_req_t *req)
{
    char body[MAX_POST_BODY];
    char ssid[33] = {0};
    char password[65] = {0};

    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (root != NULL) {
        cJSON *ssid_json = cJSON_GetObjectItem(root, "ssid");
        cJSON *password_json = cJSON_GetObjectItem(root, "password");
        if (cJSON_IsString(ssid_json)) {
            strlcpy(ssid, ssid_json->valuestring, sizeof(ssid));
        }
        if (cJSON_IsString(password_json)) {
            strlcpy(password, password_json->valuestring, sizeof(password));
        }
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

/**
 * Introduction:
 * Handles requests to forget saved Wi-Fi credentials. Clearing the Wi-Fi NVS
 * namespace and rebooting causes the next startup to enter setup access point
 * mode, allowing the node to be provisioned onto a different network.
 *
 * Parameters:
 * - req: HTTP request to answer before rebooting.
 *
 * Returns ESP_OK after sending the confirmation response and restarting.
 */
static esp_err_t reset_wifi_post_handler(httpd_req_t *req)
{
    clear_wifi_credentials();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Wi-Fi credentials cleared. Rebooting into setup mode.");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/**
 * Introduction:
 * Starts the embedded HTTP server and registers all web UI and API routes. These
 * routes provide the live dashboard, status JSON, configuration read/update
 * endpoints, Wi-Fi provisioning, and Wi-Fi reset behavior.
 *
 * Side effects:
 * - Initializes http_server.
 * - Registers every URI handler used by the firmware.
 */
static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_ERROR_CHECK(httpd_start(&http_server, &config));

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t status = {.uri = "/status.json", .method = HTTP_GET, .handler = status_get_handler};
    httpd_uri_t api_status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler};
    httpd_uri_t config_get = {.uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler};
    httpd_uri_t config_post = {.uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler};
    httpd_uri_t provision = {.uri = "/api/provision", .method = HTTP_POST, .handler = provision_post_handler};
    httpd_uri_t reset_wifi = {.uri = "/api/reset-wifi", .method = HTTP_POST, .handler = reset_wifi_post_handler};

    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &api_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &config_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &config_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &provision));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &reset_wifi));
}

/**
 * Introduction:
 * Main firmware entry point called by ESP-IDF after startup. It initializes
 * persistent storage, creates RTOS synchronization primitives, loads
 * configuration, initializes status defaults, starts the HTTP server, starts
 * Wi-Fi in the correct mode, and launches the CSI aggregation task.
 *
 * Side effects:
 * - May erase and reinitialize NVS if the stored partition is incompatible.
 * - Creates the CSI queue, mutex, event group, web server, Wi-Fi stack, and
 *   background aggregation task.
 */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    csi_queue = xQueueCreate(CSI_QUEUE_LEN, sizeof(csi_sample_t));
    state_lock = xSemaphoreCreateMutex();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(csi_queue == NULL || state_lock == NULL || wifi_event_group == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    load_config(&g_config);
    csi_min_interval_us = g_config.idle_rate_hz > 0 ? 1000000LL / g_config.idle_rate_hz : 0;
    memset(&g_status, 0, sizeof(g_status));
    g_status.state = SENSE_IDLE;
    g_status.boot_us = esp_timer_get_time();
    g_status.baseline_started_us = g_status.boot_us;
    strlcpy(g_status.ip, "0.0.0.0", sizeof(g_status.ip));

    start_http_server();
    init_wifi();
    xTaskCreate(csi_aggregation_task, "csi_aggregation", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "ESP32 CSI node ready");
}
