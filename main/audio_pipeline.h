#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opus 编码参数（与服务器协商的默认值一致：16kHz 单声道 60ms 一帧） */
#define OPUS_SAMPLE_RATE     16000
#define OPUS_FRAME_MS        60
#define OPUS_FRAME_SAMPLES   (OPUS_SAMPLE_RATE * OPUS_FRAME_MS / 1000)   /* 960 */

/**
 * @brief 初始化音频管线（I2S 麦克风/喇叭 + Opus 编解码器）
 */
esp_err_t audio_pipeline_init(void);

/**
 * @brief 从麦克风读取一帧 PCM（int16，单声道 16kHz）
 * @param pcm16    输出缓冲区
 * @param samples  期望读取的采样点数
 * @param timeout_ms 超时
 * @return 实际读到的采样点数；<0 表示失败
 */
int audio_pipeline_read_mic(int16_t *pcm16, int samples, int timeout_ms);

/**
 * @brief 把一帧 PCM 编码成 Opus
 * @return 编码后的字节数；<0 失败
 */
int audio_pipeline_encode(const int16_t *pcm16, int samples, uint8_t *out, int out_size);

/**
 * @brief 把一帧 Opus 解码并播放到喇叭
 */
void audio_pipeline_play_opus(const uint8_t *opus, int len);

/**
 * @brief 直接播放 PCM（int16，16kHz 单声道）
 */
void audio_pipeline_play_pcm(const int16_t *pcm16, int samples);

/**
 * @brief 播放提示音（电平可调）
 */
void audio_pipeline_beep(int ms, int freq);

#ifdef __cplusplus
}
#endif
