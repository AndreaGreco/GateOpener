#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"
#include "config.h"

static const char TAG[]="POW-DRV";

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

#define GPIO_POWER_P1   26
#define GPIO_POWER_P2   27
#define GPIO_OUTPUT_PIN_SEL     ((1ULL<<GPIO_POWER_P1) | (1ULL<<GPIO_POWER_P2))

#define GPIO_INPUT_SW2  0
#define GPIO_INPUT_PIN_SEL      (1ULL<<GPIO_INPUT_SW2)

#define ESP_INTR_FLAG_DEFAULT 0

#define TIME_DEFAULT 175
#define CYCLE_DEFAULT 5

struct PowerLine_st {
    uint32_t io_num;

    uint32_t down_time_ms;
    uint32_t up_time_ms;
    uint32_t cycle_cnt;

    char name[8];
};

static QueueHandle_t gpio_evt_queue = NULL;
static struct PowerLine_st *p1, *p2;
struct PowerLine_st *pl_arr[2];

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    xQueueSendFromISR(gpio_evt_queue, &p1, NULL);
    xQueueSendFromISR(gpio_evt_queue, &p2, NULL);
}

void drive_door_open(enum PowerLine pl)
{
    ESP_LOGI(TAG, "Enqued new door open request for pl[%d]", pl);

    xQueueSend(gpio_evt_queue, &pl_arr[pl], portMAX_DELAY);
}

static void drive_door_open_run(struct PowerLine_st *p)
{
    int cnt;

    ESP_LOGI(TAG, "Drive door IO:%ld, level-now:%d", p->io_num, gpio_get_level(p->io_num));

    /* Door open command */
    for(cnt = 0; cnt < p->cycle_cnt; cnt++) {
        gpio_set_level(p->io_num, 1);
        ESP_LOGD(TAG, "Drive door IO:%ld Drive:1", p->io_num);
        vTaskDelay(pdMS_TO_TICKS(p->up_time_ms));

        ESP_LOGD(TAG, "Drive door IO:%ld Drive:0", p->io_num);
        gpio_set_level(p->io_num, 0);
        vTaskDelay(pdMS_TO_TICKS(p->down_time_ms));
    }

    vTaskDelay(200);
}

esp_err_t PowerLine_ConfigSetParams(char *name, uint32_t down_time_ms, uint32_t up_time_ms, uint32_t cycle_count, char**err_txt)
{
    struct PowerLine_st *p = NULL;
    nvs_handle_t hdl;
    esp_err_t err;
    char key[64];
    int i;

    for(i = 0; i < ARRAY_SIZE(pl_arr); i++) {
        char *check_name = pl_arr[i]->name;
        ESP_LOGI(TAG, "Check `%s` ==  `%s`", check_name, name);
        if(strcmp(check_name, name) == 0) {
            p = pl_arr[i];
            break;
        }
    }

    if(p == NULL) {
        char dev_names[64];
        size_t wrt = 0;
        int i;

        for(i = 0; i < ARRAY_SIZE(pl_arr); i++) {
            wrt += sprintf(&dev_names[wrt], "%s", pl_arr[i]->name);
            strcat(&dev_names[wrt], ", ");
            wrt+=2;
        }

        asprintf(err_txt, "Dispositivo: %s Non trovato.\nDispositivi presenti:\n%s", name, dev_names);
        return ESP_ERR_NOT_FOUND;
    }

    err = nvs_open(NVS_NAME, NVS_READWRITE, &hdl);
    if(err != ESP_OK) {
        asprintf(err_txt, "Impossibile aprire NVS");
        return err;
    }

    snprintf(key, sizeof(key), "%s%s", NVS_POWER_LINE_DOWN_TIME__KEY, name);
    err = nvs_set_u32(hdl, key, down_time_ms);
    if(err != ESP_OK) {
        p->down_time_ms = TIME_DEFAULT;
        asprintf(err_txt, "Impossibile scrivere DownTime");
        goto close_and_exit;
    }

    snprintf(key, sizeof(key), "%s%s", NVS_POWER_LINE_UP_TIME__KEY, name);
    err = nvs_set_u32(hdl, key, up_time_ms);
    if(err != ESP_OK) {
        p->up_time_ms = TIME_DEFAULT;
        asprintf(err_txt, "Impossibile scrivere UpTime");
        goto close_and_exit;
    }

    snprintf(key, sizeof(key), "%s%s", NVS_POWER_LINE_COUNT, name);
    err = nvs_set_u32(hdl, key, cycle_count);
    if(err != ESP_OK) {
        p->cycle_cnt = CYCLE_DEFAULT;
        asprintf(err_txt, "Impossibile scrivere Cycle");
        goto close_and_exit;
    }
    p->cycle_cnt = cycle_count;

    *err_txt=NULL;

close_and_exit:
    nvs_close(hdl);
    return err;
}

static struct PowerLine_st* PowerLine_init(uint32_t io_num, char *name)
{
    struct PowerLine_st *p;
    nvs_handle_t hdl;
    esp_err_t err;
    char key[64];

    p = malloc(sizeof(struct PowerLine_st));
    p->io_num = io_num;
    strcpy(p->name, name);

    err = nvs_open(NVS_NAME, NVS_READONLY, &hdl);
    ESP_ERROR_CHECK_WITHOUT_ABORT(err);

    snprintf(key, sizeof(key), "%s%s", NVS_POWER_LINE_DOWN_TIME__KEY, name);
    err = nvs_get_u32(hdl, key, &p->down_time_ms);
    if(err != ESP_OK) {
        p->down_time_ms = TIME_DEFAULT;
        ESP_LOGW(TAG, "Time down %s set to default:%d", p->name, TIME_DEFAULT);
    }

    snprintf(key, sizeof(key), "%s%s", NVS_POWER_LINE_UP_TIME__KEY, name);
    err = nvs_get_u32(hdl, key, &p->up_time_ms);
    if(err != ESP_OK) {
        p->up_time_ms = TIME_DEFAULT;
        ESP_LOGW(TAG, "Time up %s set to default:%d", p->name, TIME_DEFAULT);
    }

    snprintf(key, sizeof(key), "%s%s", NVS_POWER_LINE_COUNT, name);
    err = nvs_get_u32(hdl, key, &p->cycle_cnt);
    if(err != ESP_OK) {
        p->cycle_cnt = CYCLE_DEFAULT;
        ESP_LOGW(TAG, "Cycle %s set to default:%d", p->name, CYCLE_DEFAULT);
    }

    nvs_close(hdl);

    return p;
}

static void power_task(void* arg)
{
    struct PowerLine_st *p;

    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &p, portMAX_DELAY)) {
            drive_door_open_run(p);
        }
    }
}

void power_driver_init(void)
{
    gpio_config_t io_conf;

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    /* Enable interrupt on Sw2 */
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    p1 = PowerLine_init(GPIO_POWER_P1, "p1");
    p2 = PowerLine_init(GPIO_POWER_P2, "p2");

    pl_arr[0] = p1;
    pl_arr[1] = p2;

    gpio_evt_queue = xQueueCreate(10, sizeof(struct PowerLine_st*));
    xTaskCreate(power_task, "power-task", 2048, NULL, 10, NULL);

    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(GPIO_INPUT_SW2, gpio_isr_handler, (void*) GPIO_INPUT_SW2);
}
