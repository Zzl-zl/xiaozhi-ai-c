#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 按键回调 */
typedef void (*button_press_cb_t)(void);

/* 多击回调：clicks = 连击次数（1~4），间隔 <400ms 算同一组 */
typedef void (*button_multi_click_cb_t)(int clicks);

/**
 * @brief 初始化 BOOT 按键（GPIO0）
 *
 * 短按（<1.5 秒松开）连续点击 → on_multi_click(次数)
 *   音乐播放中： 1击=音量+  2击=音量-  3击=下一首  4击=停止
 *   音乐未播放： 1击=开始对话
 * 长按（按住 3 秒）→ on_long_press（关机）
 *
 * @param on_multi_click 多击回调
 * @param on_long_press  长按回调（可为 NULL）
 */
esp_err_t button_init(button_multi_click_cb_t on_multi_click, button_press_cb_t on_long_press);

#ifdef __cplusplus
}
#endif
