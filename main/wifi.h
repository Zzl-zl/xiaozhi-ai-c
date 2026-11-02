#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 连接 Wi-Fi（STA 模式）
 *
 * @param ssid       Wi-Fi 名称；传 NULL 或空串表示从 NVS 里读取上次保存的配置
 * @param password   Wi-Fi 密码；ssid 为 NULL 时忽略
 * @param timeout_ms 等待拿到 IP 的超时时间（毫秒）
 * @return ESP_OK 成功拿到 IP；否则失败
 */
esp_err_t wifi_sta_connect(const char *ssid, const char *password, int timeout_ms);

/**
 * @brief 通过 SNTP 同步系统时间
 *
 * TLS 证书校验需要正确的系统时间，否则连接服务器会失败。
 *
 * @param timeout_ms 等待同步的超时时间（毫秒）
 * @return ESP_OK 同步成功
 */
esp_err_t wifi_sync_time(int timeout_ms);

#ifdef __cplusplus
}
#endif
