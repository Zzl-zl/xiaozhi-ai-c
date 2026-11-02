#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 最大一次读取的采样点数（60ms @16kHz） */
#define MIC_MAX_SAMPLES  960

/**
 * @brief 初始化麦克风（I2S0 RX，16kHz，32bit 单声道）
 */
esp_err_t mic_init(void);

/**
 * @brief 从麦克风读取 PCM 数据（自动把 32bit 转成 int16）
 *
 * @param pcm16      输出缓冲区
 * @param samples    期望采样点数（不超过 MIC_MAX_SAMPLES）
 * @param timeout_ms 超时时间
 * @return 实际读到的采样点数；<0 表示失败
 */
int mic_read_pcm(int16_t *pcm16, int samples, int timeout_ms);

#ifdef __cplusplus
}
#endif
