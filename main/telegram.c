#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h" 
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "config.h"
#include "freertos/queue.h"
#include "esp_crt_bundle.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "driver/gpio.h"
#include "freertos/event_groups.h"
#include "cJSON.h"

#define URL_SIZE    512
#define TOKEN_SZ    128
#define CHATID_SZ    128
#define TELEGRAM_CMD_ARG_MAX_CNT 8

static const char *TAG = "Telegram";

static char token[TOKEN_SZ];
static int64_t chatid;

typedef void command_ev_handler_t(char* cmd, int argc, char**argv);

struct command_row_t {
    char *cmd;
    command_ev_handler_t *cb;
};

struct Http_recv_st {
    uint8_t* buff;
    size_t sz;
};

struct TelegramMsg_t {
    int64_t update_id;
    int64_t chat_id;
    char *txt;
};

int64_t UpdateID;
QueueHandle_t cmd_queue;
QueueHandle_t tx_msg_queue;

static bool parse_telegram_replay(char *data, int64_t *res)
{
    cJSON *root, *item, *result;
    cJSON_bool okay;

    root = cJSON_Parse(data);

    item = cJSON_GetObjectItem(root, "ok");
    okay = cJSON_IsTrue(item);
    if(okay) {
        int64_t LastUpdateID = 0;
        int max;
        int i;

        result = cJSON_GetObjectItem(root, "result");
        max = cJSON_GetArraySize(result);

        for(i = 0; i < max; i++) {
            cJSON *ele = cJSON_GetArrayItem(result, i);
            
            int64_t chat_id, update_id;
            cJSON *message, *chat;
            char *text;

            update_id = (int64_t)cJSON_GetNumberValue( cJSON_GetObjectItem(ele, "update_id") );
            message = cJSON_GetObjectItem(ele, "message");

            text = cJSON_GetStringValue(cJSON_GetObjectItem(message, "text"));
            chat = cJSON_GetObjectItem(message, "chat");
            chat_id = (int64_t)cJSON_GetNumberValue( cJSON_GetObjectItem(chat, "id") );

            if(text != NULL) {
                if(text[0] == '/') {
                    struct TelegramMsg_t msg;

                    msg.chat_id = chat_id;
                    msg.txt = strdup(text);
                    msg.update_id = update_id;

                    xQueueSend(cmd_queue, &msg, pdMS_TO_TICKS(250));
                }
            } else {
                ESP_LOGW(TAG, "Warning can't find text:\n```%s```\n", data);
            }

            ESP_LOGD(TAG, "Message:%s\n", cJSON_Print(message) );
            LastUpdateID = update_id;
        }

        *res = (int64_t) LastUpdateID;
        return true;
    }

    cJSON_Delete(root);
    return false;
}

