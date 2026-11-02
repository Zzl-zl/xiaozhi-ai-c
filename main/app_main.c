/**
 * @file app_main.c
 * 小智 C 版 —— 程序入口（只负责组装各模块）
 *
 * 模块划分：
 *   display.c    屏幕（ST7789 + LVGL）
 *   mic.c        咪头采集
 *   speaker.c    喇叭播放
 *   opus_codec.c Opus 编解码
 *   wifi.c       Wi-Fi 连接 + 时间同步
 *   button.c     按键
 *   xiaozhi_app.c 对话业务
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "board_config.h"
#include "wifi.h"
#include "wifi_config.h"
#include "mic.h"
#include "speaker.h"
#include "opus_codec.h"
#include "display.h"
#include "weather.h"
#include "camera.h"
#include "xiaozhi_app.h"
#include "music.h"

static const char *TAG = "main";

/**
 * @brief 程序入口
 */
void app_main(void)
{
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "  XiaoZhi C 版启动（模块化）");
    ESP_LOGI(TAG, "==========================================");

    /* 0. 时区（屏幕时钟显示用；SNTP 校时后才有正确时间） */
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "时区 TZ=%s", getenv("TZ") ? getenv("TZ") : "(空)");

    /* 1. NVS 初始化 */
    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. 提前创建 MCP 引擎（此时内存最充足，晚了会分配失败） */
    ESP_ERROR_CHECK(xiaozhi_app_init_mcp());
    ESP_LOGI(TAG, "MCP 就绪，内部 RAM 剩余 %u KB，PSRAM 剩余 %u KB",
             (unsigned)(esp_get_free_heap_size() / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* 3. 屏幕（先初始化，方便观察状态） */
    ESP_ERROR_CHECK(display_init());
    display_set_state(DISPLAY_STATE_CONNECTING);
    ESP_LOGI(TAG, "显示模块就绪");

    /* 3. Wi-Fi + 时间同步 */
    const char *ssid = (WIFI_SSID[0] != '\0') ? WIFI_SSID : NULL;
    const char *pass = (WIFI_PASSWORD[0] != '\0') ? WIFI_PASSWORD : NULL;
    if(wifi_sta_connect(ssid, pass, 20000) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 连接失败，请检查 wifi_config.h");
        display_set_wifi(false);
        display_set_state(DISPLAY_STATE_ERROR);
        return;
    }
    display_set_wifi(true);
    wifi_sync_time(15000);

    /* 3.5 天气后台任务（非阻塞；拉到数据后自己刷到待机屏上） */
    if(weather_init() != ESP_OK) {
        ESP_LOGW(TAG, "天气模块启动失败（不影响对话）");
    }

    /* 4. 音频模块 */
    ESP_ERROR_CHECK(mic_init());
    ESP_ERROR_CHECK(speaker_init());
    ESP_ERROR_CHECK(opus_codec_init());

    /* 4.5 在线音乐（WiFi 已连上：播放任务 + 手机控制网页） */
    if(music_init() != ESP_OK) {
        ESP_LOGW(TAG, "音乐模块启动失败（不影响对话）");
    }
    /* 音乐网页改成首次播放时才启动（省 6KB 内部 RAM 栈，缓解启动期内存紧张） */
    speaker_beep(150, 1000);
    ESP_LOGI(TAG, "音频模块就绪");

    /* 5. 对话业务 */
    if(xiaozhi_app_start() != ESP_OK) {
        ESP_LOGE(TAG, "对话业务启动失败");
        display_set_state(DISPLAY_STATE_ERROR);
        return;
    }
    ESP_LOGI(TAG, "初始化完成，按 BOOT 键开始对话");
}
