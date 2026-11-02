/**
 * @file xiaozhi_app.c
 * 小智对话业务层：设备信息 → 聊天初始化 → 连接 → 按键触发 → 音频收发
 *
 * 依赖的模块：
 *   mic.c        咪头采集
 *   speaker.c    喇叭播放
 *   opus_codec.c Opus 编解码
 *   button.c     按键
 *   wifi.c       Wi-Fi（由 app_main 先连好）
 *
 * 重要设计：组件的音频回调运行在其内部音频任务里，栈有限，
 *          所以回调里只做"拷贝入队"，真正的 Opus 解码放到独立的播放任务里做。
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"
#include "esp_mcp_engine.h"

#include "board_config.h"
#include "mic.h"
#include "speaker.h"
#include "opus_codec.h"
#include "button.h"
#include "display.h"
#include "mcp_tools.h"
#include "wake_word.h"
#include "xiaozhi_app.h"
#include "music.h"

static const char *TAG = "xiaozhi";

/* 收到的音频包队列：回调只入队，播放任务负责解码 */
#define AUDIO_PKT_MAX     1024
#define AUDIO_QUEUE_DEPTH 16

typedef struct {
    uint16_t len;
    uint8_t data[AUDIO_PKT_MAX];
} audio_pkt_t;

static esp_xiaozhi_chat_handle_t s_chat = 0;
static esp_mcp_t *s_mcp = NULL;              /* MCP 引擎（组件启动前必须初始化） */
static QueueHandle_t s_audio_q = NULL;       /* 音频包队列 */
static EventGroupHandle_t s_chat_evt = NULL; /* 聊天事件组：等"已连接"用 */
#define CHAT_CONNECTED_BIT BIT0

static volatile bool s_audio_open = false;   /* 音频通道是否已打开 */
static volatile bool s_speaking = false;     /* 是否正在播放 TTS */
static volatile bool s_streaming = false;    /* 是否正在上传麦克风（聆听中） */
static volatile bool s_follow_up = false;    /* TTS 结束后自动继续聆听（连续对话） */
static volatile bool s_resume_music = false; /* 唤醒前正在放音乐，对话结束后续播 */
static volatile bool s_convo_active = false; /* 当前是否在一轮对话里（唤醒后到回音乐前） */

/* 等小智说完话再播的音乐请求 */
static volatile bool s_pending_music = false;
static char s_pending_music_url[512];
static char s_pending_music_kw[160];
static volatile uint32_t s_listen_start = 0; /* 本次聆听开始时间(ms) */

/**
 * @brief 聊天事件处理器：记录"传输已连接"状态
 *
 * 必须等 MQTT/WebSocket 真正连上，才能发 hello 打开音频通道，否则会失败。
 */
/* 回到音乐前把本轮对话通道彻底结束（防止残余 ASR/TTS 事件再触发对话） */
static void end_conversation_for_music(void)
{
    s_streaming = false;
    s_follow_up = false;
    s_convo_active = false;
    if(s_chat != 0 && s_audio_open) {
        esp_xiaozhi_chat_send_stop_listening(s_chat);
        vTaskDelay(pdMS_TO_TICKS(30));
        esp_xiaozhi_chat_close_audio_channel(s_chat);
        s_audio_open = false;
    }
    ESP_LOGI(TAG, "本轮对话通道已结束（音乐模式）");
}

void xiaozhi_app_cancel_music_resume(void)
{
    if(s_resume_music) {
        ESP_LOGI(TAG, "用户要求停止播放，取消续播");
    }
    s_resume_music = false;
}

bool xiaozhi_app_is_speaking(void)
{
    return s_speaking;
}

