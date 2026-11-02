/**
 * @file weather.c
 * 天气模块实现：HTTPS 拉取 open-meteo 数据 → cJSON 解析 → 推给屏幕
 *
 * 设计要点：
 *   1. 独立后台任务，栈放 PSRAM（内部 RAM 已被 ESP-SR 吃得很紧）
 *   2. 只在拉取成功后按 20 分钟刷新；失败 1 分钟后重试
 *   3. HTTPS 用证书 bundle 校验（sdkconfig 已开 CONFIG_MBEDTLS_CERTIFICATE_BUNDLE）
 *   4. 响应缓冲放 PSRAM（约 1.5KB），避免占内部 RAM
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "cJSON.h"

#include "weather.h"
#include "weather_config.h"
#include "display.h"

static const char *TAG = "weather";

#define WEATHER_BUF_SIZE   1536
#define WEATHER_TASK_STACK 8192

/* HTTP 响应缓冲（PSRAM） */
static char *s_buf = NULL;

/* ======================= WMO 天气代码 → 中文 ======================= */

const char *weather_code_text(int code)
{
    switch(code) {
    case 0:
        return "晴";
    case 1:
        return "晴间多云";
    case 2:
        return "多云";
    case 3:
        return "阴";
    case 45:
    case 48:
        return "雾";
    case 51:
    case 53:
    case 55:
        return "毛毛雨";
    case 56:
    case 57:
    case 66:
    case 67:
        return "冻雨";
    case 61:
        return "小雨";
    case 63:
        return "中雨";
    case 65:
        return "大雨";
    case 71:
        return "小雪";
    case 73:
        return "中雪";
    case 75:
        return "大雪";
    case 77:
        return "雪";
    case 80:
        return "阵雨";
    case 81:
        return "强阵雨";
    case 82:
        return "暴雨";
    case 85:
    case 86:
        return "阵雪";
    case 95:
        return "雷阵雨";
    case 96:
    case 99:
        return "雷暴冰雹";
    default:
        return "多云";
    }
}

/* ======================= HTTPS 拉取 ======================= */

static esp_err_t http_get_to_buf(const char *url)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "esp32-xiaozhi-c/1.0",
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "连接失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if(status != 200) {
        ESP_LOGW(TAG, "HTTP 状态异常: %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int want = (content_length > 0 && content_length < WEATHER_BUF_SIZE)
               ? content_length : (WEATHER_BUF_SIZE - 1);
    int total = 0;
    while(total < want) {
        int r = esp_http_client_read(client, s_buf + total, want - total);
        if(r < 0) {
            ESP_LOGW(TAG, "读取响应失败");
            err = ESP_FAIL;
            break;
        }
        if(r == 0) {
            break;
        }
        total += r;
    }
    s_buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if(err != ESP_OK) {
        return err;
    }
    if(total <= 0) {
        ESP_LOGW(TAG, "响应为空");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ======================= JSON 解析 ======================= */

static double json_number(cJSON *obj, const char *key, double def)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if(item == NULL || !cJSON_IsNumber(item)) {
        return def;
    }
    return item->valuedouble;
}

static bool parse_and_publish(void)
{
    cJSON *root = cJSON_Parse(s_buf);
    if(root == NULL) {
        ESP_LOGW(TAG, "JSON 解析失败");
        return false;
    }

    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if(current == NULL) {
        ESP_LOGW(TAG, "缺少 current 字段");
        cJSON_Delete(root);
        return false;
    }

    int code = (int)json_number(current, "weather_code", -1);
    int hum = (int)json_number(current, "relative_humidity_2m", 0);
    double temp = json_number(current, "temperature_2m", 0);
    if(code < 0) {
        ESP_LOGW(TAG, "天气代码无效");
        cJSON_Delete(root);
        return false;
    }

    /* 今天的最高/最低温（daily 数组里第 0 个值） */
    double tmax = temp;
    double tmin = temp;
    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if(daily != NULL) {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
        if(arr != NULL && arr->child != NULL && cJSON_IsNumber(arr->child)) {
            tmax = arr->child->valuedouble;
        }
        arr = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
        if(arr != NULL && arr->child != NULL && cJSON_IsNumber(arr->child)) {
            tmin = arr->child->valuedouble;
        }
    }
    cJSON_Delete(root);

    if(temp < -60.0 || temp > 70.0) {
        ESP_LOGW(TAG, "温度异常: %.1f", temp);
        return false;
    }

    ESP_LOGI(TAG, "天气：%s %d°C（%d~%d°C）湿度 %d%%",
             weather_code_text(code), (int)(temp + (temp >= 0 ? 0.5 : -0.5)),
             (int)(tmin + (tmin >= 0 ? 0.5 : -0.5)),
             (int)(tmax + (tmax >= 0 ? 0.5 : -0.5)), hum);

    /* 交给显示模块（取整显示） */
    display_set_weather((int)(temp + (temp >= 0 ? 0.5 : -0.5)),
                        (int)(tmax + (tmax >= 0 ? 0.5 : -0.5)),
                        (int)(tmin + (tmin >= 0 ? 0.5 : -0.5)),
                        hum, code, WEATHER_CITY_NAME);
    return true;
}

/* ======================= 后台任务 ======================= */

static const char *s_url_fmt =
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=%s&longitude=%s"
    "&current=temperature_2m,relative_humidity_2m,weather_code"
    "&daily=temperature_2m_max,temperature_2m_min"
    "&timezone=Asia%%2FShanghai&forecast_days=1";

static void weather_task(void *arg)
{
    (void)arg;
    char url[320];
    snprintf(url, sizeof(url), s_url_fmt, WEATHER_LATITUDE, WEATHER_LONGITUDE);
    ESP_LOGI(TAG, "天气任务启动，城市：%s", WEATHER_CITY_NAME);

    while(1) {
        esp_err_t err = http_get_to_buf(url);
        bool ok = false;
        if(err == ESP_OK) {
            ok = parse_and_publish();
        }
        else {
            ESP_LOGW(TAG, "拉取天气失败: %s", esp_err_to_name(err));
        }

        uint32_t delay_ms = ok ? WEATHER_REFRESH_MS : WEATHER_RETRY_MS;
        ESP_LOGI(TAG, "%s，%lu 秒后刷新", ok ? "成功" : "失败",
                 (unsigned long)(delay_ms / 1000));
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t weather_init(void)
{
    if(s_buf == NULL) {
        s_buf = heap_caps_malloc(WEATHER_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if(s_buf == NULL) {
            ESP_LOGE(TAG, "PSRAM 分配失败");
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t ret = xTaskCreateWithCaps(weather_task, "weather", WEATHER_TASK_STACK,
                                         NULL, 2, NULL, MALLOC_CAP_SPIRAM);
    if(ret != pdPASS) {
        ESP_LOGE(TAG, "天气任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