esp_err_t client_event_tx_handler(esp_http_client_event_handle_t evt)
{
    struct Http_recv_st *ext = evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client)) {
            if(ext->buff == NULL) {
                size_t sz = esp_http_client_get_content_length(evt->client);
                ESP_LOGD(TAG, "RecvMalloc Sz:%d", sz);
                ext->buff = malloc(sz + 1);
                if (ext->buff == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                    return ESP_FAIL;
                }
            }
            memcpy(&ext->buff[ext->sz], evt->data, evt->data_len);
        }
        ext->sz += evt->data_len;
        break;

    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        if (ext != NULL) {
            ext->buff[ext->sz] = 0;
            ESP_LOGD(TAG, "%s", ext->buff);

            free(ext->buff);
            memset(ext, 0, sizeof(struct Http_recv_st));
        }
        break;

    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
        if (err != 0) {
            ESP_LOGD(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGD(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        if (ext->buff != NULL) {
            free(ext->buff);
            memset(ext, 0, sizeof(struct Http_recv_st));
        }
        break;

    default:
        break;
    }
    return ESP_OK;
}

esp_err_t client_event_rx_handler(esp_http_client_event_handle_t evt)
{
    struct Http_recv_st *ext = evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client)) {
            if(ext->buff == NULL) {
                size_t sz = esp_http_client_get_content_length(evt->client);
                ext->buff = malloc(sz + 1);
                if (ext->buff == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                    return ESP_FAIL;
                }
            }
            memcpy(&ext->buff[ext->sz], evt->data, evt->data_len);
        }
        ext->sz += evt->data_len;
        break;

    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        if (ext != NULL) {
            int64_t new_update_id;
            ext->buff[ext->sz] = 0;
            ESP_LOGD(TAG, "%s", ext->buff);

            parse_telegram_replay((char*)ext->buff, &new_update_id);
            if(new_update_id != 0)
                UpdateID = new_update_id;

            ESP_LOGD(TAG, "UpdateID:%lld", UpdateID);
            free(ext->buff);
            memset(ext, 0, sizeof(struct Http_recv_st));
        }
        break;

    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
        if (err != 0) {
            ESP_LOGD(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGD(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        if (ext->buff != NULL) {
            free(ext->buff);
            memset(ext, 0, sizeof(struct Http_recv_st));
        }
        break;

    default:
        break;
    }
    return ESP_OK;
}

void telegram_send_msg(char* msg_json)
{
    xQueueSend(tx_msg_queue, &msg_json, portMAX_DELAY);
}

void telegram_tx_msg_task(void *pvParameters) {
    static struct Http_recv_st recvb;
    esp_http_client_handle_t client;
    esp_http_client_config_t config;
    char *url;

    memset(&config, 0, sizeof(config));
    memset(&recvb, 0, sizeof(recvb));

    asprintf(&url, "https://api.telegram.org/bot%s/sendMessage", token);

    config.url = url;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.event_handler = client_event_tx_handler;
    config.disable_auto_redirect = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.user_data = &recvb;
    config.timeout_ms = 60000;

    client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    while (true) {
        char* post_data;
        BaseType_t ret;

        ret = xQueueReceive(tx_msg_queue, &post_data, portMAX_DELAY);
        if(ret == pdTRUE) {
            esp_http_client_set_post_field(client, post_data, strlen(post_data));

            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                int status = esp_http_client_get_status_code(client);
                int64_t len = esp_http_client_get_content_length(client);

                ESP_LOGD(TAG, "HTTP POST Status = %d, content_length = %llu", status, len);
            } else {
                ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
            }

            esp_http_client_close(client);
            free(post_data);
        }
    }
}

static char* build_GetUpdate(int64_t offset) {
    cJSON *root = cJSON_CreateObject();
    static char data[512];

    if(offset != 0 && offset != 1)
        cJSON_AddItemToObject(root, "offset", cJSON_CreateNumber(offset));

    cJSON_AddItemToObject(root, "limit", cJSON_CreateNumber(10));
    cJSON_AddItemToObject(root, "timeout", cJSON_CreateNumber(1200));
    strcpy(data, cJSON_Print(root));

    cJSON_Delete(root);

    return data;
}

void telegram_rx_msg_task(void *pvParameters) {
    static struct Http_recv_st recvb;
    esp_http_client_handle_t client;
    esp_http_client_config_t config;
    char url[URL_SIZE];

    memset(&config, 0, sizeof(config));
    memset(&recvb, 0, sizeof(recvb));

    snprintf(url, URL_SIZE, "https://api.telegram.org/bot%s/getUpdates", token);
    ESP_LOGD(TAG, "Set url:%s", url);

    config.url = url;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.event_handler = client_event_rx_handler;
    config.disable_auto_redirect = true;
    config.user_data = &recvb;
    config.timeout_ms = 1200*1000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);

    while (true) {
        char* post_data = build_GetUpdate(UpdateID + 1);
        esp_http_client_set_header(client, "Content-Type", "application/json");

        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "HTTP POST Status = %d, content_length = %llu",
                    esp_http_client_get_status_code(client),
                    esp_http_client_get_content_length(client));
        } else {
            ESP_LOGD(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        }

        esp_http_client_close(client);
    }

    vTaskDelete(NULL);
}

static void door_open(char*cmd, int argc, char**argv)
{
    /* Open all */
    drive_door_open(POWER_LINE_1);
    drive_door_open(POWER_LINE_2);
}

static void abort_cmd(char*cmd, int argc, char**argv)
{
    abort();
}

static void reset_esp_cmd(char*cmd, int argc, char**argv)
{
    xTaskCreate(wait_and_restart_task, "Restart", 1024, NULL, 10, NULL);
}