void xiaozhi_app_request_music(const char *url, const char *keyword)
{
    if(s_speaking) {
        /* 还在说话：先挂起，等 TTS 结束再播（否则两路声音抢喇叭会破音/卡） */
        s_pending_music_url[0] = 0;
        s_pending_music_kw[0] = 0;
        if(url != NULL && url[0] != 0) {
            snprintf(s_pending_music_url, sizeof(s_pending_music_url), "%s", url);
        }
        else if(keyword != NULL && keyword[0] != 0) {
            snprintf(s_pending_music_kw, sizeof(s_pending_music_kw), "%s", keyword);
        }
        s_pending_music = true;
        ESP_LOGI(TAG, "小智还在说话，等说完再播放音乐");
        return;
    }

    if(url != NULL && url[0] != 0) {
        music_play_url(url);
    }
    else if(keyword != NULL && keyword[0] != 0) {
        music_play_keyword(keyword);
    }
    else {
        music_play_station(0);
    }
}

static void chat_evt_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    /* 音乐播放期间只接受"唤醒"（唤醒会先停音乐并开启新对话），其它对话事件一律忽略 */
    if(music_is_playing() && !s_convo_active) {
        if(id == ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT ||
           id == ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI ||
           id == ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE ||
           id == ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STARTED ||
           id == ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STOPPED) {
            ESP_LOGI(TAG, "音乐播放中，忽略对话事件 %d", (int)id);
            return;
        }
    }

    (void)arg;
    (void)data;
    if(base != ESP_XIAOZHI_CHAT_EVENTS) {
        return;
    }
    if(id == ESP_XIAOZHI_CHAT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "传输已连接（可以开音频通道了）");
        xEventGroupSetBits(s_chat_evt, CHAT_CONNECTED_BIT);
        display_set_state(DISPLAY_STATE_IDLE);
    }
    else if(id == ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "传输断开");
        xEventGroupClearBits(s_chat_evt, CHAT_CONNECTED_BIT);
        s_audio_open = false;
        s_streaming = false;
        s_resume_music = false;
        s_pending_music = false;
        s_convo_active = false;
        display_set_state(DISPLAY_STATE_CONNECTING);
    }
    else if(id == ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED) {
        ESP_LOGI(TAG, "音频通道已打开");
        s_audio_open = true;
    }
    else if(id == ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED) {
        ESP_LOGW(TAG, "音频通道已被关闭（下次唤醒会重新打开）");
        s_audio_open = false;
        s_streaming = false;
        if(!s_speaking) {
            display_set_state(DISPLAY_STATE_IDLE);
        }
    }
}

/* ======================= 组件回调 ======================= */

/**
 * @brief 音频回调：只把 Opus 数据拷贝进队列（这里栈很小，不能做解码）
 */
static void on_audio(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    if(s_audio_q == NULL || data == NULL || len <= 0) {
        return;
    }
    if(len > AUDIO_PKT_MAX) {
        len = AUDIO_PKT_MAX;
    }

    static audio_pkt_t pkt;     /* 回调是串行调用的，可复用静态缓冲区 */
    pkt.len = (uint16_t)len;
    memcpy(pkt.data, data, (size_t)len);
    xQueueSend(s_audio_q, &pkt, 0);   /* 队列满就丢帧，不阻塞 */
}

/**
 * @brief 事件回调：打印组件上报的事件
 */
