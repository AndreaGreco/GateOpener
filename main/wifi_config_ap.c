#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "config.h"
#include "cJSON.h"

#define WIFI_SSID           "esp-recovery"
#define WIFI_PASS           "recovery-esp"
#define WIFI_ERASE_SSID     "esp-erase-cfg"

static const char *TAG = "wifi softAP";
static uint8_t ssid[32];
static wifi_ap_record_t *ap_list;
static int ap_no = -1;

bool get_scan_ap_no(int *p_ap_no, wifi_ap_record_t **p_ap_list)
{
    *p_ap_no = ap_no;
    *p_ap_list = ap_list;

    return true;
}

static void erase_all_config() {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAME, NVS_READONLY, &nvs_handle);
    if(err != ESP_OK)
        goto err;

    nvs_erase_all(nvs_handle);
    nvs_close(nvs_handle);

err:
    ESP_LOGE(TAG, "Can't read credentials");
    memset(ssid, 0, sizeof(ssid));
}

static void wifi_event_scandone_cb(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    uint16_t apCount = 0, i;

    if(ap_list != NULL) {
        ap_no = 0;
        free(ap_list);
        ap_list = NULL;
    }

    esp_wifi_scan_get_ap_num(&apCount);
    if (apCount == 0) {
        return;
    }

    ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * apCount);
    if (!ap_list) {
        ESP_LOGE(TAG, "malloc error, ap_list is NULL");
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&apCount, ap_list));
    for(i = 0; i < apCount; i++) {
        wifi_ap_record_t *r = &ap_list[i];

        /* AP was search for found */
        if( strcmp((char*)&r->ssid, (char*)ssid) == 0) {
            ESP_LOGI(TAG, "Found: `%s` Restart and connect with ap", r->ssid);
            power_up_data.data.mode = STARTUP_MODE__STA;
            power_up_set_mode(STARTUP_MODE__STA);

            esp_restart();
        } else if (strcmp((char*)&r->ssid, WIFI_ERASE_SSID) == 0) {
            ESP_LOGW(TAG, "Found erase network");
            erase_all_config();
        } else {
            ESP_LOGW(TAG, "Can't found AP:%s", ssid);
        }
    }

    ap_no = apCount;
    ESP_ERROR_CHECK( esp_wifi_clear_ap_list() );
}

static void search_network_task(void* arg) {
    static wifi_scan_config_t config;
    config.scan_time.active.max = 200;
    config.scan_time.active.max = 250;
    config.scan_time.passive = 150;

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &wifi_event_scandone_cb, &config));

    while (true) {
        int i;
        ESP_LOGI(TAG, "Search network scan!");
        if(!power_up_get_wrong_pass()) {
            for(i = 0; i < 14; i++) {
                config.channel = i;
                ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_start(&config, true));
                vTaskDelay( pdMS_TO_TICKS(250) );
            }
        }

        vTaskDelay( pdMS_TO_TICKS(15000) );
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

void wifi_init_softap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .password = WIFI_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                    .required = false,
            },
        },
    };
    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    nvs_read_wifi_credential(ssid, NULL);
    start_config_server();

    ESP_LOGI(TAG, "AP start");
    initialise_mdns();

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s", WIFI_SSID, WIFI_PASS);

    xTaskCreate(search_network_task, "scan-networks", 2048, NULL, 10, NULL);

    vTaskDelay(pdMS_TO_TICKS(60000));

    power_up_set_mode(STARTUP_MODE__STA);
    esp_restart();
}
