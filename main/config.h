#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <stdint.h>
#include "esp_wifi.h"

#define NVS_NAME       "storage"

#define NVS_WIFI_SSID__KEY "wifi-ssid"
#define NVS_WIFI_PASS__KEY "wifi-pass"

#define NVS_MDNS_NAME__KEY  "mdns-name"

#define NVS_POWER_LINE_DOWN_TIME__KEY "down-time"
#define NVS_POWER_LINE_UP_TIME__KEY   "up-time"
#define NVS_POWER_LINE_COUNT          "cicle-count"

#define NVS_TELEGRAM_TOKEN            "telegram-token"
#define NVS_TELEGRAM_CHATID           "telegram-chatid"

#define CONFGI_STARTUP_MAGIC 0x4828

enum STARTUP_MODE {
    STARTUP_MODE__STA = 0x1,
    STARTUP_MODE__AP = 0x2,
};

void power_up_init();
void power_up_set_mode(enum STARTUP_MODE mode);
void power_up_set_wrong_pass(bool wrong_pass);
bool power_up_get_wrong_pass();
enum STARTUP_MODE power_up_get_mode();

enum PowerLine {
    POWER_LINE_1,
    POWER_LINE_2,
};

/** Power Driver **/
void power_driver_init(void);

esp_err_t PowerLine_ConfigSetParams(char *name, uint32_t down_time_ms, uint32_t up_time_ms, uint32_t cycle_count, char**err_txt);

/**
 * \brief Drive Door open
 *
 * \param p PowerLine
 */
void drive_door_open(enum PowerLine pl);

void wifi_init_softap(void);

struct PowerUpData_st {
    uint16_t magic_no;
    uint16_t power_up_cnt;
    uint8_t mode;
    uint8_t wrong_pass;
    uint16_t gap2;
};
struct PowerData_crc_st {
    struct PowerUpData_st data;
    uint32_t crc;
};

extern struct PowerData_crc_st power_up_data;

esp_err_t nvs_read_wifi_credential(uint8_t *ssid, uint8_t *password);
void set_dns_hostname(char* new_hostname);
void initialise_mdns(void);
void start_config_server();
bool get_scan_ap_no(int *p_ap_no, wifi_ap_record_t **p_ap_list);

void http_test_task(void *pvParameters);

void wait_and_restart_task(void* arg);
#endif
