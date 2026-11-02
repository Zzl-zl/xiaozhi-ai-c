/**
 * @file music.c
 * 在线音乐：WiFi 拉流（HTTP/HTTPS）→ 本地 MP3 解码 → 板载喇叭播放
 *
 * 两种用法：
 *   1. 设备自己拉流：music_play_url() / music_play_station()（语音/MCP 触发）
 *   2. 手机浏览器打开设备 IP：推 URL 播放，或上传本地 MP3 文件流式播放
 *
 * 说明：ESP32-S3 只有 BLE，不支持蓝牙经典 A2DP，所以无法蓝牙推流给音箱；
 *       这里走「WiFi 取流 + 板载喇叭」。
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
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_http_server.h"
#include "esp_netif.h"

#include "esp_mp3_dec.h"
#include "esp_audio_dec.h"

#include "speaker.h"
#include "music.h"
#include "music_config.h"
#include "cJSON.h"
#include "display.h"
#include <math.h>

static const char *TAG = "music";

#define MUSIC_READ_SIZE       2048
#define MUSIC_PCM_MAX         32768
#define MUSIC_TASK_STACK      16384
#define MUSIC_URL_MAX         512
#define MUSIC_REQ_QUEUE_LEN   2

typedef enum {
    MUSIC_REQ_PLAY_URL = 1,
    MUSIC_REQ_SEARCH = 2,
} music_req_type_t;

typedef struct {
    music_req_type_t type;
    char url[MUSIC_URL_MAX];
    char keyword[160];
} music_req_t;

static TaskHandle_t s_task = NULL;
static QueueHandle_t s_q = NULL;
static SemaphoreHandle_t s_lock = NULL;      /* 保护解码器/音乐输出模式 */

static volatile bool s_stop_req = false;
static volatile bool s_playing = false;
static volatile bool s_uploading = false;
static char s_status[96] = "空闲";

static void *s_dec = NULL;
static uint8_t *s_pcm = NULL;
static uint32_t s_pcm_cap = 0;
static int16_t *s_mono = NULL;
static int s_mono_cap = 0;
static uint32_t s_rate = 0;
static int s_station_index = 0;
static int s_volume = MUSIC_VOLUME_PERCENT;   /* 运行时音量 0~100 */
static int s_retry_left = 0;                 /* 自动重试剩余次数 */
static int s_ch = 2;
static int s_bits = 16;
static int s_dec_err = 0;
static int s_rate_hits = 0;
static uint32_t s_last_rate = 0;
static volatile bool s_play_failed = false;   /* 当前这首没播成功 */
static char s_last_url[MUSIC_URL_MAX];        /* 最后播放的地址（对话结束后续播用） */
#define MUSIC_CAND_MAX 6
static char s_cand_urls[MUSIC_CAND_MAX][320];   /* 搜索出来的候选直链 */
static int s_cand_count = 0;
static bool search_and_play_blocking(const char *keyword);   /* 前置声明：只在音乐任务里执行 */

/* ---------------- PCM 环形缓冲 + 独立 I2S 输出任务 ---------------- */
#define PCM_RING_BYTES   (64 * 1024)     /* 约 0.74 秒 @44.1kHz 单声道 */
static uint8_t *s_ring = NULL;
static volatile uint32_t s_ring_w = 0;
static volatile uint32_t s_ring_r = 0;
static SemaphoreHandle_t s_ring_lock = NULL;
static TaskHandle_t s_out_task = NULL;
static volatile bool s_out_run = false;
static volatile bool s_out_started = false;
static volatile bool s_paused = false;        /* 唤醒对话时暂停（缓冲保留） */
static uint32_t s_pause_ms = 0;               /* 暂停起始时间 */
static uint32_t s_underrun = 0;      /* 环形缓冲断流次数（诊断） */
static uint32_t s_out_frames = 0;    /* 已输出采样数（诊断） */

static uint32_t ring_used(void)
{
    uint32_t w = s_ring_w;
    uint32_t r = s_ring_r;
    return (w >= r) ? (w - r) : (PCM_RING_BYTES - r + w);
}

static uint32_t ring_free_len(void)
{
    return PCM_RING_BYTES - 1 - ring_used();
}

static void ring_write(const uint8_t *data, int len)
{
    if(s_ring == NULL || len <= 0) {
        return;
    }
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    for(int i = 0; i < len; i++) {
        s_ring[s_ring_w] = data[i];
        s_ring_w = (s_ring_w + 1) % PCM_RING_BYTES;
    }
    xSemaphoreGive(s_ring_lock);
}

static int ring_read(uint8_t *data, int len)
{
    if(s_ring == NULL || len <= 0) {
        return 0;
    }
    int n = 0;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    while(n < len && s_ring_r != s_ring_w) {
        data[n++] = s_ring[s_ring_r];
        s_ring_r = (s_ring_r + 1) % PCM_RING_BYTES;
    }
    xSemaphoreGive(s_ring_lock);
    return n;
}

