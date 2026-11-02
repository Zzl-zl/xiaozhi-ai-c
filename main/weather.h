/**
 * @file weather.h
 * 天气模块：后台任务定期从 open-meteo.com 拉取当前天气，结果推给显示模块。
 *
 * 依赖：Wi-Fi 已连接 + SNTP 已校时（TLS 证书校验需要正确时间）。
 * 数据来源：https://api.open-meteo.com/v1/forecast（免费、无需 Key）
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WMO 天气代码 → 中文短描述（晴 / 多云 / 小雨 ...）
 */
const char *weather_code_text(int code);

/**
 * @brief 启动天气后台任务（立即拉一次，之后按 weather_config.h 的间隔刷新）
 *
 * 城市/经纬度取自 main/weather_config.h。
 * 非阻塞：拉取失败只打印日志，不影响其它功能。
 */
esp_err_t weather_init(void);

#ifdef __cplusplus
}
#endif
