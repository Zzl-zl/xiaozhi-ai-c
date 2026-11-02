/**
 * @file camera.h
 * 摄像头模块：拍照 + 上传服务器做视觉解释（识图）
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化摄像头（OV2640/OV3660 等，JPEG 输出）
 *
 * 建议放在唤醒词/音频初始化之后调用（内部 RAM 优先给 esp-sr）。
 * 失败不影响语音对话。
 */
esp_err_t camera_init(void);

/**
 * @brief 摄像头是否就绪
 */
bool camera_is_ready(void);

/**
 * @brief 拍一张照片并让服务器解释（MCP 工具 self.camera.take_photo 调用）
 *
 * @param question 想问关于这张照片的问题（可为 NULL）
 * @return 服务器返回的文字描述；失败时返回错误说明（静态缓冲，勿 free）
 */
const char *camera_take_photo_explain(const char *question);

#ifdef __cplusplus
}
#endif
