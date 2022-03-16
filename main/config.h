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

enum PowerLine {
    POWER_LINE_1,
    POWER_LINE_2,
};

/** Power Driver **/
void power_driver_init(void);

/**
 * \brief Drive Door open
 *
 * \param p PowerLine
 */
void drive_door_open(enum PowerLine pl);

#endif