/**
 * @file camera.c
 * 摄像头模块实现：ESP32-S3-CAM 拍照（RGB565 采集 + 片上 JPEG 编码）+ 小智视觉解释
 *
 * 调用链：大模型调 MCP 工具 self.camera.take_photo(question)
 *        → 请求丢给本模块的独立任务（PSRAM 大栈）
 *        → 拍照 → frame2jpg → POST 到服务器 vision explain 接口 → 返回文字描述给大模型
 *
 * 为什么用独立任务：MCP 工具回调运行在对话/协议任务里，栈只有几 KB；
 * 摄像头初始化 + JPEG 编码 + TLS 上传很容易把栈打爆导致重启。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_camera.h"
#include "esp_xiaozhi_camera.h"
#include "img_converters.h"

#include "board_config.h"
#include "camera.h"
#include "speaker.h"
#include "xiaozhi_app.h"
#include "display.h"

static const char *TAG = "camera";

#define CAM_EXPLAIN_BUF_SIZE   4096
#define CAM_JPEG_QUALITY       80
#define CAM_TASK_STACK         24576
#define CAM_QUESTION_MAX       160
#define CAM_REPLY_MAX          1024

static bool s_camera_ready = false;
static esp_xiaozhi_camera_handle_t *s_explain = NULL;
static char *s_resp_buf = NULL;
static char s_err_buf[128];

static TaskHandle_t s_cam_task = NULL;
static QueueHandle_t s_cam_q = NULL;
static SemaphoreHandle_t s_cam_done = NULL;
static char s_reply[CAM_REPLY_MAX];

/* ------------------------------------------------------------------ */
/* 真正的拍照流程（只在 cam_task 里执行）                                */
/* ------------------------------------------------------------------ */
static const char *cam_do_photo(const char *question)
{
    /* 第一次拍照时才初始化摄像头（省开机内部 RAM，不影响语音对话） */
    if(!s_camera_ready) {
        ESP_LOGI(TAG, "首次拍照，初始化摄像头...");
        if(camera_init() != ESP_OK) {
            return "摄像头初始化失败，检查一下接线/供电";
        }
    }

    char url[256] = {0};
    char token[128] = {0};
    if(xiaozhi_app_get_vision(url, sizeof(url), token, sizeof(token)) != ESP_OK) {
        return "服务器还没有下发视觉识别接口，请稍后再试";
    }

    if(esp_xiaozhi_camera_set_explain_url(s_explain, url, token) != ESP_OK) {
        return "设置视觉识别接口失败";
    }

    speaker_beep(60, 2000);

    camera_fb_t *fb = esp_camera_fb_get();
    if(fb == NULL) {
        ESP_LOGE(TAG, "拍照失败");
        return "拍照失败（取不到画面）";
    }

    /* 先在屏幕上显示拍到的画面（4 秒后自动消失） */
    if(fb->format == PIXFORMAT_RGB565) {
        display_show_camera_frame((const uint16_t *)fb->buf, (int)fb->width, (int)fb->height);
    }

    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    bool enc_ok = frame2jpg(fb, CAM_JPEG_QUALITY, &jpg, &jpg_len);
    ESP_LOGI(TAG, "拍照：%ux%u RGB565（%u 字节）→ JPEG（%u 字节）",
             (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len, (unsigned)jpg_len);
    esp_camera_fb_return(fb);

    if(!enc_ok || jpg == NULL) {
        ESP_LOGE(TAG, "JPEG 编码失败");
        return "拍照成功但图片编码失败";
    }

    esp_xiaozhi_camera_frame_t frame = {
        .data = jpg,
        .len = jpg_len,
    };
    size_t out_len = 0;
    esp_err_t err = esp_xiaozhi_camera_explain(s_explain, &frame, question,
                                               s_resp_buf, CAM_EXPLAIN_BUF_SIZE, &out_len);
    free(jpg);

    if(err != ESP_OK) {
        ESP_LOGE(TAG, "上传/识图失败: %s", esp_err_to_name(err));
        snprintf(s_err_buf, sizeof(s_err_buf), "拍照成功但上传识图失败（%s）", esp_err_to_name(err));
        return s_err_buf;
    }

    if(out_len >= CAM_EXPLAIN_BUF_SIZE) {
        out_len = CAM_EXPLAIN_BUF_SIZE - 1;
    }
    s_resp_buf[out_len] = '\0';
    ESP_LOGI(TAG, "识图结果（%u 字节）：%s", (unsigned)out_len, s_resp_buf);

    /* 服务器返回形如 {"success":true,...,"text":"..."}，提取 text 更利于大模型理解 */
    static char s_text[CAM_REPLY_MAX];
    const char *p = strstr(s_resp_buf, "\"text\":\"");
    if(p != NULL) {
        p += 8;
        int n = 0;
        while(*p != '\0' && *p != '"' && n < (int)sizeof(s_text) - 1) {
            if(*p == '\\' && p[1] != '\0') {
                p++;
                s_text[n++] = (*p == 'n') ? ' ' : *p;
                p++;
                continue;
            }
            s_text[n++] = *p++;
        }
        s_text[n] = '\0';
        if(n > 0) {
            return s_text;
        }
    }
    return s_resp_buf;
}

/**
 * @brief 摄像头任务：串行处理拍照请求（大栈在 PSRAM）
 */
static void cam_task(void *arg)
{
    (void)arg;
    char question[CAM_QUESTION_MAX];

    while(1) {
        if(xQueueReceive(s_cam_q, question, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const char *result = cam_do_photo(question);
        snprintf(s_reply, sizeof(s_reply), "%s", result ? result : "拍照失败");
        xSemaphoreGive(s_cam_done);
    }
}

static bool cam_worker_start(void)
{
    if(s_cam_task != NULL) {
        return true;
    }
    s_cam_q = xQueueCreate(1, sizeof(char[CAM_QUESTION_MAX]));
    s_cam_done = xSemaphoreCreateBinary();
    if(s_cam_q == NULL || s_cam_done == NULL) {
        ESP_LOGE(TAG, "拍照任务队列创建失败");
        return false;
    }
    BaseType_t ret = xTaskCreateWithCaps(cam_task, "camera", CAM_TASK_STACK, NULL, 5,
                                         &s_cam_task, MALLOC_CAP_SPIRAM);
    if(ret != pdPASS) {
        ESP_LOGE(TAG, "拍照任务创建失败");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */
esp_err_t camera_init(void)
{
    if(s_camera_ready) {
        return ESP_OK;
    }

    camera_config_t cfg = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = CAM_XCLK_FREQ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QVGA,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .sccb_i2c_port = 0,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "摄像头初始化失败: %s（检查接线/供电）", esp_err_to_name(err));
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if(sensor != NULL) {
        ESP_LOGI(TAG, "传感器 PID=0x%02x，RGB565 QVGA → JPEG，质量 %d",
                 sensor->id.PID, CAM_JPEG_QUALITY);
    }

    s_resp_buf = heap_caps_malloc(CAM_EXPLAIN_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if(s_resp_buf == NULL) {
        ESP_LOGE(TAG, "识图响应缓冲分配失败");
        esp_camera_deinit();
        return ESP_ERR_NO_MEM;
    }

    esp_xiaozhi_camera_config_t explain_cfg = {
        .explain_url = NULL,
        .explain_token = NULL,
    };
    err = esp_xiaozhi_camera_create(&explain_cfg, &s_explain);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "视觉解释客户端创建失败: %s", esp_err_to_name(err));
        esp_camera_deinit();
        return err;
    }

    s_camera_ready = true;

    /* 自检：拍一张编码看看（前几帧可能不完整，重试几次） */
    camera_fb_t *fb = NULL;
    for(int i = 0; i < 3 && fb == NULL; i++) {
        fb = esp_camera_fb_get();
    }
    if(fb != NULL) {
        uint8_t *jpg = NULL;
        size_t jpg_len = 0;
        if(frame2jpg(fb, CAM_JPEG_QUALITY, &jpg, &jpg_len) && jpg != NULL) {
            ESP_LOGI(TAG, "自检拍照成功：%ux%u RGB565 → JPEG %u 字节",
                     (unsigned)fb->width, (unsigned)fb->height, (unsigned)jpg_len);
            free(jpg);
        }
        else {
            ESP_LOGW(TAG, "自检拍照成功但 JPEG 编码失败");
        }
        esp_camera_fb_return(fb);
    }
    else {
        ESP_LOGW(TAG, "自检拍照失败（可能是摄像头没接好）");
    }

    ESP_LOGI(TAG, "摄像头就绪（内部 RAM 剩余 %u KB，PSRAM 剩余 %u KB）",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return ESP_OK;
}

bool camera_is_ready(void)
{
    return s_camera_ready;
}

const char *camera_take_photo_explain(const char *question)
{
    if(!cam_worker_start()) {
        return "拍照任务创建失败（内存不足）";
    }

    char q[CAM_QUESTION_MAX] = {0};
    if(question != NULL) {
        snprintf(q, sizeof(q), "%s", question);
    }
    if(q[0] == '\0') {
        snprintf(q, sizeof(q), "请简要描述这张图片里有什么。");
    }

    if(xQueueSend(s_cam_q, q, pdMS_TO_TICKS(1500)) != pdTRUE) {
        return "摄像头正忙，请稍后再试";
    }
    if(xSemaphoreTake(s_cam_done, pdMS_TO_TICKS(40000)) != pdTRUE) {
        ESP_LOGE(TAG, "拍照超时");
        return "拍照/识图超时";
    }
    return s_reply;
}
