#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opus 参数：16kHz / 单声道 / 60ms 一帧 */
#define OPUS_SAMPLE_RATE     16000
#define OPUS_FRAME_MS        60
#define OPUS_FRAME_SAMPLES   (OPUS_SAMPLE_RATE * OPUS_FRAME_MS / 1000)   /* 960 */

/**
 * @brief 初始化 Opus 编码器 + 解码器
 */
esp_err_t opus_codec_init(void);

/**
 * @brief PCM → Opus 编码
 * @return 编码后的字节数；<0 失败
 */
int opus_encode_frame(const int16_t *pcm16, int samples, uint8_t *out, int out_size);

/**
 * @brief Opus → PCM 解码
 * @return 解码出的采样点数；<0 失败
 */
int opus_decode_frame(const uint8_t *data, int len, int16_t *pcm16, int pcm_capacity_samples);

#ifdef __cplusplus
}
#endif
