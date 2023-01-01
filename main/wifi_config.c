#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "mdns.h"

#include "esp_sntp.h"
#include "wifi_config.h"
#include "config.h"

static const char *TAG = "WiFi";

#define WIFI_SSID__KEY "wifi-ssid"
#define WIFI_PASS__KEY "wifi-pass"

EventGroupHandle_t s_wifi_event_group;
// struct AppConfig_t config;

#define ESP_MAXIMUM_RETRY 3
static int s_retry_num = 0;

void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Notification of a time synchronization event");
    xEventGroupSetBits(s_wifi_event_group, CLOCK_SYNC_DONE);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        wifi_event_sta_disconnected_t *d =event_data;
        ESP_LOGI(TAG, "Disconnect AP, reason:%d", d->reason);
        if (s_retry_num < ESP_MAXIMUM_RETRY) {
            /* Wrong Password */
            if(d->reason == WIFI_REASON_ASSOC_FAIL) {
                power_up_set_wrong_pass(true);
                power_up_set_mode(STARTUP_MODE__AP);
                s_retry_num = ESP_MAXIMUM_RETRY;
                esp_restart();
            } else {
                ESP_ERROR_CHECK_WITHOUT_ABORT( esp_wifi_connect() );
            }
            s_retry_num++;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            xEventGroupClearBits(s_wifi_event_group, CLOCK_SYNC_DONE);
        }
    default:
        break;
    }
}

static void event_got_ip(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;

    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));

    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    s_retry_num = 0;

    sntp_restart();
}

static wifi_config_t wifi_config = {
    .sta = {
        .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        .pmf_cfg = {
            .capable = true,
            .required = false
        },
    },
};

static void nvs_read_wifi_credential()
{
    nvs_handle_t nvs_handle;
    size_t sz;

    ESP_ERROR_CHECK( nvs_open(NVS_NAME, NVS_READONLY, &nvs_handle) );

    sz = sizeof(wifi_config.sta.ssid);
    ESP_ERROR_CHECK( nvs_get_str(nvs_handle, NVS_WIFI_SSID__KEY, (char*) wifi_config.sta.ssid, &sz) );

    sz = sizeof(wifi_config.sta.password);
    ESP_ERROR_CHECK( nvs_get_str(nvs_handle, NVS_WIFI_PASS__KEY, (char*) wifi_config.sta.password, &sz) );
    nvs_close(nvs_handle);
}

void wifi_init_sta(void)
{
    esp_err_t err;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    s_wifi_event_group = xEventGroupCreate();

    nvs_read_wifi_credential();

    esp_netif_create_default_wifi_sta();
    err = esp_wifi_init(&cfg);
    ESP_ERROR_CHECK(err);

    /* Wifi Event */
    err = esp_event_handler_instance_register(  WIFI_EVENT,
                                                ESP_EVENT_ANY_ID,
                                                &wifi_event_handler,
                                                NULL,
                                                NULL);
    ESP_ERROR_CHECK(err);

    /* IP Event */
    err = esp_event_handler_instance_register(  IP_EVENT,
                                                IP_EVENT_STA_GOT_IP,
                                                &event_got_ip,
                                                NULL,
                                                NULL);
    ESP_ERROR_CHECK(err);

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_ERROR_CHECK(err);

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    ESP_ERROR_CHECK(err);

    err = esp_wifi_start();
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    initialise_mdns();

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 wifi_config.sta.ssid, wifi_config.sta.password);
        power_up_set_wrong_pass(false);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 wifi_config.sta.ssid, wifi_config.sta.password);
        power_up_set_mode(STARTUP_MODE__AP);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        power_up_set_mode(STARTUP_MODE__AP);
        esp_restart();
    }

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);

    start_config_server();
    sntp_init();
    xTaskCreate(http_test_task, "Telegram", 8192, NULL, 10, NULL);
}
