/**
 * @file display.c
 * 屏幕显示模块：ST7789（SPI？ LVGL 赛博朋克界面
 *
 * 三个界面（自动切换）？ *   1. 开？Logo：霓？XIAOZHI" + 光晕脉冲 + 扫描线（？2.5 秒）
 *   2. 待机界面：霓虹网？+ 呼吸光晕 + 大眼睛表？+ WiFi 状态图？ *   3. 聊天界面：小头像 + 状？+ 对话气泡（中文）+ WiFi 图标
 *
 * 内存：LVGL 绘图缓冲？PSRAM；字库在 flash？ */

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#include "lvgl.h"

#include "board_config.h"
#include "display.h"
#include "weather.h"

static const char *TAG = "display";

#ifndef CANVAS_ANIM
#define CANVAS_ANIM 1
#endif

/* 完整中文字库（GB2312 6768 ？+ 标点，编译进 flash？*/
LV_FONT_DECLARE(font_gb2312_16);

#define LCD_HOST          SPI2_HOST
#define LCD_PIXEL_CLOCK   (80 * 1000 * 1000)
#define LVGL_TASK_STACK   8192
#define DISP_BUF_H        (LCD_W * 12 * 2)

#define MAX_MSGS          6
#define MSG_MAX_LEN       120

#define COLOR_NEON_CYAN   0x00E5FF
#define COLOR_NEON_PINK   0xFF2D95
#define COLOR_NEON_PURPLE 0x8A5CFF
#define COLOR_BG_DARK     0x000000   /* 纯黑背景：省 RAM/渲染，后期换图主题 */

/* 消息队列 */
typedef struct {
    char text[MSG_MAX_LEN];
    bool is_user;
} chat_msg_t;

static QueueHandle_t s_msg_q = NULL;

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static lv_display_t *s_disp = NULL;

/* 三个界面 */
static lv_obj_t *s_scr_boot = NULL;
static lv_obj_t *s_scr_idle = NULL;
static lv_obj_t *s_scr_chat = NULL;
static lv_obj_t *s_scr_music = NULL;      /* 音乐播放界面 */
static volatile bool s_music_playing = false;   /* 音乐播放中 */
static lv_timer_t *s_anim_timer = NULL;          /* 界面动画定时器 */
static volatile int s_music_screen_req = 0;      /* 1=切音乐界面 2=退出音乐界面（LVGL 任务执行） */
static lv_obj_t *s_cur_scr = NULL;

/* 待机界面控件 */
static lv_obj_t *s_idle_glow = NULL;
static lv_obj_t *s_idle_eye_l = NULL;
static lv_obj_t *s_idle_eye_r = NULL;
static lv_obj_t *s_idle_mouth = NULL;
static lv_obj_t *s_idle_scan = NULL;
static lv_obj_t *s_wifi_bars_idle[4] = {NULL};
static lv_obj_t *s_idle_arc = NULL;              /* 旋转光环 */
static lv_obj_t *s_idle_ring1 = NULL;            /* 脉冲？1 */
static lv_obj_t *s_idle_ring2 = NULL;            /* 脉冲？2 */
static lv_obj_t *s_idle_particles[8] = {NULL};   /* 漂浮粒子 */
static int s_particle_x[8];
static int s_particle_y[8];

/* ======================= 2.5D 旋转魔方（替换原来的透视网格地面？======================= */

#define CUBE_CX         120          /* 魔方中心 */
#define CUBE_CY         202
#define CUBE_SIZE       17           /* 半边长（小一圈，不挡表情？*/
#define CUBE_CAM        300          /* 透视相机距离 */
#define CUBE_VERTS      8
#define CUBE_EDGES      12

/* 魔方画布：ARGB8888 ？PSRAM，自己光栅化 ？能画半透明实心？*/
#define CUBE_CANVAS_W   96
#define CUBE_CANVAS_H   104
#define CUBE_CANVAS_X   72
#define CUBE_CANVAS_Y   140

/* sin×1000 查表？~90°，其余象限换算），避免依？libm 浮点 */
static const int16_t s_sin90[91] = {
    0, 17, 35, 52, 70, 87, 105, 122, 139, 156,
    174, 191, 208, 225, 242, 259, 276, 292, 309, 326,
    342, 358, 375, 391, 407, 423, 438, 454, 469, 485,
    500, 515, 530, 545, 559, 574, 588, 602, 616, 629,
    643, 656, 669, 682, 695, 707, 719, 731, 743, 755,
    766, 777, 788, 799, 809, 819, 829, 839, 848, 857,
    866, 875, 883, 891, 899, 906, 914, 921, 927, 934,
    940, 946, 951, 956, 961, 966, 970, 974, 978, 982,
    985, 988, 990, 993, 995, 996, 998, 999, 999, 1000,
    1000,
};

static uint8_t *s_cube_buf = NULL;        /* 画布像素缓冲（PSRAM，ARGB8888？*/
static lv_obj_t *s_cube_canvas = NULL;
static lv_obj_t *s_cube_halo = NULL;      /* 魔方背后的光？*/

static int s_cube_px[CUBE_VERTS];         /* 顶点画布坐标 */
static int s_cube_py[CUBE_VERTS];
static int s_cube_pz[CUBE_VERTS];         /* 深度？0 表示靠近观察者） */

/* 立方？12 条棱（顶点编？bit0=x, bit1=y, bit2=z？*/
static const uint8_t s_cube_edges[CUBE_EDGES][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7},   /* ？X */
    {0, 2}, {1, 3}, {4, 6}, {5, 7},   /* ？Y */
    {0, 4}, {1, 5}, {2, 6}, {3, 7},   /* ？Z */
};

/* 6 个面（顶点按环顺序），用于填色和？2×2 内格？*/
static const uint8_t s_cube_faces[6][4] = {
    {0, 1, 3, 2}, {4, 5, 7, 6},       /* -Z / +Z */
    {0, 1, 5, 4}, {2, 3, 7, 6},       /* -Y / +Y */
    {0, 2, 6, 4}, {1, 3, 7, 5},       /* -X / +X */
};

/* 6 个面的法线（×1000），判断？3 个面朝向观察？*/
static const int16_t s_cube_normals[6][3] = {
    {0, 0, -1000}, {0, 0, 1000},
    {0, -1000, 0}, {0, 1000, 0},
    {-1000, 0, 0}, {1000, 0, 0},
};

/* 面的填充色（Z ？/ Y ？/ X 粉） */
static const uint32_t s_cube_face_rgb[6] = {
    0x9FC8FF, 0x8CFFB8,     /* -Z 蓝星？/ +Z 绿星？*/
    0xC4A6FF, 0x7FF0FF,     /* -Y 紫星？/ +Y 青星？*/
    0xFFC27A, 0xFF9FD6,     /* -X 橙星？/ +X 粉星？*/
};

/* 棱的颜色：沿 X / ？Y / ？Z */
static const uint32_t s_cube_edge_rgb[3] = {0x00E5FF, 0xFF2D95, 0x8A5CFF};

/* 星球风格参数：每个面一？行星"（条纹数 / 相位 / 条纹幅度？*/
#define CUBE_CELL_N     16
static const uint8_t s_planet_bands[6] = {5, 4, 3, 6, 2, 3};
static const int16_t s_planet_phase[6] = {0, 120, 60, 200, 300, 90};
static const int16_t s_planet_band_amp[6] = {18, 22, 15, 12, 20, 16};


/* 关机动画 */
/* 关机动画 */
static lv_obj_t *s_scr_off = NULL;
static lv_obj_t *s_off_ring = NULL;
static volatile bool s_shutdown_req = false;

/* 聊天界面控件 */
static lv_obj_t *s_chat_area = NULL;
static lv_obj_t *s_chat_status = NULL;
static lv_obj_t *s_chat_eye_l = NULL;
static lv_obj_t *s_chat_eye_r = NULL;
static lv_obj_t *s_chat_mouth = NULL;
static lv_obj_t *s_wifi_bars_chat[4] = {NULL};

/* 开机界面控？*/
static lv_obj_t *s_boot_logo = NULL;
static lv_obj_t *s_boot_glow = NULL;
static lv_obj_t *s_boot_scan = NULL;

/* 状？*/
static volatile display_state_t s_state = DISPLAY_STATE_IDLE;
static volatile bool s_state_dirty = false;
static volatile bool s_wifi_ok = false;
static volatile bool s_wifi_dirty = false;
static volatile bool s_boot_done = false;

/* 聊天界面停留控制：说话结束后再停 5 秒才回待机界？*/
static bool s_in_chat_screen = false;
static uint32_t s_chat_hold_start = 0;

static int s_msg_count = 0;
static int s_tick = 0;      /* 旧的 120ms 时间步（？7 ？+1，保持原动画节奏？*/
static int s_frame = 0;     /* 真实帧计数（17ms/帧） */
static uint32_t s_draw_us = 0;   /* 最？600 帧里画布绘制总耗时(us)，诊断用 */
static uint32_t s_hdl_us = 0;    /* 最？600 帧里 LVGL 渲染+刷屏总耗时(us)，诊断用 */
static uint32_t s_flush_px = 0;   /* 诊断：刷屏像素数 */
static uint32_t s_screen_sw = 0;   /* 诊断：界面切换次数 */
static uint32_t s_flush_us = 0;    /* 诊断：SPI 刷屏耗时(us) */
static uint8_t *s_trans_buf = NULL;   /* 内部 DMA 中转缓冲（SPI 从这里发数据） */
static uint32_t s_anim_us = 0;     /* 诊断：动画回调自身耗时(us) */
static uint32_t s_flush_cnt = 0;  /* 诊断：刷屏次？*/

/* 拍照预览：最上层显示一帧照片，4 秒后自动隐藏 */
static lv_image_dsc_t s_photo_dsc;
static uint8_t *s_photo_buf = NULL;

/* 平滑动画时间：毫？*/
#define ANIM_MS   (s_frame * 17)
static int s_blink = 0;

/* ======================= 待机界面：时？/ 日期 / 天气 ======================= */

static lv_obj_t *s_idle_time = NULL;       /* 大号时间 13:34 */
static lv_obj_t *s_idle_date = NULL;       /* 11？？周日 */
static lv_obj_t *s_wx_cond = NULL;         /* 上海 ？*/
static lv_obj_t *s_wx_temp_lbl = NULL;     /* 26°C */
static lv_obj_t *s_wx_row = NULL;          /* 天气行（flex 自动居中？*/
static lv_obj_t *s_wx_icon_box = NULL;     /* 天气图标容器 */

/* 天气图标：用少量图元（圆/矩形/折线）现场拼出来，省 LVGL 对象内存 */
static lv_obj_t *s_wx_round[3] = {NULL};
static lv_obj_t *s_wx_bar[3] = {NULL};
static lv_obj_t *s_wx_bolt = NULL;

/* 天气图标区域左上？*/
#define WX_ICON_X   0
#define WX_ICON_Y   0

/* 天气数据（weather 任务写入，LVGL 任务读取？*/
static volatile bool s_weather_dirty = false;
static volatile int s_wx_code = -1;
static int s_wx_temp_c = 0;
static int s_wx_hum = 0;
static char s_wx_city[24] = {0};

/* ======================= 表情（LLM 情绪？======================= */

typedef enum {
    EMO_NEUTRAL = 0,
    EMO_HAPPY,
    EMO_LAUGH,
    EMO_SAD,
    EMO_ANGRY,
    EMO_SURPRISED,
    EMO_SLEEPY,
    EMO_THINKING,
    EMO_WINK,
    EMO_COOL,
    EMO_LOVE,
    EMO_CONFUSED,
    EMO_SILLY,
    EMO_COUNT
} emoji_id_t;

/* 每种表情的脸部参数（待机大脸 + 聊天小头像） */
typedef struct {
    uint8_t  eye_w, eye_h;       /* 圆眼尺寸？ = 不显示圆？*/
    bool     caret;              /* 弯眼 ^^ */
    bool     wink;               /* 只左眼弯 */
    bool     glass;              /* 墨镜 */
    bool     mouth_round;        /* 显示圆嘴 */
    uint8_t  mouth_w, mouth_h;
    bool     smile, frown;       /* 微笑 / 撇嘴 线条 */
    bool     blush, zz;          /* 腮红 / 打呼？Zz */
    uint8_t  chat_eye_h;         /* 小头像圆眼高度，0 = 不显？*/
    bool     chat_caret, chat_smile, chat_glass;
    uint8_t  chat_mouth_w, chat_mouth_h;
    uint32_t tint;               /* 0 = 用当前状态颜？*/
} expr_spec_t;

static const expr_spec_t s_expr[EMO_COUNT] = {
    /* NEUTRAL   */ {30, 30, false, false, false, true,  60, 12, false, false, false, false, 8,  false, false, false, 16, 4,  0},
    /* HAPPY     */ {0,  0,  true,  false, false, false, 0,  0,  true,  false, true,  false, 0,  true,  true,  false, 0,  0,  0},
    /* LAUGH     */ {0,  0,  true,  false, false, true,  34, 20, false, false, true,  false, 0,  true,  true,  false, 0,  0,  0},
    /* SAD       */ {30, 16, false, false, false, false, 0,  0,  false, true,  false, false, 4,  false, false, false, 16, 3,  0x6EA8FF},
    /* ANGRY     */ {34, 10, false, false, false, true,  60, 4,  false, false, false, false, 3,  false, false, false, 16, 2,  0xFF3B3B},
    /* SURPRISED */ {34, 34, false, false, false, true,  20, 20, false, false, false, false, 10, false, false, false, 10, 10, 0xFFD54A},
    /* SLEEPY    */ {30, 6,  false, false, false, true,  30, 6,  false, false, false, true,  3,  false, false, false, 12, 3,  0x8A5CFF},
    /* THINKING  */ {26, 26, false, false, false, true,  20, 8,  false, false, false, false, 6,  false, false, false, 10, 3,  0xB7A6FF},
    /* WINK      */ {30, 30, true,  true,  false, true,  50, 12, true,  false, true,  false, 8,  true,  true,  false, 0,  0,  0},
    /* COOL      */ {0,  0,  false, false, true,  true,  40, 10, false, false, false, false, 0,  false, false, true,  16, 4,  0xFFD54A},
    /* LOVE      */ {26, 26, true,  false, false, true,  40, 14, true,  false, true,  false, 0,  true,  true,  false, 0,  0,  0xFF2D95},
    /* CONFUSED  */ {30, 30, false, false, false, true,  20, 6,  false, true,  false, false, 6,  false, false, false, 14, 3,  0xB7A6FF},
    /* SILLY     */ {30, 30, false, false, false, true,  60, 18, false, false, true,  false, 8,  false, false, false, 16, 8,  0},
};