struct command_row_t command_table[] = {
    {.cmd = "/apri", .cb = door_open},
//    {.cmd = "/abort", .cb = abort_cmd},
    {.cmd = "/reset", .cb = reset_esp_cmd},
};

static void TelegramMsg_Delete(struct TelegramMsg_t *msg)
{
    free(msg->txt);
}

void telegram_commands_exec(void *pvParameters) {
    while (1)
    {
        struct TelegramMsg_t msg;
        BaseType_t resp;

        resp = xQueueReceive(cmd_queue, &msg, pdMS_TO_TICKS(2500));
        if(resp == pdTRUE) {
            char *save_ptr, *in, *argsv[TELEGRAM_CMD_ARG_MAX_CNT];
            int argc = 0;
            int i;

            in = msg.txt;
            for(argc = 0; argc <TELEGRAM_CMD_ARG_MAX_CNT; argc++) {
                char *ret;

                ret = strtok_r(in, " ", &save_ptr);
                in = save_ptr;

                argsv[argc] = ret;

                if(ret == NULL)
                    break;
            }

            /* Search for command inside command tables */
            for(i = 0; i < sizeof(command_table)/sizeof(command_table[0]); i++) {
                struct command_row_t *row = &command_table[i];
                if(!strcmp(row->cmd, argsv[0])) {
                    row->cb(argsv[0], argc, argsv);
                }
            }

            TelegramMsg_Delete(&msg);
        }
    }
    
}

void telegram_send_keyboard() {
    char *ret;
    cJSON *root = cJSON_CreateObject();

    cJSON_AddNumberToObject(root, "chat_id", (double)chatid);
    cJSON_AddStringToObject(root, "text", "Apri");

    {
        cJSON *replay_markup = cJSON_CreateObject();
        {
            cJSON *KeyBoardLines = cJSON_AddArrayToObject(replay_markup,"keyboard");
            cJSON *KeyBoardCell = cJSON_CreateArray();
            cJSON *Button = cJSON_CreateObject();

            cJSON_AddStringToObject(Button, "text", "/apri");

            cJSON_AddItemToArray(KeyBoardCell, Button);
            cJSON_AddItemToArray(KeyBoardLines, KeyBoardCell);
            cJSON_AddItemToObject(root, "reply_markup", replay_markup);
        }

        cJSON_AddTrueToObject(root, "one_time_keyboard");
        {
            char *tmp = cJSON_PrintUnformatted(root);
            ret = strdup(tmp);

            telegram_send_msg(ret);

            cJSON_Delete(root);
        }
    }
}

static inline bool read_telegram_token() {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    size_t sz;
    bool okay = true;

    err = nvs_open(NVS_NAME, NVS_READONLY, &nvs_handle);
    ESP_ERROR_CHECK(err);

    sz = sizeof(token);
    err = nvs_get_str(nvs_handle, NVS_TELEGRAM_TOKEN, (char*) token, &sz);
    if(err != ESP_OK) {
        okay = false;
    }

    err = nvs_get_i64(nvs_handle, NVS_TELEGRAM_CHATID, &chatid);
    if(err != ESP_OK) {
        okay = false;
    }

    nvs_close(nvs_handle);
    ESP_LOGD(TAG, "Telegram info token:%s - chatid:%lld", token, chatid);

    return okay;
}

void http_test_task(void *pvParameters) {
    bool okay;

    okay = read_telegram_token();
    if(! okay) {
        ESP_LOGE(TAG, "Can't Read Telegram Info");
        vTaskDelete(NULL);
    }

    cmd_queue = xQueueCreate(10, sizeof(struct TelegramMsg_t));
    tx_msg_queue = xQueueCreate(15, sizeof(char*));

    xTaskCreate(telegram_commands_exec, "Telegram exec", 2048, NULL, 9, NULL);
    xTaskCreate(telegram_rx_msg_task, "Telegram recv", 4096, NULL, 10, NULL);
    xTaskCreate(telegram_tx_msg_task, "Telegram send-msg", 4096, NULL, 10, NULL);

    telegram_send_keyboard();

    vTaskDelete(NULL);
}