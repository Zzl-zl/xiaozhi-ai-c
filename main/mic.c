/**
 * @file mic.c
 * 咪头模块：I2S0 采集 16kHz 单声道音频
 */

#include "mic.h"
#include "board_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "mic";

static i2s_chan_handle_t s_rx = NULL;

/**
 * @brief 初始化麦克风
 */
esp_err_t mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "创建 I2S 通道失败");

    i2s_std_config_t std_cfg = board_i2s_config(AUDIO_IN_RATE, MIC_I2S_BCLK, MIC_I2S_WS,
                                                MIC_I2S_DIN, I2S_GPIO_UNUSED);
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "初始化 I2S 失败");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "使能 I2S 失败");

    ESP_LOGI(TAG, "咪头就绪：I2S0 %d Hz，WS=%d BCLK=%d DIN=%d",
             AUDIO_IN_RATE, MIC_I2S_WS, MIC_I2S_BCLK, MIC_I2S_DIN);
    return ESP_OK;
}

/**
 * @brief 读取一帧 PCM（I2S 32bit → int16）
 */
int mic_read_pcm(int16_t *pcm16, int samples, int timeout_ms)
{
    static int32_t raw[MIC_MAX_SAMPLES];

    if(pcm16 == NULL || samples <= 0) {
        return -1;
    }
    if(samples > MIC_MAX_SAMPLES) {
        samples = MIC_MAX_SAMPLES;
    }

    size_t got = 0;
    if(i2s_channel_read(s_rx, raw, (size_t)samples * sizeof(int32_t), &got,
                        pdMS_TO_TICKS(timeout_ms)) != ESP_OK || got == 0) {
        return -1;
    }

    int n = (int)(got / sizeof(int32_t));
    for(int i = 0; i < n; i++) {
        pcm16[i] = (int16_t)(raw[i] >> 16);   /* 32 位左对齐 → 取高 16 位 */
    }
    return n;
}
