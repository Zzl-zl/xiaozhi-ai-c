/**
 * @file button.c
 * 按键模块：轮询 BOOT 键（GPIO0）
 *   - 短按：触发对话
 *   - 长按 3 秒：关机（交给上层处理）
 */

#include "button.h"
#include "board_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "button";

#define LONG_PRESS_MS   3000     /* 长按判定：3 秒 */
#define SHORT_PRESS_MS  1500     /* 短按判定：松开时按住 <1.5 秒 */

static button_press_cb_t s_on_press = NULL;
static button_press_cb_t s_on_long_press = NULL;

/**
 * @brief 按键轮询任务
 */
static void button_task(void *arg)
{
    (void)arg;
    int last_level = 1;
    int64_t press_start = 0;
    bool long_fired = false;

    while(1) {
        int level = gpio_get_level(BOOT_BTN_GPIO);

        if(last_level == 1 && level == 0) {
            /* 按下 */
            press_start = esp_timer_get_time() / 1000;
            long_fired = false;
        }
        else if(level == 0 && !long_fired) {
            /* 按住中：判断是否到长按时间 */
            int64_t held = (esp_timer_get_time() / 1000) - press_start;
            if(held >= LONG_PRESS_MS) {
                long_fired = true;
                ESP_LOGI(TAG, "长按 3 秒 → 关机");
                if(s_on_long_press != NULL) {
                    s_on_long_press();
                }
            }
        }
        else if(last_level == 0 && level == 1) {
            /* 松开 */
            int64_t held = (esp_timer_get_time() / 1000) - press_start;
            if(!long_fired && held < SHORT_PRESS_MS) {
                ESP_LOGI(TAG, "短按 → 对话");
                if(s_on_press != NULL) {
                    s_on_press();
                }
            }
        }

        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/**
 * @brief 初始化按键
 */
esp_err_t button_init(button_press_cb_t on_press, button_press_cb_t on_long_press)
{
    s_on_press = on_press;
    s_on_long_press = on_long_press;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOOT_BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "GPIO 配置失败");

    xTaskCreateWithCaps(button_task, "button", 6144, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "按键就绪：GPIO%d（短按对话 / 长按 3 秒关机）", BOOT_BTN_GPIO);
    return ESP_OK;
}
