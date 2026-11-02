/**
 * @file mcp_tools.c
 * MCP 工具模块：把本机硬件能力暴露给大模型
 *
 * 原理：小智的 MCP（Model Context Protocol）允许大模型调用设备上的"工具"。
 *       我们注册一个 self.led.set 工具，模型就能在对话中控制板载 LED。
 */

#include "mcp_tools.h"

#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "esp_mcp_data.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

#include "board_config.h"
#include "camera.h"
#include "music.h"
#include "xiaozhi_app.h"

static const char *TAG = "mcp_tools";

/* LED 当前状态 */
static bool s_led_on = false;

/**
 * @brief 工具回调：控制 LED 开/关
 *
 * 参数 on（bool）：true 开灯，false 关灯
 */
static esp_mcp_value_t led_set_cb(const esp_mcp_property_list_t *properties)
{
    bool on = esp_mcp_property_list_get_property_bool(properties, "on");
    s_led_on = on;
    gpio_set_level(LED_GPIO, on ? 1 : 0);
    ESP_LOGI(TAG, "MCP 调用：LED = %s", on ? "开" : "关");
    return esp_mcp_value_create_bool(on);
}

/**
 * @brief 工具回调：拍照 + 服务器识图
 *
 * 参数 question（string）：想问关于照片的问题
 * 返回：服务器返回的文字描述
 */
static esp_mcp_value_t music_play_cb(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    /* 正在播放：直接切下一首；没在播：播第一首 */
    if(music_is_playing()) {
        ESP_LOGI(TAG, "MCP 调用：切下一首");
        if(music_play_next()) {
            return esp_mcp_value_create_string("已切换到下一首");
        }
        return esp_mcp_value_create_string("切换失败");
    }
    ESP_LOGI(TAG, "MCP 调用：播放内置音乐（等说完再播）");
    xiaozhi_app_request_music(NULL, NULL);
    return esp_mcp_value_create_string("好的，马上播放");
}

static esp_mcp_value_t music_play_url_cb(const esp_mcp_property_list_t *properties)
{
    const char *url = esp_mcp_property_list_get_property_string(properties, "url");
    if(url == NULL || url[0] == (char)0) {
        return esp_mcp_value_create_string("缺少音频链接");
    }
    ESP_LOGI(TAG, "MCP 调用：播放音乐 URL=%s", url);
    xiaozhi_app_request_music(url, NULL);
    return esp_mcp_value_create_string("好的，马上播放");
}

static esp_mcp_value_t music_stop_cb(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "MCP 调用：停止音乐");
    music_stop();
    xiaozhi_app_cancel_music_resume();      /* 用户明确要停，别再自动续播 */
    return esp_mcp_value_create_string("已停止播放");
}

static esp_mcp_value_t music_search_cb(const esp_mcp_property_list_t *properties)
{
    const char *kw = esp_mcp_property_list_get_property_string(properties, "keyword");
    if(kw == NULL || kw[0] == 0) {
        return esp_mcp_value_create_string("缺少关键词");
    }
    ESP_LOGI(TAG, "MCP 调用：搜索并播放 %s", kw);
    xiaozhi_app_request_music(NULL, kw);
    return esp_mcp_value_create_string("好的，我来搜一下");
}

static esp_mcp_value_t music_volume_cb(const esp_mcp_property_list_t *properties)
{
    int level = esp_mcp_property_list_get_property_int(properties, "level");
    music_set_volume(level);
    static char buf[64];
    snprintf(buf, sizeof(buf), "音量已调到 %d%%", music_get_volume());
    return esp_mcp_value_create_string(buf);
}

static esp_mcp_value_t music_volume_up_cb(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    music_set_volume(music_get_volume() + 10);
    static char buf[64];
    snprintf(buf, sizeof(buf), "音量已调到 %d%%", music_get_volume());
    return esp_mcp_value_create_string(buf);
}

static esp_mcp_value_t music_volume_down_cb(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    music_set_volume(music_get_volume() - 10);
    static char buf[64];
    snprintf(buf, sizeof(buf), "音量已调到 %d%%", music_get_volume());
    return esp_mcp_value_create_string(buf);
}

