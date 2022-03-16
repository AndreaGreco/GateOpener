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

const char TAG[]="POW-DRV";

#define GPIO_POWER_P1   26
#define GPIO_POWER_P2   27
#define GPIO_OUTPUT_PIN_SEL     ((1ULL<<GPIO_POWER_P1) | (1ULL<<GPIO_POWER_P2))

#define GPIO_INPUT_SW2  0
#define GPIO_INPUT_PIN_SEL      (1ULL<<GPIO_INPUT_SW2)

#define ESP_INTR_FLAG_DEFAULT 0

#define TIME_DEFAULT 250
#define CYCLE_DEFAULT 5

struct PowerLine_st {
    SemaphoreHandle_t lock;
    uint32_t io_num;

    uint32_t down_time_ms;
    uint32_t up_time_ms;
    unsigned int cycle_cnt;

    char name[8];
};

static xQueueHandle gpio_evt_queue = NULL;
static struct PowerLine_st *p1, *p2;

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    xQueueSendFromISR(gpio_evt_queue, &p1, NULL);
    xQueueSendFromISR(gpio_evt_queue, &p2, NULL);
}

void drive_door_open(enum PowerLine pl)
{
    struct PowerLine_st *pl_arr[] = {
        p1,
        p2
    };

    xQueueSend(gpio_evt_queue, pl_arr[pl], portMAX_DELAY);
}

static void drive_door_open__task(void *arg)
{
    struct PowerLine_st *p = (struct PowerLine_st *)arg;
    int cnt;

    ESP_LOGI(TAG, "Drive door IO:%d", p->io_num);

    /* Door open command */
    for(cnt = 0; cnt < 5; cnt++) {
        gpio_set_level(p->io_num, 1);
        ESP_LOGI(TAG, "Drive door IO:%d Drive:1", p->io_num);
        vTaskDelay(pdMS_TO_TICKS(250));
        ESP_LOGI(TAG, "Drive door IO:%d Drive:0", p->io_num);
        gpio_set_level(p->io_num, 0);
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    xSemaphoreGive(p->lock);
    vTaskDelay(200);
    vTaskDelete(NULL);
}

static struct PowerLine_st* PowerLine_init(uint32_t io_num, char *name)
{
    struct PowerLine_st *p;
    nvs_handle_t hdl;
    esp_err_t err;
    char key[64];

    p = malloc(sizeof(struct PowerLine_st));
    p->lock = xSemaphoreCreateBinary();
    p->io_num = io_num;
    strcpy(p->name, name);

    err = nvs_open(NVS_NAME, NVS_READONLY, &hdl);
    ESP_ERROR_CHECK(err);

    snprintf(key, sizeof(key), "%s%s", NVS_POWER_LINE_DOWN_TIME__KEY, name);
    err = nvs_get_u32(hdl, key, &p->down_time_ms);
    if(err != ESP_OK) {
        p->down_time_ms = TIME_DEFAULT;
        ESP_LOGW(TAG, "Time down %s set to default", p->name);
    }

    snprintf(key, sizeof(key), "%s%s", NVS_POWER_LINE_UP_TIME__KEY, name);
    err = nvs_get_u32(hdl, key, &p->up_time_ms);
    if(err != ESP_OK) {
        p->up_time_ms = TIME_DEFAULT;
        ESP_LOGW(TAG, "Time up %s set to default", p->name);
    }

    snprintf(key, sizeof(key), "%s%s", NVS_POWER_LINE_COUNT, name);
    err = nvs_get_u32(hdl, key, &p->cycle_cnt);
    if(err != ESP_OK) {
        p->cycle_cnt = CYCLE_DEFAULT;
        ESP_LOGW(TAG, "Cycle %s set to default", p->name);
    }

    nvs_close(hdl);

    xSemaphoreGive(p->lock);
    return p;
}

static void power_task(void* arg)
{
    struct PowerLine_st *p;

    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &p, portMAX_DELAY)) {
            if( xSemaphoreTake( p->lock, ( TickType_t ) pdMS_TO_TICKS(10) ) == pdTRUE ) {
                ESP_LOGI(TAG, "Door open req");
                xTaskCreate(drive_door_open__task, "Door Open", 2048, p, 9, NULL);
            } else {
                ESP_LOGI(TAG, "Door Mutex locked!");
            }
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

    gpio_evt_queue = xQueueCreate(10, sizeof(struct PowerLine_st*));
    xTaskCreate(power_task, "power-task", 2048, NULL, 10, NULL);

    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(GPIO_INPUT_SW2, gpio_isr_handler, (void*) GPIO_INPUT_SW2);
}
