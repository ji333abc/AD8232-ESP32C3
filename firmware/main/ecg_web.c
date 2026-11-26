#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "board_config.h"
#include "ecg_display.h"
#include "ecg_web.h"
#include "ecg_wifi.local.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"

_Static_assert(sizeof(ECG_WIFI_SSID) > 1 && sizeof(ECG_WIFI_SSID) <= 33,
               "The Wi-Fi SSID must be 1 to 32 bytes");
_Static_assert(sizeof(ECG_WIFI_PASSWORD) >= 9 && sizeof(ECG_WIFI_PASSWORD) <= 64,
               "The WPA2 passphrase must be 8 to 63 bytes");

extern const char ecg_html[] asm("_binary_ecg_html_start");

// Little-endian header followed by signed ADC codes, from oldest to newest.
typedef struct {
    uint32_t sequence;
    uint16_t sample_hz;
    uint8_t signal;
    uint8_t seconds;
    int32_t data[ECG_SAMPLE_HZ * ECG_DISPLAY_SECONDS + ECG_DISPLAY_PEAK_CAPACITY];
} wave_frame_t;

_Static_assert(offsetof(wave_frame_t, data) == 8, "Unexpected waveform header layout");
_Static_assert(ECG_SAMPLE_HZ * ECG_DISPLAY_SECONDS <= UINT16_MAX, "Waveform window is too long");

static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static ecg_web_stats_t acquisition_stats;

void ecg_web_update_stats(const ecg_web_stats_t *stats)
{
    portENTER_CRITICAL(&stats_lock);
    acquisition_stats = *stats;
    portEXIT_CRITICAL(&stats_lock);
}

static esp_err_t page_get(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, ecg_html);
}

static bool read_cursor(httpd_req_t *request, uint32_t *cursor)
{
    char query[32];
    char value[11];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "after", value, sizeof(value)) != ESP_OK || !value[0]) {
        return false;
    }
    uint64_t sequence = 0;
    for (const char *p = value; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        sequence = sequence * 10 + (*p - '0');
    }
    if (sequence > UINT32_MAX) {
        return false;
    }
    *cursor = sequence;
    return true;
}

static esp_err_t wave_get(httpd_req_t *request)
{
    // The HTTP server runs handlers serially. The snapshot releases the display lock before I/O.
    static wave_frame_t frame = {.sample_hz = ECG_SAMPLE_HZ, .seconds = ECG_DISPLAY_SECONDS};
    uint32_t cursor;
    const bool have_cursor = read_cursor(request, &cursor);
    const ecg_display_snapshot_t snapshot =
        ecg_display_snapshot(frame.data, ECG_SAMPLE_HZ * ECG_DISPLAY_SECONDS, have_cursor ? &cursor : NULL);
    frame.sequence = snapshot.sequence;
    frame.signal = snapshot.signal;
    // Keep the waveform body compatible with clients that only plot ADC codes.
    char bpm[6];
    snprintf(bpm, sizeof(bpm), "%u", (unsigned)snapshot.bpm);
    httpd_resp_set_hdr(request, "X-ECG-BPM", bpm);
    char ages[ECG_DISPLAY_PEAK_CAPACITY * 6];
    if (snapshot.peaks) {
        size_t used = 0;
        for (unsigned i = 0; i < snapshot.peaks; ++i) {
            used += snprintf(ages + used, sizeof(ages) - used, "%s%u", i ? "," : "",
                             (unsigned)(uint16_t)frame.data[snapshot.samples + i]);
        }
        httpd_resp_set_hdr(request, "X-ECG-QRS-Ages", ages);
    }
    if (snapshot.incremental) {
        httpd_resp_set_hdr(request, "X-ECG-Delta", "1");
    }
    portENTER_CRITICAL(&stats_lock);
    const ecg_web_stats_t stats = acquisition_stats;
    portEXIT_CRITICAL(&stats_lock);
    char counters[44];  // Four decimal uint32_t fields, separators and terminator.
    snprintf(counters, sizeof(counters), "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32,
             stats.rate_hundredths, stats.missed_ticks, stats.adc_errors, stats.display_drops);
    httpd_resp_set_hdr(request, "X-ECG-Stats", counters);
    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)&frame,
                           offsetof(wave_frame_t, data) + snapshot.samples * sizeof(frame.data[0]));
}

static esp_err_t start_http_server(httpd_handle_t *server)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.task_priority = tskIDLE_PRIORITY + 2;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    static const httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_get};
    static const httpd_uri_t wave = {.uri = "/wave", .method = HTTP_GET, .handler = wave_get};

    ESP_RETURN_ON_ERROR(httpd_start(server, &config), "ecg_web", "HTTP server failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(*server, &page), "ecg_web", "Page handler failed");
    return httpd_register_uri_handler(*server, &wave);
}

static esp_err_t configure_access_point(void)
{
    wifi_config_t config = {
        .ap = {
            .ssid = ECG_WIFI_SSID,
            .ssid_len = sizeof(ECG_WIFI_SSID) - 1,
            .password = ECG_WIFI_PASSWORD,
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), "ecg_web", "Wi-Fi storage setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), "ecg_web", "Access point mode failed");
    return esp_wifi_set_config(WIFI_IF_AP, &config);
}

esp_err_t ecg_web_start(void)
{
    ESP_RETURN_ON_ERROR(nvs_flash_init(), "ecg_web", "NVS initialization failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), "ecg_web", "Network initialization failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), "ecg_web", "Event loop initialization failed");

    const esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_AP();
    esp_netif_t *netif = esp_netif_new(&netif_config);
    httpd_handle_t server = NULL;
    bool wifi_initialized = false;
    esp_err_t err = netif ? esp_netif_attach_wifi_ap(netif) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = esp_wifi_set_default_wifi_ap_handlers();
    }
    if (err == ESP_OK) {
        const wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&config);
        wifi_initialized = err == ESP_OK;
    }
    if (err == ESP_OK) {
        err = configure_access_point();
    }
    if (err == ESP_OK) {
        err = start_http_server(&server);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        if (server) {
            httpd_stop(server);
        }
        if (wifi_initialized) {
            esp_wifi_deinit();
        }
        if (netif) {
            esp_netif_destroy_default_wifi(netif);
        }
        esp_event_loop_delete_default();
    }
    return err;
}
