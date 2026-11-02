#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 显示状态 */
typedef enum {
    DISPLAY_STATE_IDLE = 0,      /* 待机 */
    DISPLAY_STATE_CONNECTING,    /* 连接中 */
    DISPLAY_STATE_LISTENING,     /* 聆听中 */
    DISPLAY_STATE_THINKING,      /* 思考中（说完话到开口回复之间） */
    DISPLAY_STATE_SPEAKING,      /* 说话中 */
    DISPLAY_STATE_ERROR,         /* 出错 */
} display_state_t;

/**
 * @brief 初始化屏幕（ST7789 SPI + LVGL 赛博朋克界面）
 */
esp_err_t display_init(void);

/**
 * @brief 更新显示状态（线程安全）
 */
void display_set_state(display_state_t state);

/**
 * @brief 更新 WiFi 状态图标
 */
void display_set_wifi(bool connected);

/**
 * @brief 播放关机动画（阻塞约 1.6 秒，之后调用方可进入深度睡眠）
 */
void display_play_shutdown(void);

/**
 * @brief 往对话框里加一条消息
 */
void display_add_message(bool is_user, const char *text);

/**
 * @brief 更新待机界面的天气信息（由 weather 模块在拉到数据后调用）
 *
 * @param temp_c     当前温度（整数摄氏度）
 * @param temp_max   今天最高温
 * @param temp_min   今天最低温
 * @param humidity   相对湿度（%）
 * @param code       WMO 天气代码（0 晴 / 2 多云 / 61 小雨 ...）
 * @param city       城市名（如 "上海"）
 */
void display_set_weather(int temp_c, int temp_max, int temp_min, int humidity,
                         int code, const char *city);

/**
 * @brief 设置表情（响应小智下发的 LLM 情绪）
 *
 * @param emoji 情绪名：neutral / happy / laughing / sad / angry / surprised /
 *              sleepy / thinking / winking / cool / loving / confused ...
 *              未知名字按 neutral 处理
 */
void display_set_emoji(const char *emoji);

/**
 * @brief 上报当前 TTS 播放音量（0~100，用于屏幕上嘴巴/声波的跳动）
 *
 * 由播放任务在解码后调用，会自动衰减。
 */
void display_set_audio_level(uint8_t level);

/**
 * @brief 在屏幕最上层显示一帧摄像头画面（RGB565），4 秒后自动隐藏
 *
 * @param rgb565 图像数据（行优先、每像素 16bit）
 * @param w      图像宽
 * @param h      图像高
 */
void display_show_camera_frame(const uint16_t *rgb565, int w, int h);

/**
 * @brief 在聊天框里插入一条系统消息（线程安全，可在任意任务调用）
 */
void display_music_notify(const char *text);

/**
 * @brief 设置是否正在播放音乐（播放时切到音乐界面，停止后回待机）
 */
void display_set_music_playing(bool playing);

/**
 * @brief 设置音乐界面显示的曲名/来源文字（线程安全）
 */
void display_set_music_info(const char *text);

/**
 * @brief 设置音乐电平 0~100（频谱条用，线程安全）
 */
void display_set_music_level(uint8_t level);

#ifdef __cplusplus
}
#endif
