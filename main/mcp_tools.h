#pragma once

#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册本机的 MCP 工具（供大模型调用）
 *
 * 目前提供：
 *   self.led.set  —— 控制板载 LED 开/关
 *
 * @param mcp  MCP 引擎（由 esp_mcp_create 创建）
 */
esp_err_t mcp_tools_init(esp_mcp_t *mcp);

#ifdef __cplusplus
}
#endif
