/**
 * @file opus_codec.c
 * Opus 编解码模块（基于 esp_audio_codec 组件）
 */

#include "opus_codec.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_audio_types.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"

static const char *TAG = "opus";

static void *s_enc = NULL;
static void *s_dec = NULL;

/**
 * @brief 初始化 Opus 编码器与解码器
 */
esp_err_t opus_codec_init(void)
{
    /* ---- 编码器：16kHz / 单声道 / 16bit / 60ms 帧 / VOIP ---- */
    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc_cfg.sample_rate = OPUS_SAMPLE_RATE;
    enc_cfg.channel = ESP_AUDIO_MONO;
    enc_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    enc_cfg.bitrate = ESP_OPUS_BITRATE_AUTO;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    enc_cfg.complexity = 1;   /* 降低复杂度，减少 CPU 和栈占用 */
    enc_cfg.enable_fec = false;
    enc_cfg.enable_dtx = false;
    enc_cfg.enable_vbr = false;

    esp_audio_err_t err = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &s_enc);
    if(err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Opus 编码器打开失败: %d", err);
        return ESP_FAIL;
    }

    /* ---- 解码器：16kHz / 单声道 / 60ms 帧 ---- */
    esp_opus_dec_cfg_t dec_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dec_cfg.sample_rate = OPUS_SAMPLE_RATE;
    dec_cfg.channel = ESP_AUDIO_MONO;
    dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    dec_cfg.self_delimited = false;

    err = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &s_dec);
    if(err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Opus 解码器打开失败: %d", err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Opus 就绪：%d Hz 单声道 %d ms/帧", OPUS_SAMPLE_RATE, OPUS_FRAME_MS);
    return ESP_OK;
}

/**
 * @brief PCM → Opus
 */
int opus_encode_frame(const int16_t *pcm16, int samples, uint8_t *out, int out_size)
{
    if(s_enc == NULL || pcm16 == NULL || out == NULL) {
        return -1;
    }

    esp_audio_enc_in_frame_t in_frame = {
        .buffer = (uint8_t *)pcm16,
        .len = (uint32_t)(samples * sizeof(int16_t)),
    };
    esp_audio_enc_out_frame_t out_frame = {
        .buffer = out,
        .len = (uint32_t)out_size,
    };

    if(esp_opus_enc_process(s_enc, &in_frame, &out_frame) != ESP_AUDIO_ERR_OK) {
        return -1;
    }
    return (int)out_frame.encoded_bytes;
}

/**
 * @brief Opus → PCM
 */
int opus_decode_frame(const uint8_t *data, int len, int16_t *pcm16, int pcm_capacity_samples)
{
    if(s_dec == NULL || data == NULL || pcm16 == NULL || len <= 0) {
        return -1;
    }

    esp_audio_dec_in_raw_t raw = {
        .buffer = (uint8_t *)data,
        .len = (uint32_t)len,
    };
    esp_audio_dec_out_frame_t frame = {
        .buffer = (uint8_t *)pcm16,
        .len = (uint32_t)(pcm_capacity_samples * sizeof(int16_t)),
    };
    esp_audio_dec_info_t info = {0};

    if(esp_opus_dec_decode(s_dec, &raw, &frame, &info) != ESP_AUDIO_ERR_OK) {
        return -1;
    }
    return (int)(frame.decoded_size / sizeof(int16_t));
}