static void on_event(esp_xiaozhi_chat_event_t event, void *event_data, void *ctx)
{
    (void)ctx;
    switch(event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STARTED:
        s_speaking = true;
        s_streaming = false;
        s_follow_up = false;
        display_set_state(DISPLAY_STATE_SPEAKING);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STOPPED:
        s_speaking = false;
        s_streaming = false;
        s_follow_up = false;
        if(s_pending_music) {
            s_pending_music = false;
            ESP_LOGI(TAG, "小智说完了，开始播放挂起的音乐");
            if(s_pending_music_url[0] != 0) {
                music_play_url(s_pending_music_url);
            }
            else if(s_pending_music_kw[0] != 0) {
                music_play_keyword(s_pending_music_kw);
            }
            else {
                music_play_station(0);
            }
            display_set_state(DISPLAY_STATE_IDLE);
            break;
        }
        if(music_is_playing() && !s_convo_active) {
            /* 已经在音乐模式：说完不进入连续对话监听 */
            break;
        }
        if(s_resume_music) {
            /* 小智说完后先留几秒（继续聆听窗口），没人再说话才回到音乐界面续播 */
            if(s_audio_open && s_chat != 0 &&
               esp_xiaozhi_chat_send_start_listening(s_chat, 0) == ESP_OK) {
                s_streaming = true;
                s_follow_up = true;
                s_listen_start = (uint32_t)(esp_timer_get_time() / 1000);
                display_set_state(DISPLAY_STATE_LISTENING);
                ESP_LOGI(TAG, "说完后先等 6 秒（可直接接着问），无语音则回音乐界面续播");
                break;
            }
            /* 通道不可用就直接续播 */
            s_resume_music = false;
            ESP_LOGI(TAG, "对话结束，音乐原位继续");
            end_conversation_for_music();
            music_unpause();
            display_set_state(DISPLAY_STATE_IDLE);
            break;
        }
        /* 连续对话：说完后自动再听 6 秒，用户可以直接接着问，不用再喊唤醒词 */
        if(s_audio_open && s_chat != 0) {
            if(esp_xiaozhi_chat_send_start_listening(s_chat, 0) == ESP_OK) {
                s_streaming = true;
                s_follow_up = true;
                s_listen_start = (uint32_t)(esp_timer_get_time() / 1000);
                display_set_state(DISPLAY_STATE_LISTENING);
                ESP_LOGI(TAG, "继续聆听（可直接接着说话）");
                break;
            }
        }
        display_set_state(DISPLAY_STATE_IDLE);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        esp_xiaozhi_chat_text_data_t *t = (esp_xiaozhi_chat_text_data_t *)event_data;
        if(t != NULL && t->text != NULL) {
            bool is_user = (t->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER);
            ESP_LOGI(TAG, "%s: %s", is_user ? "你" : "小智", t->text);
            /* 显示到屏幕的对话框里 */
            display_add_message(is_user, t->text);
            if(is_user) {
                /* 用户说完了（已拿到 ASR 结果）→ 思考中，并停止上传麦克风 */
                s_streaming = false;
                s_follow_up = false;
                display_set_state(DISPLAY_STATE_THINKING);
            }
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI:
        if(event_data != NULL) {
            ESP_LOGI(TAG, "表情: %s", (const char *)event_data);
            /* 让屏幕上的脸跟着换表情 */
            display_set_emoji((const char *)event_data);
        }
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        esp_xiaozhi_chat_error_info_t *e = (esp_xiaozhi_chat_error_info_t *)event_data;
        if(e != NULL) {
            ESP_LOGW(TAG, "协议错误: %d (%s)", e->code, e->source ? e->source : "?");
        }
        s_streaming = false;
        s_follow_up = false;
        s_resume_music = false;
        s_convo_active = false;
        display_set_state(DISPLAY_STATE_ERROR);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        esp_xiaozhi_chat_tts_state_t *s = (esp_xiaozhi_chat_tts_state_t *)event_data;
        if(s != NULL && s->state == ESP_XIAOZHI_CHAT_TTS_STATE_SENTENCE_START && s->text != NULL) {
            ESP_LOGI(TAG, "<< %s", s->text);
        }
        break;
    }
    default:
        break;
    }
}

/* ======================= 播放任务 ======================= */

/**
 * @brief 播放任务：从队列取 Opus 包 → 解码 → 喇叭播放
 *
 * 独立任务 + 大栈（32KB），避免在组件的音频任务里做解码导致栈溢出。
 */
