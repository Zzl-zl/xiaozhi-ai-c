#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 唤醒词检测回调 */
typedef void (*wake_word_cb_t)(void);

/**
 * @brief 初始化离线语音唤醒（ESP-SR + WakeNet，"你好小智"）
 *
 * @param on_wake 检测到唤醒词时的回调
 */
esp_err_t wake_word_init(wake_word_cb_t on_wake);

/**
 * @brief 送入麦克风 PCM（16kHz 单声道 int16），内部自动切片喂给 AFE
 *
 * @return 本次是否检测到唤醒词（1=是）
 */
int wake_word_feed(const int16_t *pcm16, int samples);

#ifdef __cplusplus
}
#endif
