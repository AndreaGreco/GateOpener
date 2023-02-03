#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "config.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

static const char *TAG = "http config";

static esp_err_t get_ap_list(httpd_req_t *req)
{
    cJSON *array = cJSON_CreateArray();
    wifi_ap_record_t *ap_list;
    const char *sys_info;
    int i, ap_no;

    get_scan_ap_no(&ap_no, &ap_list);

    httpd_resp_set_type(req, "application/json");

    for(i = 0; i < ap_no; i++) {
        cJSON *obj = cJSON_CreateObject();

        cJSON_AddStringToObject(obj, "ssid", (char*)ap_list[i].ssid);
        cJSON_AddNumberToObject(obj, "rssi", ap_list[i].rssi);

        cJSON_AddItemToArray(array, obj);
    }

    sys_info = cJSON_Print(array);

    httpd_resp_sendstr(req, sys_info);
    free((void *)sys_info);

    cJSON_Delete(array);

    return ESP_OK;
}

static esp_err_t system_info_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON_AddStringToObject(root, "version", IDF_VER);
    cJSON_AddNumberToObject(root, "cores", chip_info.cores);
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t system_reset_in_sta(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "reset", "okay");
    cJSON_Delete(root);

    power_up_set_mode(STARTUP_MODE__STA);
    xTaskCreate(wait_and_restart_task, "Restart", 1024, NULL, 10, NULL);
    return ESP_OK;
}

static inline void write_credential(char* ssid, char* pass, char* token, int64_t chatid)
{
    nvs_handle_t nvs_handle;
    bool commit = false;

    ESP_ERROR_CHECK( nvs_open(NVS_NAME, NVS_READWRITE, &nvs_handle) );
    if(ssid != NULL) {
        ESP_ERROR_CHECK( nvs_set_str(nvs_handle, NVS_WIFI_SSID__KEY, ssid) );
        commit = true;
    }
    
    if(pass != NULL) {
        ESP_ERROR_CHECK( nvs_set_str(nvs_handle, NVS_WIFI_PASS__KEY, pass) );
        commit = true;
    }
    
    if(token != NULL) {
        ESP_ERROR_CHECK( nvs_set_str(nvs_handle, NVS_TELEGRAM_TOKEN, token) );
        commit = true;
    }

    if(chatid != 0) {
        ESP_ERROR_CHECK( nvs_set_i64(nvs_handle, NVS_TELEGRAM_CHATID, chatid) );
        commit = true;
    }

    if(commit) {
        ESP_ERROR_CHECK( nvs_commit(nvs_handle) );
        ESP_LOGI(TAG, "New credential saved");
    }

    nvs_close(nvs_handle);
}

static esp_err_t cofig_set_credential(httpd_req_t *req)
{
    size_t sz = req->content_len;
    char* content = malloc(sz);
    int ret;

    ret = httpd_req_recv(req, content, sz);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        /* In case of error, returning ESP_FAIL will
         * ensure that the underlying socket is closed */
        return ESP_FAIL;
    } else {
        cJSON *ssid_obj, *pass_obj, *token_obj, *chatid_obj;
        cJSON *rpl_root = cJSON_CreateObject();
        cJSON *root = cJSON_Parse(content);
        char *ssid, *pass, *token, *rpl;
        int64_t chatid;
        bool done;

        ssid_obj = cJSON_GetObjectItem(root, "ssid");
        pass_obj = cJSON_GetObjectItem(root, "password");
        token_obj = cJSON_GetObjectItem(root, "token");
        chatid_obj = cJSON_GetObjectItem(root, "chatid");
        done = false;

        if(ssid_obj != NULL) {
            ssid = cJSON_GetStringValue(ssid_obj);
            ESP_LOGI(TAG, "Read SSID:%s", ssid);
            cJSON_AddTrueToObject(rpl_root, "ssid");
            done = true;
        } else {
            cJSON_AddFalseToObject(rpl_root, "ssid");
            ssid = NULL;
        }

        if(pass_obj != NULL) {
            pass = cJSON_GetStringValue(pass_obj);
            ESP_LOGI(TAG, "Read Password:%s", pass);
            cJSON_AddTrueToObject(rpl_root, "pass");
            done = true;
        } else {
            cJSON_AddFalseToObject(rpl_root, "pass");
            pass = NULL;
        }

        if(token_obj != NULL) {
            token = cJSON_GetStringValue(token_obj);
            ESP_LOGI(TAG, "Read Telegram Token:%s", token);
            cJSON_AddTrueToObject(rpl_root, "token");
            done = true;
        } else {
            cJSON_AddFalseToObject(rpl_root, "token");
            token = NULL;
        }

        if(chatid_obj != NULL) {
            double chatid_json = cJSON_GetNumberValue(chatid_obj);
            chatid = (int64_t) chatid_json;
            ESP_LOGI(TAG, "Read Telegram RowJson:%f, ChatId:%lld", chatid_json, chatid);
            cJSON_AddTrueToObject(rpl_root, "chatid");
            done = true;
        } else {
            cJSON_AddFalseToObject(rpl_root, "chatid");
            chatid = 0;
        }

        if(done) {
            cJSON_AddTrueToObject(rpl_root, "okay");
            write_credential(ssid, pass, token, chatid);

            rpl = cJSON_Print(rpl_root);

            /* Send a simple response */
            httpd_resp_set_status(req, HTTPD_200);
            httpd_resp_send(req, rpl, strlen(rpl));

            xTaskCreate(wait_and_restart_task, "Restart", 1024, NULL, 10, NULL);
            power_up_set_mode(STARTUP_MODE__STA);
        } else {
            cJSON_AddFalseToObject(rpl_root, "okay");

            rpl = cJSON_Print(rpl_root);

            httpd_resp_set_status(req, HTTPD_500_INTERNAL_SERVER_ERROR);
            httpd_resp_send(req, rpl, strlen(rpl));
        }

        cJSON_Delete(rpl_root);
        cJSON_Delete(root);
    }

    /* Reset Wrong password setting, new config written */
    power_up_set_wrong_pass(false);

    free(content);
    return ESP_OK;
}

