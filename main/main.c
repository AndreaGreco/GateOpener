#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mdns.h"

#include "wifi_config.h"
#include "config.h"

#define EXAMPLE_MDNS_INSTANCE CONFIG_MDNS_INSTANCE
static const char *TAG = "mdns-test";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    power_up_init();

    switch (power_up_get_mode()) {
    case STARTUP_MODE__STA:
        ESP_LOGI(TAG,"Start in STA MODE");
        wifi_init_sta();
        break;
    case STARTUP_MODE__AP:
        ESP_LOGI(TAG,"Start in AP MODE");
        wifi_init_softap();
        break;
    }

    power_driver_init();
}
