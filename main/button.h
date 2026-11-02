#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 按键回调 */
typedef void (*button_press_cb_t)(void);

/**
 * @brief 初始化 BOOT 按键（GPIO0）
 *
 * 短按（<1.5 秒松开） → on_press（触发对话）
 * 长按（按住 3 秒）    → on_long_press（关机）
 *
 * @param on_press       短按回调
 * @param on_long_press  长按回调（可为 NULL）
 */
esp_err_t button_init(button_press_cb_t on_press, button_press_cb_t on_long_press);

#ifdef __cplusplus
}
#endif
