#ifndef _WIFI_CONFIG_H_
#define _WIFI_CONFIG_H_

#include "freertos/event_groups.h"

/* Bit related to Wifi `s_wifi_event_group` */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define CLOCK_SYNC_DONE    BIT2

extern EventGroupHandle_t s_wifi_event_group;

void wifi_init_sta(void);

#endif
