#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
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
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_NAMESPACE "wifi"
#define CFG_NAMESPACE "nodecfg"
#define CAL_NAMESPACE "nodecal"
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define CSI_QUEUE_LEN 256
#define MAX_POST_BODY 1024
#define MIN_IDLE_RATE_HZ 10
#define MAX_IDLE_RATE_HZ 100
#define MIN_BOOST_RATE_HZ 10
#define MAX_BOOST_RATE_HZ 250
#define MIN_WINDOW_MS 0
#define MAX_WINDOW_MS 1000
#define MIN_BOOST_MS 0
#define MAX_BOOST_MS 20000
#define MIN_COOLDOWN_MS 0
#define MAX_COOLDOWN_MS 20000
#define MIN_PI_POST_MS 1000
#define MAX_PI_POST_MS 30000
#define MIN_CSI_BYTES 16
#define MIN_WINDOW_PACKETS 3
#define PACKET_SPIKE_SIGMA 6.0f
#define MIN_CSI_SNR_DB 10
#define FULL_QUALITY_CSI_SNR_DB 20
#define MIN_BASELINE_NOISE 0.25f
#define BASELINE_NOISE_FLOOR_FRACTION 0.03f
#define DETECT_HISTORY_LEN 5
#define DETECT_REQUIRED_WINDOWS 3
#define CLEAR_REQUIRED_WINDOWS 4
#define TREND_HISTORY_LEN 9
#define CALIBRATION_DURATION_MS 10000
#define CAPTIVE_DNS_PORT 53
#define CAPTIVE_IP "192.168.4.1"
#define NODE_NAME_PREFIX "Movement"
#define DEFAULT_PI_PORT 3005
#define DEFAULT_PI_API_PATH "/espdata"
#define NODE_DISCOVERY_TIMEOUT_MS 180
#define NODE_DISCOVERY_STACK 4096
#define DNS_TASK_STACK 3072
#define PI_TELEMETRY_STACK 4096
#define MOVEMENT_LED_GPIO GPIO_NUM_2
#define MOVEMENT_LED_ON_LEVEL 1
#define MOVEMENT_LED_OFF_LEVEL 0
#define IDENTIFY_DURATION_MS 10000
#define IDENTIFY_BLINK_MS 120
#define LOG_LINE_COUNT 100
#define LOG_LINE_LEN 192
#define CSI_MAC_HISTOGRAM_LEN 10

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
    float shape_variance;
    float phase_proxy;
} csi_sample_t;

typedef struct {
    int32_t device_id;
    char name[32];
    char pi_ip[16];
    uint16_t pi_port;
    char pi_api_path[64];
    char csi_source_mac[18];
    bool csi_source_filter_enabled;
    uint32_t pi_post_interval_ms;
    uint16_t idle_rate_hz;
    uint16_t boost_rate_hz;
    float movement_threshold;
    float settle_threshold;
    float motion_sensitivity;
    uint32_t boost_duration_ms;
    uint32_t cooldown_ms;
    uint32_t feature_window_ms;
} node_config_t;

typedef struct {
    sense_state_t state;
    char ip[16];
    uint32_t sample_rate_hz;
    uint32_t accepted_csi_rate_hz;
    int16_t rssi;
    int16_t noise_floor;
    float amplitude_variance;
    float subcarrier_energy_delta;
    float movement_score;
    float baseline_noise;
    float trend_score;
    float phase_score;
    bool movement_detected;
    uint32_t packet_count;
    uint32_t last_window_packets;
    uint32_t rejected_samples;
    uint32_t source_filtered_samples;
    uint32_t filtered_samples;
    uint32_t throttled_samples;
    uint32_t queue_drops;
    uint8_t confirm_windows;
    uint8_t quiet_windows;
    bool calibrating;
    uint32_t calibration_remaining_ms;
    uint32_t baseline_age_s;
    uint32_t last_packet_ms;
    uint32_t accepted_samples;
    int64_t boot_us;
    int64_t baseline_started_us;
    int64_t last_packet_us;
    int64_t state_until_us;
    bool wifi_provisioned;
    bool sta_connected;
} node_status_t;

typedef struct {
    bool valid;
    float baseline_energy;
    float baseline_variance;
    float baseline_shape;
    float baseline_phase;
    float baseline_phase_variance;
    float baseline_noise;
    float baseline_phase_noise;
    uint32_t calibration_windows;
} calibration_data_t;

static const char *TAG = "csi-node";
static QueueHandle_t csi_queue;
static SemaphoreHandle_t state_lock;
static EventGroupHandle_t wifi_event_group;
static node_config_t g_config;
static node_status_t g_status;
static calibration_data_t g_calibration;
static httpd_handle_t http_server;
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;
static int wifi_retry_count;
static bool setup_mode_active;
static bool config_needs_auto_name;
static bool auto_name_started;
static volatile int64_t csi_min_interval_us = 333333;
static volatile int64_t csi_last_sample_us;
static volatile uint32_t csi_rejected_samples;
static volatile uint32_t csi_source_filtered_samples;
static volatile uint32_t csi_filtered_samples;
static volatile uint32_t csi_throttled_samples;
static volatile uint32_t csi_queue_drops;
static volatile uint32_t csi_accepted_samples;
static volatile bool csi_source_filter_enabled;
static uint8_t csi_source_mac[6];
static volatile bool csi_configured_source_mac_valid;
static uint8_t csi_configured_source_mac[6];
static volatile uint32_t csi_source_seen_before_filter_samples;
static volatile uint32_t csi_source_accepted_after_gates_samples;
static volatile int64_t csi_source_last_seen_us;
static volatile int64_t csi_source_last_accepted_us;
static volatile bool csi_last_mac_valid;
static volatile bool csi_last_filtered_mac_valid;
static volatile bool csi_last_accepted_mac_valid;
static uint8_t csi_last_mac[6];
static uint8_t csi_last_filtered_mac[6];
static uint8_t csi_last_accepted_mac[6];
static volatile bool calibration_requested;
static volatile bool calibration_delete_requested;
static volatile bool calibration_apply_requested;
static volatile bool identify_active;
static volatile int64_t identify_until_us;
static TaskHandle_t identify_task_handle;
static portMUX_TYPE log_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t original_log_vprintf;
static char log_lines[LOG_LINE_COUNT][LOG_LINE_LEN];
static size_t log_next_line;
static size_t log_line_count;

typedef struct {
    uint32_t ip;
    uint32_t netmask;
} discovery_request_t;

typedef struct {
    uint8_t mac[6];
    uint32_t count;
    int64_t last_seen_us;
    bool valid;
} csi_mac_histogram_entry_t;

static portMUX_TYPE csi_mac_histogram_lock = portMUX_INITIALIZER_UNLOCKED;
static csi_mac_histogram_entry_t csi_mac_histogram[CSI_MAC_HISTOGRAM_LEN];

static void trim_log_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        len--;
    }
}

static int captured_log_vprintf(const char *format, va_list args)
{
    char line[LOG_LINE_LEN];
    line[0] = '\0';
    va_list capture_args;
    va_copy(capture_args, args);
    int result = vsnprintf(line, sizeof(line), format, capture_args);
    va_end(capture_args);
    trim_log_line(line);

    portENTER_CRITICAL(&log_lock);
    strlcpy(log_lines[log_next_line], line, sizeof(log_lines[log_next_line]));
    log_next_line = (log_next_line + 1U) % LOG_LINE_COUNT;
    if (log_line_count < LOG_LINE_COUNT) {
        log_line_count++;
    }
    portEXIT_CRITICAL(&log_lock);

    if (original_log_vprintf != NULL) {
        va_list original_args;
        va_copy(original_args, args);
        result = original_log_vprintf(format, original_args);
        va_end(original_args);
    }
    return result;
}

static void install_log_capture(void)
{
    original_log_vprintf = esp_log_set_vprintf(captured_log_vprintf);
}

static bool parse_mac(const char *value, uint8_t out[6]);

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

static void sort_float_array(float *values, size_t count)
{
    for (size_t i = 1; i < count; i++) {
        float key = values[i];
        size_t j = i;
        while (j > 0 && values[j - 1] > key) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = key;
    }
}

static float median_float(const float *values, size_t count)
{
    float scratch[TREND_HISTORY_LEN];
    if (count == 0) {
        return 0.0f;
    }
    if (count > TREND_HISTORY_LEN) {
        count = TREND_HISTORY_LEN;
    }
    for (size_t i = 0; i < count; i++) {
        scratch[i] = values[i];
    }
    sort_float_array(scratch, count);
    if ((count & 1U) == 0) {
        return (scratch[(count / 2U) - 1U] + scratch[count / 2U]) * 0.5f;
    }
    return scratch[count / 2U];
}

static float mad_float(const float *values, size_t count, float median)
{
    float scratch[TREND_HISTORY_LEN];
    if (count == 0) {
        return 0.0f;
    }
    if (count > TREND_HISTORY_LEN) {
        count = TREND_HISTORY_LEN;
    }
    for (size_t i = 0; i < count; i++) {
        scratch[i] = fabsf(values[i] - median);
    }
    return median_float(scratch, count);
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

static void movement_led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << MOVEMENT_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(MOVEMENT_LED_GPIO, MOVEMENT_LED_OFF_LEVEL));
}

static void movement_led_set(bool movement_detected)
{
    if (identify_active) {
        return;
    }
    gpio_set_level(MOVEMENT_LED_GPIO, movement_detected ? MOVEMENT_LED_ON_LEVEL : MOVEMENT_LED_OFF_LEVEL);
}

static void identify_led_task(void *arg)
{
    (void)arg;
    bool on = false;
    while (esp_timer_get_time() < identify_until_us) {
        on = !on;
        gpio_set_level(MOVEMENT_LED_GPIO, on ? MOVEMENT_LED_ON_LEVEL : MOVEMENT_LED_OFF_LEVEL);
        vTaskDelay(pdMS_TO_TICKS(IDENTIFY_BLINK_MS));
    }

    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool movement_detected = g_status.movement_detected;
    xSemaphoreGive(state_lock);
    identify_active = false;
    gpio_set_level(MOVEMENT_LED_GPIO, movement_detected ? MOVEMENT_LED_ON_LEVEL : MOVEMENT_LED_OFF_LEVEL);
    identify_task_handle = NULL;
    vTaskDelete(NULL);
}

static void start_identify_blink(void)
{
    identify_until_us = esp_timer_get_time() + ((int64_t)IDENTIFY_DURATION_MS * 1000);
    identify_active = true;
    if (identify_task_handle == NULL) {
        xTaskCreate(identify_led_task, "identify_led", 2048, NULL, 6, &identify_task_handle);
    }
}

static void apply_configured_sample_rate_locked(void)
{
    uint16_t target_rate = g_status.state == SENSE_BOOST ? g_config.boost_rate_hz : g_config.idle_rate_hz;
    csi_min_interval_us = target_rate > 0 ? 1000000LL / target_rate : 0;
}

static void apply_csi_source_filter_locked(void)
{
    uint8_t parsed[6] = {0};
    bool configured_source_valid = parse_mac(g_config.csi_source_mac, parsed);
    if (configured_source_valid) {
        if (!csi_configured_source_mac_valid ||
            memcmp(csi_configured_source_mac, parsed, sizeof(csi_configured_source_mac)) != 0) {
            csi_source_seen_before_filter_samples = 0;
            csi_source_accepted_after_gates_samples = 0;
            csi_source_last_seen_us = 0;
            csi_source_last_accepted_us = 0;
        }
        memcpy(csi_configured_source_mac, parsed, sizeof(csi_configured_source_mac));
        csi_configured_source_mac_valid = true;
    } else {
        memset(csi_configured_source_mac, 0, sizeof(csi_configured_source_mac));
        csi_configured_source_mac_valid = false;
        csi_source_seen_before_filter_samples = 0;
        csi_source_accepted_after_gates_samples = 0;
        csi_source_last_seen_us = 0;
        csi_source_last_accepted_us = 0;
    }

    if (g_config.csi_source_filter_enabled && configured_source_valid) {
        memcpy(csi_source_mac, parsed, sizeof(csi_source_mac));
        csi_source_filter_enabled = true;
    } else {
        memset(csi_source_mac, 0, sizeof(csi_source_mac));
        csi_source_filter_enabled = false;
    }
}

static bool valid_ipv4_or_empty(const char *value)
{
    struct in_addr parsed;
    return value == NULL || value[0] == '\0' || inet_pton(AF_INET, value, &parsed) == 1;
}

static bool parse_mac(const char *value, uint8_t out[6])
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    unsigned int bytes[6];
    if (sscanf(value, "%02x:%02x:%02x:%02x:%02x:%02x", &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return false;
    }
    for (size_t i = 0; i < 6; i++) {
        if (bytes[i] > 0xffU) {
            return false;
        }
        out[i] = (uint8_t)bytes[i];
    }
    return true;
}

static bool valid_mac_or_empty(const char *value)
{
    uint8_t parsed[6];
    return value == NULL || value[0] == '\0' || parse_mac(value, parsed);
}

