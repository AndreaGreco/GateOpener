#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_crc.h"
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "mdns.h"

#define CRC_SEED 0x87485837
static const char *TAG = "config";
#define DEFAULT_NAME    "door-lock"
#define NAME_MAX_SZ     64

__NOINIT_ATTR struct PowerData_crc_st power_up_data;

void power_up_init() {
    uint32_t calculated_crc;

    calculated_crc = esp_crc32_be(CRC_SEED, (uint8_t*)&power_up_data.data, sizeof(power_up_data.data));

    ESP_LOGI(TAG, "CRC 0x%08lx | 0x%08lx - PowerUp Cnt:%d",
                    calculated_crc,
                    power_up_data.crc,
                    power_up_data.data.power_up_cnt);

    if( (calculated_crc != power_up_data.crc) || (power_up_data.data.magic_no != CONFGI_STARTUP_MAGIC) ) {
        memset(&power_up_data.data, 0, sizeof(power_up_data));
        power_up_data.data.magic_no = CONFGI_STARTUP_MAGIC;
        power_up_data.data.mode = STARTUP_MODE__STA;
    }

    power_up_data.data.power_up_cnt++;
    power_up_data.crc = esp_crc32_be(CRC_SEED, (uint8_t*)&power_up_data.data, sizeof(power_up_data.data));
}

void power_up_set_mode(enum STARTUP_MODE mode)
{
    power_up_data.data.mode = (uint8_t) mode;
    power_up_data.crc = esp_crc32_be(CRC_SEED, (uint8_t*)&power_up_data.data, sizeof(power_up_data.data));
}

void power_up_set_wrong_pass(bool wrong_pass)
{
    power_up_data.data.wrong_pass = wrong_pass;
    power_up_data.crc = esp_crc32_be(CRC_SEED, (uint8_t*)&power_up_data.data, sizeof(power_up_data.data));
}

bool power_up_get_wrong_pass() {
    return !!power_up_data.data.wrong_pass;
}

enum STARTUP_MODE power_up_get_mode() {
    return (enum STARTUP_MODE)power_up_data.data.mode;
}

esp_err_t nvs_read_wifi_credential(uint8_t *ssid, uint8_t *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    size_t sz;

    err = nvs_open(NVS_NAME, NVS_READONLY, &nvs_handle);
    if(err != ESP_OK)
        return err;

    if(ssid) {
        /* ESP SSID len*/
        sz = 32;
        err = nvs_get_str(nvs_handle, NVS_WIFI_SSID__KEY, (char*) ssid, &sz);
        if(err != ESP_OK)
            goto close_and_ret;
    }

    if(password) {
        /* ESP MAX Password len*/
        sz = 64;
        err = nvs_get_str(nvs_handle, NVS_WIFI_PASS__KEY, (char*) password, &sz);
        if(err != ESP_OK)
            goto close_and_ret;
    }

close_and_ret:
    nvs_close(nvs_handle);
    return err;
}

void set_dns_hostname(char* new_hostname)
{
    nvs_handle_t nvs_handle;

    ESP_ERROR_CHECK( nvs_open(NVS_NAME, NVS_READWRITE, &nvs_handle) );
    ESP_ERROR_CHECK( nvs_set_str(nvs_handle, NVS_MDNS_NAME__KEY, new_hostname) );
    ESP_ERROR_CHECK( nvs_commit(nvs_handle) );

    ESP_LOGI(TAG, "New credential saved");
    nvs_close(nvs_handle);
}

void initialise_mdns(void)
{
    nvs_handle_t nvs_handle;
    char hostname[64];
    esp_err_t err;
    size_t sz;

    ESP_LOGI(TAG, "Start MDNS");

    err = nvs_open(NVS_NAME, NVS_READONLY, &nvs_handle);
    ESP_ERROR_CHECK_WITHOUT_ABORT(err);

    if(err == ESP_OK) {
        sz = sizeof(hostname);
        err = nvs_get_str(nvs_handle, NVS_MDNS_NAME__KEY, (char*) hostname, &sz);
        if(err != ESP_OK) {
            snprintf(hostname, NAME_MAX_SZ, DEFAULT_NAME);
            ESP_LOGE(TAG, "Can't read from NVS Name, use default:%s", hostname);
        }

        nvs_close(nvs_handle);
    } else {
        strcpy(hostname, "apri-cancello-default");
    }

    err = mdns_init();
    ESP_ERROR_CHECK(err);

    err = mdns_hostname_set(hostname);
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "mdns hostname set to: [%s]", hostname);
}

void wait_and_restart_task(void* arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
