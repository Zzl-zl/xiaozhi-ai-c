/**
 * @file audio_pipeline.c
 * 音频管线：I2S 麦克风/喇叭 + Opus 编解码
 *
 * 数据流：
 *   麦克风(I2S 32bit) → 转 int16 PCM → Opus 编码 → 发给服务器
 *   服务器 Opus → Opus 解码 → int16 PCM → 扩展成 32bit → 喇叭(I2S)
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "soc/soc_caps.h"
#include "driver/i2s_std.h"

#include "esp_audio_types.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"

#include "board_config.h"
#include "audio_pipeline.h"

static const char *TAG = "audio";

static i2s_chan_handle_t s_mic_rx = NULL;
static i2s_chan_handle_t s_spk_tx = NULL;
static void *s_opus_enc = NULL;
static void *s_opus_dec = NULL;

/* I2S 输出统一用 16kHz（和 Opus 一致，避免重采样） */
#define SPK_RATE   OPUS_SAMPLE_RATE

/**
 * @brief 初始化一路 std 模式 I2S 通道（32bit / 单声道 / 左声道）
 */
static esp_err_t i2s_channel_setup(i2s_chan_handle_t *handle, int port, bool is_rx,
                                   int rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t din, gpio_num_t dout)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    if(is_rx) {
        ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, handle), TAG, "RX 通道创建失败");
    }
    else {
        ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, handle, NULL), TAG, "TX 通道创建失败");
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = AUDIO_BITS,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = AUDIO_BITS,
            .ws_pol = false,
            .bit_shift = true,
#if SOC_I2S_HW_VERSION_1
            .msb_right = false,
#else
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*handle, &std_cfg), TAG, "标准模式初始化失败");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(*handle), TAG, "通道使能失败");
    return ESP_OK;
}

/**
 * @brief 初始化 Opus 编码器和解码器（16kHz 单声道 60ms/帧）
 */
static esp_err_t opus_codec_init(void)
{
    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc_cfg.sample_rate = OPUS_SAMPLE_RATE;
    enc_cfg.channel = ESP_AUDIO_MONO;
    enc_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    enc_cfg.bitrate = ESP_OPUS_BITRATE_AUTO;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    enc_cfg.complexity = 3;
    enc_cfg.enable_fec = false;
    enc_cfg.enable_dtx = false;
    enc_cfg.enable_vbr = false;
    esp_audio_err_t err = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &s_opus_enc);
    if(err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Opus 编码器打开失败: %d", err);
        return ESP_FAIL;
    }

    esp_opus_dec_cfg_t dec_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dec_cfg.sample_rate = OPUS_SAMPLE_RATE;
    dec_cfg.channel = ESP_AUDIO_MONO;
    dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    dec_cfg.self_delimited = false;
    err = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &s_opus_dec);
    if(err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Opus 解码器打开失败: %d", err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Opus 编解码器就绪（16kHz 单声道 60ms/帧）");
    return ESP_OK;
}

/**
 * @brief 初始化音频管线
 */
esp_err_t audio_pipeline_init(void)
{
    ESP_RETURN_ON_ERROR(i2s_channel_setup(&s_mic_rx, I2S_NUM_0, true, OPUS_SAMPLE_RATE,
                                          MIC_I2S_BCLK, MIC_I2S_WS, MIC_I2S_DIN, I2S_GPIO_UNUSED),
                        TAG, "麦克风初始化失败");
    ESP_RETURN_ON_ERROR(i2s_channel_setup(&s_spk_tx, I2S_NUM_1, false, SPK_RATE,
                                          SPK_I2S_BCLK, SPK_I2S_LRCK, I2S_GPIO_UNUSED, SPK_I2S_DOUT),
                        TAG, "喇叭初始化失败");
    return opus_codec_init();
}

/**
 * @brief 从麦克风读取 PCM（I2S 32bit → int16）
 */
int audio_pipeline_read_mic(int16_t *pcm16, int samples, int timeout_ms)
{
    static int32_t raw[OPUS_FRAME_SAMPLES];
    int cap = (samples > OPUS_FRAME_SAMPLES) ? OPUS_FRAME_SAMPLES : samples;
    size_t got = 0;
    esp_err_t err = i2s_channel_read(s_mic_rx, raw, cap * sizeof(int32_t), &got, pdMS_TO_TICKS(timeout_ms));
    if(err != ESP_OK || got == 0) {
        return -1;
    }
    int n = (int)(got / sizeof(int32_t));
    for(int i = 0; i < n; i++) {
        pcm16[i] = (int16_t)(raw[i] >> 16);   /* 32 位左对齐 → 取高 16 位 */
    }
    return n;
}

/**
 * @brief PCM → Opus 编码
 */
int audio_pipeline_encode(const int16_t *pcm16, int samples, uint8_t *out, int out_size)
{
    esp_audio_enc_in_frame_t in_frame = {
        .buffer = (uint8_t *)pcm16,
        .len = (uint32_t)(samples * sizeof(int16_t)),
    };
    esp_audio_enc_out_frame_t out_frame = {
        .buffer = out,
        .len = (uint32_t)out_size,
    };
    esp_audio_err_t err = esp_opus_enc_process(s_opus_enc, &in_frame, &out_frame);
    if(err != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "Opus 编码失败: %d", err);
        return -1;
    }
    return (int)out_frame.encoded_bytes;
}

/**
 * @brief 播放 PCM（int16 → I2S 32bit）
 */
void audio_pipeline_play_pcm(const int16_t *pcm16, int samples)
{
    static int32_t buf[OPUS_FRAME_SAMPLES];
    int n = (samples > OPUS_FRAME_SAMPLES) ? OPUS_FRAME_SAMPLES : samples;
    for(int i = 0; i < n; i++) {
        buf[i] = ((int32_t)pcm16[i]) << 16;
    }
    size_t written = 0;
    i2s_channel_write(s_spk_tx, buf, (size_t)n * sizeof(int32_t), &written, pdMS_TO_TICKS(500));
}

/**
 * @brief Opus → 解码 → 播放
 */
void audio_pipeline_play_opus(const uint8_t *opus, int len)
{
    static int16_t pcm_buf[OPUS_FRAME_SAMPLES * 2];
    esp_audio_dec_in_raw_t raw = {
        .buffer = (uint8_t *)opus,
        .len = (uint32_t)len,
    };
    esp_audio_dec_out_frame_t frame = {
        .buffer = (uint8_t *)pcm_buf,
        .len = (uint32_t)sizeof(pcm_buf),
    };
    esp_audio_dec_info_t info = {0};

    esp_audio_err_t err = esp_opus_dec_decode(s_opus_dec, &raw, &frame, &info);
    if(err != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "Opus 解码失败: %d", err);
        return;
    }
    int samples = (int)(frame.decoded_size / sizeof(int16_t));
    if(samples > 0) {
        audio_pipeline_play_pcm(pcm_buf, samples);
    }
}

/**
 * @brief 播放提示音
 */
void audio_pipeline_beep(int ms, int freq)
{
    const int total = OPUS_SAMPLE_RATE * ms / 1000;
    int16_t *buf = malloc(total * sizeof(int16_t));
    if(buf == NULL) {
        return;
    }
    for(int i = 0; i < total; i++) {
        buf[i] = (int16_t)(0.15 * 32767.0 * sin(2.0 * 3.141592653589793 * freq * i / OPUS_SAMPLE_RATE));
    }
    audio_pipeline_play_pcm(buf, total);
    free(buf);
}