static esp_mcp_value_t music_next_cb(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "MCP 调用：下一首");
    if(music_play_next()) {
        return esp_mcp_value_create_string("已切换到下一首");
    }
    return esp_mcp_value_create_string("切换失败");
}

static esp_mcp_value_t music_status_cb(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    return esp_mcp_value_create_string(music_status());
}

static esp_mcp_value_t camera_photo_cb(const esp_mcp_property_list_t *properties)
{
    const char *question = esp_mcp_property_list_get_property_string(properties, "question");
    ESP_LOGI(TAG, "MCP 调用：拍照识图（问题：%s）", question ? question : "(无)");
    const char *reply = camera_take_photo_explain(question);
    return esp_mcp_value_create_string(reply ? reply : "拍照失败");
}

/**
 * @brief 注册 MCP 工具
 */
esp_err_t mcp_tools_init(esp_mcp_t *mcp)
{
    if(mcp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* LED 引脚初始化（板载 LED 为低电平点亮，按 C++ 版习惯先灭） */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "LED GPIO 配置失败");
    gpio_set_level(LED_GPIO, 1);
    s_led_on = false;

    /* ---- 工具：LED 开关 ---- */
    esp_mcp_tool_t *led_tool = esp_mcp_tool_create(
                                   "self.led.set",
                                   "控制板载 LED 灯的开和关。参数 on=true 表示开灯，on=false 表示关灯。",
                                   led_set_cb);
    ESP_RETURN_ON_FALSE(led_tool != NULL, ESP_FAIL, TAG, "创建 LED 工具失败");

    esp_mcp_property_t *on_prop = esp_mcp_property_create_with_bool("on", true);
    ESP_RETURN_ON_FALSE(on_prop != NULL, ESP_FAIL, TAG, "创建 on 属性失败");
    ESP_RETURN_ON_ERROR(esp_mcp_tool_add_property(led_tool, on_prop), TAG, "添加 on 属性失败");

    ESP_RETURN_ON_ERROR(esp_mcp_add_tool(mcp, led_tool), TAG, "注册 LED 工具失败");

    /* ---- 工具：拍照识图 ---- */
    esp_mcp_tool_t *cam_tool = esp_mcp_tool_create(
                                   "self.camera.take_photo",
                                   "你有一台摄像头。当用户让你看看某样东西、问照片里是什么、或者需要你根据画面回答时，"
                                   "调用这个工具拍一张照片。参数 question 是你要问关于这张照片的问题。"
                                   "工具会返回照片的文字描述，请据此回答用户。",
                                   camera_photo_cb);
    if(cam_tool != NULL) {
        esp_mcp_property_t *q_prop = esp_mcp_property_create_with_string("question", "");
        if(q_prop != NULL) {
            esp_mcp_tool_add_property(cam_tool, q_prop);
        }
        if(esp_mcp_add_tool(mcp, cam_tool) == ESP_OK) {
            ESP_LOGI(TAG, "MCP 工具已注册：self.camera.take_photo（拍照识图）");
        }
        else {
            ESP_LOGW(TAG, "注册摄像头工具失败");
        }
    }
    else {
        ESP_LOGW(TAG, "创建摄像头工具失败");
    }

    /* ---- 工具：在线音乐 ---- */
    esp_mcp_tool_t *music_play_tool = esp_mcp_tool_create(
                                          "self.music.play",
                                          "播放网络音乐/歌曲。用户说“放首歌、放音乐、来点音乐、换一首”时调用。"
                                          "不需要参数：未播放时播内置曲目，正在播放时自动切下一首。",
                                          music_play_cb);
    if(music_play_tool != NULL && esp_mcp_add_tool(mcp, music_play_tool) == ESP_OK) {
        ESP_LOGI(TAG, "MCP 工具已注册：self.music.play（在线音乐）");
    }

    esp_mcp_tool_t *music_play_url_tool = esp_mcp_tool_create(
                                              "self.music.play_url",
                                              "播放指定的音频 URL（用户明确给了音乐/音频直链时使用）。",
                                              music_play_url_cb);
    if(music_play_url_tool != NULL) {
        esp_mcp_property_t *url_prop = esp_mcp_property_create_with_string("url", "");
        if(url_prop != NULL) {
            esp_mcp_tool_add_property(music_play_url_tool, url_prop);
        }
        if(esp_mcp_add_tool(mcp, music_play_url_tool) == ESP_OK) {
            ESP_LOGI(TAG, "MCP 工具已注册：self.music.play_url");
        }
    }

    esp_mcp_tool_t *music_stop_tool = esp_mcp_tool_create(
                                          "self.music.stop",
                                          "停止播放当前的网络音乐。",
                                          music_stop_cb);
    if(music_stop_tool != NULL && esp_mcp_add_tool(mcp, music_stop_tool) == ESP_OK) {
        ESP_LOGI(TAG, "MCP 工具已注册：self.music.stop");
    }

    esp_mcp_tool_t *music_search_tool = esp_mcp_tool_create(
                                            "self.music.search",
                                            "按歌名/歌手搜索并播放网络音乐。用户说“放周杰伦的晴天”“来首邓紫棋的歌”时调用。"
                                            "参数 keyword 是搜索关键词（歌名+歌手更好）。",
                                            music_search_cb);
    if(music_search_tool != NULL) {
        esp_mcp_property_t *kw_prop = esp_mcp_property_create_with_string("keyword", "");
        if(kw_prop != NULL) {
            esp_mcp_tool_add_property(music_search_tool, kw_prop);
        }
        if(esp_mcp_add_tool(mcp, music_search_tool) == ESP_OK) {
            ESP_LOGI(TAG, "MCP 工具已注册：self.music.search");
        }
    }

    esp_mcp_tool_t *music_vol_tool = esp_mcp_tool_create(
                                         "self.music.volume",
                                         "设置音乐音量。参数 level 是 0~100 的整数（默认 40）。",
                                         music_volume_cb);
    if(music_vol_tool != NULL) {
        esp_mcp_property_t *lvl_prop = esp_mcp_property_create_with_int_and_range("level", 40, 0, 100);
        if(lvl_prop != NULL) {
            esp_mcp_tool_add_property(music_vol_tool, lvl_prop);
        }
        if(esp_mcp_add_tool(mcp, music_vol_tool) == ESP_OK) {
            ESP_LOGI(TAG, "MCP 工具已注册：self.music.volume");
        }
    }

    esp_mcp_tool_t *music_vol_up_tool = esp_mcp_tool_create(
                                            "self.music.volume_up",
                                            "音乐音量调大一点。",
                                            music_volume_up_cb);
    if(music_vol_up_tool != NULL && esp_mcp_add_tool(mcp, music_vol_up_tool) == ESP_OK) {
        ESP_LOGI(TAG, "MCP 工具已注册：self.music.volume_up");
    }

    esp_mcp_tool_t *music_vol_down_tool = esp_mcp_tool_create(
                                              "self.music.volume_down",
                                              "音乐音量调小一点。",
                                              music_volume_down_cb);
    if(music_vol_down_tool != NULL && esp_mcp_add_tool(mcp, music_vol_down_tool) == ESP_OK) {
        ESP_LOGI(TAG, "MCP 工具已注册：self.music.volume_down");
    }

    esp_mcp_tool_t *music_next_tool = esp_mcp_tool_create(
                                          "self.music.next",
                                          "播放下一首内置曲目。",
                                          music_next_cb);
    if(music_next_tool != NULL && esp_mcp_add_tool(mcp, music_next_tool) == ESP_OK) {
        ESP_LOGI(TAG, "MCP 工具已注册：self.music.next");
    }

    esp_mcp_tool_t *music_status_tool = esp_mcp_tool_create(
                                            "self.music.status",
                                            "查询当前音乐播放状态。",
                                            music_status_cb);
    if(music_status_tool != NULL && esp_mcp_add_tool(mcp, music_status_tool) == ESP_OK) {
        ESP_LOGI(TAG, "MCP 工具已注册：self.music.status");
    }

    ESP_LOGI(TAG, "MCP 工具已注册：self.led.set / self.camera.take_photo / self.music.play / self.music.stop");
    return ESP_OK;
}