/* 解码出来的 PCM 写进环形缓冲（写满就等一等，形成背压） */
static void ring_put_pcm(const int16_t *pcm, int samples)
{
    int off = 0;
    while(off < samples && s_out_run && !s_stop_req) {
        if(s_paused) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint32_t space = ring_free_len();
        if(space < 2) {
            vTaskDelay(pdMS_TO_TICKS(8));
            continue;
        }
        int n = samples - off;
        if((uint32_t)(n * 2) > space) {
            n = (int)(space / 2);
        }
        ring_write((const uint8_t *)(pcm + off), n * 2);
        off += n;
        if(s_out_task != NULL) {
            xTaskNotifyGive(s_out_task);
        }
    }
}

/* 独立音频输出任务：始终喂 I2S，不受网络抖动影响 */
static void music_out_task(void *arg)
{
    (void)arg;
    static int16_t buf[1024];

    uint32_t last_log = 0;
    while(1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(25));
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if(now_ms - last_log >= 5000) {
            last_log = now_ms;
            ESP_LOGI(TAG, "音频输出：%u 采样，断流 %u 次",
                     (unsigned)s_out_frames, (unsigned)s_underrun);
        }
        while(s_out_run) {
            if(s_paused) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            uint32_t used = ring_used();
            if(!s_out_started) {
                /* 预缓冲约 0.5 秒，避免一上来就断音 */
                if(used < 22050) {
                    break;
                }
                s_out_started = true;
            }
            if(used < 2) {
                s_underrun++;            /* 断流：解码/网络跟不上 */
                break;
            }
            int n = (int)(used / 2);
            if(n > (int)(sizeof(buf) / sizeof(buf[0]))) {
                n = (int)(sizeof(buf) / sizeof(buf[0]));
            }
            int got = ring_read((uint8_t *)buf, n * 2);
            if(got < 2) {
                break;
            }
            speaker_music_write(buf, got / 2);
            s_out_frames += (uint32_t)(got / 2);
        }
    }
}

static bool audio_out_start(void)
{
    if(s_ring == NULL) {
        s_ring = heap_caps_malloc(PCM_RING_BYTES, MALLOC_CAP_SPIRAM);
        if(s_ring == NULL) {
            ESP_LOGE(TAG, "PCM 环形缓冲分配失败");
            return false;
        }
    }
    if(s_ring_lock == NULL) {
        s_ring_lock = xSemaphoreCreateMutex();
        if(s_ring_lock == NULL) {
            return false;
        }
    }
    if(s_out_task == NULL) {
        if(xTaskCreate(music_out_task, "music_out", 3072, NULL, 7, &s_out_task) != pdPASS) {
            ESP_LOGE(TAG, "音频输出任务创建失败");
            return false;
        }
    }
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    s_ring_w = 0;
    s_ring_r = 0;
    xSemaphoreGive(s_ring_lock);
    s_out_run = true;
    s_out_started = false;
    return true;
}

static void audio_out_stop(void)
{
    s_out_run = false;
    if(s_out_task != NULL) {
        xTaskNotifyGive(s_out_task);
    }
    vTaskDelay(pdMS_TO_TICKS(60));       /* 让最后一点数据播完 */
}


/* ------------------------------------------------------------------ */
/* 解码 + 输出                                                         */
/* ------------------------------------------------------------------ */
static bool dec_open(void)
{
    if(s_dec != NULL) {
        return true;
    }
    if(esp_mp3_dec_open(NULL, 0, &s_dec) != ESP_AUDIO_ERR_OK || s_dec == NULL) {
        ESP_LOGE(TAG, "MP3 解码器打开失败");
        s_dec = NULL;
        return false;
    }
    if(s_pcm == NULL) {
        s_pcm_cap = MUSIC_PCM_MAX;
        s_pcm = heap_caps_malloc(s_pcm_cap, MALLOC_CAP_SPIRAM);
        if(s_pcm == NULL) {
            s_pcm_cap = 8192;
            s_pcm = heap_caps_malloc(s_pcm_cap, MALLOC_CAP_SPIRAM);
        }
        if(s_pcm == NULL) {
            ESP_LOGE(TAG, "PCM 缓冲分配失败");
            esp_mp3_dec_close(s_dec);
            s_dec = NULL;
            return false;
        }
    }
    s_rate = 0;
    s_ch = 2;
    s_bits = 16;
    s_dec_err = 0;
    s_rate_hits = 0;
    s_last_rate = 0;
    return true;
}

static void dec_close(void)
{
    if(s_dec != NULL) {
        esp_mp3_dec_close(s_dec);
        s_dec = NULL;
    }
    if(s_mono != NULL) {
        free(s_mono);
        s_mono = NULL;
        s_mono_cap = 0;
    }
    s_rate = 0;
}