static esp_err_t system_ota_flash(httpd_req_t *req)
{
    const unsigned BUFF_SZ = 1024;
    size_t total_read = 0;
    char *buff = malloc(BUFF_SZ);
    esp_ota_handle_t handle;
    esp_err_t err;

    const esp_partition_t *cur_part = esp_ota_get_boot_partition();
    const esp_partition_t *next_part = esp_ota_get_next_update_partition(cur_part);

    ESP_LOGI(TAG, "Boot Partition:%s", cur_part->label);
    ESP_LOGI(TAG, "Next_update:%s", next_part->label);

    err = esp_ota_begin(next_part, req->content_len, &handle);
    if(err != ESP_OK)
        goto error_label;

    while (true) {
        int read_sz = httpd_req_recv(req, buff, BUFF_SZ);
        if(read_sz <= 0) {
            switch (read_sz) {
            case HTTPD_SOCK_ERR_TIMEOUT:
                ESP_LOGI(TAG, "TIMEOUT");
                break;
            case HTTPD_SOCK_ERR_INVALID:
                ESP_LOGI(TAG, "ERR_INVALID");
                break;
            case HTTPD_SOCK_ERR_FAIL:
                ESP_LOGI(TAG, "ERR_FAIL");
                break;
            }
            break;
        }

        err = esp_ota_write(handle, buff, read_sz);
        if(err != ESP_OK)
            goto error_label;

        total_read += read_sz;
        if(total_read == req->content_len) {
            /* All data read*/
            err = esp_ota_end(handle);
            if(err != ESP_OK)
                goto error_label;
        }
    }

    {
        char okay_msg[] = "Upgrade done!";

        httpd_resp_set_status(req, HTTPD_200);
        httpd_resp_send(req, okay_msg, strlen(okay_msg));

        esp_ota_set_boot_partition(next_part);
        // esp_restart();
    }
    return ESP_OK;

error_label:
    {
        const char *err_str = esp_err_to_name(err);
        httpd_resp_set_status(req, HTTPD_500_INTERNAL_SERVER_ERROR);
        httpd_resp_send(req, err_str, strlen(err_str));
        return err;
    }
}

const httpd_uri_t ap_list_get_uri = {
    .uri = "/api/v1/ap-list",
    .method = HTTP_GET,
    .handler = get_ap_list,
};

const httpd_uri_t system_info_get_uri = {
    .uri = "/api/v1/system/info",
    .method = HTTP_GET,
    .handler = system_info_get_handler,
};

const httpd_uri_t system_reset_in_sta_uri = {
    .uri = "/api/v1/system/reset-sta",
    .method = HTTP_GET,
    .handler = system_reset_in_sta,
};

const httpd_uri_t config_set_wifi_credentials = {
    .uri = "/api/v1/config/wifi",
    .method = HTTP_POST,
    .handler = cofig_set_credential,
};

const httpd_uri_t system_ota = {
    .uri = "/ota",
    .method = HTTP_POST,
    .handler = system_ota_flash,
};

void start_config_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    esp_err_t err;

    ESP_LOGI(TAG, "Starting HTTP Server");

    err = httpd_start(&server, &config);
    if(err  != ESP_OK)
        ESP_LOGE(TAG, "Start server failed");

    httpd_register_uri_handler(server, &ap_list_get_uri);
    httpd_register_uri_handler(server, &system_info_get_uri);
    httpd_register_uri_handler(server, &config_set_wifi_credentials);
    httpd_register_uri_handler(server, &system_reset_in_sta_uri);
    httpd_register_uri_handler(server, &system_ota);
}