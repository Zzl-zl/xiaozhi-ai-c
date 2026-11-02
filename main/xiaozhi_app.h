#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 提前创建 MCP 引擎并注册工具
 *
 * 必须在启动早期调用（屏幕/LVGL/网络初始化之前），
 * 因为 MCP 引擎需要较大的连续内存。
 */
esp_err_t xiaozhi_app_init_mcp(void);

/**
 * @brief 启动小智对话（获取设备信息 + 初始化 + 连接服务器）
 *
 * 需要先连接 Wi-Fi，并且已调用过 xiaozhi_app_init_mcp()。
 */
esp_err_t xiaozhi_app_start(void);

/**
 * @brief 打开音频通道并开始一轮对话（由按键/唤醒词触发）
 */
void xiaozhi_app_start_listening(void);

/**
 * @brief 取服务器下发的视觉识别（vision explain）接口地址和 token
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 表示聊天还没连上/服务器没下发
 */
esp_err_t xiaozhi_app_get_vision(char *url, size_t url_size, char *token, size_t token_size);

/**
 * @brief 请求播放音乐（会等小智说完话再真正开始，避免和 TTS 抢喇叭）
 *
 * @param url     音频直链；为 NULL 时按下方关键词/内置曲目处理
 * @param keyword 搜索关键词；为 NULL 时播内置曲目
 */
void xiaozhi_app_request_music(const char *url, const char *keyword);

/**
 * @brief 是否正在说话（TTS 播放中）
 */
bool xiaozhi_app_is_speaking(void);

/**
 * @brief 取消"对话结束自动续播音乐"（用户明确说停止播放时调用）
 */
void xiaozhi_app_cancel_music_resume(void);

#ifdef __cplusplus
}
#endif