static volatile int s_emoji = EMO_NEUTRAL;    /* 当前表情（事件回调写入） */
static volatile bool s_emoji_dirty = false;   /* 需要重画表？*/
static bool s_blink_active = false;           /* 眨眼进行？*/
static uint32_t s_face_color = COLOR_NEON_CYAN;   /* 当前状态色 */

/* 聊天小头像的表情附加图元 */
static lv_obj_t *s_chat_caret[2] = {NULL};
static lv_obj_t *s_chat_smile = NULL;
static lv_obj_t *s_chat_glass = NULL;

/* 聊天界面：说话声？+ 聆听光环 */
static lv_obj_t *s_chat_wave[5] = {NULL};
static lv_obj_t *s_chat_ring = NULL;
static volatile uint8_t s_audio_level = 0;    /* 0~100，播放任务上？*/

/* 表情折线坐标（静态数组：lv_line 只保存指针） */
static lv_point_precise_t s_pt_caret_l[3] = {{46, 88}, {60, 76}, {74, 88}};
static lv_point_precise_t s_pt_caret_r[3] = {{166, 88}, {180, 76}, {194, 88}};
static lv_point_precise_t s_pt_smile[3] = {{94, 117}, {120, 127}, {146, 117}};
static lv_point_precise_t s_pt_frown[3] = {{94, 127}, {120, 117}, {146, 127}};
static lv_point_precise_t s_pt_bolt[4] = {{15, 14}, {12, 20}, {17, 20}, {13, 26}};
static lv_point_precise_t s_pt_chat_caret_l[3] = {{8, 24}, {14, 19}, {20, 24}};
static lv_point_precise_t s_pt_chat_caret_r[3] = {{40, 24}, {46, 19}, {52, 24}};
static lv_point_precise_t s_pt_chat_smile[3] = {{18, 36}, {30, 40}, {42, 36}};

/* ======================= LCD 底层 ======================= */

static esp_err_t lcd_init(void)
{
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_CLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 12 * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "SPI 总线初始化失败");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_DC,
        .cs_gpio_num = LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io),
                        TAG, "LCD IO 创建失败");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel), TAG, "ST7789 创建失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "屏幕复位失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "屏幕初始化失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "反色设置失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "开屏失败");

    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(LCD_BL, 1);
    ESP_LOGI(TAG, "ST7789 就绪");
    return ESP_OK;
}

/* ======================= LVGL 对接 ======================= */

static uint32_t tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    size_t bytes = (size_t)w * h * 2;
    if(bytes > DISP_BUF_H) {
        bytes = DISP_BUF_H;
    }
    memcpy(s_trans_buf, px_map, bytes);
    lv_draw_sw_rgb565_swap(s_trans_buf, (uint32_t)(w * h));
    s_flush_px += (uint32_t)(w * h);
    s_flush_cnt++;
    int64_t tf0 = esp_timer_get_time();
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, s_trans_buf);
    s_flush_us += (uint32_t)(esp_timer_get_time() - tf0);
    lv_display_flush_ready(disp);
}