static void normalize_mac_text(char *value, size_t value_len)
{
    uint8_t parsed[6];
    if (value == NULL || value_len == 0) {
        return;
    }
    value[value_len - 1] = '\0';
    if (!parse_mac(value, parsed)) {
        value[0] = '\0';
        return;
    }
    snprintf(value, value_len, "%02X:%02X:%02X:%02X:%02X:%02X", parsed[0], parsed[1], parsed[2], parsed[3], parsed[4], parsed[5]);
}

static void format_mac(const uint8_t mac[6], char *out, size_t out_len)
{
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void update_csi_mac_histogram(const uint8_t mac[6], int64_t now_us)
{
    int empty_index = -1;
    int replace_index = 0;
    uint32_t lowest_count = UINT32_MAX;
    int64_t oldest_seen_us = INT64_MAX;

    portENTER_CRITICAL(&csi_mac_histogram_lock);
    for (int i = 0; i < CSI_MAC_HISTOGRAM_LEN; i++) {
        csi_mac_histogram_entry_t *entry = &csi_mac_histogram[i];
        if (entry->valid && memcmp(entry->mac, mac, sizeof(entry->mac)) == 0) {
            if (entry->count < UINT32_MAX) {
                entry->count++;
            }
            entry->last_seen_us = now_us;
            portEXIT_CRITICAL(&csi_mac_histogram_lock);
            return;
        }
        if (!entry->valid && empty_index < 0) {
            empty_index = i;
        } else if (entry->valid && (entry->count < lowest_count ||
                   (entry->count == lowest_count && entry->last_seen_us < oldest_seen_us))) {
            lowest_count = entry->count;
            oldest_seen_us = entry->last_seen_us;
            replace_index = i;
        }
    }

    csi_mac_histogram_entry_t *entry = &csi_mac_histogram[empty_index >= 0 ? empty_index : replace_index];
    memcpy(entry->mac, mac, sizeof(entry->mac));
    entry->count = 1;
    entry->last_seen_us = now_us;
    entry->valid = true;
    portEXIT_CRITICAL(&csi_mac_histogram_lock);
}

static void sanitize_api_path(char *path, size_t path_len)
{
    if (path == NULL || path_len == 0) {
        return;
    }

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

static void format_node_name(int32_t id, char *out, size_t out_len)
{
    snprintf(out, out_len, NODE_NAME_PREFIX "%02lX", (long)id);
}

static bool parse_node_name_id(const char *name, int32_t *id)
{
    size_t prefix_len = strlen(NODE_NAME_PREFIX);
    if (strncmp(name, NODE_NAME_PREFIX, prefix_len) != 0 || !isxdigit((unsigned char)name[prefix_len])) {
        return false;
    }

    char *end = NULL;
    long value = strtol(name + prefix_len, &end, 16);
    if (end == name + prefix_len || *end != '\0' || value < 0 || value > 255) {
        return false;
    }

    *id = (int32_t)value;
    return true;
}

static bool parse_peer_node_id(const char *body, int32_t *id)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return false;
    }

    bool found = false;
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *device_id = cJSON_GetObjectItem(root, "device_id");
    if (cJSON_IsString(name) && name->valuestring != NULL && parse_node_name_id(name->valuestring, id)) {
        found = true;
    } else if (cJSON_IsNumber(device_id) && device_id->valueint >= 0 && device_id->valueint <= 255) {
        *id = device_id->valueint;
        found = true;
    }

    cJSON_Delete(root);
    return found;
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
    strlcpy(cfg->name, NODE_NAME_PREFIX "00", sizeof(cfg->name));
    cfg->pi_ip[0] = '\0';
    cfg->pi_port = DEFAULT_PI_PORT;
    strlcpy(cfg->pi_api_path, DEFAULT_PI_API_PATH, sizeof(cfg->pi_api_path));
    cfg->csi_source_mac[0] = '\0';
    cfg->csi_source_filter_enabled = false;
    cfg->pi_post_interval_ms = 5000;
    cfg->idle_rate_hz = 10;
    cfg->boost_rate_hz = 80;
    cfg->movement_threshold = 3.0f;
    cfg->settle_threshold = 1.2f;
    cfg->motion_sensitivity = 1.0f;
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
    if (cfg->device_id < 0) {
        cfg->device_id = 0;
    } else if (cfg->device_id > 255) {
        cfg->device_id = 255;
    }
    cfg->idle_rate_hz = clamp_u32(cfg->idle_rate_hz, MIN_IDLE_RATE_HZ, MAX_IDLE_RATE_HZ);
    cfg->boost_rate_hz = clamp_u32(cfg->boost_rate_hz, MIN_BOOST_RATE_HZ, MAX_BOOST_RATE_HZ);
    if (cfg->movement_threshold < 1.0f) {
        cfg->movement_threshold = 3.0f;
    }
    if (cfg->settle_threshold < 0.2f || cfg->settle_threshold >= cfg->movement_threshold) {
        cfg->settle_threshold = cfg->movement_threshold * 0.4f;
    }
    if (cfg->motion_sensitivity < 0.3f || cfg->motion_sensitivity > 3.0f) {
        cfg->motion_sensitivity = 1.0f;
    }
    cfg->movement_threshold = clamp_f32(cfg->movement_threshold, 1.0f, 10.0f);
    cfg->settle_threshold = clamp_f32(cfg->settle_threshold, 0.2f, cfg->movement_threshold);
    cfg->motion_sensitivity = clamp_f32(cfg->motion_sensitivity, 0.3f, 3.0f);
    cfg->boost_duration_ms = clamp_u32(cfg->boost_duration_ms, MIN_BOOST_MS, MAX_BOOST_MS);
    cfg->cooldown_ms = clamp_u32(cfg->cooldown_ms, MIN_COOLDOWN_MS, MAX_COOLDOWN_MS);
    cfg->feature_window_ms = clamp_u32(cfg->feature_window_ms, MIN_WINDOW_MS, MAX_WINDOW_MS);
    cfg->pi_post_interval_ms = clamp_u32(cfg->pi_post_interval_ms, MIN_PI_POST_MS, MAX_PI_POST_MS);
    cfg->name[sizeof(cfg->name) - 1] = '\0';
    cfg->pi_ip[sizeof(cfg->pi_ip) - 1] = '\0';
    cfg->pi_api_path[sizeof(cfg->pi_api_path) - 1] = '\0';
    if (!valid_ipv4_or_empty(cfg->pi_ip)) {
        cfg->pi_ip[0] = '\0';
    }
    if (!valid_mac_or_empty(cfg->csi_source_mac)) {
        cfg->csi_source_mac[0] = '\0';
        cfg->csi_source_filter_enabled = false;
    } else if (cfg->csi_source_mac[0] != '\0') {
        normalize_mac_text(cfg->csi_source_mac, sizeof(cfg->csi_source_mac));
    }
    if (cfg->pi_port == 0) {
        cfg->pi_port = DEFAULT_PI_PORT;
    }
    sanitize_api_path(cfg->pi_api_path, sizeof(cfg->pi_api_path));
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
    size_t pi_ip_len = sizeof(cfg->pi_ip);
    size_t pi_api_path_len = sizeof(cfg->pi_api_path);
    size_t csi_source_mac_len = sizeof(cfg->csi_source_mac);
    nvs_get_i32(nvs, "device_id", &cfg->device_id);
    nvs_get_str(nvs, "name", cfg->name, &name_len);
    nvs_get_str(nvs, "pi_ip", cfg->pi_ip, &pi_ip_len);
    nvs_get_u16(nvs, "pi_port", &cfg->pi_port);
    nvs_get_str(nvs, "pi_path", cfg->pi_api_path, &pi_api_path_len);
    nvs_get_str(nvs, "src_mac", cfg->csi_source_mac, &csi_source_mac_len);
    uint8_t src_filter = 0;
    nvs_get_u8(nvs, "src_filter", &src_filter);
    cfg->csi_source_filter_enabled = src_filter != 0;
    nvs_get_u32(nvs, "pi_post_ms", &cfg->pi_post_interval_ms);
    nvs_get_u16(nvs, "idle_hz", &cfg->idle_rate_hz);
    nvs_get_u16(nvs, "boost_hz", &cfg->boost_rate_hz);
    nvs_get_u32(nvs, "boost_ms", &cfg->boost_duration_ms);
    nvs_get_u32(nvs, "cool_ms", &cfg->cooldown_ms);
    nvs_get_u32(nvs, "window_ms", &cfg->feature_window_ms);

    size_t f_len = sizeof(float);
    nvs_get_blob(nvs, "move_th", &cfg->movement_threshold, &f_len);
    f_len = sizeof(float);
    nvs_get_blob(nvs, "settle_th", &cfg->settle_threshold, &f_len);
    f_len = sizeof(float);
    nvs_get_blob(nvs, "sens", &cfg->motion_sensitivity, &f_len);
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

    if ((err = nvs_set_i32(nvs, "device_id", cfg->device_id)) != ESP_OK ||
        (err = nvs_set_str(nvs, "name", cfg->name)) != ESP_OK ||
        (err = nvs_set_str(nvs, "pi_ip", cfg->pi_ip)) != ESP_OK ||
        (err = nvs_set_u16(nvs, "pi_port", cfg->pi_port)) != ESP_OK ||
        (err = nvs_set_str(nvs, "pi_path", cfg->pi_api_path)) != ESP_OK ||
        (err = nvs_set_str(nvs, "src_mac", cfg->csi_source_mac)) != ESP_OK ||
        (err = nvs_set_u8(nvs, "src_filter", cfg->csi_source_filter_enabled ? 1 : 0)) != ESP_OK ||
        (err = nvs_set_u32(nvs, "pi_post_ms", cfg->pi_post_interval_ms)) != ESP_OK ||
        (err = nvs_set_u16(nvs, "idle_hz", cfg->idle_rate_hz)) != ESP_OK ||
        (err = nvs_set_u16(nvs, "boost_hz", cfg->boost_rate_hz)) != ESP_OK ||
        (err = nvs_set_u32(nvs, "boost_ms", cfg->boost_duration_ms)) != ESP_OK ||
        (err = nvs_set_u32(nvs, "cool_ms", cfg->cooldown_ms)) != ESP_OK ||
        (err = nvs_set_u32(nvs, "window_ms", cfg->feature_window_ms)) != ESP_OK ||
        (err = nvs_set_blob(nvs, "move_th", &cfg->movement_threshold, sizeof(float))) != ESP_OK ||
        (err = nvs_set_blob(nvs, "settle_th", &cfg->settle_threshold, sizeof(float))) != ESP_OK ||
        (err = nvs_set_blob(nvs, "sens", &cfg->motion_sensitivity, sizeof(float))) != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void default_calibration(calibration_data_t *cal)
{
    memset(cal, 0, sizeof(*cal));
    cal->baseline_noise = MIN_BASELINE_NOISE;
    cal->baseline_phase_noise = 0.01f;
}

static float baseline_noise_floor(float baseline_energy)
{
    return fmaxf(MIN_BASELINE_NOISE, fabsf(baseline_energy) * BASELINE_NOISE_FLOOR_FRACTION);
}

static void sanitize_calibration(calibration_data_t *cal)
{
    if (!isfinite(cal->baseline_energy) || cal->baseline_energy < 0.0f) {
        cal->baseline_energy = 0.0f;
    }
    if (!isfinite(cal->baseline_variance) || cal->baseline_variance < 0.0f) {
        cal->baseline_variance = 0.0f;
    }
    if (!isfinite(cal->baseline_shape) || cal->baseline_shape < 0.0f) {
        cal->baseline_shape = 0.0f;
    }
    if (!isfinite(cal->baseline_phase)) {
        cal->baseline_phase = 0.0f;
    }
    if (!isfinite(cal->baseline_phase_variance) || cal->baseline_phase_variance < 0.0f) {
        cal->baseline_phase_variance = 0.0f;
    }
    if (!isfinite(cal->baseline_noise) || cal->baseline_noise < baseline_noise_floor(cal->baseline_energy)) {
        cal->baseline_noise = baseline_noise_floor(cal->baseline_energy);
    }
    if (!isfinite(cal->baseline_phase_noise) || cal->baseline_phase_noise < 0.01f) {
        cal->baseline_phase_noise = 0.01f;
    }
    cal->valid = cal->valid && cal->baseline_energy > 0.01f;
    if (!cal->valid) {
        default_calibration(cal);
    }
}

static esp_err_t load_calibration(calibration_data_t *cal)
{
    default_calibration(cal);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CAL_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t valid = 0;
    nvs_get_u8(nvs, "valid", &valid);
    cal->valid = valid != 0;
    nvs_get_u32(nvs, "windows", &cal->calibration_windows);

    size_t f_len = sizeof(float);
    nvs_get_blob(nvs, "energy", &cal->baseline_energy, &f_len);
    f_len = sizeof(float);
    nvs_get_blob(nvs, "variance", &cal->baseline_variance, &f_len);
    f_len = sizeof(float);
    nvs_get_blob(nvs, "shape", &cal->baseline_shape, &f_len);
    f_len = sizeof(float);
    nvs_get_blob(nvs, "phase", &cal->baseline_phase, &f_len);
    f_len = sizeof(float);
    nvs_get_blob(nvs, "phase_var", &cal->baseline_phase_variance, &f_len);
    f_len = sizeof(float);
    nvs_get_blob(nvs, "noise", &cal->baseline_noise, &f_len);
    f_len = sizeof(float);
    nvs_get_blob(nvs, "phase_noise", &cal->baseline_phase_noise, &f_len);
    nvs_close(nvs);
    sanitize_calibration(cal);
    return ESP_OK;
}

static esp_err_t save_calibration(const calibration_data_t *cal)
{
    calibration_data_t copy = *cal;
    sanitize_calibration(&copy);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CAL_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t valid = copy.valid ? 1 : 0;
    if ((err = nvs_set_u8(nvs, "valid", valid)) != ESP_OK ||
        (err = nvs_set_u32(nvs, "windows", copy.calibration_windows)) != ESP_OK ||
        (err = nvs_set_blob(nvs, "energy", &copy.baseline_energy, sizeof(float))) != ESP_OK ||
        (err = nvs_set_blob(nvs, "variance", &copy.baseline_variance, sizeof(float))) != ESP_OK ||
        (err = nvs_set_blob(nvs, "shape", &copy.baseline_shape, sizeof(float))) != ESP_OK ||
        (err = nvs_set_blob(nvs, "phase", &copy.baseline_phase, sizeof(float))) != ESP_OK ||
        (err = nvs_set_blob(nvs, "phase_var", &copy.baseline_phase_variance, sizeof(float))) != ESP_OK ||
        (err = nvs_set_blob(nvs, "noise", &copy.baseline_noise, sizeof(float))) != ESP_OK ||
        (err = nvs_set_blob(nvs, "phase_noise", &copy.baseline_phase_noise, sizeof(float))) != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t clear_calibration(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CAL_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
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

static int build_dns_response(const uint8_t *query, int query_len, uint8_t *response, size_t response_len)
{
    if (query_len < 12 || response_len < (size_t)query_len + 16) {
        return 0;
    }

    int pos = 12;
    while (pos < query_len && query[pos] != 0) {
        pos += query[pos] + 1;
    }
    if (pos + 5 > query_len) {
        return 0;
    }

    memcpy(response, query, query_len);
    response[2] = 0x81;
    response[3] = 0x80;
    response[6] = 0x00;
    response[7] = 0x01;

    int answer = query_len;
    response[answer++] = 0xc0;
    response[answer++] = 0x0c;
    response[answer++] = 0x00;
    response[answer++] = 0x01;
    response[answer++] = 0x00;
    response[answer++] = 0x01;
    response[answer++] = 0x00;
    response[answer++] = 0x00;
    response[answer++] = 0x00;
    response[answer++] = 0x1e;
    response[answer++] = 0x00;
    response[answer++] = 0x04;
    response[answer++] = 192;
    response[answer++] = 168;
    response[answer++] = 4;
    response[answer++] = 1;
    return answer;
}

static void captive_dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "Captive DNS socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port = htons(CAPTIVE_DNS_PORT);
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        ESP_LOGW(TAG, "Captive DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t query[256];
    uint8_t response[272];
    while (setup_mode_active) {
        struct sockaddr_in client = {0};
        socklen_t client_len = sizeof(client);
        int len = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&client, &client_len);
        if (len > 0) {
            int response_len = build_dns_response(query, len, response, sizeof(response));
            if (response_len > 0) {
                sendto(sock, response, response_len, 0, (struct sockaddr *)&client, client_len);
            }
        }
    }

    close(sock);
    vTaskDelete(NULL);
}

static bool probe_node_at(uint32_t ip, int32_t *id)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return false;
    }

    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = NODE_DISCOVERY_TIMEOUT_MS * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(80);
    dest.sin_addr.s_addr = ip;

    bool found = false;
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) == 0) {
        static const char request[] = "GET /status.json HTTP/1.0\r\nHost: movement-node\r\n\r\n";
        if (send(sock, request, sizeof(request) - 1, 0) > 0) {
            char buffer[768];
            int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (len > 0) {
                buffer[len] = '\0';
                char *body = strstr(buffer, "\r\n\r\n");
                if (body != NULL) {
                    body += 4;
                    found = parse_peer_node_id(body, id);
                }
            }
        }
    }

    close(sock);
    return found;
}

