#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 最大一次播放的采样点数 */
#define SPEAKER_MAX_SAMPLES  2048

/**
 * @brief 初始化喇叭（I2S1 TX，16kHz，32bit 单声道）
 */
esp_err_t speaker_init(void);

/**
 * @brief 播放 PCM（int16 单声道 16kHz）
 */
void speaker_play_pcm(const int16_t *pcm16, int samples);

/**
 * @brief 播放提示音
 * @param ms   时长（毫秒）
 * @param freq 频率（Hz）
 */
void speaker_beep(int ms, int freq);

/**
 * @brief 切到音乐播放模式（I2S 采样率改为指定值，单声道 32bit slot）
 *
 * @param sample_rate 音乐采样率（如 44100/48000）
 */
esp_err_t speaker_music_begin(uint32_t sample_rate);

/**
 * @brief 播放音乐 PCM（单声道 int16，采样率为 speaker_music_begin 设置的值）
 */
void speaker_music_write(const int16_t *mono, int samples);

/**
 * @brief 退出音乐模式，恢复 16kHz 语音输出
 */
void speaker_music_end(void);

#ifdef __cplusplus
}
#endif