static esp_err_t lvgl_port_init(void)
{
    lv_init();
    lv_tick_set_cb(tick_get_cb);

    s_disp = lv_display_create(LCD_W, LCD_H);
    if(s_disp == NULL) {
        return ESP_FAIL;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);

    /* 注意：必须在内部 DMA RAM 里，否则每次刷屏都要临时申请内部 bounce buffer，
     * 内部 RAM 紧张时申请失败 → 屏幕花屏/掉帧 */
    /* 和 C++ 参考工程一致：LVGL 绘制缓冲放 PSRAM（省内部 RAM），
     * 刷屏时再拷到内部 DMA 中转缓冲去发 SPI */
    uint8_t *buf1 = heap_caps_malloc(DISP_BUF_H, MALLOC_CAP_SPIRAM);
    uint8_t *buf2 = heap_caps_malloc(DISP_BUF_H, MALLOC_CAP_SPIRAM);
    s_trans_buf = heap_caps_malloc(DISP_BUF_H, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if(buf1 == NULL || buf2 == NULL || s_trans_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(s_disp, buf1, buf2, DISP_BUF_H, LV_DISPLAY_RENDER_MODE_PARTIAL);
    return ESP_OK;
}

/* ======================= 通用绘制工具 ======================= */

/**
 * @brief 创建圆角矩形/？ */
static lv_obj_t *mk_round(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/**
 * @brief 创建全屏容器
 */
static lv_obj_t *mk_screen(void)
{
    lv_obj_t *scr = lv_obj_create(lv_screen_active());
    lv_obj_set_size(scr, LCD_W, LCD_H);
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

/**
 * @brief 霓虹网格背景（赛博朋克）
 */
static void mk_neon_grid(lv_obj_t *parent)
{
    for(int x = 20; x < LCD_W; x += 40) {
        lv_obj_t *line = mk_round(parent, x, 0, 1, LCD_H, COLOR_NEON_CYAN, 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_10, 0);
    }
    for(int y = 20; y < LCD_H; y += 40) {
        lv_obj_t *line = mk_round(parent, 0, y, LCD_W, 1, COLOR_NEON_CYAN, 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_10, 0);
    }
}

/**
 * @brief 机器人脸（含眼睛、嘴？ */
static void mk_face(lv_obj_t *parent, int cx, int cy, int eye_size, lv_obj_t **eye_l,
                    lv_obj_t **eye_r, lv_obj_t **mouth)
{
    /* 头部轮廓（发光圆角矩形） */
    lv_obj_t *head = lv_obj_create(parent);
    lv_obj_set_size(head, eye_size * 4 + 16, eye_size * 2 + 22);
    lv_obj_set_pos(head, cx - (eye_size * 4 + 16) / 2, cy - eye_size - 6);
    lv_obj_set_style_radius(head, 22, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(0x0A1428), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_70, 0);
    lv_obj_set_style_border_width(head, 2, 0);
    lv_obj_set_style_border_color(head, lv_color_hex(COLOR_NEON_CYAN), 0);
    lv_obj_set_style_shadow_width(head, 20, 0);
    lv_obj_set_style_shadow_color(head, lv_color_hex(COLOR_NEON_CYAN), 0);
    lv_obj_set_style_shadow_opa(head, LV_OPA_50, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    int gap = eye_size * 2;
    *eye_l = mk_round(parent, cx - gap - eye_size / 2, cy - eye_size / 2, eye_size, eye_size,
                      COLOR_NEON_CYAN, LV_RADIUS_CIRCLE);
    *eye_r = mk_round(parent, cx + gap - eye_size / 2, cy - eye_size / 2, eye_size, eye_size,
                      COLOR_NEON_CYAN, LV_RADIUS_CIRCLE);
    *mouth = mk_round(parent, cx - eye_size, cy + eye_size / 2 + 2, eye_size * 2, eye_size / 3 + 2,
                      COLOR_NEON_CYAN, LV_RADIUS_CIRCLE);
}

/**
 * @brief WiFi 信号格：4 根竖条（越高越强？ *
 * 用实心条而不是弧形图标：更直观，而且最后创建保证在最上层，不会被挡住？ */
static void mk_wifi(lv_obj_t *parent, int x, int y, lv_obj_t *bars[4])
{
    static const int hs[4] = {6, 10, 14, 18};
    for(int i = 0; i < 4; i++) {
        bars[i] = mk_round(parent, x + i * 7, y - hs[i], 5, hs[i], 0x4A5A78, 2);
    }
}

/**
 * @brief 刷新 WiFi 信号格颜色（连上青色 / 断开粉色？ */
static void ui_update_wifi(void)
{
    uint32_t color = s_wifi_ok ? COLOR_NEON_CYAN : COLOR_NEON_PINK;
    for(int i = 0; i < 4; i++) {
        if(s_wifi_bars_idle[i] != NULL) {
            lv_obj_set_style_bg_color(s_wifi_bars_idle[i], lv_color_hex(color), 0);
        }
        if(s_wifi_bars_chat[i] != NULL) {
            lv_obj_set_style_bg_color(s_wifi_bars_chat[i], lv_color_hex(color), 0);
        }
    }
}

/* ======================= 时间 / 天气 / 表情 工具 ======================= */

static void set_visible(lv_obj_t *o, bool v)
{
    if(o == NULL) {
        return;
    }
    if(v) {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void line_style(lv_obj_t *o, uint32_t color, int width)
{
    if(o == NULL) {
        return;
    }
    lv_obj_set_style_line_width(o, width, 0);
    lv_obj_set_style_line_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(o, true, 0);
}

/**
 * @brief 待机界面时钟（每秒刷新一次；时间没同步时显示占位？ */
static void ui_update_clock(void)
{
    time_t now = 0;
    time(&now);
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    bool synced = (tm_info.tm_year + 1900) >= 2024;

    /* 每分钟打一条日志，方便核对时区/校时是否正常 */
    {
        static int last_log_min = -1;
        if(tm_info.tm_min != last_log_min) {
            last_log_min = tm_info.tm_min;
            struct tm gmt;
            gmtime_r(&now, &gmt);
            ESP_LOGI(TAG, "时钟: 本地 %02d:%02d  UTC %02d:%02d  内部RAM %uKB",
                     tm_info.tm_hour, tm_info.tm_min, gmt.tm_hour, gmt.tm_min,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        }
    }

    if(s_idle_time != NULL) {
        if(synced) {
            lv_label_set_text_fmt(s_idle_time, "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
        }
        else {
            lv_label_set_text(s_idle_time, "--:--");
        }
    }
    if(s_idle_date != NULL) {
        if(synced) {
            static const char *week[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
            int wd = tm_info.tm_wday;
            if(wd < 0 || wd > 6) {
                wd = 0;
            }
            lv_label_set_text_fmt(s_idle_date, "%d月%d日 %s",
                                  tm_info.tm_mon + 1, tm_info.tm_mday, week[wd]);
        }
        else {
            lv_label_set_text(s_idle_date, "时间未同步");
        }
    }
}

/* ---- 天气图标：复用图元拼出来（省 LVGL 对象内存？---- */

static void wx_hide_all(void)
{
    for(int i = 0; i < 3; i++) {
        set_visible(s_wx_round[i], false);
        set_visible(s_wx_bar[i], false);
    }
    set_visible(s_wx_bolt, false);
}

static void wx_set_round(int i, int x, int y, int w, int h, uint32_t color)
{
    if(s_wx_round[i] == NULL) {
        return;
    }
    lv_obj_set_size(s_wx_round[i], w, h);
    lv_obj_set_pos(s_wx_round[i], x, y);
    lv_obj_set_style_bg_color(s_wx_round[i], lv_color_hex(color), 0);
    set_visible(s_wx_round[i], true);
}

static void wx_set_bar(int i, int x, int y, int w, int h, int radius, uint32_t color)
{
    if(s_wx_bar[i] == NULL) {
        return;
    }
    lv_obj_set_size(s_wx_bar[i], w, h);
    lv_obj_set_pos(s_wx_bar[i], x, y);
    lv_obj_set_style_radius(s_wx_bar[i], radius, 0);
    lv_obj_set_style_bg_color(s_wx_bar[i], lv_color_hex(color), 0);
    set_visible(s_wx_bar[i], true);
}

/**
 * @brief ？WMO 天气代码画图标（？/ 多云 / ？/ ？/ ？/ ？/ 雷）
 */
/**
 * @brief ？WMO 天气代码画图标（28×28 图标框内，云 = 两圆 + 一条底座）
 */
static void ui_apply_weather_icon(int code)
{
    const uint32_t C_SUN = 0xFFD54A, C_CLOUD = 0xC8D8F0, C_RAIN = 0x5FD3FF;
    const uint32_t C_SNOW = 0xEAF4FF, C_FOG = 0x9FB0CC;

    wx_hide_all();
    /* 太阳光晕只在晴天/多云时用，先复位 */
    lv_obj_set_style_shadow_width(s_wx_round[0], 0, 0);

    if(code == 0) {
        /* 晴：大太？+ 光晕 */
        wx_set_round(0, 4, 4, 20, 20, C_SUN);
        lv_obj_set_style_shadow_width(s_wx_round[0], 12, 0);
        lv_obj_set_style_shadow_color(s_wx_round[0], lv_color_hex(C_SUN), 0);
        lv_obj_set_style_shadow_opa(s_wx_round[0], LV_OPA_60, 0);
    }
    else if(code == 1 || code == 2 || code == 3) {
        if(code != 3) {
            /* 晴间多云 / 多云：左上一颗太？+ 右下一朵云 */
            wx_set_round(0, 2, 2, 13, 13, C_SUN);
            lv_obj_set_style_shadow_width(s_wx_round[0], 8, 0);
            lv_obj_set_style_shadow_color(s_wx_round[0], lv_color_hex(C_SUN), 0);
            lv_obj_set_style_shadow_opa(s_wx_round[0], LV_OPA_40, 0);
            wx_set_bar(0, 8, 15, 17, 7, 3, C_CLOUD);
            wx_set_round(1, 10, 10, 10, 10, C_CLOUD);
            wx_set_round(2, 15, 9, 11, 11, C_CLOUD);
        }
        else {
            /* 阴：一大朵？*/
            wx_set_bar(0, 4, 13, 20, 7, 3, C_CLOUD);
            wx_set_round(1, 5, 8, 11, 11, C_CLOUD);
            wx_set_round(2, 12, 7, 12, 12, C_CLOUD);
        }
    }
    else if(code == 45 || code == 48) {
        /* 雾：三条横线 */
        wx_set_bar(0, 3, 8, 22, 2, 1, C_FOG);
        wx_set_bar(1, 3, 14, 22, 2, 1, C_FOG);
        wx_set_bar(2, 3, 20, 22, 2, 1, C_FOG);
    }
    else if((code >= 71 && code <= 77) || code == 85 || code == 86) {
        /* 雪：？+ 两个雪点 */
        wx_set_bar(0, 4, 10, 20, 7, 3, C_CLOUD);
        wx_set_round(1, 5, 5, 11, 11, C_CLOUD);
        wx_set_round(2, 12, 4, 12, 12, C_CLOUD);
        wx_set_round(0, 9, 20, 3, 3, C_SNOW);
        wx_set_bar(1, 16, 20, 3, 3, 1, C_SNOW);
    }
    else if(code >= 95) {
        /* 雷阵雨：？+ 闪电 */
        wx_set_bar(0, 4, 9, 20, 7, 3, C_CLOUD);
        wx_set_round(1, 5, 4, 11, 11, C_CLOUD);
        wx_set_round(2, 12, 3, 12, 12, C_CLOUD);
        set_visible(s_wx_bolt, true);
    }
    else {
        /* 各种雨：？+ 两条雨丝 */
        wx_set_bar(0, 4, 10, 20, 7, 3, C_CLOUD);
        wx_set_round(1, 5, 5, 11, 11, C_CLOUD);
        wx_set_round(2, 12, 4, 12, 12, C_CLOUD);
        wx_set_bar(1, 9, 20, 2, 5, 1, C_RAIN);
        wx_set_bar(2, 16, 20, 2, 5, 1, C_RAIN);
    }
}

/**
 * @brief 天气数据上屏
 */
static void ui_update_weather(void)
{
    if(s_wx_code < 0) {
        return;
    }
    if(s_wx_temp_lbl != NULL) {
        lv_label_set_text_fmt(s_wx_temp_lbl, "%d°C", s_wx_temp_c);
    }
    if(s_wx_cond != NULL) {
        lv_label_set_text_fmt(s_wx_cond, "%s %s", s_wx_city, weather_code_text(s_wx_code));
    }
    ui_apply_weather_icon(s_wx_code);
    ESP_LOGI(TAG, "天气上屏：%s %d°C 湿度 %d%%", s_wx_city, s_wx_temp_c, s_wx_hum);
}

/**
 * @brief 按当前情？+ 眨眼状态，重画两张？ *
 * 只在表情/眨眼变化时调用，避免每帧都改样式？ */
static void emoji_apply_now(void)
{
    int id = s_emoji;
    if(id < 0 || id >= EMO_COUNT) {
        id = EMO_NEUTRAL;
    }
    const expr_spec_t *e = &s_expr[id];
    uint32_t color = (e->tint != 0) ? e->tint : s_face_color;
    lv_color_t lc = lv_color_hex(color);

    /* ---------------- 聊天小头像：眼中？(14,26) / (46,26) ---------------- */
    int ceh = e->chat_eye_h;
    if(s_blink_active && ceh > 4) {
        ceh = 3;
    }

    if(e->chat_glass) {
        set_visible(s_chat_eye_l, false);
        set_visible(s_chat_eye_r, false);
        set_visible(s_chat_caret[0], false);
        set_visible(s_chat_caret[1], false);
        lv_obj_set_style_border_color(s_chat_glass, lc, 0);
        set_visible(s_chat_glass, true);
    }
    else if(e->chat_caret) {
        set_visible(s_chat_eye_l, false);
        set_visible(s_chat_eye_r, false);
        line_style(s_chat_caret[0], color, 2);
        line_style(s_chat_caret[1], color, 2);
        set_visible(s_chat_caret[0], true);
        set_visible(s_chat_caret[1], true);
        set_visible(s_chat_glass, false);
    }
    else if(ceh > 0) {
        set_visible(s_chat_eye_l, true);
        set_visible(s_chat_eye_r, true);
        lv_obj_set_size(s_chat_eye_l, 8, ceh);
        lv_obj_set_size(s_chat_eye_r, 8, ceh);
        lv_obj_set_pos(s_chat_eye_l, 10, 26 - ceh / 2);
        lv_obj_set_pos(s_chat_eye_r, 42, 26 - ceh / 2);
        lv_obj_set_style_bg_color(s_chat_eye_l, lc, 0);
        lv_obj_set_style_bg_color(s_chat_eye_r, lc, 0);
        set_visible(s_chat_caret[0], false);
        set_visible(s_chat_caret[1], false);
        set_visible(s_chat_glass, false);
    }
    else {
        set_visible(s_chat_eye_l, false);
        set_visible(s_chat_eye_r, false);
        set_visible(s_chat_caret[0], false);
        set_visible(s_chat_caret[1], false);
        set_visible(s_chat_glass, false);
    }
    line_style(s_chat_smile, color, 2);
    set_visible(s_chat_smile, e->chat_smile);

    /* 小嘴 */
    if(e->chat_mouth_w > 0 && e->chat_mouth_h > 0) {
        lv_obj_set_size(s_chat_mouth, e->chat_mouth_w, e->chat_mouth_h);
        lv_obj_set_pos(s_chat_mouth, 30 - e->chat_mouth_w / 2, 34 - e->chat_mouth_h / 2);
        lv_obj_set_style_bg_color(s_chat_mouth, lc, 0);
        set_visible(s_chat_mouth, true);
    }
    else {
        set_visible(s_chat_mouth, false);
    }
}

/* ---- 2.5D 旋转魔方工具 ---- */

static inline int sin1000(int deg)
{
    deg %= 360;
    if(deg < 0) {
        deg += 360;
    }
    if(deg <= 90) {
        return s_sin90[deg];
    }
    if(deg <= 180) {
        return s_sin90[180 - deg];
    }
    if(deg <= 270) {
        return -s_sin90[deg - 180];
    }
    return -s_sin90[360 - deg];
}

static inline int cos1000(int deg)
{
    return sin1000(deg + 90);
}

/* ---------------- 画布像素操作（ARGB8888：A<<24 | R<<16 | G<<8 | B？---------------- */

static inline void cv_px(int x, int y, uint32_t argb)
{
    if(x < 0 || x >= CUBE_CANVAS_W || y < 0 || y >= CUBE_CANVAS_H) {
        return;
    }
    ((uint32_t *)s_cube_buf)[y * CUBE_CANVAS_W + x] = argb;
}

static void cv_line(int x0, int y0, int x1, int y1, uint32_t argb)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while(1) {
        cv_px(x0, y0, argb);
        if(x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = err * 2;
        if(e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if(e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* 凸四边形扫描线填充（立方体的面都是凸的） */
static void cv_fill_quad(const int *xs, const int *ys, uint32_t argb)
{
    int ymin = ys[0], ymax = ys[0];
    for(int i = 1; i < 4; i++) {
        if(ys[i] < ymin) {
            ymin = ys[i];
        }
        if(ys[i] > ymax) {
            ymax = ys[i];
        }
    }
    if(ymin < 0) {
        ymin = 0;
    }
    if(ymax > CUBE_CANVAS_H - 1) {
        ymax = CUBE_CANVAS_H - 1;
    }

    for(int y = ymin; y <= ymax; y++) {
        int x_min = 32767, x_max = -32768;
        for(int i = 0; i < 4; i++) {
            int j = (i + 1) & 3;
            int ya = ys[i], yb = ys[j];
            if(ya == yb) {
                continue;
            }
            if((y < ya && y < yb) || (y > ya && y > yb)) {
                continue;
            }
            int x = xs[i] + (xs[j] - xs[i]) * (y - ya) / (yb - ya);
            if(x < x_min) {
                x_min = x;
            }
            if(x > x_max) {
                x_max = x;
            }
        }
        for(int x = x_min; x <= x_max; x++) {
            cv_px(x, y, argb);
        }
    }
}

static void cv_dot(int cx, int cy, int r, uint32_t argb)
{
    for(int dy = -r; dy <= r; dy++) {
        for(int dx = -r; dx <= r; dx++) {
            if(dx * dx + dy * dy <= r * r) {
                cv_px(cx + dx, cy + dy, argb);
            }
        }
    }
}

/* 颜色亮度缩放（pct: 100 = 原色？*/
static uint32_t rgb_scale(uint32_t rgb, int pct)
{
    int r = (int)((rgb >> 16) & 0xFF) * pct / 100;
    int g = (int)((rgb >> 8) & 0xFF) * pct / 100;
    int b = (int)(rgb & 0xFF) * pct / 100;
    if(r > 255) {
        r = 255;
    }
    if(g > 255) {
        g = 255;
    }
    if(b > 255) {
        b = 255;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* 四边形内？u,v？~1000）双线性插值取点，用于把面切成 3×3 贴纸 */
static void quad_pt(int ax, int ay, int bx, int by, int cx, int cy, int dx, int dy,
                    int u, int v, int *ox, int *oy)
{
    int w00 = (1000 - u) * (1000 - v) / 1000;
    int w10 = u * (1000 - v) / 1000;
    int w11 = u * v / 1000;
    int w01 = (1000 - u) * v / 1000;
    *ox = (ax * w00 + bx * w10 + cx * w11 + dx * w01) / 1000;
    *oy = (ay * w00 + by * w10 + cy * w11 + dy * w01) / 1000;
}

/**
 * @brief 创建 2.5D 旋转魔方（ARGB 画布 + 背后光晕？ */
static void cube_create(lv_obj_t *parent)
{
    /* 背后光晕（让魔方？发光悬浮"的感觉） */
    s_cube_halo = mk_round(parent, CUBE_CX - 52, CUBE_CY - 42, 104, 84,
                           COLOR_NEON_PURPLE, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_cube_halo, LV_OPA_10, 0);
    lv_obj_set_style_shadow_width(s_cube_halo, 30, 0);
    lv_obj_set_style_shadow_color(s_cube_halo, lv_color_hex(COLOR_NEON_CYAN), 0);
    lv_obj_set_style_shadow_opa(s_cube_halo, LV_OPA_30, 0);

    /* 画布缓冲？PSRAM */
    s_cube_buf = heap_caps_malloc((size_t)CUBE_CANVAS_W * CUBE_CANVAS_H * 4, MALLOC_CAP_SPIRAM);
    if(s_cube_buf == NULL) {
        ESP_LOGE(TAG, "魔方画布内存分配失败");
        return;
    }
    memset(s_cube_buf, 0, (size_t)CUBE_CANVAS_W * CUBE_CANVAS_H * 4);

    s_cube_canvas = lv_canvas_create(parent);
    if(s_cube_canvas == NULL) {
        ESP_LOGE(TAG, "魔方画布创建失败");
        return;
    }
    lv_canvas_set_buffer(s_cube_canvas, s_cube_buf, CUBE_CANVAS_W, CUBE_CANVAS_H,
                         LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(s_cube_canvas, CUBE_CANVAS_X, CUBE_CANVAS_Y);
    lv_obj_set_style_pad_all(s_cube_canvas, 0, 0);
    lv_obj_set_style_border_width(s_cube_canvas, 0, 0);
    lv_obj_set_style_bg_opa(s_cube_canvas, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_cube_canvas, LV_OBJ_FLAG_SCROLLABLE);
}

/**
 * @brief 每帧重绘魔方？D 顶点 ？透视投影 ？光栅？3 个可见面/？内格？顶点
 */
/**
 * @brief 每帧重绘魔方？D 顶点 ？透视投影 ？暗底 + 3×3 霓虹贴纸 + ？+ 顶点光点
 */
static void cube_update(void)
{
    if(s_cube_canvas == NULL || s_cube_buf == NULL) {
        return;
    }
    memset(s_cube_buf, 0, (size_t)CUBE_CANVAS_W * CUBE_CANVAS_H * 4);

    int tms = ANIM_MS;
    int yaw = (tms * 4 / 120) % 360;                       /* ？Y 轴旋转（？10 秒一圈） */
    int pitch = 28 + sin1000((tms * 3 / 120) % 360) / 80;  /* ？X 轴摆？±12° */
    int roll = -24 + sin1000((tms * 2 / 120) % 360) / 200; /* 屏幕平面内歪？*/
    int cy = (CUBE_CY - CUBE_CANVAS_Y) + sin1000((tms * 4 / 120) % 360) / 700;
    int cx = CUBE_CX - CUBE_CANVAS_X;
    int cosY = cos1000(yaw), sinY = sin1000(yaw);
    int cosP = cos1000(pitch), sinP = sin1000(pitch);
    int cosR = cos1000(roll), sinR = sin1000(roll);
    int pulse = 100 + sin1000((tms * 6 / 120) % 360) / 30; /* 整体亮度呼吸 */

    for(int i = 0; i < CUBE_VERTS; i++) {
        int x = (i & 1) ? CUBE_SIZE : -CUBE_SIZE;
        int y = (i & 2) ? CUBE_SIZE : -CUBE_SIZE;
        int z = (i & 4) ? CUBE_SIZE : -CUBE_SIZE;

        /* ？Y ？*/
        int x1 = (x * cosY - z * sinY) / 1000;
        int z1 = (x * sinY + z * cosY) / 1000;
        /* ？X ？*/
        int y1 = (y * cosP - z1 * sinP) / 1000;
        int z2 = (y * sinP + z1 * cosP) / 1000;
        /* 透视投影 */
        /* 再绕 Z 轴歪一下：魔方看起来是"歪着"的，不是正正方方 */
        int x2 = (x1 * cosR - y1 * sinR) / 1000;
        int y2 = (x1 * sinR + y1 * cosR) / 1000;
        int f = CUBE_CAM * 256 / (CUBE_CAM + z2);
        s_cube_px[i] = cx + (x2 * f) / 256;
        s_cube_py[i] = cy + (y2 * f) / 256;
        s_cube_pz[i] = z2;
    }

    /* 1) 三个可见面：星球质感（球面明？+ 高光 + 行星条纹 + 星点），不是纯色填充 */
    for(int f = 0; f < 6; f++) {
        int nx = s_cube_normals[f][0], ny = s_cube_normals[f][1], nz = s_cube_normals[f][2];
        int nx1 = (nx * cosY - nz * sinY) / 1000;
        int nz1 = (nx * sinY + nz * cosY) / 1000;
        int nz2 = (ny * sinP + nz1 * cosP) / 1000;
        if(nz2 >= 0) {
            continue;                                      /* 背面，跳？*/
        }

        int a = s_cube_faces[f][0], b = s_cube_faces[f][1];
        int c = s_cube_faces[f][2], d = s_cube_faces[f][3];
        int ax = s_cube_px[a], ay = s_cube_py[a];
        int bx = s_cube_px[b], by = s_cube_py[b];
        int qcx = s_cube_px[c], qcy = s_cube_py[c];
        int dx = s_cube_px[d], dy = s_cube_py[d];

        int face_lv = 55 + (-nz2) * 60 / 1000;        /* 面朝？？整体 55%~115% 明暗 */

        for(int iu = 0; iu < CUBE_CELL_N; iu++) {
            for(int iv = 0; iv < CUBE_CELL_N; iv++) {
                int u0 = iu * 1000 / CUBE_CELL_N;
                int v0 = iv * 1000 / CUBE_CELL_N;
                int u1 = (iu + 1) * 1000 / CUBE_CELL_N + 8;   /* 稍微重叠，消除格？*/
                int v1 = (iv + 1) * 1000 / CUBE_CELL_N + 8;
                if(u1 > 1000) {
                    u1 = 1000;
                }
                if(v1 > 1000) {
                    v1 = 1000;
                }

                int sx[4], sy[4];
                quad_pt(ax, ay, bx, by, qcx, qcy, dx, dy, u0, v0, &sx[0], &sy[0]);
                quad_pt(ax, ay, bx, by, qcx, qcy, dx, dy, u1, v0, &sx[1], &sy[1]);
                quad_pt(ax, ay, bx, by, qcx, qcy, dx, dy, u1, v1, &sx[2], &sy[2]);
                quad_pt(ax, ay, bx, by, qcx, qcy, dx, dy, u0, v1, &sx[3], &sy[3]);

                int u = (u0 + u1) / 2;
                int v = (v0 + v1) / 2;

                /* 球面明暗：中间亮、边缘暗（limb darkening？*/
                int du = (u - 500) / 5;
                int dv = (v - 500) / 5;
                int limb = 100 - (du * du + dv * dv) * 45 / 10000;
                if(limb < 30) {
                    limb = 30;
                }

                /* 高光：左上偏亮，像被恒星照亮 */
                int hu = (u - 320) / 7;
                int hv = (v - 270) / 7;
                int hd = hu * hu + hv * hv;
                int spec = (hd < 1800) ? (1800 - hd) / 22 : 0;

                /* 行星条纹：随时间缓慢流动 */
                int band = sin1000((v * s_planet_bands[f] * 36 / 100 + s_planet_phase[f] + s_tick * 2) % 360)
                           * s_planet_band_amp[f] / 1000;

                int shade = (limb + spec + band) * face_lv / 100 * pulse / 100;
                if(shade < 18) {
                    shade = 18;
                }
                if(shade > 235) {
                    shade = 235;
                }

                cv_fill_quad(sx, sy, ((uint32_t)242u << 24) |
                             rgb_scale(s_cube_face_rgb[f], shade));

                /* 星点：确定性伪随机，少量白点，像星球表面的星光 */
                if(((iu * 7 + iv * 13 + f * 29) % 47) == 0) {
                    int px = (sx[0] + sx[1] + sx[2] + sx[3]) / 4;
                    int py = (sy[0] + sy[1] + sy[2] + sy[3]) / 4;
                    cv_dot(px, py, 1, 0xF0EAF6FF);
                }
            }
        }
    }

    /* 2) 12 条棱：前面的亮、后面的？*/
    for(int e = 0; e < CUBE_EDGES; e++) {
        int a = s_cube_edges[e][0], b = s_cube_edges[e][1];
        int front = (s_cube_pz[a] + s_cube_pz[b]) < 0;
        uint32_t rgb = s_cube_edge_rgb[e / 4];
        uint32_t argb = front ? (((uint32_t)255u << 24) | rgb) : (((uint32_t)110u << 24) | rgb);
        cv_line(s_cube_px[a], s_cube_py[a], s_cube_px[b], s_cube_py[b], argb);
        if(front) {
            /* 前面的棱加粗一像素，更？*/
            cv_line(s_cube_px[a] + 1, s_cube_py[a], s_cube_px[b] + 1, s_cube_py[b], argb);
        }
    }

    /* 3) 8 个顶点光点（近大远小、近亮远暗） */
    for(int i = 0; i < CUBE_VERTS; i++) {
        int front = s_cube_pz[i] < 0;
        uint32_t argb = front ? 0xFF7BF5FFu : 0x7066C8FFu;
        cv_dot(s_cube_px[i], s_cube_py[i], front ? 2 : 1, argb);
    }

    lv_obj_invalidate(s_cube_canvas);
}

/* ======================= 星系背景（静？ARGB 画布，渲染一次零每帧开销？======================= */

#define GALAXY_W         LCD_W
#define GALAXY_H         LCD_H
#define GALAXY_NEBULAS   3
#define GALAXY_STARS     190
#define GALAXY_TWINKLE   8

static uint32_t s_galaxy_rng = 1;

static uint32_t galaxy_rand(void)
{
    s_galaxy_rng = s_galaxy_rng * 1664525u + 1013904223u;
    return s_galaxy_rng >> 8;
}

/* RGB565 打包 */
static inline uint16_t galaxy_pack565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* 往画布像素上叠加亮度（0..255？*/
static inline void galaxy_add(uint16_t *buf, int x, int y, int add)
{
    if(x < 0 || x >= GALAXY_W || y < 0 || y >= GALAXY_H || add <= 0) {
        return;
    }
    uint16_t px = buf[y * GALAXY_W + x];
    int r = ((px >> 11) & 0x1F) * 255 / 31;
    int g = ((px >> 5) & 0x3F) * 255 / 63;
    int b = (px & 0x1F) * 255 / 31;
    r += (255 - r) * add / 255;
    g += (255 - g) * add / 255;
    b += (255 - b) * add / 255;
    buf[y * GALAXY_W + x] = galaxy_pack565(r, g, b);
}

/**
 * @brief 渲染一张星系背景（深空渐变 + 星云 + 银河带星？+ 亮星十字光晕？ */
static void galaxy_render(uint16_t *buf, uint32_t seed)
{
    /* 1) 深空渐变 */
    for(int y = 0; y < GALAXY_H; y++) {
        int t = y * 255 / (GALAXY_H - 1);
        int r = 3 + t * 26 / 255;
        int g = 5 + t * 12 / 255;
        int b = 14 + t * 52 / 255;
        uint16_t base = galaxy_pack565(r, g, b);
        for(int x = 0; x < GALAXY_W; x++) {
            buf[y * GALAXY_W + x] = base;
        }
    }

    /* 2) 星云：柔和彩光（平方距离衰减，不？sqrt？*/
    static const struct {
        int cx, cy, radius;
        uint32_t rgb;
        int strength;
    } blobs[GALAXY_NEBULAS] = {
        {44, 66, 94, 0x7A3CFF, 95},
        {198, 44, 82, 0xFF2D95, 72},
        {126, 184, 108, 0x1E6CFF, 66},
    };
    for(int k = 0; k < GALAXY_NEBULAS; k++) {
        int cx = blobs[k].cx, cy = blobs[k].cy, rr = blobs[k].radius;
        int rr2 = rr * rr;
        int nr = (int)((blobs[k].rgb >> 16) & 0xFF);
        int ng = (int)((blobs[k].rgb >> 8) & 0xFF);
        int nb = (int)(blobs[k].rgb & 0xFF);
        int y0 = cy - rr, y1 = cy + rr, x0 = cx - rr, x1 = cx + rr;
        if(y0 < 0) {
            y0 = 0;
        }
        if(x0 < 0) {
            x0 = 0;
        }
        if(y1 > GALAXY_H - 1) {
            y1 = GALAXY_H - 1;
        }
        if(x1 > GALAXY_W - 1) {
            x1 = GALAXY_W - 1;
        }
        for(int y = y0; y <= y1; y++) {
            int dy = y - cy;
            for(int x = x0; x <= x1; x++) {
                int dx = x - cx;
                int d2 = dx * dx + dy * dy;
                if(d2 >= rr2) {
                    continue;
                }
                int add = (rr2 - d2) * blobs[k].strength / rr2;
                uint16_t px = buf[y * GALAXY_W + x];
                int r = ((px >> 11) & 0x1F) * 255 / 31 + nr * add / 255;
                int g = ((px >> 5) & 0x3F) * 255 / 63 + ng * add / 255;
                int b = (px & 0x1F) * 255 / 31 + nb * add / 255;
                if(r > 255) {
                    r = 255;
                }
                if(g > 255) {
                    g = 255;
                }
                if(b > 255) {
                    b = 255;
                }
                buf[y * GALAXY_W + x] = galaxy_pack565(r, g, b);
            }
        }
    }

    /* 3) 星场？/3 ？银河？斜向分布 */
    s_galaxy_rng = seed;
    for(int i = 0; i < GALAXY_STARS; i++) {
        int x = (int)(galaxy_rand() % GALAXY_W);
        int y;
        if((i % 3) == 0) {
            y = (x * 13 / 10 - 40 + (int)(galaxy_rand() % 110)) % GALAXY_H;
            if(y < 0) {
                y += GALAXY_H;
            }
        }
        else {
            y = (int)(galaxy_rand() % GALAXY_H);
        }
        galaxy_add(buf, x, y, 70 + (int)(galaxy_rand() % 180));
    }

    /* 4) 3 颗亮星：核心 + 十字光晕 */
    for(int i = 0; i < 3; i++) {
        int x = 26 + (int)(galaxy_rand() % (GALAXY_W - 52));
        int y = 22 + (int)(galaxy_rand() % (GALAXY_H - 44));
        galaxy_add(buf, x, y, 235);
        galaxy_add(buf, x - 1, y, 150);
        galaxy_add(buf, x + 1, y, 150);
        galaxy_add(buf, x, y - 1, 150);
        galaxy_add(buf, x, y + 1, 150);
        for(int d = 2; d <= 5; d++) {
            int a = 120 - d * 20;
            galaxy_add(buf, x - d, y, a);
            galaxy_add(buf, x + d, y, a);
            galaxy_add(buf, x, y - d, a);
            galaxy_add(buf, x, y + d, a);
        }
    }
}

/**
 * @brief 创建一个全屏星系背景画布（放在每个界面的最底层？ */
static lv_obj_t *galaxy_bg_create(lv_obj_t *parent)
{
    uint16_t *buf = heap_caps_malloc((size_t)GALAXY_W * GALAXY_H * 2, MALLOC_CAP_SPIRAM);
    if(buf == NULL) {
        ESP_LOGW(TAG, "星系背景内存不足");
        return NULL;
    }
    static uint32_t s_seed = 0x51A7F00Du;
    s_seed += 0x9E3779B9u;
    galaxy_render(buf, s_seed);

    lv_obj_t *cv = lv_canvas_create(parent);
    if(cv == NULL) {
        return NULL;
    }
    lv_canvas_set_buffer(cv, buf, GALAXY_W, GALAXY_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(cv, 0, 0);
    lv_obj_set_style_pad_all(cv, 0, 0);
    lv_obj_set_style_border_width(cv, 0, 0);
    lv_obj_set_style_bg_opa(cv, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_SCROLLABLE);
    return cv;
}

/* 待机界面的闪烁星？+ 流星 */
static lv_obj_t *s_twinkle[GALAXY_TWINKLE] = {NULL};
static lv_obj_t *s_shoot = NULL;
static lv_point_precise_t s_shoot_pts[2];

/* ======================= 全息机器人脸（ARGB 画布逐帧手绘？======================= */

#define FACE_CANVAS_W   184
#define FACE_CANVAS_H   144
#define FACE_CANVAS_X   28
#define FACE_CANVAS_Y   18
#define FACE_CX         92
#define FACE_CY         72
#define FACE_R          48
#define FACE_EYE_DX     19
#define FACE_EYE_DY     (-8)
#define FACE_MOUTH_DY   24

#define FC_ARGB(a, rgb) (((uint32_t)(a) << 24) | (uint32_t)(rgb))

static uint8_t *s_face_buf = NULL;
static lv_obj_t *s_face_canvas = NULL;

/* ---- 画布基础操作 ---- */

static inline void fc_px(int x, int y, uint32_t argb)
{
    if(x < 0 || x >= FACE_CANVAS_W || y < 0 || y >= FACE_CANVAS_H) {
        return;
    }
    ((uint32_t *)s_face_buf)[y * FACE_CANVAS_W + x] = argb;
}

static void fc_line(int x0, int y0, int x1, int y1, uint32_t argb)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    while(1) {
        fc_px(x0, y0, argb);
        if(x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = err * 2;
        if(e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if(e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static int fc_isqrt(int v)
{
    int r = 0;
    while((r + 1) * (r + 1) <= v) {
        r++;
    }
    return r;
}

/* 实心？*/
static void fc_disc(int cx, int cy, int r, uint32_t argb)
{
    if(r <= 0) {
        return;
    }
    for(int dy = -r; dy <= r; dy++) {
        int w = fc_isqrt(r * r - dy * dy);
        for(int x = cx - w; x <= cx + w; x++) {
            fc_px(x, cy + dy, argb);
        }
    }
}

/* 圆环 */
static void fc_ring(int cx, int cy, int r, int th, uint32_t argb)
{
    int ri = r - th;
    if(ri < 0) {
        ri = 0;
    }
    for(int dy = -r; dy <= r; dy++) {
        int wo = fc_isqrt(r * r - dy * dy);
        int wi = (dy >= -ri && dy <= ri) ? fc_isqrt(ri * ri - dy * dy) : -1;
        if(wi < 0) {
            for(int x = cx - wo; x <= cx + wo; x++) {
                fc_px(x, cy + dy, argb);
            }
        }
        else {
            for(int x = cx - wo; x <= cx - wi; x++) {
                fc_px(x, cy + dy, argb);
            }
            for(int x = cx + wi; x <= cx + wo; x++) {
                fc_px(x, cy + dy, argb);
            }
        }
    }
}

/* 圆弧（用点连成） */
static void fc_arc(int cx, int cy, int r, int a0, int a1, int th, uint32_t argb)
{
    for(int a = a0; a <= a1; a += 2) {
        int x = cx + r * cos1000(a) / 1000;
        int y = cy + r * sin1000(a) / 1000;
        fc_disc(x, y, th, argb);
    }
}

static void fc_bar(int x, int y, int w, int h, uint32_t argb)
{
    for(int yy = y; yy < y + h; yy++) {
        for(int xx = x; xx < x + w; xx++) {
            fc_px(xx, yy, argb);
        }
    }
}

static uint32_t fc_lighten(uint32_t rgb, int pct)
{
    int r = (int)((rgb >> 16) & 0xFF);
    int g = (int)((rgb >> 8) & 0xFF);
    int b = (int)(rgb & 0xFF);
    r += (255 - r) * pct / 100;
    g += (255 - g) * pct / 100;
    b += (255 - b) * pct / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ---- 眼睛 / ？---- */

static void fc_eye(int ex, int ey, bool left, int id, uint32_t color)
{
    uint32_t bright = fc_lighten(color, 60);

    if(s_blink_active) {
        fc_bar(ex - 7, ey - 1, 14, 3, FC_ARGB(210, color));
        return;
    }

    switch(id) {
    case EMO_HAPPY:
    case EMO_LAUGH:
    case EMO_SILLY:
        fc_arc(ex, ey + 3, 8, 200, 340, 1, FC_ARGB(240, bright));
        break;
    case EMO_LOVE:
        fc_disc(ex, ey, 8, FC_ARGB(90, 0xFF2D95));
        fc_ring(ex, ey, 8, 2, FC_ARGB(240, 0xFF2D95));
        fc_disc(ex, ey, 3, FC_ARGB(255, 0xFFD1EC));
        break;
    case EMO_SAD:
    case EMO_THINKING:
        fc_arc(ex, ey - 3, 8, 20, 160, 1, FC_ARGB(230, bright));
        break;
    case EMO_SLEEPY:
        fc_bar(ex - 8, ey, 16, 3, FC_ARGB(210, bright));
        break;
    case EMO_ANGRY:
        fc_line(ex - 8, ey - 5, ex + 8, ey + 4, FC_ARGB(240, 0xFF3B3B));
        fc_line(ex - 8, ey + 4, ex + 8, ey - 5, FC_ARGB(240, color));
        break;
    case EMO_COOL:
        /* 墨镜统一？face_draw 里画 */
        break;
    case EMO_WINK:
        if(left) {
            fc_arc(ex, ey + 3, 8, 200, 340, 1, FC_ARGB(240, bright));
        }
        else {
            fc_disc(ex, ey, 6, FC_ARGB(90, color));
            fc_ring(ex, ey, 7, 2, FC_ARGB(240, color));
            fc_disc(ex, ey, 3, FC_ARGB(255, bright));
        }
        break;
    case EMO_CONFUSED:
        fc_ring(ex, ey, 7, 2, FC_ARGB(200, color));
        fc_disc(ex + (left ? 2 : -2), ey, 3, FC_ARGB(220, bright));
        break;
    case EMO_SURPRISED:
        fc_disc(ex, ey, 9, FC_ARGB(80, color));
        fc_ring(ex, ey, 9, 2, FC_ARGB(240, color));
        fc_disc(ex, ey, 5, FC_ARGB(230, bright));
        fc_disc(ex - 2, ey - 2, 2, FC_ARGB(240, 0xFFFFFF));
        break;
    default:   /* neutral：发光机械眼 */
        fc_disc(ex, ey, 9, FC_ARGB(70, color));
        fc_ring(ex, ey, 8, 2, FC_ARGB(240, color));
        fc_disc(ex, ey, 4, FC_ARGB(255, bright));
        fc_disc(ex - 2, ey - 2, 2, FC_ARGB(220, 0xFFFFFF));
        /* 虹膜上扫描的小亮？*/
        {
            int a = (ANIM_MS * 22 / 120) + (left ? 0 : 180);
            int hx = ex + cos1000(a) * 6 / 1000;
            int hy = ey + sin1000(a) * 6 / 1000;
            fc_disc(hx, hy, 1, FC_ARGB(230, 0xFFFFFF));
        }
        break;
    }
}

static void fc_mouth(int id, uint32_t color)
{
    int mx = FACE_CX;
    int my = FACE_CY + FACE_MOUTH_DY;
    uint32_t bright = fc_lighten(color, 55);

    /* 说话中：音量驱动的均衡器 */
    if(s_state == DISPLAY_STATE_SPEAKING) {
        int lvl = s_audio_level;
        for(int i = 0; i < 7; i++) {
            int mul = (i < 4) ? (i + 2) : (9 - i);
            int h = 3 + lvl * mul / 55 + (int)((s_tick + i * 3) % 3);
            if(h > 16) {
                h = 16;
            }
            int x = mx - 21 + i * 6;
            fc_bar(x, my - h / 2, 4, h, FC_ARGB(200, color));
            fc_bar(x, my - h / 2, 4, 1, FC_ARGB(255, bright));
        }
        return;
    }

    switch(id) {
    case EMO_HAPPY:
    case EMO_LAUGH:
    case EMO_LOVE:
        fc_arc(mx, my - 4, 11, 20, 160, 1, FC_ARGB(235, bright));
        break;
    case EMO_SAD:
        fc_arc(mx, my + 5, 11, 200, 340, 1, FC_ARGB(220, bright));
        break;
    case EMO_ANGRY:
        fc_bar(mx - 12, my - 1, 24, 3, FC_ARGB(235, 0xFF3B3B));
        break;
    case EMO_SURPRISED:
        fc_disc(mx, my, 6, FC_ARGB(80, color));
        fc_ring(mx, my, 6, 2, FC_ARGB(235, color));
        break;
    case EMO_SLEEPY:
        fc_bar(mx - 8, my, 16, 3, FC_ARGB(180, color));
        break;
    default:
        fc_bar(mx - 9, my, 18, 3, FC_ARGB(220, color));
        fc_bar(mx - 9, my, 18, 1, FC_ARGB(255, bright));
        break;
    }
}

/**
 * @brief 创建全息机器人脸画布
 */
static void face_canvas_create(lv_obj_t *parent)
{
    s_face_buf = heap_caps_malloc((size_t)FACE_CANVAS_W * FACE_CANVAS_H * 4, MALLOC_CAP_SPIRAM);
    if(s_face_buf == NULL) {
        ESP_LOGE(TAG, "机器人脸画布内存分配失败");
        return;
    }
    memset(s_face_buf, 0, (size_t)FACE_CANVAS_W * FACE_CANVAS_H * 4);

    s_face_canvas = lv_canvas_create(parent);
    if(s_face_canvas == NULL) {
        return;
    }
    lv_canvas_set_buffer(s_face_canvas, s_face_buf, FACE_CANVAS_W, FACE_CANVAS_H,
                         LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(s_face_canvas, FACE_CANVAS_X, FACE_CANVAS_Y);
    lv_obj_set_style_pad_all(s_face_canvas, 0, 0);
    lv_obj_set_style_border_width(s_face_canvas, 0, 0);
    lv_obj_set_style_bg_opa(s_face_canvas, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_face_canvas, LV_OBJ_FLAG_SCROLLABLE);
}

/**
 * @brief 逐帧重绘全息机器人脸（发光轮？+ 旋转？+ 扫描？+ 故障？+ 音量嘴）
 */

/* 2.5D 椭圆轨道 */
static void fc_orbit_arc(int cx, int cy, int rx, int ry, int a0, int a1, int alpha, uint32_t rgb)
{
    int px = 0, py = 0, first = 1;
    for(int a = a0; a <= a1; a += 2) {
        int x = cx + rx * cos1000(a) / 1000;
        int y = cy + ry * sin1000(a) / 1000;
        if(!first) {
            fc_line(px, py, x, y, FC_ARGB(alpha, rgb));
        }
        px = x;
        py = y;
        first = 0;
    }
}

/* 轨道上的小卫星（前后大小/亮度不同？*/
static void fc_satellite(int x, int y, int front, uint32_t color)
{
    int r = front ? 3 : 2;
    uint32_t body = fc_lighten(color, front ? 55 : 20);
    fc_disc(x, y, r + 4, FC_ARGB(front ? 55 : 25, color));
    fc_disc(x, y, r, FC_ARGB(front ? 250 : 160, body));
    fc_line(x - 5, y, x + 5, y, FC_ARGB(front ? 170 : 80, color));
    fc_line(x, y - 4, x, y + 4, FC_ARGB(front ? 170 : 80, color));
    if(front) {
        fc_disc(x - 1, y - 1, 1, FC_ARGB(240, 0xFFFFFF));
    }
}

static void face_draw(void)
{
    if(s_face_canvas == NULL || s_face_buf == NULL) {
        return;
    }
    memset(s_face_buf, 0, (size_t)FACE_CANVAS_W * FACE_CANVAS_H * 4);

    int cx = FACE_CX, cy = FACE_CY;
    uint32_t color = s_face_color;
    int id = s_emoji;
    if(id < 0 || id >= EMO_COUNT) {
        id = EMO_NEUTRAL;
    }
    uint32_t accent = 0x8A5CFF;
    uint32_t bright = fc_lighten(color, 65);

    /* 1) 2.5D 轨道：先画绕到脑袋后面的半圈 + 背后的卫？*/
    int orb_cx = cx;
    int orb_cy = cy + 16;
    int orb_rx = 80;
    int orb_ry = 26;
    fc_orbit_arc(orb_cx, orb_cy, orb_rx, orb_ry, 180, 360, 55, accent);
    {
        int sa = ((ANIM_MS * 5 / 120) % 360);
        if(sin1000(sa) < 0) {
            fc_satellite(orb_cx + orb_rx * cos1000(sa) / 1000,
                         orb_cy + orb_ry * sin1000(sa) / 1000, 0, color);
        }
        int sb = ((-(ANIM_MS * 3 / 120) + 150) % 360);
        if(sin1000(sb) < 0) {
            fc_satellite(orb_cx + orb_rx * cos1000(sb) / 1000,
                         orb_cy + orb_ry * sin1000(sb) / 1000, 0, color);
        }
    }

    /* 2) 头部：深色底 + 主边？+ 外发？+ 内细？*/
    fc_disc(cx, cy, FACE_R, FC_ARGB(215, 0x071026));
    fc_ring(cx, cy, FACE_R, 2, FC_ARGB(255, color));
    fc_ring(cx, cy, FACE_R + 3, 4, FC_ARGB(55, color));
    fc_ring(cx, cy, FACE_R - 7, 1, FC_ARGB(35, color));

    /* 头顶装饰？*/
    fc_arc(cx, cy, FACE_R - 12, 200, 340, 1, FC_ARGB(45, accent));

    /* 3) 眼睛 */
    if(id == EMO_COOL) {
        fc_bar(cx - 34, cy + FACE_EYE_DY - 7, 68, 14, FC_ARGB(225, 0x0A1020));
        fc_bar(cx - 34, cy + FACE_EYE_DY - 7, 68, 2, FC_ARGB(235, color));
        fc_bar(cx - 34, cy + FACE_EYE_DY + 5, 68, 2, FC_ARGB(180, color));
    }
    else {
        fc_eye(cx - FACE_EYE_DX, cy + FACE_EYE_DY, true, id, color);
        fc_eye(cx + FACE_EYE_DX, cy + FACE_EYE_DY, false, id, color);
    }

    /* 4) ？*/
    fc_mouth(id, color);

    /* 4.5) 轨道从下方绕回来？.5D 立体感）+ 刻度 + 前面的卫？*/
    fc_orbit_arc(orb_cx, orb_cy, orb_rx, orb_ry + 1, 0, 180, 45, color);
    fc_orbit_arc(orb_cx, orb_cy, orb_rx, orb_ry, 0, 180, 145, color);
    for(int i = 0; i < 24; i++) {
        int a = i * 15;
        int x = orb_cx + orb_rx * cos1000(a) / 1000;
        int y = orb_cy + orb_ry * sin1000(a) / 1000;
        fc_line(x, y - 2, x, y + 2, FC_ARGB((sin1000(a) >= 0) ? 115 : 45, accent));
    }
    {
        int sa = ((ANIM_MS * 5 / 120) % 360);
        if(sin1000(sa) >= 0) {
            fc_satellite(orb_cx + orb_rx * cos1000(sa) / 1000,
                         orb_cy + orb_ry * sin1000(sa) / 1000, 1, color);
        }
        int sb = ((-(ANIM_MS * 3 / 120) + 150) % 360);
        if(sin1000(sb) >= 0) {
            fc_satellite(orb_cx + orb_rx * cos1000(sb) / 1000,
                         orb_cy + orb_ry * sin1000(sb) / 1000, 1, color);
        }
    }

    /* 5) 扫描？+ 全息闪烁（逐行压暗一部分 + 整体透明度轻微起伏） */
    {
        int flick = 235 + sin1000((ANIM_MS * 9 / 120) % 360) / 24;    /* ？225~245 */
        uint32_t tint = 0;
        if(id == EMO_ANGRY) {
            tint = 0x3A0000;
        }
        else if(id == EMO_LOVE) {
            tint = 0x3A0020;
        }
        else if(id == EMO_SURPRISED) {
            tint = 0x2A2400;
        }
        int tr = (int)((tint >> 16) & 0xFF) / 3;
        int tg = (int)((tint >> 8) & 0xFF) / 3;
        int tb = (int)(tint & 0xFF) / 3;
        for(int y = 0; y < FACE_CANVAS_H; y++) {
            uint32_t *row = (uint32_t *)s_face_buf + y * FACE_CANVAS_W;
            int scan = ((y % 3) == 0) ? 165 : 255;
            for(int x = 0; x < FACE_CANVAS_W; x++) {
                uint32_t px = row[x];
                uint32_t a = (px >> 24) & 0xFF;
                if(a == 0) {
                    continue;
                }
                a = a * flick / 255 * scan / 255;
                int r = (int)((px >> 16) & 0xFF) + tr;
                int g = (int)((px >> 8) & 0xFF) + tg;
                int b = (int)(px & 0xFF) + tb;
                if(r > 255) {
                    r = 255;
                }
                if(g > 255) {
                    g = 255;
                }
                if(b > 255) {
                    b = 255;
                }
                row[x] = ((a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
            }
        }
    }

    /* 6) 故障闪：偶尔撕裂几条亮线 + 压暗一条带 */
    if((s_frame % 672) < 35) {
        for(int k = 0; k < 3; k++) {
            int gy = 24 + (int)((s_frame * 5 + k * 29) % 100);
            int gx = 60 + (int)((s_frame * 3 + k * 41) % 70);
            fc_line(gx, gy, gx + 46, gy, FC_ARGB(110, 0xBFF6FF));
        }
        int gy = 30 + (int)((s_frame * 3) % 80);
        for(int y = gy; y < gy + 4 && y < FACE_CANVAS_H; y++) {
            uint32_t *row = (uint32_t *)s_face_buf + y * FACE_CANVAS_W;
            for(int x = 0; x < FACE_CANVAS_W; x++) {
                uint32_t px = row[x];
                uint32_t a = (px >> 24) & 0xFF;
                if(a != 0) {
                    row[x] = (px & 0x00FFFFFFu) | ((a / 3) << 24);
                }
            }
        }
    }

    lv_obj_invalidate(s_face_canvas);
}

/* ======================= 1. 开机界？======================= */

static void ui_boot_create(void)
{
    s_scr_boot = mk_screen();
    /* 星系背景已去掉（纯黑背景，省 PSRAM/渲染；后期用切图主题恢复） */

    /* 光晕 */
    s_boot_glow = mk_round(s_scr_boot, 40, 40, 160, 160, COLOR_NEON_PURPLE, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_boot_glow, LV_OPA_20, 0);

    /* Logo 文字（霓虹） */
    s_boot_logo = lv_label_create(s_scr_boot);
    lv_label_set_text(s_boot_logo, "XIAOZHI");
    lv_obj_set_style_text_font(s_boot_logo, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_boot_logo, lv_color_hex(COLOR_NEON_CYAN), 0);
    lv_obj_set_style_text_letter_space(s_boot_logo, 2, 0);
    lv_obj_center(s_boot_logo);
    lv_obj_set_y(s_boot_logo, -10);

    /* 副标？*/
    lv_obj_t *sub = lv_label_create(s_scr_boot);
    lv_label_set_text(sub, "C EDITION");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(COLOR_NEON_PINK), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 28);

    /* 扫描？*/
    s_boot_scan = mk_round(s_scr_boot, 0, 0, LCD_W, 3, COLOR_NEON_CYAN, 0);
    lv_obj_set_style_bg_opa(s_boot_scan, LV_OPA_60, 0);
}

/* ======================= 2. 待机界面 ======================= */

static void ui_idle_create(void)
{
    s_scr_idle = mk_screen();
    /* 星系背景已去掉（纯黑背景，省 PSRAM/渲染；后期用切图主题恢复） */


    /* 全息机器人脸（ARGB 画布逐帧手绘？*/
    face_canvas_create(s_scr_idle);

    /* ---- 时间 / 日期（左上角？---- */
    s_idle_time = lv_label_create(s_scr_idle);
    lv_label_set_text(s_idle_time, "--:--");
    lv_obj_set_style_text_font(s_idle_time, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_idle_time, lv_color_hex(COLOR_NEON_CYAN), 0);
    lv_obj_set_pos(s_idle_time, 14, 6);

    s_idle_date = lv_label_create(s_scr_idle);
    lv_label_set_text(s_idle_date, "时间未同步");
    lv_obj_set_style_text_font(s_idle_date, &font_gb2312_16, 0);
    lv_obj_set_style_text_color(s_idle_date, lv_color_hex(0x7FB2C8), 0);
    lv_obj_set_pos(s_idle_date, 14, 34);

    /* ---- 天气行：图标 + 温度 + 城市/天气（flex 行，整体自动居中？---- */
    s_wx_row = lv_obj_create(s_scr_idle);
    lv_obj_set_size(s_wx_row, LCD_W, 32);
    lv_obj_set_pos(s_wx_row, 0, 132);
    lv_obj_set_style_bg_opa(s_wx_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wx_row, 0, 0);
    lv_obj_set_style_pad_all(s_wx_row, 0, 0);
    lv_obj_set_style_pad_column(s_wx_row, 6, 0);
    lv_obj_clear_flag(s_wx_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_wx_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_wx_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 天气图标容器（图元相对容器定位） */
    s_wx_icon_box = lv_obj_create(s_wx_row);
    lv_obj_set_size(s_wx_icon_box, 28, 28);
    lv_obj_set_style_bg_opa(s_wx_icon_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wx_icon_box, 0, 0);
    lv_obj_set_style_pad_all(s_wx_icon_box, 0, 0);
    lv_obj_clear_flag(s_wx_icon_box, LV_OBJ_FLAG_SCROLLABLE);

    for(int i = 0; i < 3; i++) {
        s_wx_round[i] = mk_round(s_wx_icon_box, WX_ICON_X, WX_ICON_Y, 10, 10, 0xFFD54A, LV_RADIUS_CIRCLE);
        set_visible(s_wx_round[i], false);
        s_wx_bar[i] = mk_round(s_wx_icon_box, WX_ICON_X, WX_ICON_Y, 10, 4, 0xC8D8F0, 2);
        set_visible(s_wx_bar[i], false);
    }
    s_wx_bolt = lv_line_create(s_wx_icon_box);
    lv_line_set_points(s_wx_bolt, s_pt_bolt, 4);
    lv_obj_set_style_line_width(s_wx_bolt, 2, 0);
    lv_obj_set_style_line_color(s_wx_bolt, lv_color_hex(0xFFD54A), 0);
    lv_obj_set_style_line_rounded(s_wx_bolt, true, 0);
    lv_obj_set_pos(s_wx_bolt, 0, 0);
    set_visible(s_wx_bolt, false);

    /* 温度（montserrat 字体自带 ° 符号？*/
    s_wx_temp_lbl = lv_label_create(s_wx_row);
    lv_label_set_text(s_wx_temp_lbl, "");
    lv_obj_set_style_text_font(s_wx_temp_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_wx_temp_lbl, lv_color_hex(0xFFD54A), 0);

    /* 城市 + 天气现象（中文） */
    s_wx_cond = lv_label_create(s_wx_row);
    lv_label_set_text(s_wx_cond, "");
    lv_obj_set_style_text_font(s_wx_cond, &font_gb2312_16, 0);
    lv_obj_set_style_text_color(s_wx_cond, lv_color_hex(0xB8F4FF), 0);

    /* 底部提示已去掉（用户要求？*/

    /* 圆形光环/脉冲圈已换成全息脸画布里？2.5D 轨道 */

    /* HUD 四角？*/
    for(int i = 0; i < 4; i++) {
        int x = (i % 2) ? 218 : 4;
        int y = (i / 2) ? 234 : 4;
        lv_obj_t *c1 = mk_round(s_scr_idle, x, y, 18, 2, COLOR_NEON_CYAN, 0);
        lv_obj_t *c2 = mk_round(s_scr_idle, x, y, 2, 18, COLOR_NEON_CYAN, 0);
        if(i % 2) {
            lv_obj_set_x(c1, 218);
            lv_obj_set_x(c2, 234);
        }
        if(i / 2) {
            lv_obj_set_y(c1, 234);
            lv_obj_set_y(c2, 218);
        }
        lv_obj_set_style_bg_opa(c1, LV_OPA_60, 0);
        lv_obj_set_style_bg_opa(c2, LV_OPA_60, 0);
    }

    /* 漂浮粒子 */
    for(int i = 0; i < 8; i++) {
        s_particle_x[i] = 10 + (i * 29) % 220;
        s_particle_y[i] = (i * 37) % LCD_H;
        s_idle_particles[i] = mk_round(s_scr_idle, s_particle_x[i], s_particle_y[i], 3, 3,
                                       (i % 2) ? COLOR_NEON_CYAN : COLOR_NEON_PINK, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_opa(s_idle_particles[i], LV_OPA_60, 0);
    }

    /* ===== 2.5D 旋转魔方（替换原来的透视网格地面？===== */
    cube_create(s_scr_idle);

    /* 星系背景的闪烁星？+ 流星 */
    for(int i = 0; i < GALAXY_TWINKLE; i++) {
        int tx = 12 + (i * 53) % 210;
        int ty = 14 + (i * 71) % 122;
        uint32_t tc = (i % 3 == 0) ? 0xFFFFFF : ((i % 3 == 1) ? 0x9FE8FF : 0xFFC8F0);
        s_twinkle[i] = mk_round(s_scr_idle, tx, ty, 3, 3, tc, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_opa(s_twinkle[i], LV_OPA_80, 0);
        lv_obj_set_style_shadow_width(s_twinkle[i], 6, 0);
        lv_obj_set_style_shadow_color(s_twinkle[i], lv_color_hex(0xBFF0FF), 0);
        lv_obj_set_style_shadow_opa(s_twinkle[i], LV_OPA_40, 0);
    }
    s_shoot = lv_line_create(s_scr_idle);
    lv_line_set_points_mutable(s_shoot, s_shoot_pts, 2);
    lv_obj_set_pos(s_shoot, 0, 0);
    line_style(s_shoot, 0xDFF6FF, 2);
    lv_obj_set_style_line_opa(s_shoot, LV_OPA_TRANSP, 0);

    /* 扫描？*/
    s_idle_scan = mk_round(s_scr_idle, 0, 0, LCD_W, 2, COLOR_NEON_CYAN, 0);
    lv_obj_set_style_bg_opa(s_idle_scan, LV_OPA_40, 0);

    /* WiFi 信号格：最后创？= 最上层，不会被光环/扫描线挡？*/
    mk_wifi(s_scr_idle, 198, 34, s_wifi_bars_idle);
    ui_update_wifi();
}

/* ======================= 3. 聊天界面 ======================= */

static void ui_chat_create(void)
{
    s_scr_chat = mk_screen();
    /* 星系背景已去掉（纯黑背景，省 PSRAM/渲染；后期用切图主题恢复） */

    /* 顶部：小头像 + 状？+ WiFi */
    mk_face(s_scr_chat, 30, 26, 8, &s_chat_eye_l, &s_chat_eye_r, &s_chat_mouth);

    s_chat_status = lv_label_create(s_scr_chat);
    lv_label_set_text(s_chat_status, "待机");
    lv_obj_set_style_text_font(s_chat_status, &font_gb2312_16, 0);
    lv_obj_set_style_text_color(s_chat_status, lv_color_hex(COLOR_NEON_CYAN), 0);
    lv_obj_set_pos(s_chat_status, 62, 16);

    mk_wifi(s_scr_chat, 198, 34, s_wifi_bars_chat);

    /* ---- 小头像的表情附加图元 ---- */
    s_chat_caret[0] = lv_line_create(s_scr_chat);
    lv_line_set_points(s_chat_caret[0], s_pt_chat_caret_l, 3);
    lv_obj_set_pos(s_chat_caret[0], 0, 0);
    line_style(s_chat_caret[0], COLOR_NEON_CYAN, 2);
    set_visible(s_chat_caret[0], false);

    s_chat_caret[1] = lv_line_create(s_scr_chat);
    lv_line_set_points(s_chat_caret[1], s_pt_chat_caret_r, 3);
    lv_obj_set_pos(s_chat_caret[1], 0, 0);
    line_style(s_chat_caret[1], COLOR_NEON_CYAN, 2);
    set_visible(s_chat_caret[1], false);

    s_chat_smile = lv_line_create(s_scr_chat);
    lv_line_set_points(s_chat_smile, s_pt_chat_smile, 3);
    lv_obj_set_pos(s_chat_smile, 0, 0);
    line_style(s_chat_smile, COLOR_NEON_CYAN, 2);
    set_visible(s_chat_smile, false);

    /* 小墨？*/
    s_chat_glass = mk_round(s_scr_chat, 8, 21, 44, 10, 0x0A1020, 4);
    lv_obj_set_style_border_width(s_chat_glass, 1, 0);
    lv_obj_set_style_border_color(s_chat_glass, lv_color_hex(COLOR_NEON_CYAN), 0);
    set_visible(s_chat_glass, false);

    /* ---- 聆听光环（围绕小头像脉动？---- */
    s_chat_ring = mk_round(s_scr_chat, 6, 2, 48, 48, COLOR_NEON_CYAN, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_chat_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat_ring, 2, 0);
    lv_obj_set_style_border_color(s_chat_ring, lv_color_hex(COLOR_NEON_CYAN), 0);
    set_visible(s_chat_ring, false);

    /* ---- 说话声波？ 根，随播放音量跳动） ---- */
    for(int i = 0; i < 5; i++) {
        s_chat_wave[i] = mk_round(s_scr_chat, 152 + i * 8, 34, 5, 4,
                                  (i % 2) ? COLOR_NEON_PINK : COLOR_NEON_CYAN, 2);
        set_visible(s_chat_wave[i], false);
    }

    /* 霓虹分隔？*/
    lv_obj_t *line = mk_round(s_scr_chat, 8, 48, 224, 2, COLOR_NEON_PINK, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_40, 0);

    /* 聊天区域（可滚动？*/
    s_chat_area = lv_obj_create(s_scr_chat);
    lv_obj_set_size(s_chat_area, 226, 180);
    lv_obj_set_pos(s_chat_area, 7, 54);
    lv_obj_set_style_bg_opa(s_chat_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat_area, 0, 0);
    lv_obj_set_style_pad_all(s_chat_area, 2, 0);
    lv_obj_set_style_pad_row(s_chat_area, 4, 0);
    lv_obj_set_flex_flow(s_chat_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_chat_area, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat_area, LV_SCROLLBAR_MODE_OFF);
}

/**
 * @brief 添加对话气泡
 */
static void ui_add_bubble(const char *text, bool is_user)
{
    if(s_chat_area == NULL || text == NULL) {
        return;
    }
    if(s_msg_count >= MAX_MSGS) {
        lv_obj_t *oldest = lv_obj_get_child(s_chat_area, 0);
        if(oldest != NULL) {
            lv_obj_delete(oldest);
            s_msg_count--;
        }
    }

    lv_obj_t *bubble = lv_obj_create(s_chat_area);
    lv_obj_set_width(bubble, LV_PCT(is_user ? 76 : 88));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_pad_all(bubble, 6, 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_80, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_align(bubble, is_user ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0);

    if(is_user) {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x101A3A), 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(COLOR_NEON_CYAN), 0);
        lv_obj_set_style_shadow_color(bubble, lv_color_hex(COLOR_NEON_CYAN), 0);
    }
    else {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x2A1038), 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(COLOR_NEON_PINK), 0);
        lv_obj_set_style_shadow_color(bubble, lv_color_hex(COLOR_NEON_PINK), 0);
    }
    lv_obj_set_style_shadow_width(bubble, 10, 0);
    lv_obj_set_style_shadow_opa(bubble, LV_OPA_40, 0);

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, &font_gb2312_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(is_user ? 0xB8F4FF : 0xFFD1EC), 0);

    s_msg_count++;
    lv_obj_scroll_to_view(bubble, LV_ANIM_ON);
}

/* ======================= 4. 关机界面 ======================= */

static void ui_shutdown_create(void)
{
    s_scr_off = mk_screen();
    s_off_ring = mk_round(s_scr_off, 70, 70, 100, 100, COLOR_NEON_CYAN, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_off_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_off_ring, 3, 0);
    lv_obj_set_style_border_color(s_off_ring, lv_color_hex(COLOR_NEON_CYAN), 0);
    lv_obj_set_style_shadow_width(s_off_ring, 24, 0);
    lv_obj_set_style_shadow_color(s_off_ring, lv_color_hex(COLOR_NEON_CYAN), 0);
    lv_obj_set_style_shadow_opa(s_off_ring, LV_OPA_50, 0);

    lv_obj_t *lbl = lv_label_create(s_scr_off);
    lv_label_set_text(lbl, "关机");
    lv_obj_set_style_text_font(lbl, &font_gb2312_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_NEON_PINK), 0);
    lv_obj_center(lbl);
}

/* ======================= 界面切换 ======================= */

static void show_screen(lv_obj_t *scr)
{
    if(scr == NULL || scr == s_cur_scr) {
        return;
    }
    if(s_cur_scr != NULL) {
        lv_obj_add_flag(s_cur_scr, LV_OBJ_FLAG_HIDDEN);
    }
    s_screen_sw++;
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_HIDDEN);
    s_cur_scr = scr;
    s_in_chat_screen = (scr == s_scr_chat);
}

/**
 * @brief 根据状态切换界？+ 刷新表情
 */
static void ui_apply_state(display_state_t state)
{
    uint32_t color = COLOR_NEON_CYAN;
    const char *text = "待机";

    switch(state) {
    case DISPLAY_STATE_CONNECTING:
        color = COLOR_NEON_PURPLE;
        text = "连接中";
        break;
    case DISPLAY_STATE_LISTENING:
        color = COLOR_NEON_CYAN;
        text = "聆听中";
        break;
    case DISPLAY_STATE_THINKING:
        color = 0xB7A6FF;
        text = "思考中";
        break;
    case DISPLAY_STATE_SPEAKING:
        color = COLOR_NEON_PINK;
        text = "说话中";
        break;
    case DISPLAY_STATE_ERROR:
        color = 0xFF3B3B;
        text = "出错了";
        break;
    case DISPLAY_STATE_IDLE:
    default:
        color = COLOR_NEON_CYAN;
        text = "在线";
        break;
    }

    /* 界面切换：开机动画结束前不切；空？连接？？待机（聊天中延迟 5 秒返回）；其？？聊天 */
    if(!s_boot_done) {
        /* 开？Logo 阶段保持开机界？*/
    }
    else if(state == DISPLAY_STATE_IDLE || state == DISPLAY_STATE_CONNECTING) {
        if(s_music_playing && s_scr_music != NULL) {
            show_screen(s_scr_music);        /* 待机时显示音乐界面 */
        }
        else if(!s_in_chat_screen) {
            show_screen(s_scr_idle);
        }
        else {
            s_chat_hold_start = (uint32_t)(esp_timer_get_time() / 1000);
        }
    }
    else {
        show_screen(s_scr_chat);
    }

    /* 更新聊天界面状态文？*/
    if(s_chat_status != NULL) {
        lv_label_set_text(s_chat_status, text);
        lv_obj_set_style_text_color(s_chat_status, lv_color_hex(color), 0);
    }

    /* 回到待机/出错时恢复默认表？*/
    if(state == DISPLAY_STATE_IDLE || state == DISPLAY_STATE_ERROR) {
        s_emoji = EMO_NEUTRAL;
    }

    /* 状态色变了：表情颜色跟随状态色，由动画定时器重？*/
    s_face_color = color;
    s_emoji_dirty = true;
}

/* ======================= 动画 ======================= */

/* ---- 拍照预览：作为一条图片消息显示在聊天框里 ---- */

#define PHOTO_W 200
#define PHOTO_H 150

#define PHOTO_BUBBLE_TAG  ((void *)0xC0FFEEu)
#define MUSIC_BARS 12
static lv_obj_t *s_music_bars[MUSIC_BARS];
static lv_obj_t *s_music_ring = NULL;
static lv_obj_t *s_music_title = NULL;
static lv_obj_t *s_music_info = NULL;
static volatile uint8_t s_music_level = 0;
static volatile int s_music_level_smooth = 0;
static volatile bool s_music_info_pending = false;
static char s_music_info_buf[64];
static volatile bool s_music_msg_pending = false;  /* 待插入的提示消息 */
static char s_music_msg[96];
static volatile bool s_photo_pending = false;   /* 摄像头任务置位，LVGL 任务消费 */

static void photo_preview_init(void)
{
    s_photo_buf = heap_caps_malloc((size_t)PHOTO_W * PHOTO_H * 2, MALLOC_CAP_SPIRAM);
    if(s_photo_buf == NULL) {
        ESP_LOGW(TAG, "拍照预览缓冲分配失败");
        return;
    }
    memset(s_photo_buf, 0, (size_t)PHOTO_W * PHOTO_H * 2);
    memset(&s_photo_dsc, 0, sizeof(s_photo_dsc));
    s_photo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_photo_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_photo_dsc.header.w = PHOTO_W;
    s_photo_dsc.header.h = PHOTO_H;
    s_photo_dsc.header.stride = PHOTO_W * 2;
    s_photo_dsc.data_size = (uint32_t)PHOTO_W * PHOTO_H * 2;
    s_photo_dsc.data = s_photo_buf;
}

void display_show_camera_frame(const uint16_t *rgb565, int w, int h)
{
    /* 本函数在摄像头任务里调用：LVGL 不是线程安全的，
     * 这里只做像素缩放/字节序处理，界面更新交给 LVGL 任务里的 photo_bubble_update() */
    if(s_photo_buf == NULL || rgb565 == NULL || w <= 0 || h <= 0) {
        return;
    }
    uint16_t *dst = (uint16_t *)s_photo_buf;
    for(int dy = 0; dy < PHOTO_H; dy++) {
        int sy = dy * h / PHOTO_H;
        const uint16_t *src = rgb565 + (size_t)sy * w;
        for(int dx = 0; dx < PHOTO_W; dx++) {
            int sx = dx * w / PHOTO_W;
            dst[dy * PHOTO_W + dx] = src[sx];
        }
    }
    /* 摄像头 RGB565 字节序和 LVGL 相反，换一下 */
    lv_draw_sw_rgb565_swap(dst, (uint32_t)(PHOTO_W * PHOTO_H));
    s_photo_pending = true;
}

/**
 * @brief 在 LVGL 任务里把待显示的照片变成一条图片消息（线程安全）
 */
static void photo_bubble_update(void)
{
    if(!s_photo_pending || s_chat_area == NULL || s_photo_buf == NULL) {
        return;
    }
    s_photo_pending = false;

    /* 删掉旧的图片气泡（按 tag 遍历，不能存裸指针：可能已被消息上限淘汰） */
    uint32_t child_cnt = lv_obj_get_child_count(s_chat_area);
    for(uint32_t ci = child_cnt; ci > 0; ci--) {
        lv_obj_t *ch = lv_obj_get_child(s_chat_area, (int32_t)(ci - 1));
        if(ch != NULL && lv_obj_get_user_data(ch) == PHOTO_BUBBLE_TAG) {
            lv_obj_delete(ch);
            if(s_msg_count > 0) {
                s_msg_count--;
            }
        }
    }
    if(s_msg_count >= MAX_MSGS) {
        lv_obj_t *oldest = lv_obj_get_child(s_chat_area, 0);
        if(oldest != NULL) {
            lv_obj_delete(oldest);
            s_msg_count--;
        }
    }

    lv_obj_t *bubble = lv_obj_create(s_chat_area);
    lv_obj_set_size(bubble, PHOTO_W + 12, PHOTO_H + 12);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_pad_all(bubble, 6, 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(0x2A1038), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_80, 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(COLOR_NEON_PINK), 0);
    lv_obj_set_style_shadow_color(bubble, lv_color_hex(COLOR_NEON_PINK), 0);
    lv_obj_set_style_shadow_width(bubble, 10, 0);
    lv_obj_set_style_shadow_opa(bubble, LV_OPA_40, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_align(bubble, LV_ALIGN_LEFT_MID, 0);
    lv_obj_set_user_data(bubble, PHOTO_BUBBLE_TAG);

    lv_obj_t *img = lv_image_create(bubble);
    lv_image_set_src(img, &s_photo_dsc);
    lv_obj_center(img);

    s_msg_count++;
    lv_obj_scroll_to_view(bubble, LV_ANIM_ON);
}

void display_music_notify(const char *text)
{
    if(text == NULL) {
        return;
    }
    snprintf(s_music_msg, sizeof(s_music_msg), "%s", text);
    s_music_msg_pending = true;      /* 由 LVGL 任务消费，线程安全 */
}

/**
 * @brief 创建音乐播放界面（旋转光晕 + 频谱条 + 曲名）
 */
static void ui_music_create(void)
{
    s_scr_music = mk_screen();
    /* 星系背景已去掉（纯黑背景，省 PSRAM/渲染；后期用切图主题恢复） */

    s_music_title = lv_label_create(s_scr_music);
    lv_label_set_text(s_music_title, "♪ 音乐");
    lv_obj_set_style_text_font(s_music_title, &font_gb2312_16, 0);
    lv_obj_set_style_text_color(s_music_title, lv_color_hex(COLOR_NEON_CYAN), 0);
    lv_obj_align(s_music_title, LV_ALIGN_TOP_MID, 0, 12);

    s_music_info = lv_label_create(s_scr_music);
    lv_label_set_text(s_music_info, "网络音乐");
    lv_obj_set_width(s_music_info, 200);
    lv_label_set_long_mode(s_music_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_music_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_music_info, &font_gb2312_16, 0);
    lv_obj_set_style_text_color(s_music_info, lv_color_hex(0xFFD1EC), 0);
    lv_obj_align(s_music_info, LV_ALIGN_TOP_MID, 0, 34);

    /* 旋转光晕（呼吸圆环） */
    s_music_ring = mk_round(s_scr_music, 76, 62, 88, 88, COLOR_NEON_CYAN, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_music_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_music_ring, 2, 0);
    lv_obj_set_style_border_color(s_music_ring, lv_color_hex(COLOR_NEON_CYAN), 0);
    lv_obj_set_style_shadow_width(s_music_ring, 18, 0);
    lv_obj_set_style_shadow_color(s_music_ring, lv_color_hex(COLOR_NEON_CYAN), 0);
    lv_obj_set_style_shadow_opa(s_music_ring, LV_OPA_30, 0);

    lv_obj_t *note = lv_label_create(s_scr_music);
    lv_label_set_text(note, "♪");
    lv_obj_set_style_text_font(note, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(0xB8F4FF), 0);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 94);

    /* 频谱条 */
    for(int i = 0; i < MUSIC_BARS; i++) {
        int x = 18 + i * 17;
        s_music_bars[i] = mk_round(s_scr_music, x, 186, 12, 10,
                                   (i % 2) ? COLOR_NEON_PINK : COLOR_NEON_CYAN, 3);
    }
    lv_obj_add_flag(s_scr_music, LV_OBJ_FLAG_HIDDEN);
}

void display_set_music_playing(bool playing)
{
    /* 只在任意任务里置标志，真正的 LVGL 操作由动画回调（LVGL 任务）来做 */
    s_music_playing = playing;
    s_music_screen_req = playing ? 1 : 2;
}

void display_set_music_info(const char *text)
{
    if(text == NULL) {
        return;
    }
    snprintf(s_music_info_buf, sizeof(s_music_info_buf), "%s", text);
    s_music_info_pending = true;
}

void display_set_music_level(uint8_t level)
{
    s_music_level = level;
}

static void ui_anim_cb(lv_timer_t *t)
{
    (void)t;
    int64_t tanim0 = esp_timer_get_time();
    s_frame++;
    photo_bubble_update();

    /* 音乐界面切换请求（必须由 LVGL 任务执行） */
    if(s_music_screen_req != 0) {
        int req = s_music_screen_req;
        s_music_screen_req = 0;
        if(s_anim_timer != NULL) {
            lv_timer_set_period(s_anim_timer, (req == 1) ? 150 : 33);
        }
        if(req == 1) {
            s_music_level_smooth = 0;
            if(s_scr_music != NULL) {
                ESP_LOGI(TAG, "切换到音乐界面");
                show_screen(s_scr_music);
            }
        }
        else if(s_cur_scr == s_scr_music) {
            ESP_LOGI(TAG, "退出音乐界面");
            show_screen(s_scr_idle);
        }
    }

    /* 音乐界面：曲名 + 频谱动画 */
    if(s_music_info_pending) {
        s_music_info_pending = false;
        if(s_music_info != NULL) {
            lv_label_set_text(s_music_info, s_music_info_buf);
        }
    }
    if(s_cur_scr == s_scr_music) {
        s_music_level_smooth = (s_music_level_smooth * 5 + (int)s_music_level) / 6;
        int lvl = s_music_level_smooth;
        for(int i = 0; i < MUSIC_BARS; i++) {
            if((s_frame % 6) != 0) {
                break;
            }
            if(s_music_bars[i] == NULL) {
                continue;
            }
            int ph = ((ANIM_MS * 7 / 120) + i * 41) % 360;
            int amp = (sin1000(ph) + 1000) / 2;          /* 0..1000 */
            int h = 8 + ((lvl + 15) * (20 + amp * 2 / 25)) / 100;
            if(h < 6) {
                h = 6;
            }
            if(h > 78) {
                h = 78;
            }
            lv_obj_set_size(s_music_bars[i], 12, h);
            lv_obj_set_y(s_music_bars[i], 200 - h);
        }
        if(s_music_ring != NULL && (s_frame % 12) == 0) {
            int pulse = (sin1000((ANIM_MS * 5 / 120) % 360) + 1000) * 10 / 2000;   /* 0..10 */
            int sz = 82 + pulse;
            lv_obj_set_size(s_music_ring, sz, sz);
            lv_obj_set_pos(s_music_ring, 120 - sz / 2, 106 - sz / 2);
        }
    }

    if((s_frame % 300) == 0) {
        ESP_LOGI(TAG, "渲染 %d 帧：动画 %u us，画布 %u us，渲染 %u us（SPI %u us），刷屏 %u 块/%u px，切屏 %u，内部RAM %uKB(最低%uKB)，PSRAM %uKB(最低%uKB)",
                 s_frame, (unsigned)s_anim_us, (unsigned)s_draw_us, (unsigned)s_hdl_us,
                 (unsigned)s_flush_us, (unsigned)s_flush_cnt, (unsigned)s_flush_px,
                 (unsigned)s_screen_sw,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024));
        s_screen_sw = 0;
        s_flush_us = 0;
        s_anim_us = 0;
        s_draw_us = 0;
        s_hdl_us = 0;
        s_flush_px = 0;
        s_flush_cnt = 0;
    }
    if((s_frame % 7) == 0) {
        s_tick++;
    }

    /* ---- 开机界？---- */
    if(!s_boot_done) {
        /* 扫描线上下移？*/
        int y = (s_tick * 6) % (LCD_H + 20) - 10;
        if(s_boot_scan) {
            lv_obj_set_y(s_boot_scan, y);
        }
        /* 光晕呼吸 */
        if(s_boot_glow) {
            lv_obj_set_style_bg_opa(s_boot_glow, (s_tick % 10 < 5) ? LV_OPA_30 : LV_OPA_10, 0);
        }
        /* 2.5 秒后进待？*/
        if(s_tick >= 21) {
            s_boot_done = true;
            show_screen(s_scr_idle);
            ui_update_clock();
        }
        return;
    }

    /* ---- 眨眼（每 3 秒） ---- */
    if(s_tick % 25 == 0) {
        s_blink = 2;
    }
    bool blinking = (s_blink > 0);
    if(blinking) {
        s_blink--;
    }
    if(blinking != s_blink_active) {
        s_blink_active = blinking;
        s_emoji_dirty = true;
    }

    /* ---- 表情 / 时钟 ---- */
    if(s_emoji_dirty) {
        s_emoji_dirty = false;
        emoji_apply_now();
    }
    if(s_tick % 8 == 0) {
        ui_update_clock();
    }

    /* 聊天界面：空？5 秒后自动回待？*/
    if(s_in_chat_screen && s_state == DISPLAY_STATE_IDLE) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if(now - s_chat_hold_start > 5000) {
            show_screen(s_scr_idle);
        }
    }

    if(s_cur_scr == s_scr_idle) {
        /* 扫描？*/
        if((s_frame % 6) == 0) {
            lv_obj_set_y(s_idle_scan, (ANIM_MS * 9 / 120) % LCD_H);
        }
        /* 漂浮粒子向上？*/
        for(int i = 0; i < 8; i++) {
            if(s_idle_particles[i] == NULL) {
                continue;
            }
            if((s_frame % 3) == 0) {
                s_particle_y[i] -= 1;
            }
            if(s_particle_y[i] < 0) {
                s_particle_y[i] = LCD_H;
            }
            lv_obj_set_pos(s_idle_particles[i], s_particle_x[i], s_particle_y[i]);
        }
    }
    else if(s_cur_scr == s_scr_chat) {
        if(s_state == DISPLAY_STATE_SPEAKING) {
            /* 说话中：嘴巴高度 + 声波？TTS 实际音量跳动 */
            int lvl = s_audio_level;
            set_visible(s_chat_mouth, true);
            int mh = 4 + lvl / 8;
            lv_obj_set_size(s_chat_mouth, 16, mh);
            lv_obj_set_pos(s_chat_mouth, 22, 34 - mh / 2);
            for(int i = 0; i < 5; i++) {
                int bh = 3 + lvl * (i + 2) / 45 + (int)((s_tick + i * 3) % 3);
                if(bh > 18) {
                    bh = 18;
                }
                lv_obj_set_size(s_chat_wave[i], 5, bh);
                lv_obj_set_pos(s_chat_wave[i], 152 + i * 8, 40 - bh);
                set_visible(s_chat_wave[i], true);
            }
            set_visible(s_chat_ring, false);
        }
        else {
            for(int i = 0; i < 5; i++) {
                set_visible(s_chat_wave[i], false);
            }
            /* 聆听中：头像外圈脉动 */
            if((s_state == DISPLAY_STATE_LISTENING || s_state == DISPLAY_STATE_THINKING) &&
               s_chat_ring != NULL) {
                int ph = (s_frame % 112) / 7;
                int r = 44 + ((ph < 8) ? ph : (16 - ph)) * 2;
                lv_obj_set_size(s_chat_ring, r, r);
                lv_obj_set_pos(s_chat_ring, 30 - r / 2, 26 - r / 2);
                lv_obj_set_style_border_color(s_chat_ring,
                    lv_color_hex((s_state == DISPLAY_STATE_THINKING) ? 0xB7A6FF : COLOR_NEON_CYAN), 0);
                lv_obj_set_style_border_opa(s_chat_ring, (ph < 8) ? LV_OPA_60 : LV_OPA_20, 0);
                set_visible(s_chat_ring, true);
            }
            else {
                set_visible(s_chat_ring, false);
            }
        }
    }

    /* 思考中：状态文字点点动？*/
    if(s_chat_status != NULL && s_state == DISPLAY_STATE_THINKING) {
        static const char *dots[] = {"思考中", "思考中.", "思考中..", "思考中..."};
        lv_label_set_text(s_chat_status, dots[(s_tick / 3) % 4]);
    }

    /* 音量自然衰减（播放任务会持续上报新值） */
    if(s_audio_level > 0) {
        s_audio_level = (s_audio_level > 10) ? (uint8_t)(s_audio_level - 10) : 0;
    }

    /* ---- 2.5D 旋转魔方 ---- */
    if(s_cur_scr == s_scr_idle) {
        int64_t t0 = esp_timer_get_time();
#if CANVAS_ANIM
        int cube_div = s_music_playing ? 12 : 6;
        int face_div = s_music_playing ? 16 : 8;
        if((s_frame % cube_div) == 0) {
            cube_update();
        }
        if((s_frame % face_div) == 0) {
            face_draw();
        }
#endif
        s_draw_us += (uint32_t)(esp_timer_get_time() - t0);
    }

    /* ---- 星系元素：星星闪？+ 流星 ---- */
    if((s_frame % 8) == 0) {
        static lv_opa_t s_tw_last[GALAXY_TWINKLE];
        for(int i = 0; i < GALAXY_TWINKLE; i++) {
            if(s_twinkle[i] == NULL) {
                continue;
            }
            int ph = ((ANIM_MS * 7 / 120) + i * 53) % 360;
            int o = 60 + (sin1000(ph) + 1000) * 155 / 2000;     /* 60..215 */
            int d = o - (int)s_tw_last[i];
            if(d < 0) {
                d = -d;
            }
            if(d >= 8 || s_tw_last[i] == 0) {
                s_tw_last[i] = (lv_opa_t)o;
                lv_obj_set_style_bg_opa(s_twinkle[i], (lv_opa_t)o, 0);
            }
        }
    }
    if(s_shoot != NULL && (s_frame % 2) == 0) {
        int t = (s_frame % 560) / 7;
        if(s_cur_scr == s_scr_idle && t < 14) {
            int shx = 24 + t * 13;
            int shy = 10 + t * 8;
            s_shoot_pts[0].x = shx;
            s_shoot_pts[0].y = shy;
            s_shoot_pts[1].x = shx - 18;
            s_shoot_pts[1].y = shy - 11;
            lv_line_set_points_mutable(s_shoot, s_shoot_pts, 2);
            lv_obj_set_style_line_opa(s_shoot, (lv_opa_t)(230 - t * 15), 0);
        }
        else {
            lv_obj_set_style_line_opa(s_shoot, LV_OPA_TRANSP, 0);
        }
    }

    /* ---- 关机动画：光环收？---- */
    if(s_cur_scr == s_scr_off && s_off_ring != NULL) {
        static int off_tick = 0;
        off_tick++;
        int r = 100 - off_tick * 2;
        if(r < 12) {
            r = 12;
        }
        lv_obj_set_size(s_off_ring, r, r);
        lv_obj_set_pos(s_off_ring, 120 - r / 2, 120 - r / 2);
    }

    /* WiFi 断开时信号格闪烁 */
    lv_opa_t wopa = (!s_wifi_ok && (s_tick % 8 < 4)) ? LV_OPA_20 : LV_OPA_COVER;
    static lv_opa_t s_wifi_last_opa = LV_OPA_COVER;
    if(wopa != s_wifi_last_opa) {
        s_wifi_last_opa = wopa;
        for(int i = 0; i < 4; i++) {
            if(s_wifi_bars_idle[i] != NULL) {
                lv_obj_set_style_bg_opa(s_wifi_bars_idle[i], wopa, 0);
            }
            if(s_wifi_bars_chat[i] != NULL) {
                lv_obj_set_style_bg_opa(s_wifi_bars_chat[i], wopa, 0);
            }
        }
    }

    s_anim_us += (uint32_t)(esp_timer_get_time() - tanim0);
}

/* ======================= LVGL 任务 ======================= */

static void lvgl_task(void *arg)
{
    (void)arg;
    chat_msg_t msg;

    while(1) {
        if(s_state_dirty) {
            s_state_dirty = false;
            ui_apply_state(s_state);
        }
        if(s_wifi_dirty) {
            s_wifi_dirty = false;
            ui_update_wifi();
        }
        if(s_weather_dirty) {
            s_weather_dirty = false;
            ui_update_weather();
        }
        if(s_shutdown_req) {
            s_shutdown_req = false;
            show_screen(s_scr_off);
        }
        while(xQueueReceive(s_msg_q, &msg, 0) == pdTRUE) {
            ui_add_bubble(msg.text, msg.is_user);
        }
        int64_t th0 = esp_timer_get_time();
        uint32_t delay = lv_timer_handler();
        s_hdl_us += (uint32_t)(esp_timer_get_time() - th0);
        if(delay == 0 || delay > 20) {
            delay = 20;
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

/* ======================= 对外接口 ======================= */

esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "LCD 初始化失败");
    ESP_RETURN_ON_ERROR(lvgl_port_init(), TAG, "LVGL 初始化失败");

    s_msg_q = xQueueCreate(8, sizeof(chat_msg_t));
    if(s_msg_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 四个界面 */
    ui_boot_create();
    ui_idle_create();
    ui_chat_create();
    ui_shutdown_create();

    /* 默认显示开机界？*/
    lv_obj_add_flag(s_scr_idle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_scr_chat, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_scr_off, LV_OBJ_FLAG_HIDDEN);
    s_cur_scr = s_scr_boot;

    /* 初始表情 / 时钟 */
    emoji_apply_now();
    ui_update_clock();

    photo_preview_init();
    ui_music_create();
    s_anim_timer = lv_timer_create(ui_anim_cb, 33, NULL);

    /* 关键：LVGL 任务栈必须放内部 RAM，否则刷屏命令参数不能直接 DMA，
     * 每次刷屏都要临时申请 bounce buffer，渲染会慢好几倍 */
    xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, 5, NULL);   /* LVGL 任务栈保留在内部 RAM */
    ESP_LOGI(TAG, "显示模块就绪（赛博朋克界面 + 时钟/天气 + 表情）");
    return ESP_OK;
}

void display_set_state(display_state_t state)
{
    s_state = state;
    s_state_dirty = true;
}

void display_set_wifi(bool connected)
{
    s_wifi_ok = connected;
    s_wifi_dirty = true;
}

void display_add_message(bool is_user, const char *text)
{
    if(s_msg_q == NULL || text == NULL) {
        return;
    }
    chat_msg_t msg;
    msg.is_user = is_user;
    strncpy(msg.text, text, MSG_MAX_LEN - 1);
    msg.text[MSG_MAX_LEN - 1] = '\0';
    xQueueSend(s_msg_q, &msg, 0);
}

/**
 * @brief 播放关机动画（阻塞约 1.6 秒）
 */
void display_play_shutdown(void)
{
    s_shutdown_req = true;
    vTaskDelay(pdMS_TO_TICKS(1600));
    /* 关屏幕显？+ 关背光（否则深度睡眠后背光还亮着？*/
    if(s_panel != NULL) {
        esp_lcd_panel_disp_on_off(s_panel, false);
    }
    gpio_set_level(LCD_BL, 0);
    ESP_LOGI(TAG, "背光已关闭");
}

/* ======================= 天气 / 表情 / 音量 对外接口 ======================= */

void display_set_weather(int temp_c, int temp_max, int temp_min, int humidity,
                         int code, const char *city)
{
    (void)temp_max;     /* 暂未显示最？最低温，先留着参数方便以后扩展 */
    (void)temp_min;

    s_wx_temp_c = temp_c;
    s_wx_hum = humidity;
    s_wx_code = code;
    memset(s_wx_city, 0, sizeof(s_wx_city));
    if(city != NULL) {
        strncpy(s_wx_city, city, sizeof(s_wx_city) - 1);
    }
    s_weather_dirty = true;
}

void display_set_emoji(const char *emoji)
{
    if(emoji == NULL) {
        return;
    }

    int id = EMO_NEUTRAL;
    if(strcmp(emoji, "happy") == 0 || strcmp(emoji, "loving") == 0 ||
       strcmp(emoji, "confident") == 0 || strcmp(emoji, "delicious") == 0 ||
       strcmp(emoji, "embarrassed") == 0 || strcmp(emoji, "kissy") == 0 ||
       strcmp(emoji, "relaxed") == 0) {
        id = EMO_HAPPY;
    }
    else if(strcmp(emoji, "laughing") == 0 || strcmp(emoji, "funny") == 0) {
        id = EMO_LAUGH;
    }
    else if(strcmp(emoji, "sad") == 0 || strcmp(emoji, "crying") == 0) {
        id = EMO_SAD;
    }
    else if(strcmp(emoji, "angry") == 0) {
        id = EMO_ANGRY;
    }
    else if(strcmp(emoji, "surprised") == 0 || strcmp(emoji, "shocked") == 0) {
        id = EMO_SURPRISED;
    }
    else if(strcmp(emoji, "sleepy") == 0) {
        id = EMO_SLEEPY;
    }
    else if(strcmp(emoji, "thinking") == 0) {
        id = EMO_THINKING;
    }
    else if(strcmp(emoji, "winking") == 0) {
        id = EMO_WINK;
    }
    else if(strcmp(emoji, "cool") == 0) {
        id = EMO_COOL;
    }
    else if(strcmp(emoji, "confused") == 0) {
        id = EMO_CONFUSED;
    }
    else if(strcmp(emoji, "silly") == 0) {
        id = EMO_SILLY;
    }
    else if(strcmp(emoji, "neutral") == 0 || strcmp(emoji, "calm") == 0) {
        id = EMO_NEUTRAL;
    }
    else {
        id = EMO_NEUTRAL;
    }

    if(id != s_emoji) {
        s_emoji = id;
        s_emoji_dirty = true;
    }
}

void display_set_audio_level(uint8_t level)
{
    if(level > 100) {
        level = 100;
    }
    s_audio_level = level;
}