static void node_discovery_task(void *arg)
{
    discovery_request_t request = *(discovery_request_t *)arg;
    free(arg);

    bool used[256] = {0};
    uint32_t network = request.ip & request.netmask;
    uint32_t broadcast = network | ~request.netmask;
    int32_t peer_id = 0;

    for (uint32_t ip = network + 1; ip < broadcast; ip++) {
        if (ip == request.ip) {
            continue;
        }
        if (probe_node_at(htonl(ip), &peer_id)) {
            used[peer_id] = true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    int32_t next_id = 0;
    while (next_id < 255 && used[next_id]) {
        next_id++;
    }

    node_config_t cfg;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cfg = g_config;
    cfg.device_id = next_id;
    format_node_name(next_id, cfg.name, sizeof(cfg.name));
    g_config = cfg;
    xSemaphoreGive(state_lock);

    save_config(&cfg);
    if (sta_netif != NULL) {
        esp_netif_set_hostname(sta_netif, cfg.name);
    }
    ESP_LOGI(TAG, "Assigned node identity %s / 0x%02lx", cfg.name, (long)cfg.device_id);
    vTaskDelete(NULL);
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
        csi_rejected_samples++;
        return;
    }

    int64_t now = esp_timer_get_time();
    memcpy(csi_last_mac, info->mac, sizeof(csi_last_mac));
    csi_last_mac_valid = true;
    update_csi_mac_histogram(info->mac, now);
    bool configured_source_match = csi_configured_source_mac_valid &&
                                   memcmp(info->mac, csi_configured_source_mac, sizeof(csi_configured_source_mac)) == 0;
    if (configured_source_match) {
        csi_source_seen_before_filter_samples++;
        csi_source_last_seen_us = now;
    }

    if (csi_source_filter_enabled && memcmp(info->mac, csi_source_mac, sizeof(csi_source_mac)) != 0) {
        memcpy(csi_last_filtered_mac, info->mac, sizeof(csi_last_filtered_mac));
        csi_last_filtered_mac_valid = true;
        csi_source_filtered_samples++;
        return;
    }

    int64_t min_interval_us = csi_min_interval_us;
    if (min_interval_us > 0 && csi_last_sample_us > 0 && now - csi_last_sample_us < min_interval_us) {
        csi_throttled_samples++;
        return;
    }

    int start = info->first_word_invalid ? 4 : 0;
    int len = info->len;
    if (len - start < MIN_CSI_BYTES) {
        csi_rejected_samples++;
        return;
    }

    if (info->rx_ctrl.noise_floor != 0 && info->rx_ctrl.rssi - info->rx_ctrl.noise_floor < MIN_CSI_SNR_DB) {
        csi_rejected_samples++;
        return;
    }

    float mean_power = 0.0f;
    float m2_power = 0.0f;
    float phase_proxy_sum = 0.0f;
    uint16_t pair_count = 0;
    for (int i = start; i + 1 < len; i += 2) {
        float imag = (float)info->buf[i];
        float real = (float)info->buf[i + 1];
        float power = (real * real) + (imag * imag);
        if (power <= 0.0f) {
            continue;
        }
        pair_count++;
        float delta = power - mean_power;
        mean_power += delta / pair_count;
        m2_power += delta * (power - mean_power);
        phase_proxy_sum += imag / sqrtf(power);
    }

    if (pair_count < (MIN_CSI_BYTES / 2) || mean_power <= 0.0f) {
        csi_rejected_samples++;
        return;
    }

    float variance_power = pair_count > 1 ? m2_power / (float)(pair_count - 1) : 0.0f;
    float shape_variance = variance_power / fmaxf(mean_power * mean_power, 1.0f);
    float feature = log1pf(mean_power) * (1.0f + sqrtf(fmaxf(shape_variance, 0.0f)));

    static bool filter_ready;
    static uint32_t filter_warmup;
    static float filter_center;
    static float filter_deviation;
    if (!filter_ready) {
        filter_center = feature;
        filter_deviation = fmaxf(feature * 0.10f, 0.05f);
        filter_ready = true;
    } else {
        float diff = fabsf(feature - filter_center);
        float limit = PACKET_SPIKE_SIGMA * fmaxf(filter_deviation, 0.05f);
        if (filter_warmup > 20 && diff > limit) {
            feature = feature > filter_center ? filter_center + limit : filter_center - limit;
            csi_filtered_samples++;
        }
        filter_center = (filter_center * 0.995f) + (feature * 0.005f);
        filter_deviation = (filter_deviation * 0.995f) + (fabsf(feature - filter_center) * 0.005f);
    }
    filter_warmup++;

    csi_sample_t sample = {
        .timestamp_us = now,
        .rssi = info->rx_ctrl.rssi,
        .noise_floor = info->rx_ctrl.noise_floor,
        .csi_len = info->len,
        .energy = feature,
        .shape_variance = shape_variance,
        .phase_proxy = phase_proxy_sum / (float)pair_count,
    };

    if (csi_queue != NULL) {
        csi_last_sample_us = now;
        csi_accepted_samples++;
        if (xQueueSend(csi_queue, &sample, 0) != pdTRUE) {
            csi_queue_drops++;
        } else {
            memcpy(csi_last_accepted_mac, info->mac, sizeof(csi_last_accepted_mac));
            csi_last_accepted_mac_valid = true;
            if (configured_source_match) {
                csi_source_accepted_after_gates_samples++;
                csi_source_last_accepted_us = now;
            }
        }
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
        if (config_needs_auto_name && !auto_name_started) {
            discovery_request_t *request = calloc(1, sizeof(*request));
            if (request != NULL) {
                request->ip = ntohl(event->ip_info.ip.addr);
                request->netmask = ntohl(event->ip_info.netmask.addr);
                auto_name_started = true;
                xTaskCreate(node_discovery_task, "node_discovery", NODE_DISCOVERY_STACK, request, 4, NULL);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
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
    setup_mode_active = true;
    if (ap_netif != NULL) {
        esp_netif_set_hostname(ap_netif, "Movement-Setup");
        uint8_t offer_dns = 0x02;
        esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_config_t ap_config = {0};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "Movement-Setup-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    update_status_ip(CAPTIVE_IP);
    xTaskCreate(captive_dns_task, "captive_dns", DNS_TASK_STACK, NULL, 4, NULL);
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
    setup_mode_active = false;
    if (sta_netif != NULL) {
        esp_netif_set_hostname(sta_netif, g_config.name);
    }

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
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
static void publish_window(float mean_energy, float variance, float mean_shape, float mean_phase, float phase_variance, uint32_t window_packets, uint32_t window_ms, const csi_sample_t *last)
{
    static float baseline_energy = 0.0f;
    static float baseline_variance = 0.0f;
    static float baseline_shape = 0.0f;
    static float baseline_phase = 0.0f;
    static float baseline_phase_variance = 0.0f;
    static float baseline_phase_noise = 0.01f;
    static float baseline_noise = 0.05f;
    static float trend_history[TREND_HISTORY_LEN];
    static uint8_t trend_count;
    static uint8_t trend_index;
    static uint8_t detection_history;
    static uint8_t detection_count;
    static uint8_t confirm_windows;
    static uint8_t quiet_windows;
    static bool calibrating;
    static bool baseline_loaded;
    static int64_t calibration_end_us;
    static float calibration_energy_sum;
    static float calibration_variance_sum;
    static float calibration_shape_sum;
    static float calibration_phase_sum;
    static float calibration_phase_variance_sum;
    static uint32_t calibration_windows;

    int64_t now = esp_timer_get_time();
    xSemaphoreTake(state_lock, portMAX_DELAY);

    if (!baseline_loaded || calibration_apply_requested) {
        calibration_apply_requested = false;
        if (g_calibration.valid) {
            baseline_energy = g_calibration.baseline_energy;
            baseline_variance = g_calibration.baseline_variance;
            baseline_shape = g_calibration.baseline_shape;
            baseline_phase = g_calibration.baseline_phase;
            baseline_phase_variance = g_calibration.baseline_phase_variance;
            baseline_noise = g_calibration.baseline_noise;
            baseline_phase_noise = g_calibration.baseline_phase_noise;
            g_status.baseline_started_us = now;
            ESP_LOGI(TAG, "Loaded persisted CSI calibration baseline");
        }
        baseline_loaded = true;
    }

    if (calibration_requested) {
        calibration_requested = false;
        calibrating = true;
        calibration_end_us = now + ((int64_t)CALIBRATION_DURATION_MS * 1000);
        calibration_energy_sum = 0.0f;
        calibration_variance_sum = 0.0f;
        calibration_shape_sum = 0.0f;
        calibration_phase_sum = 0.0f;
        calibration_phase_variance_sum = 0.0f;
        calibration_windows = 0;
        trend_count = 0;
        trend_index = 0;
        detection_history = 0;
        detection_count = 0;
        confirm_windows = 0;
        quiet_windows = 0;
        g_status.state = SENSE_IDLE;
        ESP_LOGI(TAG, "CSI stillness calibration started");
    }

    if (calibration_delete_requested) {
        calibration_delete_requested = false;
        calibrating = false;
        calibration_windows = 0;
        baseline_energy = 0.0f;
        baseline_variance = 0.0f;
        baseline_shape = 0.0f;
        baseline_phase = 0.0f;
        baseline_phase_variance = 0.0f;
        baseline_phase_noise = 0.01f;
        baseline_noise = 0.05f;
        baseline_loaded = true;
        default_calibration(&g_calibration);
        trend_count = 0;
        trend_index = 0;
        detection_history = 0;
        detection_count = 0;
        confirm_windows = 0;
        quiet_windows = 0;
        g_status.state = SENSE_IDLE;
        g_status.baseline_started_us = now;
        clear_calibration();
        ESP_LOGI(TAG, "CSI calibration data deleted; baseline will rebuild from live windows");
    }

    if (baseline_energy <= 0.01f) {
        baseline_energy = mean_energy;
        baseline_variance = variance;
        baseline_shape = mean_shape;
        baseline_phase = mean_phase;
        baseline_phase_variance = phase_variance;
        baseline_phase_noise = fmaxf(sqrtf(fmaxf(phase_variance, 0.0f)), 0.01f);
        baseline_noise = fmaxf(sqrtf(fmaxf(variance, 0.0f)), baseline_noise_floor(baseline_energy));
        g_status.baseline_started_us = now;
    }

    if (calibrating) {
        calibration_energy_sum += mean_energy;
        calibration_variance_sum += variance;
        calibration_shape_sum += mean_shape;
        calibration_phase_sum += mean_phase;
        calibration_phase_variance_sum += phase_variance;
        calibration_windows++;
        if (now >= calibration_end_us && calibration_windows > 0) {
            baseline_energy = calibration_energy_sum / (float)calibration_windows;
            baseline_variance = calibration_variance_sum / (float)calibration_windows;
            baseline_shape = calibration_shape_sum / (float)calibration_windows;
            baseline_phase = calibration_phase_sum / (float)calibration_windows;
            baseline_phase_variance = calibration_phase_variance_sum / (float)calibration_windows;
            baseline_phase_noise = fmaxf(sqrtf(fmaxf(baseline_phase_variance, 0.0f)), 0.01f);
            baseline_noise = fmaxf(sqrtf(fmaxf(baseline_variance, 0.0f)), baseline_noise_floor(baseline_energy));
            g_calibration.valid = true;
            g_calibration.baseline_energy = baseline_energy;
            g_calibration.baseline_variance = baseline_variance;
            g_calibration.baseline_shape = baseline_shape;
            g_calibration.baseline_phase = baseline_phase;
            g_calibration.baseline_phase_variance = baseline_phase_variance;
            g_calibration.baseline_noise = baseline_noise;
            g_calibration.baseline_phase_noise = baseline_phase_noise;
            g_calibration.calibration_windows = calibration_windows;
            esp_err_t save_err = save_calibration(&g_calibration);
            if (save_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save CSI calibration: %s", esp_err_to_name(save_err));
            }
            g_status.baseline_started_us = now;
            calibrating = false;
            ESP_LOGI(TAG, "CSI stillness calibration complete");
        }
    }

    float noise_denominator = fmaxf(baseline_noise, baseline_noise_floor(baseline_energy));
    float energy_z = fabsf(mean_energy - baseline_energy) / noise_denominator;
    float variance_delta = fabsf(variance - baseline_variance) / fmaxf(baseline_variance + noise_denominator, 1.0f);
    float shape_delta = fabsf(mean_shape - baseline_shape) / fmaxf(baseline_shape + 0.01f, 0.01f);
    float phase_delta = (fabsf(mean_phase - baseline_phase) * 0.5f) / fmaxf(baseline_phase_noise, 0.01f);
    float phase_variance_delta = fabsf(phase_variance - baseline_phase_variance) / fmaxf(baseline_phase_variance + baseline_phase_noise, 0.01f);
    float phase_score = clamp_f32((phase_delta * 0.45f) + (phase_variance_delta * 0.55f), 0.0f, 10.0f);
    float trend_score = 0.0f;
    if (trend_count >= 3) {
        float trend_median = median_float(trend_history, trend_count);
        float trend_mad = mad_float(trend_history, trend_count, trend_median);
        float robust_sigma = fmaxf(trend_mad * 1.4826f, noise_denominator);
        trend_score = clamp_f32(fabsf(mean_energy - trend_median) / robust_sigma, 0.0f, 10.0f);
    }
    trend_history[trend_index] = mean_energy;
    trend_index = (trend_index + 1U) % TREND_HISTORY_LEN;
    if (trend_count < TREND_HISTORY_LEN) {
        trend_count++;
    }
    float signal_quality = 0.75f;
    if (last->noise_floor != 0) {
        float snr = (float)(last->rssi - last->noise_floor);
        signal_quality = clamp_f32((snr - (float)MIN_CSI_SNR_DB) / (float)(FULL_QUALITY_CSI_SNR_DB - MIN_CSI_SNR_DB), 0.0f, 1.0f);
    }
    float packet_quality = window_packets >= MIN_WINDOW_PACKETS ? signal_quality : 0.0f;
    float fused_score = (0.42f * energy_z) + (0.22f * variance_delta) + (0.14f * shape_delta) + (0.14f * trend_score) + (0.08f * phase_score);
    float score_quality = (!calibrating && g_calibration.valid) ? packet_quality : 0.0f;
    float score = score_quality * clamp_f32(fused_score * g_config.motion_sensitivity, 0.0f, 10.0f);

    bool window_detected = !calibrating && packet_quality > 0.0f && score >= g_config.movement_threshold;
    bool window_quiet = packet_quality > 0.0f && (calibrating || score <= g_config.settle_threshold);
    uint8_t oldest = (detection_history >> (DETECT_HISTORY_LEN - 1)) & 0x01;
    detection_count -= oldest;
    detection_history = ((detection_history << 1) | (window_detected ? 1 : 0)) & ((1 << DETECT_HISTORY_LEN) - 1);
    detection_count += window_detected ? 1 : 0;

    if (window_detected) {
        if (confirm_windows < DETECT_REQUIRED_WINDOWS) {
            confirm_windows++;
        }
        quiet_windows = 0;
    } else if (window_quiet) {
        if (quiet_windows < CLEAR_REQUIRED_WINDOWS) {
            quiet_windows++;
        }
        confirm_windows = 0;
    } else {
        quiet_windows = 0;
    }

    if (g_status.state == SENSE_IDLE && detection_count >= DETECT_REQUIRED_WINDOWS && confirm_windows >= 2) {
        g_status.state = SENSE_BOOST;
        g_status.state_until_us = now + ((int64_t)g_config.boost_duration_ms * 1000);
    } else if (g_status.state == SENSE_BOOST && now >= g_status.state_until_us) {
        g_status.state = SENSE_COOLDOWN;
        g_status.state_until_us = now + ((int64_t)g_config.cooldown_ms * 1000);
    } else if (g_status.state == SENSE_COOLDOWN && now >= g_status.state_until_us && quiet_windows >= CLEAR_REQUIRED_WINDOWS) {
        g_status.state = SENSE_IDLE;
    }

    bool stable = window_quiet && g_status.state != SENSE_BOOST;
    if (stable) {
        float energy_residual = fabsf(mean_energy - baseline_energy);
        baseline_energy = (baseline_energy * 0.99f) + (mean_energy * 0.01f);
        baseline_variance = (baseline_variance * 0.99f) + (variance * 0.01f);
        baseline_shape = (baseline_shape * 0.99f) + (mean_shape * 0.01f);
        baseline_phase = (baseline_phase * 0.99f) + (mean_phase * 0.01f);
        baseline_phase_variance = (baseline_phase_variance * 0.99f) + (phase_variance * 0.01f);
        baseline_phase_noise = (baseline_phase_noise * 0.99f) + (fabsf(mean_phase - baseline_phase) * 0.01f);
        baseline_noise = fmaxf((baseline_noise * 0.99f) + (energy_residual * 0.01f), baseline_noise_floor(baseline_energy));
    }

    apply_configured_sample_rate_locked();
    g_status.sample_rate_hz = window_ms > 0 ? (window_packets * 1000U) / window_ms : 0;
    g_status.rssi = last->rssi;
    g_status.noise_floor = last->noise_floor;
    g_status.amplitude_variance = variance;
    g_status.subcarrier_energy_delta = energy_z;
    g_status.movement_score = score;
    g_status.baseline_noise = baseline_noise;
    g_status.trend_score = trend_score;
    g_status.phase_score = phase_score;
    g_status.movement_detected = g_status.state == SENSE_BOOST || detection_count >= DETECT_REQUIRED_WINDOWS;
    movement_led_set(g_status.movement_detected);
    g_status.packet_count += window_packets;
    g_status.last_window_packets = window_packets;
    g_status.rejected_samples = csi_rejected_samples;
    g_status.source_filtered_samples = csi_source_filtered_samples;
    g_status.filtered_samples = csi_filtered_samples;
    g_status.throttled_samples = csi_throttled_samples;
    g_status.queue_drops = csi_queue_drops;
    g_status.confirm_windows = confirm_windows;
    g_status.quiet_windows = quiet_windows;
    g_status.calibrating = calibrating;
    g_status.calibration_remaining_ms = calibrating && now < calibration_end_us ? (uint32_t)((calibration_end_us - now) / 1000LL) : 0;
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
    float shape_mean = 0.0f;
    float phase_mean = 0.0f;
    float phase_m2 = 0.0f;
    uint32_t count = 0;
    int64_t window_start_us = esp_timer_get_time();
    int64_t rate_start_us = window_start_us;
    uint32_t last_accepted_total = csi_accepted_samples;

    while (true) {
        uint32_t configured_window_ms;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        configured_window_ms = g_config.feature_window_ms;
        xSemaphoreGive(state_lock);

        uint32_t effective_window_ms = configured_window_ms > 0 ? configured_window_ms : 1;
        TickType_t timeout = pdMS_TO_TICKS(effective_window_ms);
        if (xQueueReceive(csi_queue, &sample, timeout) == pdTRUE) {
            last = sample;
            count++;
            float delta = sample.energy - mean;
            mean += delta / (float)count;
            float delta2 = sample.energy - mean;
            m2 += delta * delta2;
            shape_mean += (sample.shape_variance - shape_mean) / (float)count;
            float phase_delta = sample.phase_proxy - phase_mean;
            phase_mean += phase_delta / (float)count;
            phase_m2 += phase_delta * (sample.phase_proxy - phase_mean);
        }

        int64_t now = esp_timer_get_time();
        uint32_t rate_elapsed_ms = (uint32_t)((now - rate_start_us) / 1000LL);
        if (rate_elapsed_ms >= 1000) {
            uint32_t accepted_total = csi_accepted_samples;
            uint32_t accepted_delta = accepted_total - last_accepted_total;
            xSemaphoreTake(state_lock, portMAX_DELAY);
            g_status.accepted_csi_rate_hz = (accepted_delta * 1000U) / rate_elapsed_ms;
            g_status.accepted_samples = accepted_total;
            xSemaphoreGive(state_lock);
            last_accepted_total = accepted_total;
            rate_start_us = now;
        }

        uint32_t elapsed_ms = (uint32_t)((now - window_start_us) / 1000LL);
        if (elapsed_ms >= effective_window_ms) {
            if (count > 0) {
                float variance = count > 1 ? m2 / (float)(count - 1) : 0.0f;
                float phase_variance = count > 1 ? phase_m2 / (float)(count - 1) : 0.0f;
                publish_window(mean, variance, shape_mean, phase_mean, phase_variance, count, elapsed_ms, &last);
            } else {
                xSemaphoreTake(state_lock, portMAX_DELAY);
                g_status.last_packet_ms = g_status.last_packet_us > 0 ? (uint32_t)((now - g_status.last_packet_us) / 1000LL) : 0;
                g_status.sample_rate_hz = 0;
                g_status.last_window_packets = 0;
                g_status.rejected_samples = csi_rejected_samples;
                g_status.source_filtered_samples = csi_source_filtered_samples;
                g_status.filtered_samples = csi_filtered_samples;
                g_status.throttled_samples = csi_throttled_samples;
                g_status.queue_drops = csi_queue_drops;
                xSemaphoreGive(state_lock);
            }

            mean = 0.0f;
            m2 = 0.0f;
            shape_mean = 0.0f;
            phase_mean = 0.0f;
            phase_m2 = 0.0f;
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
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
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
    calibration_data_t cal;
    uint8_t last_mac[6];
    uint8_t last_filtered_mac[6];
    uint8_t last_accepted_mac[6];
    uint8_t configured_source_mac[6];
    bool last_mac_valid = csi_last_mac_valid;
    bool last_filtered_mac_valid = csi_last_filtered_mac_valid;
    bool last_accepted_mac_valid = csi_last_accepted_mac_valid;
    bool configured_source_valid = csi_configured_source_mac_valid;
    uint32_t source_seen_before_filter = csi_source_seen_before_filter_samples;
    uint32_t source_accepted_after_gates = csi_source_accepted_after_gates_samples;
    int64_t source_last_seen_us = csi_source_last_seen_us;
    int64_t source_last_accepted_us = csi_source_last_accepted_us;
    memcpy(last_mac, csi_last_mac, sizeof(last_mac));
    memcpy(last_filtered_mac, csi_last_filtered_mac, sizeof(last_filtered_mac));
    memcpy(last_accepted_mac, csi_last_accepted_mac, sizeof(last_accepted_mac));
    memcpy(configured_source_mac, csi_configured_source_mac, sizeof(configured_source_mac));
    csi_mac_histogram_entry_t mac_histogram[CSI_MAC_HISTOGRAM_LEN];
    portENTER_CRITICAL(&csi_mac_histogram_lock);
    memcpy(mac_histogram, csi_mac_histogram, sizeof(mac_histogram));
    portEXIT_CRITICAL(&csi_mac_histogram_lock);
    for (int i = 0; i < CSI_MAC_HISTOGRAM_LEN - 1; i++) {
        for (int j = i + 1; j < CSI_MAC_HISTOGRAM_LEN; j++) {
            if ((!mac_histogram[i].valid && mac_histogram[j].valid) ||
                (mac_histogram[i].valid && mac_histogram[j].valid && mac_histogram[j].count > mac_histogram[i].count)) {
                csi_mac_histogram_entry_t tmp = mac_histogram[i];
                mac_histogram[i] = mac_histogram[j];
                mac_histogram[j] = tmp;
            }
        }
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cfg = g_config;
    status = g_status;
    cal = g_calibration;
    status.last_packet_ms = status.last_packet_us > 0 ? (uint32_t)((esp_timer_get_time() - status.last_packet_us) / 1000LL) : 0;
    xSemaphoreGive(state_lock);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "device_id", cfg.device_id);
    cJSON_AddStringToObject(root, "name", cfg.name);
    cJSON_AddStringToObject(root, "ip", status.ip);
    cJSON_AddNumberToObject(root, "uptime_ms", (esp_timer_get_time() - status.boot_us) / 1000LL);
    cJSON_AddNumberToObject(root, "sample_rate_hz", status.sample_rate_hz);
    cJSON_AddNumberToObject(root, "accepted_csi_rate_hz", status.accepted_csi_rate_hz);
    cJSON_AddNumberToObject(root, "rssi", status.rssi);
    cJSON_AddNumberToObject(root, "noise_floor", status.noise_floor);
    cJSON_AddNumberToObject(root, "amplitude_variance", status.amplitude_variance);
    cJSON_AddNumberToObject(root, "subcarrier_energy_delta", status.subcarrier_energy_delta);
    cJSON_AddNumberToObject(root, "movement_score", status.movement_score);
    cJSON_AddNumberToObject(root, "baseline_noise", status.baseline_noise);
    cJSON_AddNumberToObject(root, "trend_score", status.trend_score);
    cJSON_AddNumberToObject(root, "phase_score", status.phase_score);
    cJSON_AddBoolToObject(root, "movement_detected", status.movement_detected);
    cJSON_AddStringToObject(root, "csi_source_mac", cfg.csi_source_mac);
    cJSON_AddBoolToObject(root, "csi_source_filter_enabled", cfg.csi_source_filter_enabled);
    char last_mac_text[18] = {0};
    char last_filtered_mac_text[18] = {0};
    char last_accepted_mac_text[18] = {0};
    if (last_mac_valid) {
        format_mac(last_mac, last_mac_text, sizeof(last_mac_text));
    }
    if (last_filtered_mac_valid) {
        format_mac(last_filtered_mac, last_filtered_mac_text, sizeof(last_filtered_mac_text));
    }
    if (last_accepted_mac_valid) {
        format_mac(last_accepted_mac, last_accepted_mac_text, sizeof(last_accepted_mac_text));
    }
    cJSON_AddStringToObject(root, "last_csi_mac", last_mac_valid ? last_mac_text : "");
    cJSON_AddStringToObject(root, "last_filtered_csi_mac", last_filtered_mac_valid ? last_filtered_mac_text : "");
    cJSON_AddStringToObject(root, "last_accepted_csi_mac", last_accepted_mac_valid ? last_accepted_mac_text : "");
    cJSON *source_diag = cJSON_AddObjectToObject(root, "csi_source_mac_diagnostics");
    if (source_diag != NULL) {
        char configured_source_mac_text[18] = {0};
        if (configured_source_valid) {
            format_mac(configured_source_mac, configured_source_mac_text, sizeof(configured_source_mac_text));
        }
        int64_t now_us = esp_timer_get_time();
        cJSON_AddStringToObject(source_diag, "mac", configured_source_valid ? configured_source_mac_text : "");
        cJSON_AddBoolToObject(source_diag, "configured", configured_source_valid);
        cJSON_AddBoolToObject(source_diag, "filter_enabled", cfg.csi_source_filter_enabled);
        cJSON_AddNumberToObject(source_diag, "seen_before_filter", source_seen_before_filter);
        cJSON_AddNumberToObject(source_diag, "accepted_after_gates", source_accepted_after_gates);
        if (source_last_seen_us > 0) {
            cJSON_AddNumberToObject(source_diag, "last_seen_ms", (uint32_t)((now_us - source_last_seen_us) / 1000LL));
        } else {
            cJSON_AddNullToObject(source_diag, "last_seen_ms");
        }
        if (source_last_accepted_us > 0) {
            cJSON_AddNumberToObject(source_diag, "last_accepted_ms", (uint32_t)((now_us - source_last_accepted_us) / 1000LL));
        } else {
            cJSON_AddNullToObject(source_diag, "last_accepted_ms");
        }
    }
    cJSON *histogram = cJSON_AddArrayToObject(root, "csi_mac_histogram");
    if (histogram != NULL) {
        int64_t now_us = esp_timer_get_time();
        for (int i = 0; i < CSI_MAC_HISTOGRAM_LEN; i++) {
            if (!mac_histogram[i].valid) {
                continue;
            }
            char mac_text[18] = {0};
            format_mac(mac_histogram[i].mac, mac_text, sizeof(mac_text));
            cJSON *entry = cJSON_CreateObject();
            if (entry == NULL) {
                continue;
            }
            cJSON_AddStringToObject(entry, "mac", mac_text);
            cJSON_AddNumberToObject(entry, "count", mac_histogram[i].count);
            cJSON_AddNumberToObject(entry, "last_seen_ms",
                                    mac_histogram[i].last_seen_us > 0 ? (uint32_t)((now_us - mac_histogram[i].last_seen_us) / 1000LL) : 0);
            cJSON_AddItemToArray(histogram, entry);
        }
    }
    cJSON_AddStringToObject(root, "sensing_state", sense_state_name(status.state));
    cJSON_AddNumberToObject(root, "baseline_age_s", status.baseline_age_s);
    cJSON_AddNumberToObject(root, "last_packet_ms", status.last_packet_ms);
    cJSON_AddNumberToObject(root, "accepted_samples", status.accepted_samples);
    cJSON_AddNumberToObject(root, "packet_count", status.packet_count);
    cJSON_AddNumberToObject(root, "last_window_packets", status.last_window_packets);
    cJSON_AddNumberToObject(root, "rejected_samples", status.rejected_samples);
    cJSON_AddNumberToObject(root, "source_filtered_samples", status.source_filtered_samples);
    cJSON_AddNumberToObject(root, "filtered_samples", status.filtered_samples);
    cJSON_AddNumberToObject(root, "throttled_samples", status.throttled_samples);
    cJSON_AddNumberToObject(root, "queue_drops", status.queue_drops);
    cJSON_AddNumberToObject(root, "confirm_windows", status.confirm_windows);
    cJSON_AddNumberToObject(root, "quiet_windows", status.quiet_windows);
    cJSON_AddBoolToObject(root, "calibrating", status.calibrating);
    cJSON_AddNumberToObject(root, "calibration_remaining_ms", status.calibration_remaining_ms);
    cJSON_AddBoolToObject(root, "calibration_persisted", cal.valid);
    cJSON_AddNumberToObject(root, "calibration_windows", cal.calibration_windows);
    cJSON_AddBoolToObject(root, "identifying", identify_active);
    cJSON_AddBoolToObject(root, "wifi_provisioned", status.wifi_provisioned);
    cJSON_AddBoolToObject(root, "sta_connected", status.sta_connected);
    return root;
}

static bool pi_response_acknowledged(const char *response, int http_status)
{
    if (http_status < 200 || http_status >= 300) {
        return false;
    }

    const char *body = strstr(response, "\r\n\r\n");
    if (body == NULL) {
        return false;
    }
    body += 4;

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return false;
    }

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    bool acknowledged = cJSON_IsTrue(ok);
    cJSON_Delete(root);
    return acknowledged;
}

static void pi_telemetry_task(void *arg)
{
    (void)arg;

    while (true) {
        node_config_t cfg;
        node_status_t status;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        cfg = g_config;
        status = g_status;
        xSemaphoreGive(state_lock);

        if (status.sta_connected && cfg.pi_ip[0] != '\0') {
            cJSON *json = status_to_json();
            char *body = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);

            if (body != NULL) {
                int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
                if (sock >= 0) {
                    struct timeval timeout = {
                        .tv_sec = 3,
                        .tv_usec = 0,
                    };
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

                    struct sockaddr_in dest = {0};
                    dest.sin_family = AF_INET;
                    dest.sin_port = htons(cfg.pi_port);
                    inet_pton(AF_INET, cfg.pi_ip, &dest.sin_addr);

                    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) == 0) {
                        char header[384];
                        int header_len = snprintf(header, sizeof(header),
                                                  "POST %s HTTP/1.1\r\n"
                                                  "Host: %s:%u\r\n"
                                                  "Content-Type: application/json\r\n"
                                                  "Content-Length: %u\r\n"
                                                  "Connection: close\r\n\r\n",
                                                  cfg.pi_api_path, cfg.pi_ip, (unsigned)cfg.pi_port,
                                                  (unsigned)strlen(body));

                        if (header_len > 0 && header_len < (int)sizeof(header) &&
                            send(sock, header, header_len, 0) == header_len &&
                            send(sock, body, strlen(body), 0) == (int)strlen(body)) {
                            char response[1024];
                            int total = 0;
                            while (total < (int)sizeof(response) - 1) {
                                int len = recv(sock, response + total, sizeof(response) - 1 - total, 0);
                                if (len <= 0) {
                                    break;
                                }
                                total += len;
                            }
                            if (total > 0) {
                                response[total] = '\0';
                                int http_status = 0;
                                sscanf(response, "HTTP/%*s %d", &http_status);
                                if (pi_response_acknowledged(response, http_status)) {
                                    ESP_LOGI(TAG, "Pi acknowledged telemetry receipt from %s:%u%s", cfg.pi_ip,
                                             (unsigned)cfg.pi_port, cfg.pi_api_path);
                                } else {
                                    ESP_LOGW(TAG, "Pi telemetry post returned HTTP %d without acknowledgement", http_status);
                                }
                            } else {
                                ESP_LOGW(TAG, "Pi telemetry post did not return a response");
                            }
                        } else {
                            ESP_LOGW(TAG, "Pi telemetry post failed while sending request");
                        }
                    } else {
                        ESP_LOGW(TAG, "Pi telemetry connection failed to %s:%u", cfg.pi_ip, (unsigned)cfg.pi_port);
                    }
                    close(sock);
                }
                free(body);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(cfg.pi_post_interval_ms));
    }
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
    cJSON_AddStringToObject(root, "pi_ip", cfg.pi_ip);
    cJSON_AddNumberToObject(root, "pi_port", cfg.pi_port);
    cJSON_AddStringToObject(root, "pi_api_path", cfg.pi_api_path);
    cJSON_AddStringToObject(root, "csi_source_mac", cfg.csi_source_mac);
    cJSON_AddBoolToObject(root, "csi_source_filter_enabled", cfg.csi_source_filter_enabled);
    char pi_api_url[128];
    snprintf(pi_api_url, sizeof(pi_api_url), "http://%s:%u%s", cfg.pi_ip[0] ? cfg.pi_ip : "{pi-IP-address}",
             (unsigned)cfg.pi_port, cfg.pi_api_path);
    cJSON_AddStringToObject(root, "pi_api_url", pi_api_url);
    cJSON_AddNumberToObject(root, "pi_post_interval_ms", cfg.pi_post_interval_ms);
    cJSON_AddNumberToObject(root, "idle_rate_hz", cfg.idle_rate_hz);
    cJSON_AddNumberToObject(root, "boost_rate_hz", cfg.boost_rate_hz);
    cJSON_AddNumberToObject(root, "movement_threshold", cfg.movement_threshold);
    cJSON_AddNumberToObject(root, "settle_threshold", cfg.settle_threshold);
    cJSON_AddNumberToObject(root, "motion_sensitivity", cfg.motion_sensitivity);
    cJSON_AddNumberToObject(root, "boost_duration_ms", cfg.boost_duration_ms);
    cJSON_AddNumberToObject(root, "cooldown_ms", cfg.cooldown_ms);
    cJSON_AddNumberToObject(root, "feature_window_ms", cfg.feature_window_ms);
    return root;
}

static cJSON *calibration_to_json(void)
{
    calibration_data_t cal;
    node_status_t status;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cal = g_calibration;
    status = g_status;
    xSemaphoreGive(state_lock);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "valid", cal.valid);
    cJSON_AddNumberToObject(root, "baseline_energy", cal.baseline_energy);
    cJSON_AddNumberToObject(root, "baseline_variance", cal.baseline_variance);
    cJSON_AddNumberToObject(root, "baseline_shape", cal.baseline_shape);
    cJSON_AddNumberToObject(root, "baseline_phase", cal.baseline_phase);
    cJSON_AddNumberToObject(root, "baseline_phase_variance", cal.baseline_phase_variance);
    cJSON_AddNumberToObject(root, "baseline_noise", cal.baseline_noise);
    cJSON_AddNumberToObject(root, "baseline_phase_noise", cal.baseline_phase_noise);
    cJSON_AddNumberToObject(root, "calibration_windows", cal.calibration_windows);
    cJSON_AddBoolToObject(root, "calibrating", status.calibrating);
    cJSON_AddNumberToObject(root, "calibration_remaining_ms", status.calibration_remaining_ms);
    cJSON_AddNumberToObject(root, "baseline_age_s", status.baseline_age_s);
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
        "main{max-width:980px;margin:0 auto;padding:24px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}"
        ".tile{border:1px solid #33404b;border-radius:8px;padding:14px;background:#182029}.label{color:#9fb0bf;font-size:12px;text-transform:uppercase}"
        ".value{font-size:26px;font-weight:700;margin-top:6px}input,button{font:inherit;padding:10px;border-radius:6px;border:1px solid #465562}"
        "input{background:#0d1116;color:#ecf2f8}button{background:#2e7d5b;color:white;cursor:pointer}form{display:grid;gap:10px;max-width:420px;margin-top:24px}"
        ".controls{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px;margin-top:20px}.control{display:grid;gap:6px;border:1px solid #33404b;border-radius:8px;padding:12px;background:#182029}"
        ".control span{color:#9fb0bf;font-size:12px;text-transform:uppercase}.control b{font-size:18px;overflow-wrap:anywhere}input[type=range]{width:100%;padding:0}.actions{display:flex;gap:10px;align-items:center;margin-top:14px;flex-wrap:wrap}"
        ".calform{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:10px;margin-top:14px}.calform label{display:grid;gap:5px;color:#9fb0bf;font-size:12px;text-transform:uppercase}.calform input{width:100%;box-sizing:border-box}.calform .wide{grid-column:1/-1}"
        ".legend{display:flex;gap:14px;align-items:center;margin-top:10px;color:#9fb0bf;font-size:13px;flex-wrap:wrap}.sw{display:inline-block;width:22px;height:3px;margin-right:6px;vertical-align:middle}.score{background:#5bd19a}.rate{background:#62a8ff}.th{background:#f6c85f}.settle{background:#5d6670}"
        ".mach{display:grid;gap:10px;margin-top:10px}.mach.empty{color:#9fb0bf}.macrow{display:grid;grid-template-columns:58px 34px minmax(120px,1fr) 88px;align-items:center;gap:10px;min-height:126px;border:1px solid #33404b;border-radius:8px;padding:8px;background:#182029}.macrow strong{text-align:right}.macrow small{color:#9fb0bf;text-align:right}.maclabel{position:relative;width:34px;height:116px;color:#9fb0bf;font:12px Consolas,monospace}.maclabel span{position:absolute;left:50%;top:50%;white-space:nowrap;transform:translate(-50%,-50%) rotate(90deg)}.mactrack{height:16px;overflow:hidden;border-radius:4px;background:#0d1116}.macbar{height:100%;border-radius:inherit;background:#62a8ff}.srcdiag{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-top:12px}.srcdiag div{border:1px solid #33404b;border-radius:8px;padding:10px;background:#182029}.srcdiag span{display:block;color:#9fb0bf;font-size:12px;text-transform:uppercase}.srcdiag b{display:block;margin-top:4px;overflow-wrap:anywhere}"
        ".glossary{margin-top:28px;border-top:1px solid #33404b;padding-top:18px}.glossary h2{margin:0 0 12px}.glossary dl{display:grid;grid-template-columns:minmax(160px,240px) 1fr;gap:9px 14px}.glossary dt{color:#ecf2f8;font-weight:700}.glossary dd{margin:0;color:#b9c7d3;line-height:1.45}@media(max-width:640px){.glossary dl{grid-template-columns:1fr}.glossary dd{margin-bottom:8px}}"
        "canvas{width:100%;height:220px;margin-top:18px;border:1px solid #33404b;border-radius:8px;background:#0d1116}"
        "</style></head><body><main><h1 id=\"title\">ESP32 CSI Node</h1><div class=\"grid\" id=\"grid\"></div><canvas id=\"chart\" width=\"900\" height=\"220\"></canvas>"
        "<div class=\"legend\"><span><i class=\"sw score\"></i>Movement score</span><span><i class=\"sw rate\"></i>Sample rate</span><span><i class=\"sw th\"></i>Detection threshold</span><span><i class=\"sw settle\"></i>Settle threshold</span></div>"
        "<div class=\"controls\"><label class=\"control\"><span>Device ID</span><b id=\"didv\">0</b><input id=\"did\" type=\"number\" min=\"0\" max=\"255\" step=\"1\"></label>"
        "<label class=\"control\"><span>Raspberry Pi IP</span><b id=\"piv\">unset</b><input id=\"pi\" type=\"text\" placeholder=\"192.168.1.10\"></label>"
        "<label class=\"control\"><span>Raspberry Pi Port</span><b id=\"ppv\">3005</b><input id=\"pp\" type=\"number\" min=\"1\" max=\"65535\" step=\"1\"></label>"
        "<label class=\"control\"><span>Pi API Address</span><b id=\"pav\">http://{pi-IP-address}:3005/espdata</b><input id=\"pa\" type=\"text\" placeholder=\"/espdata\"></label>"
        "<label class=\"control\"><span>Pi Posting Interval</span><b id=\"ptv\">5s</b><input id=\"pt\" type=\"range\" min=\"1\" max=\"30\" step=\"1\"></label>"
        "<label class=\"control\"><span>Detect Threshold</span><b id=\"mv\">3.0</b><input id=\"m\" type=\"range\" min=\"1\" max=\"10\" step=\"0.1\"></label>"
        "<label class=\"control\"><span>Settle Threshold</span><b id=\"sv\">1.2</b><input id=\"se\" type=\"range\" min=\"0.2\" max=\"8\" step=\"0.1\"></label>"
        "<label class=\"control\"><span>Sensitivity</span><b id=\"snv\">1.0</b><input id=\"sn\" type=\"range\" min=\"0.3\" max=\"3\" step=\"0.1\"></label>"
        "<label class=\"control\"><span>Minimum Sample Rate</span><b id=\"iv\">10</b><input id=\"ir\" type=\"range\" min=\"10\" max=\"100\" step=\"1\"></label>"
        "<label class=\"control\"><span>Maximum Sample Rate</span><b id=\"bv\">80</b><input id=\"br\" type=\"range\" min=\"10\" max=\"250\" step=\"5\"></label>"
        "<label class=\"control\"><span>Boost Hold Time</span><b id=\"bdv\">8s</b><input id=\"bd\" type=\"range\" min=\"0\" max=\"20\" step=\"1\"></label>"
        "<label class=\"control\"><span>Cooldown Time</span><b id=\"cdv\">15s</b><input id=\"cd\" type=\"range\" min=\"0\" max=\"20\" step=\"1\"></label>"
        "<label class=\"control\"><span>Feature Window</span><b id=\"fwv\">250ms</b><input id=\"fw\" type=\"range\" min=\"0\" max=\"1000\" step=\"50\"></label>"
        "<label class=\"control\"><span>Graph Score Max</span><b id=\"gmv\">10</b><input id=\"gm\" type=\"range\" min=\"1\" max=\"20\" step=\"1\" value=\"10\"></label>"
        "<label class=\"control\"><span>Graph Rate Max</span><b id=\"grv\">160</b><input id=\"gr\" type=\"range\" min=\"10\" max=\"250\" step=\"10\" value=\"160\"></label>"
        "<label class=\"control\"><span>Graph Update Rate</span><b id=\"guv\">1.0 Hz</b><input id=\"gu\" type=\"range\" min=\"0.2\" max=\"20\" step=\"0.2\" value=\"1\"></label></div>"
        "<section><h2>CSI MAC Histogram</h2><div id=\"mach\" class=\"mach empty\">No CSI MACs observed yet.</div><div id=\"srcdiag\" class=\"srcdiag\"></div></section>"
        "<div class=\"actions\"><button id=\"cal\" type=\"button\">Auto-calibrate stillness</button><button id=\"delcal\" type=\"button\">Delete calibration data</button><span id=\"calmsg\"></span></div>"
        "<section><h2>Persisted Calibration</h2><div class=\"calform\">"
        "<label>Valid<input id=\"cv\" type=\"text\" readonly></label><label>Windows<input id=\"cw\" type=\"number\" min=\"0\" step=\"1\"></label>"
        "<label>Energy<input id=\"ce\" type=\"number\" min=\"0\" step=\"0.000001\"></label><label>Variance<input id=\"cvar\" type=\"number\" min=\"0\" step=\"0.000001\"></label>"
        "<label>Shape<input id=\"csh\" type=\"number\" min=\"0\" step=\"0.000001\"></label><label>Phase<input id=\"cph\" type=\"number\" step=\"0.000001\"></label>"
        "<label>Phase variance<input id=\"cpv\" type=\"number\" min=\"0\" step=\"0.000001\"></label><label>Noise<input id=\"cno\" type=\"number\" min=\"0.05\" step=\"0.000001\"></label>"
        "<label>Phase noise<input id=\"cpn\" type=\"number\" min=\"0.01\" step=\"0.000001\"></label><div class=\"actions wide\"><button id=\"savecal\" type=\"button\">Save Calibration Values</button><button id=\"reloadcal\" type=\"button\">Reload Calibration Values</button></div>"
        "</div></section>"
        "<form method=\"post\" action=\"/api/provision\"><h2>Wi-Fi Provisioning</h2><input name=\"ssid\" placeholder=\"2.4 GHz SSID\" required>"
        "<input name=\"password\" type=\"password\" placeholder=\"Wi-Fi password\"><button type=\"submit\">Save Wi-Fi</button></form>"
        "<section class=\"glossary\"><h2>Dashboard Glossary</h2><dl>"
        "<dt>sensing_state</dt><dd>The detector mode. idle uses the minimum sample-rate cap, boost follows confirmed movement and uses the maximum sample-rate cap, and cooldown waits before settling back to idle.</dd>"
        "<dt>sample_rate_hz</dt><dd>The measured CSI samples used in the most recent feature window. This is not the requested transmit rate; it is what the ESP32 actually accepted and processed in that window.</dd>"
        "<dt>accepted_csi_rate_hz</dt><dd>The longer one-second accepted CSI callback rate. Use this to judge whether probe traffic is really reaching the CSI callback.</dd>"
        "<dt>movement_score</dt><dd>The fused movement score after sensitivity is applied. It combines energy change, variance change, subcarrier shape change, trend deviation, and a weak phase proxy. One spike above threshold is evidence, not automatically a movement event.</dd>"
        "<dt>baseline_noise</dt><dd>The detector's current estimate of still-room CSI noise. A higher value means the room/link is noisier, so the same physical motion may score lower.</dd>"
        "<dt>calibration_persisted</dt><dd>Whether a stillness calibration baseline is saved in NVS and will be reused after reboot or power loss.</dd>"
        "<dt>trend_score</dt><dd>A robust comparison against the recent rolling median. It helps catch sudden departures from the recent normal pattern while ignoring isolated outliers.</dd>"
        "<dt>phase_score</dt><dd>A cautious phase-like component derived from CSI I/Q changes. It can help, but is deliberately weighted lightly because ESP32 phase is noisy.</dd>"
        "<dt>confirm_windows</dt><dd>How many consecutive feature windows have crossed the detection threshold. Movement normally needs repeated windows, so this explains why a brief spike may not latch.</dd>"
        "<dt>quiet_windows</dt><dd>How many consecutive windows are below the settle threshold. These are used to decide when the detector is calm enough to leave cooldown/return to idle.</dd>"
        "<dt>movement_detected</dt><dd>The final event flag. It turns true only after enough recent windows support movement, or while the node is already in boost state.</dd>"
        "<dt>rssi</dt><dd>Received signal strength in dBm for the most recent CSI packet. Less negative is stronger. Very weak or unstable RSSI can make CSI less reliable.</dd>"
        "<dt>noise_floor</dt><dd>The radio's reported background noise level for the latest packet. Together with RSSI it gives a rough signal-to-noise sense.</dd>"
        "<dt>rejected_samples</dt><dd>CSI packets discarded before analysis because they were malformed, too short, or too poor in quality.</dd>"
        "<dt>filtered_samples</dt><dd>Packets accepted but clipped/filtered because they looked like isolated spikes. This protects the baseline and reduces false positives.</dd>"
        "<dt>throttled_samples</dt><dd>CSI packets intentionally skipped because they arrived faster than the configured idle or boost sample-rate cap.</dd>"
        "<dt>queue_drops</dt><dd>CSI packets lost because the FreeRTOS queue was full. If this rises, the callback is producing data faster than the aggregation task can drain it.</dd>"
        "<dt>last_packet_ms</dt><dd>Milliseconds since the last accepted CSI packet. Large values mean the node is not currently receiving usable CSI.</dd>"
        "<dt>accepted_samples</dt><dd>Total CSI samples accepted since boot. It should climb steadily when probe traffic is present.</dd>"
        "<dt>packet_count</dt><dd>Total CSI samples included in completed detection windows since boot.</dd>"
        "<dt>Device ID</dt><dd>The node identity shown in decimal here. Changing it updates the friendly Movement name unless a name is explicitly supplied through the API.</dd>"
        "<dt>Raspberry Pi IP</dt><dd>The Pi server address the node should use for reporting/debug integration. Leave blank until the Pi address is known.</dd>"
        "<dt>Raspberry Pi Port</dt><dd>The Pi HTTP port for the receiving API. The default is 3005.</dd>"
        "<dt>Pi API Address</dt><dd>The receiving path on the Pi. The full target is shown as http://Pi-IP:port/path and defaults to /espdata.</dd>"
        "<dt>Pi Posting Interval</dt><dd>How often the node should post its compact telemetry to the Pi. The allowed range is 1 to 30 seconds.</dd>"
        "<dt>Detect Threshold</dt><dd>The movement_score level that marks a feature window as movement evidence. Lower is more sensitive but risks more false positives.</dd>"
        "<dt>Settle Threshold</dt><dd>The score level considered quiet. This should stay below the detect threshold and controls how readily the node returns to idle.</dd>"
        "<dt>Sensitivity</dt><dd>A multiplier applied to the fused score. It makes scores larger or smaller, but it does not bypass the repeated-window confirmation rule.</dd>"
        "<dt>Minimum Sample Rate</dt><dd>The CSI ingest cap used when idle or cooling down. It limits accepted local samples; it cannot force the router or Pi to transmit faster.</dd>"
        "<dt>Maximum Sample Rate</dt><dd>The CSI ingest cap used during boost after movement is confirmed.</dd>"
        "<dt>Boost Hold Time</dt><dd>How long the detector remains in boost after movement is confirmed before entering cooldown.</dd>"
        "<dt>Cooldown Time</dt><dd>How long the detector waits after boost before it is allowed to return to idle, provided enough quiet windows have arrived.</dd>"
        "<dt>Feature Window</dt><dd>The CSI analysis window: how much time is grouped before one movement score is calculated. Shorter can react faster but may contain too few packets; longer is smoother and rejects more false positives but responds more slowly. 0 means a near-immediate 1 ms window internally, not event-triggered scoring, so it can produce mostly zero scores when fewer than 3 packets arrive in a window.</dd>"
        "<dt>Graph Score Max</dt><dd>The top of the movement-score axis on the chart. It changes display scale only and is not saved.</dd>"
        "<dt>Graph Rate Max</dt><dd>The top of the sample-rate axis on the chart. It changes display scale only and is not saved.</dd>"
        "<dt>Graph Update Rate</dt><dd>How often this browser page polls and redraws. It affects the page only; high values add extra HTTP traffic.</dd>"
        "</dl></section>"
        "<script>const keys=['sensing_state','sample_rate_hz','accepted_csi_rate_hz','movement_score','baseline_noise','trend_score','phase_score','confirm_windows','quiet_windows','movement_detected','calibration_persisted','calibration_windows','rssi','noise_floor','rejected_samples','filtered_samples','throttled_samples','queue_drops','last_csi_mac','last_filtered_csi_mac','last_accepted_csi_mac','last_packet_ms','accepted_samples','packet_count'];"
        "let cfg=null,scoreMax=10,rateMax=160,timer=null,busy=false,msgUntil=0,calWas=false;const hist=[];function setText(e,v,d=1){e.textContent=Number(v).toFixed(d)}"
        "function fmt(k,v){return (['movement_score','baseline_noise','trend_score','phase_score'].includes(k))?Number(v).toFixed(3):v}"
        "function idLabel(v){return String(Number(v)||0)}"
        "function apiPath(){let p=pa.value.trim()||'/espdata';return p[0]=='/'?p:'/'+p}"
        "function apiUrl(){return 'http://'+(pi.value.trim()||'{pi-IP-address}')+':'+(pp.value||3005)+apiPath()}"
        "function refreshPi(){piv.textContent=pi.value.trim()||'unset';ppv.textContent=pp.value||3005;pav.textContent=apiUrl()}"
        "function renderSourceDiag(s){const d=s.csi_source_mac_diagnostics||{},items=[['Configured source',d.mac||'unset'],['Filter',d.filter_enabled?'enabled':'disabled'],['Seen before filter',d.seen_before_filter??0],['Accepted after gates',d.accepted_after_gates??0],['Last seen',d.last_seen_ms==null?'never':d.last_seen_ms+' ms ago'],['Last accepted',d.last_accepted_ms==null?'never':d.last_accepted_ms+' ms ago']];srcdiag.innerHTML=items.map(x=>'<div><span>'+x[0]+'</span><b>'+x[1]+'</b></div>').join('')}"
        "function renderMach(s){const rows=Array.isArray(s.csi_mac_histogram)?s.csi_mac_histogram.filter(x=>x&&x.mac).sort((a,b)=>(b.count||0)-(a.count||0)):[];renderSourceDiag(s);if(!rows.length){mach.className='mach empty';mach.textContent='No CSI MACs observed yet.';return}const max=Math.max(...rows.map(x=>+x.count||0),1);mach.className='mach';mach.innerHTML=rows.map(x=>'<div class=macrow><strong>'+Number(x.count||0)+'</strong><div class=maclabel><span>'+String(x.mac)+'</span></div><div class=mactrack><div class=macbar style=\"width:'+Math.max(4,((+x.count||0)/max)*100)+'%\"></div></div><small>'+String(x.last_seen_ms==null?'n/a':x.last_seen_ms)+' ms ago</small></div>').join('')}"
        "function setCal(x){cv.value=x.valid?'yes':'no';cw.value=x.calibration_windows||0;ce.value=Number(x.baseline_energy||0).toFixed(6);cvar.value=Number(x.baseline_variance||0).toFixed(6);csh.value=Number(x.baseline_shape||0).toFixed(6);cph.value=Number(x.baseline_phase||0).toFixed(6);cpv.value=Number(x.baseline_phase_variance||0).toFixed(6);cno.value=Number(x.baseline_noise||0.05).toFixed(6);cpn.value=Number(x.baseline_phase_noise||0.01).toFixed(6)}"
        "async function loadCal(){setCal(await(await fetch('/api/calibration')).json())}"
        "async function saveCal(){const body={calibration_windows:+cw.value,baseline_energy:+ce.value,baseline_variance:+cvar.value,baseline_shape:+csh.value,baseline_phase:+cph.value,baseline_phase_variance:+cpv.value,baseline_noise:+cno.value,baseline_phase_noise:+cpn.value};setCal(await(await fetch('/api/calibration',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})).json());calmsg.textContent='Calibration values saved';msgUntil=Date.now()+3000}"
        "async function saveCfg(){const body={device_id:+did.value,pi_ip:pi.value.trim(),pi_port:+pp.value,pi_api_path:apiPath(),pi_post_interval_ms:+pt.value*1000,movement_threshold:+m.value,settle_threshold:+se.value,motion_sensitivity:+sn.value,idle_rate_hz:+ir.value,boost_rate_hz:+br.value,boost_duration_ms:+bd.value*1000,cooldown_ms:+cd.value*1000,feature_window_ms:+fw.value};cfg=await(await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})).json();did.value=cfg.device_id;didv.textContent=idLabel(cfg.device_id);pi.value=cfg.pi_ip||'';pp.value=cfg.pi_port||3005;pa.value=cfg.pi_api_path||'/espdata';pt.value=Math.round((cfg.pi_post_interval_ms||5000)/1000);ptv.textContent=pt.value+'s';refreshPi()}"
        "function dims(){return{l:44,r:48,t:12,b:24,w:chart.width-92,h:chart.height-36}}"
        "function y(v,max,d){return d.t+d.h-Math.min(1,Math.max(0,v/max))*d.h}"
        "function axes(c,d){c.font='12px system-ui';c.fillStyle='#9fb0bf';c.strokeStyle='#26313b';c.lineWidth=1;c.beginPath();c.moveTo(d.l,d.t);c.lineTo(d.l,d.t+d.h);c.lineTo(d.l+d.w,d.t+d.h);c.lineTo(d.l+d.w,d.t);c.stroke();c.fillText(scoreMax+' score',4,d.t+10);c.fillText('0',28,d.t+d.h);c.fillText(rateMax+' Hz',d.l+d.w+8,d.t+10);c.fillText('0',d.l+d.w+8,d.t+d.h)}"
        "function line(c,a,color,max,d){c.strokeStyle=color;c.beginPath();a.forEach((v,i)=>{let x=d.l+i*(d.w/119),yy=y(v,max,d);i?c.lineTo(x,yy):c.moveTo(x,yy)});c.stroke()}"
        "function hline(c,v,max,color,d,dash){let yy=y(v,max,d);c.strokeStyle=color;c.setLineDash(dash?[6,4]:[]);c.beginPath();c.moveTo(d.l,yy);c.lineTo(d.l+d.w,yy);c.stroke();c.setLineDash([])}"
        "function draw(){const c=chart.getContext('2d'),d=dims();c.clearRect(0,0,chart.width,chart.height);axes(c,d);c.lineWidth=2;line(c,hist.map(x=>x.m),'#5bd19a',scoreMax,d);line(c,hist.map(x=>x.r),'#62a8ff',rateMax,d);hline(c,+m.value,scoreMax,'#f6c85f',d,true);hline(c,+se.value,scoreMax,'#5d6670',d,false)}"
        "async function init(){cfg=await(await fetch('/api/config')).json();did.value=cfg.device_id;pi.value=cfg.pi_ip||'';pp.value=cfg.pi_port||3005;pa.value=cfg.pi_api_path||'/espdata';pt.value=Math.round((cfg.pi_post_interval_ms||5000)/1000);m.value=cfg.movement_threshold;se.value=cfg.settle_threshold;sn.value=cfg.motion_sensitivity;ir.value=cfg.idle_rate_hz;br.value=cfg.boost_rate_hz;bd.value=Math.round(cfg.boost_duration_ms/1000);cd.value=Math.round(cfg.cooldown_ms/1000);fw.value=cfg.feature_window_ms;didv.textContent=idLabel(did.value);refreshPi();ptv.textContent=pt.value+'s';setText(mv,m.value);setText(sv,se.value);setText(snv,sn.value);iv.textContent=ir.value;bv.textContent=br.value;bdv.textContent=bd.value+'s';cdv.textContent=cd.value+'s';fwv.textContent=fw.value+'ms';await loadCal()}"
        "function schedule(){clearTimeout(timer);timer=setTimeout(tick,Math.round(1000/Math.max(0.2,+gu.value)))}"
        "did.oninput=()=>didv.textContent=idLabel(did.value);pi.oninput=refreshPi;pp.oninput=refreshPi;pa.oninput=refreshPi;pt.oninput=()=>ptv.textContent=pt.value+'s';m.oninput=()=>{setText(mv,m.value);draw()};se.oninput=()=>{setText(sv,se.value);draw()};sn.oninput=()=>setText(snv,sn.value);ir.oninput=()=>iv.textContent=ir.value;br.oninput=()=>bv.textContent=br.value;bd.oninput=()=>bdv.textContent=bd.value+'s';cd.oninput=()=>cdv.textContent=cd.value+'s';fw.oninput=()=>fwv.textContent=fw.value+'ms';did.onchange=saveCfg;pi.onchange=saveCfg;pp.onchange=saveCfg;pa.onchange=saveCfg;pt.onchange=saveCfg;m.onchange=saveCfg;se.onchange=saveCfg;sn.onchange=saveCfg;ir.onchange=saveCfg;br.onchange=saveCfg;bd.onchange=saveCfg;cd.onchange=saveCfg;fw.onchange=saveCfg;gm.oninput=()=>{scoreMax=+gm.value;gmv.textContent=scoreMax;draw()};gr.oninput=()=>{rateMax=+gr.value;grv.textContent=rateMax;draw()};gu.oninput=()=>{guv.textContent=Number(gu.value).toFixed(1)+' Hz';schedule()};"
        "savecal.onclick=saveCal;reloadcal.onclick=loadCal;"
        "cal.onclick=async()=>{cal.disabled=true;calmsg.textContent='Keep the area still for 10 seconds';await fetch('/api/calibrate',{method:'POST'})};"
        "delcal.onclick=async()=>{delcal.disabled=true;calmsg.textContent='Calibration data deleted';msgUntil=Date.now()+3000;await fetch('/api/calibration',{method:'DELETE'});await loadCal();setTimeout(()=>delcal.disabled=false,1000)};"
        "async function tick(){if(busy){schedule();return}busy=true;try{const r=await fetch('/status.json');const s=await r.json();title.textContent=s.name+' '+s.ip;"
        "grid.innerHTML=keys.map(k=>'<div class=tile><div class=label>'+k+'</div><div class=value>'+fmt(k,s[k])+'</div></div>').join('');"
        "renderMach(s);"
        "cal.disabled=!!s.calibrating;calmsg.textContent=s.calibrating?'Calibrating '+Math.ceil(s.calibration_remaining_ms/1000)+'s':(Date.now()<msgUntil?calmsg.textContent:'');"
        "if(calWas&&!s.calibrating)loadCal();calWas=!!s.calibrating;hist.push({m:s.movement_score,r:s.sample_rate_hz});if(hist.length>120)hist.shift();draw()}finally{busy=false;schedule()}}"
        "init().then(()=>{guv.textContent=Number(gu.value).toFixed(1)+' Hz';tick()})</script></main></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_get_handler(httpd_req_t *req)
{
    if (setup_mode_active) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" CAPTIVE_IP "/");
        httpd_resp_sendstr(req, "Open http://" CAPTIVE_IP "/ to set up this Movement node.");
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
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

static esp_err_t calibration_get_handler(httpd_req_t *req)
{
    cJSON *json = calibration_to_json();
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

static esp_err_t calibration_post_handler(httpd_req_t *req)
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

    calibration_data_t cal;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cal = g_calibration;
    xSemaphoreGive(state_lock);

    double value = cal.baseline_energy;
    apply_json_number(root, "baseline_energy", &value);
    cal.baseline_energy = (float)value;
    value = cal.baseline_variance;
    apply_json_number(root, "baseline_variance", &value);
    cal.baseline_variance = (float)value;
    value = cal.baseline_shape;
    apply_json_number(root, "baseline_shape", &value);
    cal.baseline_shape = (float)value;
    value = cal.baseline_phase;
    apply_json_number(root, "baseline_phase", &value);
    cal.baseline_phase = (float)value;
    value = cal.baseline_phase_variance;
    apply_json_number(root, "baseline_phase_variance", &value);
    cal.baseline_phase_variance = (float)value;
    value = cal.baseline_noise;
    apply_json_number(root, "baseline_noise", &value);
    cal.baseline_noise = (float)value;
    value = cal.baseline_phase_noise;
    apply_json_number(root, "baseline_phase_noise", &value);
    cal.baseline_phase_noise = (float)value;
    value = cal.calibration_windows;
    apply_json_number(root, "calibration_windows", &value);
    cal.calibration_windows = value > 0.0 ? (uint32_t)value : 0;
    cal.valid = true;
    sanitize_calibration(&cal);

    esp_err_t save_err = save_calibration(&cal);
    if (save_err != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to save calibration to NVS: %s", esp_err_to_name(save_err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save calibration");
        return ESP_FAIL;
    }

    xSemaphoreTake(state_lock, portMAX_DELAY);
    g_calibration = cal;
    calibration_apply_requested = true;
    g_status.baseline_started_us = esp_timer_get_time();
    xSemaphoreGive(state_lock);
    cJSON_Delete(root);

    cJSON *reply = calibration_to_json();
    esp_err_t err = send_json(req, reply);
    cJSON_Delete(reply);
    return err;
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
    bool name_supplied = false;
    if (cJSON_IsString(name) && name->valuestring != NULL) {
        strlcpy(cfg.name, name->valuestring, sizeof(cfg.name));
        name_supplied = true;
    }

    cJSON *pi_ip = cJSON_GetObjectItem(root, "pi_ip");
    if (cJSON_IsString(pi_ip) && pi_ip->valuestring != NULL) {
        strlcpy(cfg.pi_ip, pi_ip->valuestring, sizeof(cfg.pi_ip));
    }

    cJSON *pi_api_path = cJSON_GetObjectItem(root, "pi_api_path");
    if (cJSON_IsString(pi_api_path) && pi_api_path->valuestring != NULL) {
        strlcpy(cfg.pi_api_path, pi_api_path->valuestring, sizeof(cfg.pi_api_path));
    }

    cJSON *csi_source_mac = cJSON_GetObjectItem(root, "csi_source_mac");
    if (cJSON_IsString(csi_source_mac) && csi_source_mac->valuestring != NULL) {
        strlcpy(cfg.csi_source_mac, csi_source_mac->valuestring, sizeof(cfg.csi_source_mac));
    }

    cJSON *csi_source_filter_enabled = cJSON_GetObjectItem(root, "csi_source_filter_enabled");
    if (cJSON_IsBool(csi_source_filter_enabled)) {
        cfg.csi_source_filter_enabled = cJSON_IsTrue(csi_source_filter_enabled);
    }

    double value;
    int32_t old_device_id = cfg.device_id;
    value = cfg.device_id;
    apply_json_number(root, "device_id", &value);
    cfg.device_id = (int32_t)value;
    if (!name_supplied && cfg.device_id != old_device_id) {
        format_node_name(cfg.device_id, cfg.name, sizeof(cfg.name));
    }
    value = cfg.pi_port;
    apply_json_number(root, "pi_port", &value);
    cfg.pi_port = value >= 1.0 && value <= 65535.0 ? (uint16_t)value : DEFAULT_PI_PORT;
    value = cfg.pi_post_interval_ms;
    apply_json_number(root, "pi_post_interval_ms", &value);
    cfg.pi_post_interval_ms = (uint32_t)value;
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
    value = cfg.motion_sensitivity;
    apply_json_number(root, "motion_sensitivity", &value);
    cfg.motion_sensitivity = (float)value;
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

    esp_err_t save_err = save_config(&cfg);
    if (save_err != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to save config to NVS: %s", esp_err_to_name(save_err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config");
        return ESP_FAIL;
    }

    xSemaphoreTake(state_lock, portMAX_DELAY);
    g_config = cfg;
    if (sta_netif != NULL) {
        esp_netif_set_hostname(sta_netif, g_config.name);
    }
    if (g_status.state == SENSE_BOOST) {
        g_status.state_until_us = esp_timer_get_time() + ((int64_t)g_config.boost_duration_ms * 1000);
    }
    apply_configured_sample_rate_locked();
    apply_csi_source_filter_locked();
    xSemaphoreGive(state_lock);
    cJSON_Delete(root);

    cJSON *reply = config_to_json();
    esp_err_t err = send_json(req, reply);
    cJSON_Delete(reply);
    return err;
}

static esp_err_t options_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t log_get_handler(httpd_req_t *req)
{
    char (*snapshot)[LOG_LINE_LEN] = calloc(LOG_LINE_COUNT, LOG_LINE_LEN);
    if (snapshot == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Log snapshot allocation failed");
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&log_lock);
    size_t count = log_line_count;
    size_t first = (log_next_line + LOG_LINE_COUNT - count) % LOG_LINE_COUNT;
    for (size_t i = 0; i < count; i++) {
        size_t idx = (first + i) % LOG_LINE_COUNT;
        strlcpy(snapshot[i], log_lines[idx], LOG_LINE_LEN);
    }
    portEXIT_CRITICAL(&log_lock);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    for (size_t i = 0; i < count; i++) {
        httpd_resp_send_chunk(req, snapshot[i], HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, "\n", 1);
    }
    free(snapshot);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t calibrate_post_handler(httpd_req_t *req)
{
    calibration_requested = true;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    g_status.calibrating = true;
    g_status.calibration_remaining_ms = CALIBRATION_DURATION_MS;
    xSemaphoreGive(state_lock);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"calibrating\":true,\"duration_ms\":10000}");
}

static esp_err_t identify_post_handler(httpd_req_t *req)
{
    start_identify_blink();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"identifying\":true,\"duration_ms\":10000}");
}

static esp_err_t calibration_delete_handler(httpd_req_t *req)
{
    calibration_delete_requested = true;
    esp_err_t clear_err = clear_calibration();
    xSemaphoreTake(state_lock, portMAX_DELAY);
    default_calibration(&g_calibration);
    g_status.calibrating = false;
    g_status.calibration_remaining_ms = 0;
    g_status.baseline_age_s = 0;
    xSemaphoreGive(state_lock);
    if (clear_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear CSI calibration: %s", esp_err_to_name(clear_err));
    }
    ESP_LOGI(TAG, "CSI calibration deletion requested");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"calibration_deleted\":true}");
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
    config.max_uri_handlers = 20;

    ESP_ERROR_CHECK(httpd_start(&http_server, &config));

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t status = {.uri = "/status.json", .method = HTTP_GET, .handler = status_get_handler};
    httpd_uri_t api_status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler};
    httpd_uri_t config_get = {.uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler};
    httpd_uri_t config_post = {.uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler};
    httpd_uri_t config_options = {.uri = "/api/config", .method = HTTP_OPTIONS, .handler = options_handler};
    httpd_uri_t calibrate = {.uri = "/api/calibrate", .method = HTTP_POST, .handler = calibrate_post_handler};
    httpd_uri_t identify = {.uri = "/api/identify", .method = HTTP_POST, .handler = identify_post_handler};
    httpd_uri_t identify_options = {.uri = "/api/identify", .method = HTTP_OPTIONS, .handler = options_handler};
    httpd_uri_t calibration_get = {.uri = "/api/calibration", .method = HTTP_GET, .handler = calibration_get_handler};
    httpd_uri_t calibration_post = {.uri = "/api/calibration", .method = HTTP_POST, .handler = calibration_post_handler};
    httpd_uri_t calibration_options = {.uri = "/api/calibration", .method = HTTP_OPTIONS, .handler = options_handler};
    httpd_uri_t calibration_delete = {.uri = "/api/calibration", .method = HTTP_DELETE, .handler = calibration_delete_handler};
    httpd_uri_t provision = {.uri = "/api/provision", .method = HTTP_POST, .handler = provision_post_handler};
    httpd_uri_t reset_wifi = {.uri = "/api/reset-wifi", .method = HTTP_POST, .handler = reset_wifi_post_handler};
    httpd_uri_t log_slash = {.uri = "/log/", .method = HTTP_GET, .handler = log_get_handler};
    httpd_uri_t log = {.uri = "/log", .method = HTTP_GET, .handler = log_get_handler};
    httpd_uri_t captive = {.uri = "/*", .method = HTTP_GET, .handler = captive_get_handler};

    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &api_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &config_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &config_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &config_options));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &calibrate));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &identify));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &identify_options));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &calibration_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &calibration_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &calibration_options));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &calibration_delete));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &provision));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &reset_wifi));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &log_slash));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &log));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &captive));
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
 * - Creates the CSI queue, mutex, event group, Wi-Fi stack, web server, and
 *   background aggregation task.
 */
void app_main(void)
{
    install_log_capture();

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

    config_needs_auto_name = load_config(&g_config) != ESP_OK;
    if (load_calibration(&g_calibration) == ESP_OK && g_calibration.valid) {
        ESP_LOGI(TAG, "Persisted CSI calibration loaded from NVS");
    }
    csi_min_interval_us = g_config.idle_rate_hz > 0 ? 1000000LL / g_config.idle_rate_hz : 0;
    apply_csi_source_filter_locked();
    movement_led_init();
    memset(&g_status, 0, sizeof(g_status));
    g_status.state = SENSE_IDLE;
    g_status.boot_us = esp_timer_get_time();
    g_status.baseline_started_us = g_status.boot_us;
    strlcpy(g_status.ip, "0.0.0.0", sizeof(g_status.ip));

    init_wifi();
    start_http_server();
    xTaskCreate(csi_aggregation_task, "csi_aggregation", 4096, NULL, 5, NULL);
    xTaskCreate(pi_telemetry_task, "pi_telemetry", PI_TELEMETRY_STACK, NULL, 4, NULL);

    ESP_LOGI(TAG, "ESP32 CSI node ready");
}
