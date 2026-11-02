/**
 * @file wifi.c
 * Wi-Fi STA 连接实现（ESP-IDF C API）
 *
 * 说明：如果传 NULL 作为 SSID，会尝试从 NVS 读取上次保存的 Wi-Fi 配置
 *       （之前的官方小智固件配过网的话，NVS 里可能还存着，可以直接复用）。
 */

#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"

#include "wifi.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_evt = NULL;
#define WIFI_CONNECTED_BIT BIT0

/**
 * @brief Wi-Fi 事件处理：启动时自动连接，拿到 IP 后置位事件
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA 启动，开始连接...");
        esp_wifi_connect();
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "连接断开 (reason=%d)，3 秒后重试", d->reason);
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_wifi_connect();
    }
    else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "拿到 IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief 连接 Wi-Fi（STA 模式）
 */
esp_err_t wifi_sta_connect(const char *ssid, const char *password, int timeout_ms)
{
    s_wifi_evt = xEventGroupCreate();
    if(s_wifi_evt == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    esp_err_t err;

    if(ssid != NULL && ssid[0] != '\0') {
        strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        if(password != NULL) {
            strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
        }
        ESP_LOGI(TAG, "使用指定 SSID: %s", ssid);
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_ERROR_CHECK(err);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    }
    else {
        /* 从 NVS 读取上次保存的配置（可能来自之前的固件） */
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_ERROR_CHECK(err);
        err = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
        if(err == ESP_OK && wifi_cfg.sta.ssid[0] != '\0') {
            ESP_LOGI(TAG, "使用 NVS 中保存的 SSID: %s", wifi_cfg.sta.ssid);
        }
        else {
            ESP_LOGW(TAG, "NVS 里没有 Wi-Fi 配置，需要手动指定 SSID/密码");
            return ESP_ERR_NOT_FOUND;
        }
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if(bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "连接超时（%d ms）", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 通过 SNTP 同步系统时间（TLS 证书校验需要正确时间）
 */
esp_err_t wifi_sync_time(int timeout_ms)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if(err == ESP_OK) {
        time_t now = 0;
        time(&now);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        ESP_LOGI(TAG, "时间已同步: %04d-%02d-%02d %02d:%02d:%02d",
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    }
    else {
        ESP_LOGW(TAG, "时间同步失败: %s（TLS 可能连不上服务器）", esp_err_to_name(err));
    }
    return err;
}
