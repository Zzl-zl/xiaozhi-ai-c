/**
 * @file speaker.c
 * 喇叭模块：I2S1 播放 16kHz 单声道音频
 */

#include <stdlib.h>
#include <math.h>

#include "speaker.h"
#include "board_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include <stdint.h>

static const char *TAG = "speaker";

static i2s_chan_handle_t s_tx = NULL;
static uint32_t s_cur_rate = AUDIO_OUT_RATE;   /* 当前 I2S 采样率 */

/**
 * @brief 初始化喇叭
 */
esp_err_t speaker_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "创建 I2S 通道失败");

    i2s_std_config_t std_cfg = board_i2s_config(AUDIO_OUT_RATE, SPK_I2S_BCLK, SPK_I2S_LRCK,
                                                I2S_GPIO_UNUSED, SPK_I2S_DOUT);
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "初始化 I2S 失败");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "使能 I2S 失败");

    ESP_LOGI(TAG, "喇叭就绪：I2S1 %d Hz，DOUT=%d BCLK=%d LRCK=%d",
             AUDIO_OUT_RATE, SPK_I2S_DOUT, SPK_I2S_BCLK, SPK_I2S_LRCK);
    return ESP_OK;
}

/**
 * @brief 播放 PCM（int16 → I2S 32bit）
 */
void speaker_play_pcm(const int16_t *pcm16, int samples)
{
    static int32_t buf[SPEAKER_MAX_SAMPLES];

    if(pcm16 == NULL || samples <= 0) {
        return;
    }
    if(samples > SPEAKER_MAX_SAMPLES) {
        samples = SPEAKER_MAX_SAMPLES;
    }

    for(int i = 0; i < samples; i++) {
        buf[i] = ((int32_t)pcm16[i]) << 16;   /* 16 位左移成 32 位 */
    }

    size_t written = 0;
    i2s_channel_write(s_tx, buf, (size_t)samples * sizeof(int32_t), &written,
                      pdMS_TO_TICKS(500));
}

/**
 * @brief 播放提示音
 */
void speaker_beep(int ms, int freq)
{
    const int total = AUDIO_OUT_RATE * ms / 1000;
    int16_t *buf = malloc((size_t)total * sizeof(int16_t));
    if(buf == NULL) {
        return;
    }
    for(int i = 0; i < total; i++) {
        buf[i] = (int16_t)(0.15 * 32767.0 * sin(2.0 * 3.141592653589793 * freq * i / AUDIO_OUT_RATE));
    }
    speaker_play_pcm(buf, total);
    free(buf);
}

/**
 * @brief 切换 I2S 采样率（只改时钟，slot 仍为 32bit 单声道左）
 */
static esp_err_t speaker_set_rate(uint32_t rate)
{
    if(s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if(rate == 0 || rate == s_cur_rate) {
        return ESP_OK;
    }
    i2s_channel_disable(s_tx);
    i2s_std_config_t std_cfg = board_i2s_config((int32_t)rate, SPK_I2S_BCLK, SPK_I2S_LRCK,
                                                I2S_GPIO_UNUSED, SPK_I2S_DOUT);
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &std_cfg.clk_cfg);
    i2s_channel_enable(s_tx);
    if(err == ESP_OK) {
        s_cur_rate = rate;
        ESP_LOGI(TAG, "I2S 采样率切换为 %u Hz", (unsigned)rate);
    }
    else {
        ESP_LOGE(TAG, "I2S 采样率切换失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t speaker_music_begin(uint32_t sample_rate)
{
    return speaker_set_rate(sample_rate > 0 ? sample_rate : 44100);
}

void speaker_music_write(const int16_t *mono, int samples)
{
    static int32_t buf[2048];

    if(s_tx == NULL || mono == NULL || samples <= 0) {
        return;
    }
    int off = 0;
    while(off < samples) {
        int n = samples - off;
        if(n > (int)(sizeof(buf) / sizeof(buf[0]))) {
            n = (int)(sizeof(buf) / sizeof(buf[0]));
        }
        for(int i = 0; i < n; i++) {
            buf[i] = ((int32_t)mono[off + i]) << 16;   /* 16 位左移成 32 位 */
        }
        size_t written = 0;
        i2s_channel_write(s_tx, buf, (size_t)n * sizeof(int32_t), &written, pdMS_TO_TICKS(500));
        off += n;
    }
}

void speaker_music_end(void)
{
    speaker_set_rate(AUDIO_OUT_RATE);
}