/* 把解码出的 PCM 转成单声道 16bit 并写到喇叭 */
static void write_pcm(const uint8_t *pcm, int size, int channels, int bits)
{
    if(pcm == NULL || size <= 0 || bits != 16) {
        return;
    }
    if(channels < 1) {
        channels = 1;
    }
    int frames = size / (2 * channels);
    if(frames <= 0) {
        return;
    }
    if(s_mono == NULL || s_mono_cap < frames) {
        if(s_mono != NULL) {
            free(s_mono);
            s_mono = NULL;
        }
        s_mono_cap = frames + 256;
        s_mono = heap_caps_malloc((size_t)s_mono_cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if(s_mono == NULL) {
            s_mono_cap = 0;
            return;
        }
    }

    const int16_t *src = (const int16_t *)pcm;
    int64_t sum = 0;
    for(int i = 0; i < frames; i++) {
        int32_t v;
        if(channels == 1) {
            v = src[i];
        }
        else {
            v = ((int32_t)src[i * channels] + (int32_t)src[i * channels + 1]) / 2;
        }
        v = v * s_volume / 100;
        s_mono[i] = (int16_t)v;
        sum += (int64_t)v * v;
    }
    ring_put_pcm(s_mono, frames);

    /* 给音乐界面的频谱条算个电平（RMS） */
    int rms = (int)sqrt((double)(sum / (frames > 0 ? frames : 1)));
    int level = rms * 160 / 32768;
    if(level > 100) {
        level = 100;
    }
    display_set_music_level((uint8_t)level);
}

/* 喂一段 MP3 数据给解码器 */
static void dec_feed(const uint8_t *data, int len)
{
    if(data == NULL || len <= 0 || s_dec == NULL) {
        return;
    }

    esp_audio_dec_in_raw_t raw = {0};
    raw.buffer = (uint8_t *)data;
    raw.len = (uint32_t)len;

    while(raw.len > 0 && !s_stop_req) {
        esp_audio_dec_out_frame_t out = {0};
        out.buffer = s_pcm;
        out.len = s_pcm_cap;

        esp_audio_dec_info_t info = {0};
        esp_audio_err_t err = esp_mp3_dec_decode(s_dec, &raw, &out, &info);

        if(err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            /* 输出缓冲不够：扩大后重试 */
            uint32_t need = out.needed_size + 1024;
            if(need > 256 * 1024) {
                break;
            }
            uint8_t *nb = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
            if(nb == NULL) {
                break;
            }
            free(s_pcm);
            s_pcm = nb;
            s_pcm_cap = need;
            continue;
        }

        if(err != ESP_AUDIO_ERR_OK || out.decoded_size == 0) {
            s_dec_err++;
            /* 偶发坏帧：每 50 次错误重置一次解码器继续播，不中断整首歌 */
            if((s_dec_err % 50) == 0 && s_dec != NULL) {
                esp_mp3_dec_reset(s_dec);
            }
            if(s_dec_err > 1500) {
                ESP_LOGW(TAG, "连续解码失败过多，判定播放失败");
                s_play_failed = true;
                break;
            }
            if(raw.consumed == 0) {
                break;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            continue;
        }

        if(out.decoded_size > s_pcm_cap) {
            break;
        }
        /* 只认第一帧解出的参数：后面解码器偶尔会回乱值，不能反复切 I2S 采样率 */
        if(s_rate == 0) {
            uint32_t r = (uint32_t)info.sample_rate;
            bool valid = (r == 8000 || r == 11025 || r == 12000 || r == 16000 ||
                          r == 22050 || r == 24000 || r == 32000 || r == 44100 || r == 48000);
            if(valid) {
                if(r == s_last_rate) {
                    s_rate_hits++;
                }
                else {
                    s_last_rate = r;
                    s_rate_hits = 1;
                }
                if(s_rate_hits >= 3) {
                    s_rate = r;
                    s_ch = (info.channel >= 1 && info.channel <= 2) ? info.channel : 2;
                    s_bits = 16;
                    ESP_LOGI(TAG, "音乐输出参数（锁定）：%u Hz / %d 声道 / %d bit",
                             (unsigned)s_rate, s_ch, s_bits);
                    speaker_music_begin(s_rate);
                }
                else {
                    continue;      /* 还没确认，先不输出 */
                }
            }
            else {
                continue;
            }
        }
        s_dec_err = 0;
        write_pcm(s_pcm, (int)out.decoded_size, s_ch, s_bits);

        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }
}

/* ------------------------------------------------------------------ */
/* URL 拉流播放                                                         */
/* ------------------------------------------------------------------ */
/* 打开 HTTP 流，手动跟随 3xx 重定向（手动 open/read 模式不会自动跳转） */
static bool http_stream_open(esp_http_client_handle_t cli)
{
    for(int hop = 0; hop < 6; hop++) {
        esp_err_t err = esp_http_client_open(cli, 0);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP 连接失败: %s", esp_err_to_name(err));
            return false;
        }
        esp_http_client_fetch_headers(cli);
        int status = esp_http_client_get_status_code(cli);
        ESP_LOGI(TAG, "HTTP 状态 %d（第 %d 跳）", status, hop + 1);
        if(status >= 300 && status < 400) {
            char *location = NULL;
            esp_http_client_get_header(cli, "Location", &location);
            char manual[512] = {0};
            if(location != NULL && location[0] != 0) {
                snprintf(manual, sizeof(manual), "%s", location);
            }
            esp_http_client_close(cli);
            if(esp_http_client_set_redirection(cli) != ESP_OK) {
                /* https → http 的跳转 esp_http_client 会拒绝，这里手动处理 */
                if(manual[0] != 0) {
                    ESP_LOGI(TAG, "手动跟随重定向：%s", manual);
                    esp_http_client_set_url(cli, manual);
                    continue;
                }
                ESP_LOGE(TAG, "重定向地址获取失败");
                return false;
            }
            continue;
        }
        if(status != 200) {
            ESP_LOGE(TAG, "HTTP 状态异常：%d", status);
            return false;
        }
        return true;
    }
    return false;
}

/* URL 编码（用于搜索关键词） */
static void url_encode(const char *in, char *out, size_t out_size)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for(const unsigned char *p = (const unsigned char *)in; *p != 0 && o + 4 < out_size; p++) {
        if((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        }
        else {
            out[o++] = '%';
            out[o++] = hex[(*p >> 4) & 0x0F];
            out[o++] = hex[*p & 0x0F];
        }
    }
    out[o] = 0;
}

/* 简单 HTTP GET（返回 JSON 文本，缓冲由调用者提供） */
static int http_get_text(const char *url, char *out, int out_size)
{
    esp_http_client_config_t cfg = {0};
    cfg.url = url;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 2048;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if(cli == NULL) {
        return -1;
    }
    esp_http_client_set_header(cli, "User-Agent", "Mozilla/5.0 (xiaozhi-c)");
    esp_http_client_set_header(cli, "Referer", "https://music.163.com/");
    int total = 0;
    if(esp_http_client_open(cli, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        while(total < out_size - 1) {
            int r = esp_http_client_read(cli, out + total, out_size - 1 - total);
            if(r <= 0) {
                break;
            }
            total += r;
        }
        out[total] = 0;
        esp_http_client_close(cli);
    }
    else {
        total = -1;
    }
    esp_http_client_cleanup(cli);
    return total;
}

/* 网易云公开搜索：拿候选歌曲 id，拼成外链 */
static int netease_search_urls(const char *keyword, char urls[][320], int max_urls)
{
    char enc[200] = {0};
    url_encode(keyword, enc, sizeof(enc));
    char *url = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    char *resp = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if(url == NULL || resp == NULL) {
        free(url);
        free(resp);
        return 0;
    }
    snprintf(url, 512,
             "https://music.163.com/api/search/get/web?csrf_token=&s=%s&type=1&offset=0&total=true&limit=%d",
             enc, max_urls);
    int n = http_get_text(url, resp, 8192);
    int found = 0;
    if(n > 0) {
        cJSON *root = cJSON_Parse(resp);
        if(root != NULL) {
            cJSON *result = cJSON_GetObjectItem(root, "result");
            cJSON *songs = result ? cJSON_GetObjectItem(result, "songs") : NULL;
            if(cJSON_IsArray(songs)) {
                cJSON *song = NULL;
                cJSON_ArrayForEach(song, songs) {
                    if(found >= max_urls) {
                        break;
                    }
                    cJSON *id = cJSON_GetObjectItem(song, "id");
                    if(cJSON_IsNumber(id)) {
                        snprintf(urls[found], 320,
                                 "http://music.163.com/song/media/outer/url?id=%d.mp3", id->valueint);
                        found++;
                    }
                }
            }
            cJSON_Delete(root);
        }
    }
    ESP_LOGI(TAG, "搜索“%s”：找到 %d 首可尝试的曲目", keyword, found);
    free(url);
    free(resp);
    return found;
}

static void music_play_url_blocking(const char *url)
{
    s_stop_req = false;
    s_paused = false;
    s_playing = true;
    s_play_failed = true;                 /* 真正拿到数据后才算成功 */
    snprintf(s_status, sizeof(s_status), "正在播放");
    display_set_music_playing(true);

    uint8_t *buf = heap_caps_malloc(MUSIC_READ_SIZE, MALLOC_CAP_SPIRAM);
    if(buf == NULL) {
        snprintf(s_status, sizeof(s_status), "内存不足");
        s_playing = false;
        return;
    }

    esp_http_client_config_t cfg = {0};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 4096;
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 5;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if(cli == NULL) {
        ESP_LOGE(TAG, "HTTP 客户端创建失败");
        free(buf);
        s_playing = false;
        snprintf(s_status, sizeof(s_status), "启动失败");
        return;
    }
    esp_http_client_set_header(cli, "User-Agent", "Mozilla/5.0 (xiaozhi-c)");
    esp_http_client_set_header(cli, "Connection", "close");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if(!dec_open()) {
        xSemaphoreGive(s_lock);
        esp_http_client_cleanup(cli);
        free(buf);
        s_playing = false;
        snprintf(s_status, sizeof(s_status), "解码器打开失败");
        return;
    }
    speaker_music_begin(44100);
    audio_out_start();
    xSemaphoreGive(s_lock);

    if(s_retry_left <= 0) {
        s_retry_left = 5;          /* 失败最多自动试 5 首，避免死循环 */
    }
    music_http_start();          /* 首次播放时才启动手机控制网页（省内部 RAM） */
    snprintf(s_last_url, sizeof(s_last_url), "%s", url);
    ESP_LOGI(TAG, "开始拉流：%s", url);
    if(!http_stream_open(cli)) {
        snprintf(s_status, sizeof(s_status), "连接失败");
        goto cleanup;
    }
    ESP_LOGI(TAG, "流已打开，长度 %lld", (long long)esp_http_client_get_content_length(cli));
    s_play_failed = false;

    {
        /* 跳过 ID3v2 标签 */
        int got = 0;
        while(got < 10 && !s_stop_req) {
            int r = esp_http_client_read(cli, (char *)buf + got, 10 - got);
            if(r <= 0) {
                break;
            }
            got += r;
        }
        size_t skip = 0;
        if(got >= 10 && memcmp(buf, "ID3", 3) == 0) {
            size_t tag = ((size_t)(buf[6] & 0x7f) << 21) | ((size_t)(buf[7] & 0x7f) << 14) |
                         ((size_t)(buf[8] & 0x7f) << 7) | (size_t)(buf[9] & 0x7f);
            skip = tag + 10 - (size_t)got;
            ESP_LOGI(TAG, "跳过 ID3 标签 %u 字节", (unsigned)(tag + 10));
        }
        else if(got > 0) {
            if(buf[0] == '<' || buf[0] == '{' || buf[0] == 0xEF) {
                ESP_LOGE(TAG, "返回的不是音频（可能是错误页/版权页）");
                snprintf(s_status, sizeof(s_status), "这首放不了");
                goto cleanup;
            }
            dec_feed(buf, got);
        }
        while(skip > 0 && !s_stop_req) {
            int want = (skip < MUSIC_READ_SIZE) ? (int)skip : MUSIC_READ_SIZE;
            int r = esp_http_client_read(cli, (char *)buf, want);
            if(r <= 0) {
                break;
            }
            skip -= (size_t)r;
        }
    }

    while(!s_stop_req) {
        /* 对话中暂停：不解码也不读网络，缓冲保留 */
        if(s_paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        /* 播放过程中收到新的播放请求（切歌/换台）就立刻停下当前这首 */
        music_req_t pending;
        if(xQueuePeek(s_q, &pending, 0) == pdTRUE) {
            ESP_LOGI(TAG, "收到新的播放请求，切歌");
            break;
        }
        int r = esp_http_client_read(cli, (char *)buf, MUSIC_READ_SIZE);
        if(r < 0) {
            break;
        }
        if(r == 0) {
            if(esp_http_client_is_complete_data_received(cli)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        dec_feed(buf, r);
    }

cleanup:
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    free(buf);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    audio_out_stop();
    dec_close();
    speaker_music_end();
    xSemaphoreGive(s_lock);

    s_playing = false;
    s_paused = false;
    bool failed = s_play_failed;
    s_play_failed = false;
    s_stop_req = false;
    snprintf(s_status, sizeof(s_status), "已停止");
    bool will_retry = failed && (s_retry_left > 0);
    if(!will_retry) {
        display_set_music_playing(false);
        s_retry_left = 0;
    }
    ESP_LOGI(TAG, "播放结束（%s）", failed ? "失败" : "正常");
    if(failed && !s_stop_req && s_retry_left > 0) {
        s_retry_left--;
        if(s_cand_count > 0) {
            /* 搜索候选还有下一首：继续试 */
            char next[320];
            snprintf(next, sizeof(next), "%s", s_cand_urls[0]);
            for(int i = 1; i < s_cand_count; i++) {
                memmove(s_cand_urls[i - 1], s_cand_urls[i], sizeof(s_cand_urls[0]));
            }
            s_cand_count--;
            ESP_LOGW(TAG, "这首放不了（可能没版权），试下一首候选");
            music_play_url(next);
        }
        else {
            ESP_LOGW(TAG, "这首放不了（可能没版权），自动切下一首");
            music_play_next();
        }
    }
}

/* ------------------------------------------------------------------ */
/* 手机上传流式播放                                                     */
/* ------------------------------------------------------------------ */
bool music_upload_begin(void)
{
    music_stop();                                  /* 停掉正在播的 */
    if(s_playing) {
        ESP_LOGW(TAG, "上一首还没停完，拒绝本次推送");
        return false;
    }
    if(!xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000))) {
        return false;
    }
    bool ok = dec_open();
    if(ok) {
        speaker_music_begin(44100);
        audio_out_start();
        s_stop_req = false;
        s_playing = true;
        s_uploading = true;
        snprintf(s_status, sizeof(s_status), "手机推送播放");
        display_set_music_playing(true);
        display_set_music_info("手机推送播放");
        ESP_LOGI(TAG, "开始接收手机上传播放的音频流");
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool music_upload_write(const uint8_t *data, int len)
{
    if(!s_uploading || data == NULL || len <= 0) {
        return false;
    }
    if(!xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000))) {
        return false;
    }
    dec_feed(data, len);
    xSemaphoreGive(s_lock);
    return true;
}

void music_upload_end(void)
{
    if(!s_uploading) {
        return;
    }
    xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000));
    audio_out_stop();
    dec_close();
    speaker_music_end();
    xSemaphoreGive(s_lock);
    s_uploading = false;
    s_playing = false;
    snprintf(s_status, sizeof(s_status), "已停止");
    display_set_music_playing(false);
    ESP_LOGI(TAG, "手机上传播放结束");
}

/* ------------------------------------------------------------------ */
/* 播放任务 + 对外接口                                                  */
/* ------------------------------------------------------------------ */
static void music_task(void *arg)
{
    (void)arg;
    music_req_t req;
    while(1) {
        if(xQueueReceive(s_q, &req, portMAX_DELAY) == pdTRUE) {
            if(req.type == MUSIC_REQ_PLAY_URL) {
                music_play_url_blocking(req.url);
            }
            else if(req.type == MUSIC_REQ_SEARCH) {
                if(!search_and_play_blocking(req.keyword)) {
                    snprintf(s_status, sizeof(s_status), "没找到能播的歌");
                }
            }
        }
    }
}

esp_err_t music_init(void)
{
    if(s_task != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreate(MUSIC_REQ_QUEUE_LEN, sizeof(music_req_t));
    if(s_lock == NULL || s_q == NULL) {
        ESP_LOGE(TAG, "音乐模块初始化失败");
        return ESP_ERR_NO_MEM;
    }
    /* 按 C++/LVGL 的思路：任务栈放内部 RAM（响应快），PCM/解码缓冲仍留 PSRAM */
    BaseType_t ret = xTaskCreate(music_task, "music", 10240, NULL, 5, &s_task);
    if(ret != pdPASS) {
        ESP_LOGE(TAG, "音乐任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "音乐模块就绪（WiFi 拉流 + 本地 MP3 解码 + 板载喇叭），内部 RAM %u KB，PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return ESP_OK;
}

bool music_play_url(const char *url)
{
    if(url == NULL || url[0] == '\0' || s_q == NULL) {
        return false;
    }
    s_stop_req = true;          /* 让正在播的那首先停 */
    music_req_t req = {0};
    req.type = MUSIC_REQ_PLAY_URL;
    snprintf(req.url, sizeof(req.url), "%s", url);
    if(xQueueSend(s_q, &req, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "播放队列已满");
        return false;
    }
    return true;
}

bool music_play_station(int index)
{
    if(index < 0 || index >= MUSIC_STATION_COUNT) {
        index = 0;
    }
    s_station_index = index;
    ESP_LOGI(TAG, "播放内置曲目 %d：%s", index, MUSIC_STATION_NAMES[index]);
    display_set_music_info(MUSIC_STATION_NAMES[index]);
    return music_play_url(MUSIC_STATIONS[index]);
}

void music_set_volume(int percent)
{
    if(percent < 0) {
        percent = 0;
    }
    if(percent > 100) {
        percent = 100;
    }
    s_volume = percent;
    ESP_LOGI(TAG, "音乐音量设为 %d%%", s_volume);
}

int music_get_volume(void)
{
    return s_volume;
}

void music_pause(void)
{
    if(!s_paused) {
        s_paused = true;
        s_pause_ms = (uint32_t)(esp_timer_get_time() / 1000);
        /* 暂停后把 I2S 切回语音采样率，保证小智说话正常 */
        if(xSemaphoreTake(s_lock, pdMS_TO_TICKS(500))) {
            if(s_playing && !s_uploading) {
                speaker_music_end();
            }
            xSemaphoreGive(s_lock);
        }
        ESP_LOGI(TAG, "音乐暂停（对话中），I2S 已切回语音");
    }
}

void music_unpause(void)
{
    if(!s_paused) {
        return;
    }
    s_paused = false;
    uint32_t paused_for = (uint32_t)(esp_timer_get_time() / 1000) - s_pause_ms;
    /* 切回音乐采样率再继续出音 */
    if(xSemaphoreTake(s_lock, pdMS_TO_TICKS(500))) {
        if(s_playing && !s_uploading && s_rate > 0) {
            speaker_music_begin(s_rate);
        }
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "音乐继续（暂停了 %u ms）", (unsigned)paused_for);
    if(paused_for > 15000) {
        /* 暂停太久网络流可能已断开：重开一次 */
        ESP_LOGW(TAG, "暂停过久，重新拉流");
        music_resume();
    }
}

bool music_resume(void)
{
    if(s_last_url[0] == 0) {
        return false;
    }
    ESP_LOGI(TAG, "续播：%s", s_last_url);
    return music_play_url(s_last_url);
}

bool music_play_next(void)
{
    int next = (s_station_index + 1) % MUSIC_STATION_COUNT;
    return music_play_station(next);
}

/* 真正执行搜索（只能在音乐任务里调用：https + JSON 解析需要大栈） */
static bool search_and_play_blocking(const char *keyword)
{
    display_set_music_info(keyword);
    s_cand_count = netease_search_urls(keyword, s_cand_urls, MUSIC_CAND_MAX);
    if(s_cand_count <= 0) {
        ESP_LOGW(TAG, "没搜到可播放的歌曲：%s", keyword);
        return false;
    }
    char first[320];
    snprintf(first, sizeof(first), "%s", s_cand_urls[0]);
    for(int i = 1; i < s_cand_count; i++) {
        memmove(s_cand_urls[i - 1], s_cand_urls[i], sizeof(s_cand_urls[0]));
    }
    s_cand_count--;
    return music_play_url(first);
}

bool music_play_keyword(const char *keyword)
{
    if(keyword == NULL || keyword[0] == 0 || s_q == NULL) {
        return false;
    }
    s_stop_req = true;          /* 先停当前播放 */
    s_cand_count = 0;
    music_req_t req = {0};
    req.type = MUSIC_REQ_SEARCH;
    snprintf(req.keyword, sizeof(req.keyword), "%s", keyword);
    return (xQueueSend(s_q, &req, pdMS_TO_TICKS(1000)) == pdTRUE);
}

void music_stop(void)
{
    s_stop_req = true;
    s_paused = false;
    s_uploading = false;
    /* 等播放线程收尾（恢复语音采样率），最多等 1 秒，避免唤醒响应被拖慢 */
    for(int i = 0; i < 20 && s_playing; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if(!s_playing) {
        s_stop_req = false;
    }
}

bool music_is_playing(void)
{
    return s_playing;
}

const char *music_status(void)
{
    return s_status;
}

/* ------------------------------------------------------------------ */
/* 手机控制网页（HTTP 服务器）                                          */
/* ------------------------------------------------------------------ */
#define MUSIC_HTTP_STACK   6144

static const char *MUSIC_HTML =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>小智音乐</title><style>"
    "body{background:#0b1020;color:#dff;font-family:sans-serif;padding:16px;max-width:520px;margin:auto}"
    "h3{color:#8ff}h4{color:#f9f;margin-top:24px}"
    "input,button{font-size:16px;padding:10px;margin:6px 0;width:100%;box-sizing:border-box;border-radius:8px}"
    "input{background:#131a30;border:1px solid #335;color:#dff}"
    "button{background:#2a1b3d;color:#9ff;border:1px solid #6ff}"
    "p{color:#9ad;font-size:14px;min-height:20px}"
    "</style></head><body>"
    "<h3>小智 · 在线音乐</h3>"
    "<form action=\"/play\" method=\"get\">"
    "<input name=\"url\" placeholder=\"粘贴音频直链 http(s)://...\" value=\"\">"
    "<button>播放这个 URL</button></form>"
    "<h4>播放内置曲目</h4><button onclick=\"fetch('/station')\">播放默认曲目</button>"
    "<h4>播放手机里的音频</h4>"
    "<input type=\"file\" id=\"file\" accept=\"audio/*\">"
    "<button type=\"button\" onclick=\"up()\">上传并播放</button>"
    "<h4>控制</h4><button onclick=\"fetch('/stop')\">停止播放</button>"
    "<p id=\"s\"></p>"
    "<script>"
    "async function up(){const f=document.getElementById('file').files[0];"
    "if(!f){alert('先选一个音频文件');return}"
    "document.getElementById('s').textContent='上传中，请稍候...';"
    "try{const r=await fetch('/upload',{method:'POST',body:f});"
    "document.getElementById('s').textContent=await r.text();}"
    "catch(e){document.getElementById('s').textContent='上传失败: '+e;}}"
    "</script></body></html>";

static esp_err_t http_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, MUSIC_HTML, HTTPD_RESP_USE_STRLEN);
}

/* URL 解码（处理 %XX 和 +） */
static void url_decode_inplace(char *s)
{
    char *out = s;
    for(char *p = s; *p != '\0'; p++) {
        if(*p == '%' && p[1] != '\0' && p[2] != '\0') {
            int hi = (p[1] >= '0' && p[1] <= '9') ? (p[1] - '0') :
                     ((p[1] >= 'a' && p[1] <= 'f') ? (p[1] - 'a' + 10) :
                      ((p[1] >= 'A' && p[1] <= 'F') ? (p[1] - 'A' + 10) : -1));
            int lo = (p[2] >= '0' && p[2] <= '9') ? (p[2] - '0') :
                     ((p[2] >= 'a' && p[2] <= 'f') ? (p[2] - 'a' + 10) :
                      ((p[2] >= 'A' && p[2] <= 'F') ? (p[2] - 'A' + 10) : -1));
            if(hi >= 0 && lo >= 0) {
                *out++ = (char)((hi << 4) | lo);
                p += 2;
                continue;
            }
        }
        if(*p == '+') {
            *out++ = ' ';
            continue;
        }
        *out++ = *p;
    }
    *out = '\0';
}

static esp_err_t http_play_handler(httpd_req_t *req)
{
    char qs[640] = {0};
    char url[MUSIC_URL_MAX] = {0};

    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if(qlen > 1 && qlen <= sizeof(qs)) {
        if(httpd_req_get_url_query_str(req, qs, qlen) == ESP_OK) {
            if(httpd_query_key_value(qs, "url", url, sizeof(url)) != ESP_OK) {
                url[0] = '\0';
            }
        }
    }

    /* 也支持 form body：url=... */
    if(url[0] == '\0' && req->content_len > 0 && req->content_len < (int)sizeof(qs)) {
        int r = httpd_req_recv(req, qs, req->content_len);
        if(r > 0) {
            qs[r] = '\0';
            char *p = strstr(qs, "url=");
            if(p != NULL) {
                p += 4;
                char *end = strchr(p, '&');
                if(end != NULL) {
                    *end = '\0';
                }
                snprintf(url, sizeof(url), "%s", p);
            }
        }
    }

    if(url[0] != '\0') {
        url_decode_inplace(url);
    }
    if(url[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "缺少 url 参数");
    }
    ESP_LOGI(TAG, "网页请求播放：%s", url);
    music_play_url(url);
    return httpd_resp_sendstr(req, "开始播放");
}

static esp_err_t http_station_handler(httpd_req_t *req)
{
    music_play_station(0);
    return httpd_resp_sendstr(req, "开始播放内置曲目");
}

static esp_err_t http_search_handler(httpd_req_t *req)
{
    char qs[320] = {0};
    char kw[160] = {0};
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if(qlen > 1 && qlen <= sizeof(qs)) {
        if(httpd_req_get_url_query_str(req, qs, qlen) == ESP_OK) {
            httpd_query_key_value(qs, "kw", kw, sizeof(kw));
        }
    }
    if(kw[0] == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "缺少 kw 参数");
    }
    url_decode_inplace(kw);          /* 网页/curl 传过来是 URL 编码的 */
    if(music_play_keyword(kw)) {
        return httpd_resp_sendstr(req, "开始搜索并播放");
    }
    return httpd_resp_sendstr(req, "没找到能播的歌曲");
}

static esp_err_t http_next_handler(httpd_req_t *req)
{
    music_play_next();
    return httpd_resp_sendstr(req, "已切换下一首");
}

static esp_err_t http_stop_handler(httpd_req_t *req)
{
    music_stop();
    return httpd_resp_sendstr(req, "已停止");
}

static esp_err_t http_upload_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "手机上传：content_len=%d", req->content_len);
    if(req->content_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "没有收到音频数据");
    }
    if(!music_upload_begin()) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "播放启动失败");
    }

    char buf[2048];
    int remaining = req->content_len;
    int ret = ESP_OK;
    while(remaining > 0) {
        int want = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
        int r = httpd_req_recv(req, buf, want);
        if(r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if(r <= 0) {
            ret = ESP_FAIL;
            break;
        }
        music_upload_write((const uint8_t *)buf, r);
        remaining -= r;
    }
    music_upload_end();

    if(ret != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "上传中断");
    }
    return httpd_resp_sendstr(req, "播放结束");
}

