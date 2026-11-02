/**
 * @file wake_word.c
 * 离线语音唤醒模块（ESP-SR + WakeNet）—— 按 C++ 版架构：
 *
 *   Feed 侧：麦克风任务调用 wake_word_feed()，只把数据喂进 AFE（不阻塞）
 *   Fetch 侧：独立的唤醒任务阻塞在 fetch_with_delay(portMAX_DELAY)，
 *             拿到结果后判断 wakeup_state，检测到唤醒词就回调
 *
 * 唤醒词：CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS（"你好小智"）
 */

#include <string.h>

#include "wake_word.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"

#include "model_path.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"

static const char *TAG = "wake";

#define WAKE_TASK_STACK   8192

static const esp_afe_sr_iface_t *s_afe = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static srmodel_list_t *s_models = NULL;
static int s_feed_chunk = 0;
static int16_t s_feed_buf[1024];   /* 喂 AFE 的累积缓冲（对齐 chunk_512） */
static int s_feed_len = 0;
static wake_word_cb_t s_on_wake = NULL;
static TaskHandle_t s_wake_action_task = NULL;   /* 执行唤醒动作的任务（不阻塞 fetch） */

/**
 * @brief 唤醒检测任务（对应 C++ 的 ProcessingTask）
 *
 * 阻塞等 AFE 出结果，不忙等、不占 CPU。
 */
static void wake_fetch_task(void *arg)
{
    (void)arg;

    while(1) {
        afe_fetch_result_t *res = s_afe->fetch_with_delay(s_afe_data, portMAX_DELAY);
        if(res == NULL) {
            continue;
        }

        if(res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "★ 检测到唤醒词！");
            /* 只通知动作任务，自己立刻回去 fetch（阻塞式调用会导致后续唤醒失灵） */
            if(s_wake_action_task != NULL) {
                xTaskNotifyGive(s_wake_action_task);
            }
        }
    }
}

/**
 * @brief 唤醒动作任务：收到通知后才执行（开音频通道/开始对话可阻塞）
 */
static void wake_action_task(void *arg)
{
    (void)arg;
    while(1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if(s_on_wake != NULL) {
            s_on_wake();
        }
        vTaskDelay(pdMS_TO_TICKS(300));   /* 防止连击 */
    }
}

/**
 * @brief 初始化唤醒词检测
 */
esp_err_t wake_word_init(wake_word_cb_t on_wake)
{
    s_on_wake = on_wake;

    /* 1. 加载模型（模型在 model 分区里） */
    s_models = esp_srmodel_init("model");
    if(s_models == NULL || s_models->num <= 0) {
        ESP_LOGE(TAG, "模型加载失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "共加载 %d 个模型", s_models->num);

    /* 2. 挑出唤醒词模型 */
    char *wn_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    if(wn_name == NULL) {
        ESP_LOGE(TAG, "没找到唤醒词模型");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "唤醒词模型: %s", wn_name);

    /* 3. AFE 配置（单麦） */
    afe_config_t *afe_config = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if(afe_config == NULL) {
        return ESP_FAIL;
    }
    afe_config->aec_init = false;      /* 无回采，不做 AEC */
    afe_config->ns_init = false;
    afe_config->vad_init = false;
    afe_config->wakenet_init = true;
    afe_config->wakenet_model_name = wn_name;

    /* 4. 创建 AFE 实例 */
    s_afe = esp_afe_handle_from_config(afe_config);
    if(s_afe == NULL) {
        return ESP_FAIL;
    }
    s_afe_data = s_afe->create_from_config(afe_config);
    if(s_afe_data == NULL) {
        ESP_LOGE(TAG, "AFE 实例创建失败");
        return ESP_FAIL;
    }

    s_feed_chunk = s_afe->get_feed_chunksize(s_afe_data);
    ESP_LOGI(TAG, "唤醒就绪：喂 %d 采样/次，采样率 %d",
             s_feed_chunk, s_afe->get_samp_rate(s_afe_data));

    /* 5. 唤醒动作任务 + fetch 任务 */
    xTaskCreateWithCaps(wake_action_task, "wake_act", 8192, NULL, 2, &s_wake_action_task, MALLOC_CAP_SPIRAM);

    BaseType_t ret = xTaskCreateWithCaps(wake_fetch_task, "wake_fetch", WAKE_TASK_STACK, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "唤醒 fetch 任务创建: %s", (ret == pdPASS) ? "成功" : "失败");
    return ESP_OK;
}

/**
 * @brief 喂音频（只做 feed，不阻塞）
 */
int wake_word_feed(const int16_t *pcm16, int samples)
{
    if(s_afe == NULL || s_afe_data == NULL || s_feed_chunk <= 0 ||
       s_feed_chunk > (int)(sizeof(s_feed_buf) / sizeof(s_feed_buf[0]))) {
        return 0;
    }

    /* 关键：麦克风一次读 960 采样，AFE 每块要 512，必须缓存剩余数据，
     * 否则每帧丢 448 个采样 → 音频不连续 → 唤醒只能偶尔触发 */
    int i = 0;
    while(i < samples) {
        int need = s_feed_chunk - s_feed_len;
        int take = (samples - i < need) ? (samples - i) : need;
        memcpy(&s_feed_buf[s_feed_len], pcm16 + i, (size_t)take * sizeof(int16_t));
        s_feed_len += take;
        i += take;
        if(s_feed_len >= s_feed_chunk) {
            s_afe->feed(s_afe_data, s_feed_buf);
            s_feed_len = 0;
        }
    }
    return 0;
}