static void player_task(void *arg)
{
    (void)arg;
    static int16_t pcm[OPUS_FRAME_SAMPLES * 4];   /* 最大 240ms 音频 */
    static audio_pkt_t pkt;

    while(1) {
        if(xQueueReceive(s_audio_q, &pkt, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        int n = opus_decode_frame(pkt.data, pkt.len, pcm, (int)(sizeof(pcm) / sizeof(pcm[0])));
        if(n > 0) {
            /* 算一下这段 TTS 的峰值音量，让屏幕嘴巴/声波跟着动 */
            int peak = 0;
            for(int i = 0; i < n; i++) {
                int v = pcm[i];
                if(v < 0) {
                    v = -v;
                }
                if(v > peak) {
                    peak = v;
                }
            }
            int level = peak * 100 / 32768;
            if(level > 100) {
                level = 100;
            }
            display_set_audio_level((uint8_t)level);
            speaker_play_pcm(pcm, n);
        }
    }
}

/* ======================= 音频发送任务 ======================= */

/**
 * @brief 采集麦克风 → Opus 编码 → 发给服务器
 *
 * 只在音频通道已打开且不在播放 TTS 时才发送。
 * 注意：Opus 编码器很吃栈，这里给 32KB。
 */
static void mic_send_task(void *arg)
{
    (void)arg;
    static int16_t pcm[OPUS_FRAME_SAMPLES];
    static uint8_t opus[1024];

    while(1) {
        if(s_chat == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* 聆听超时兜底：一直没说话就把状态收回待机（避免卡在"聆听中"） */
        {
            uint32_t timeout_ms = s_follow_up ? 6000 : 12000;
            if(s_streaming && (uint32_t)(esp_timer_get_time() / 1000) - s_listen_start > timeout_ms) {
                s_streaming = false;
                s_follow_up = false;
                ESP_LOGI(TAG, "聆听超时（%lu ms）", (unsigned long)timeout_ms);
                if(s_chat != 0) {
                    esp_xiaozhi_chat_send_stop_listening(s_chat);
                }
                if(s_resume_music) {
                    s_resume_music = false;
                    ESP_LOGI(TAG, "没人说话，音乐原位继续并回到音乐界面");
                    end_conversation_for_music();
                    music_unpause();
                }
                else {
                    s_convo_active = false;
                }
                display_set_state(DISPLAY_STATE_IDLE);
            }
        }

        int n = mic_read_pcm(pcm, OPUS_FRAME_SAMPLES, 200);
        if(n <= 0) {
            continue;
        }

        /* 1. 始终做离线唤醒词检测（"你好小智"） */
        wake_word_feed(pcm, n);

        /* 2. 只有音频通道打开且不在播放时才上传语音 */
        if(s_audio_open && s_streaming && !s_speaking) {
            int len = opus_encode_frame(pcm, n, opus, (int)sizeof(opus));
            if(len > 0) {
                esp_xiaozhi_chat_send_audio_data(s_chat, (const char *)opus, (size_t)len);
            }
        }
    }
}

/* ======================= 对外接口 ======================= */

/**
 * @brief 打开音频通道并开始监听（按键触发时调用）
 */
void xiaozhi_app_start_listening(void)
{
    if(s_chat == 0) {
        ESP_LOGW(TAG, "chat 未初始化");
        return;
    }

    display_set_state(DISPLAY_STATE_LISTENING);

    if(!s_audio_open) {
        /* 先等传输连接就绪（MQTT/WebSocket 建立需要时间） */
        if(s_chat_evt != NULL &&
           !(xEventGroupGetBits(s_chat_evt) & CHAT_CONNECTED_BIT)) {
            ESP_LOGI(TAG, "S0: 等待传输连接...");
            EventBits_t bits = xEventGroupWaitBits(s_chat_evt, CHAT_CONNECTED_BIT, pdFALSE, pdTRUE,
                                                   pdMS_TO_TICKS(60000));
            if(!(bits & CHAT_CONNECTED_BIT)) {
                ESP_LOGW(TAG, "S0: 等待连接超时，仍然尝试");
            }
        }

        /* 打开音频通道（失败重试 3 次） */
        esp_err_t err = ESP_FAIL;
        for(int i = 0; i < 3; i++) {
            ESP_LOGI(TAG, "S1: 打开音频通道（第 %d 次）...", i + 1);
            err = esp_xiaozhi_chat_open_audio_channel(s_chat, NULL, NULL, 0);
            ESP_LOGI(TAG, "S2: 打开音频通道 = %s", esp_err_to_name(err));
            if(err == ESP_OK) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        if(err != ESP_OK) {
            return;
        }
    }

    ESP_LOGI(TAG, "S3: 发送唤醒词...");
    esp_err_t e1 = esp_xiaozhi_chat_send_wake_word(s_chat, "你好小智");
    ESP_LOGI(TAG, "S4: 唤醒词 = %s", esp_err_to_name(e1));

    ESP_LOGI(TAG, "S5: 发送开始监听...");
    esp_err_t e2 = esp_xiaozhi_chat_send_start_listening(s_chat, 0);
    ESP_LOGI(TAG, "S6: 开始监听 = %s", esp_err_to_name(e2));

    /* 通道可能已被服务器关掉：发送失败就重开一次再试 */
    if(e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGW(TAG, "发送失败，重新打开音频通道后重试");
        esp_xiaozhi_chat_close_audio_channel(s_chat);
        s_audio_open = false;
        vTaskDelay(pdMS_TO_TICKS(300));
        if(esp_xiaozhi_chat_open_audio_channel(s_chat, NULL, NULL, 0) == ESP_OK) {
            s_audio_open = true;
            e1 = esp_xiaozhi_chat_send_wake_word(s_chat, "你好小智");
            e2 = esp_xiaozhi_chat_send_start_listening(s_chat, 0);
            ESP_LOGI(TAG, "重试结果：唤醒词=%s，开始监听=%s",
                     esp_err_to_name(e1), esp_err_to_name(e2));
        }
    }

    s_audio_open = true;
    s_follow_up = false;
    s_convo_active = true;
    s_streaming = (e1 == ESP_OK && e2 == ESP_OK);
    s_listen_start = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "S7: 音频通道就绪（streaming=%d）", (int)s_streaming);
}

/**
 * @brief 按键回调（交给 button 模块调用）
 */
static void on_button_press(void)
{
    ESP_LOGI(TAG, "按键触发对话");
    xiaozhi_app_start_listening();
}

/**
 * @brief 语音唤醒回调（交给 wake_word 模块调用）：直接开始一轮对话
 */
static void on_wake_word(void)
{
    ESP_LOGI(TAG, "语音唤醒成功，开始对话");
    if(music_is_playing()) {
        ESP_LOGI(TAG, "唤醒：音乐暂停（对话结束后原位继续）");
        s_resume_music = true;
        music_pause();          /* 暂停而不是停止：音量/停止等命令才能作用在正在播的音乐上 */
    }
    xiaozhi_app_start_listening();
}

/**
 * @brief 长按按键：关机（关机动画 → 深度睡眠；按 BOOT 键可重新开机）
 */
static void on_long_press(void)
{
    ESP_LOGW(TAG, "准备关机...");
    display_play_shutdown();
    esp_sleep_enable_ext1_wakeup(1ULL << BOOT_BTN_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
    ESP_LOGW(TAG, "进入深度睡眠（按 BOOT 键开机）");
    esp_deep_sleep_start();
}

/**
 * @brief 提前创建 MCP 引擎并注册工具（启动早期调用，内存最充足）
 */
esp_err_t xiaozhi_app_init_mcp(void)
{
    if(s_mcp != NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_mcp_create(&s_mcp), TAG, "MCP 引擎创建失败");
    ESP_LOGI(TAG, "MCP 引擎已创建");
    ESP_RETURN_ON_ERROR(mcp_tools_init(s_mcp), TAG, "MCP 工具注册失败");
    return ESP_OK;
}

/**
 * @brief 启动小智对话
 */
esp_err_t xiaozhi_app_start(void)
{
    /* 1. 获取设备信息（激活码 / MQTT / WebSocket 配置） */
    esp_xiaozhi_chat_info_t info = {0};
    esp_err_t err = esp_xiaozhi_chat_get_info(&info);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "获取设备信息失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "---- 设备信息 ----");
    ESP_LOGI(TAG, "序列号    : %s", (info.has_serial_number && info.serial_number) ? info.serial_number : "(无)");
    ESP_LOGI(TAG, "固件版本  : %s", info.current_version ? info.current_version : "(无)");
    ESP_LOGI(TAG, "MQTT 配置 : %s", info.has_mqtt_config ? "有" : "无");
    ESP_LOGI(TAG, "WebSocket : %s", info.has_websocket_config ? "有" : "无");
    if(info.has_activation_code) {
        ESP_LOGW(TAG, "!!! 设备需要激活，激活码: %s", info.activation_code ? info.activation_code : "?");
        ESP_LOGW(TAG, "!!! 请到 https://xiaozhi.me/ 输入激活码绑定设备");
    }
    if(info.activation_message != NULL) {
        ESP_LOGI(TAG, "提示信息  : %s", info.activation_message);
    }

    /* 2. MCP 引擎（正常情况下已在启动早期创建好） */
    ESP_RETURN_ON_ERROR(xiaozhi_app_init_mcp(), TAG, "MCP 初始化失败");

    /* 3. 初始化聊天
     * 注意：优先用 WebSocket（比 MQTT 稳定，MQTT 偶发被服务器断开） */
    esp_xiaozhi_chat_config_t cfg = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    cfg.audio_callback = on_audio;
    cfg.event_callback = on_event;
    cfg.has_mqtt_config = false;
    cfg.has_websocket_config = info.has_websocket_config;
    cfg.mcp_engine = s_mcp;
    cfg.owns_mcp_engine = true;

    esp_xiaozhi_chat_free_info(&info);

    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_init(&cfg, &s_chat), TAG, "chat 初始化失败");

    /* 4. 事件组 + 音频包队列 + 播放任务 */
    s_chat_evt = xEventGroupCreate();
    if(s_chat_evt == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID, chat_evt_handler, NULL),
        TAG, "事件处理器注册失败");

    s_audio_q = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(audio_pkt_t));
    if(s_audio_q == NULL) {
        ESP_LOGE(TAG, "音频队列创建失败");
        return ESP_ERR_NO_MEM;
    }
    xTaskCreateWithCaps(player_task, "player", 16384, NULL, 6, NULL, MALLOC_CAP_SPIRAM);

    /* 5. 启动会话 */
    display_set_state(DISPLAY_STATE_CONNECTING);
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_start(s_chat), TAG, "chat 启动失败");
    ESP_LOGI(TAG, "对话服务已启动");

    /* 6. 离线语音唤醒（ESP-SR） */
    ESP_RETURN_ON_ERROR(wake_word_init(on_wake_word), TAG, "唤醒模块初始化失败");

    /* 7. 音频发送任务 + 按键 */
    BaseType_t mic_ret = xTaskCreateWithCaps(mic_send_task, "mic_send", 32768, NULL, 6, NULL, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "麦克风任务创建: %s", (mic_ret == pdPASS) ? "成功" : "失败");
    ESP_RETURN_ON_ERROR(button_init(on_button_press, on_long_press), TAG, "按键初始化失败");

    return ESP_OK;
}


/**
 * @brief 取服务器下发的视觉识别接口（供摄像头模块使用）
 */
esp_err_t xiaozhi_app_get_vision(char *url, size_t url_size, char *token, size_t token_size)
{
    if(s_chat == 0 || url == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_xiaozhi_chat_get_vision_explain(s_chat, url, url_size, token, token_size);
}