esp_err_t music_http_start(void)
{
    static httpd_handle_t server = NULL;
    if(server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = MUSIC_HTTP_STACK;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 20;
    config.send_wait_timeout = 20;

    esp_err_t err = httpd_start(&server, &config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "音乐网页服务启动失败: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = http_root_handler };
    httpd_uri_t uri_play = { .uri = "/play", .method = HTTP_GET, .handler = http_play_handler };
    httpd_uri_t uri_station = { .uri = "/station", .method = HTTP_GET, .handler = http_station_handler };
    httpd_uri_t uri_search = { .uri = "/search", .method = HTTP_GET, .handler = http_search_handler };
    httpd_uri_t uri_next = { .uri = "/next", .method = HTTP_GET, .handler = http_next_handler };
    httpd_uri_t uri_stop = { .uri = "/stop", .method = HTTP_GET, .handler = http_stop_handler };
    httpd_uri_t uri_upload = { .uri = "/upload", .method = HTTP_POST, .handler = http_upload_handler };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_play);
    httpd_register_uri_handler(server, &uri_station);
    httpd_register_uri_handler(server, &uri_search);
    httpd_register_uri_handler(server, &uri_next);
    httpd_register_uri_handler(server, &uri_stop);
    httpd_register_uri_handler(server, &uri_upload);

    {
        esp_netif_ip_info_t ip = {0};
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if(netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            ESP_LOGI(TAG, "手机控制网页：http://" IPSTR "/ （浏览器打开即可推歌/上传）",
                     IP2STR(&ip.ip));
        }
        else {
            ESP_LOGI(TAG, "音乐网页已就绪（端口 80，IP 见 WiFi 日志）");
        }
    }
    return ESP_OK;
}
