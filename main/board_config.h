#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "soc/soc_caps.h"

/* ======================= 板级配置（移植自 C++ 版 config.h） =======================
 * 板型：面包板 DIY ESP32-S3-CAM（bread-compact-wifi-s3cam）
 * 屏幕：ST7789 SPI，默认 240x240
 * 音频：simplex I2S（麦克风与喇叭各用一路 I2S）
 * ======================================================================== */

/* ---------------- 音频：麦克风（I2S0 RX） ---------------- */
#define MIC_I2S_WS        GPIO_NUM_1
#define MIC_I2S_BCLK      GPIO_NUM_2
#define MIC_I2S_DIN       GPIO_NUM_42

/* ---------------- 音频：喇叭（I2S1 TX） ---------------- */
#define SPK_I2S_DOUT      GPIO_NUM_39
#define SPK_I2S_BCLK      GPIO_NUM_40
#define SPK_I2S_LRCK      GPIO_NUM_41

#define AUDIO_IN_RATE     16000
#define AUDIO_OUT_RATE    16000

/* 关键：C++ 版用的是 32 位数据位宽（不是 16 位） */
#define AUDIO_BITS        I2S_DATA_BIT_WIDTH_32BIT

/* ---------------- LED / 按键 ---------------- */
#define LED_GPIO          GPIO_NUM_48
#define BOOT_BTN_GPIO     GPIO_NUM_0
#define LAMP_GPIO         GPIO_NUM_14      /* MCP 示例：控制一盏灯 */

/* ---------------- 显示屏 ST7789（SPI） ---------------- */
#define LCD_BL            GPIO_NUM_38
#define LCD_MOSI          GPIO_NUM_20
#define LCD_CLK           GPIO_NUM_19
#define LCD_DC            GPIO_NUM_47
#define LCD_RST           GPIO_NUM_21
#define LCD_CS            GPIO_NUM_45
#define LCD_W             240
#define LCD_H             240

/* ---------------- 摄像头（OV2640/OV3660 等，先预留） ---------------- */
#define CAM_PIN_D0        GPIO_NUM_11
#define CAM_PIN_D1        GPIO_NUM_9
#define CAM_PIN_D2        GPIO_NUM_8
#define CAM_PIN_D3        GPIO_NUM_10
#define CAM_PIN_D4        GPIO_NUM_12
#define CAM_PIN_D5        GPIO_NUM_18
#define CAM_PIN_D6        GPIO_NUM_17
#define CAM_PIN_D7        GPIO_NUM_16
#define CAM_PIN_XCLK      GPIO_NUM_15
#define CAM_PIN_PCLK      GPIO_NUM_13
#define CAM_PIN_VSYNC     GPIO_NUM_6
#define CAM_PIN_HREF      GPIO_NUM_7
#define CAM_PIN_SIOC      GPIO_NUM_5
#define CAM_PIN_SIOD      GPIO_NUM_4
#define CAM_PIN_PWDN      GPIO_NUM_NC
#define CAM_PIN_RESET     GPIO_NUM_NC
#define CAM_XCLK_FREQ     20000000

/**
 * @brief 生成板子统一风格的 I2S std 配置（32bit / 单声道 / 左声道）
 *
 * 麦克风和喇叭共用这份配置，保证两路 I2S 参数一致。
 */
static inline i2s_std_config_t board_i2s_config(int32_t rate, gpio_num_t bclk,
                                               gpio_num_t ws, gpio_num_t din, gpio_num_t dout)
{
    i2s_std_config_t cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = AUDIO_BITS,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = AUDIO_BITS,
            .ws_pol = false,
            .bit_shift = true,
#if SOC_I2S_HW_VERSION_1
            .msb_right = false,
#else
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    return cfg;
}
