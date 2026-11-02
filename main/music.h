#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化音乐模块（创建播放任务；不启动网页服务器）
 */
esp_err_t music_init(void);

/**
 * @brief 启动手机控制网页（http://<设备IP>/），需要 WiFi 已连上
 */
esp_err_t music_http_start(void);

/**
 * @brief 播放指定 URL 的音频（异步，支持 http/https，MP3 流/文件）
 */
bool music_play_url(const char *url);

/**
 * @brief 播放内置曲目
 *
 * @param index 曲目序号（0 ~ MUSIC_STATION_COUNT-1）
 */
bool music_play_station(int index);

/**
 * @brief 播放下一首内置曲目
 */
bool music_play_next(void);

/**
 * @brief 续播最后播放的地址（对话结束后恢复音乐用）
 */
bool music_resume(void);

/**
 * @brief 暂停播放（唤醒对话时用，保留缓冲，对话结束可原位续播）
 */
void music_pause(void);

/**
 * @brief 恢复暂停的播放
 */
void music_unpause(void);

/**
 * @brief 设置音乐音量（0~100）
 */
void music_set_volume(int percent);

/**
 * @brief 当前音乐音量（0~100）
 */
int music_get_volume(void);

/**
 * @brief 按关键词搜索并播放（网易云公开搜索接口，无需登录）
 *
 * @param keyword 歌名/歌手，如 "周杰伦 晴天"
 */
bool music_play_keyword(const char *keyword);

/**
 * @brief 停止播放（会等播放任务退出，恢复语音音量/采样率）
 */
void music_stop(void);

/**
 * @brief 是否正在播放
 */
bool music_is_playing(void);

/**
 * @brief 当前状态描述（"正在播放" / "已停止" / ...）
 */
const char *music_status(void);

/* ---------- 手机 HTTP 上传流式播放（由 music.c 内部网页服务调用） ---------- */

bool music_upload_begin(void);
bool music_upload_write(const uint8_t *data, int len);
void music_upload_end(void);

#ifdef __cplusplus
}
#endif
