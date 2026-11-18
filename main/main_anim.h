#pragma once
#include <stdint.h>

/* 由 tools/pack_main_anim.py 生成（主界面 JPEG 帧） */
#define MAIN_ANIM_W 240
#define MAIN_ANIM_H 240
#define MAIN_ANIM_N 30
#define MAIN_ANIM_RGB_BYTES (MAIN_ANIM_W * MAIN_ANIM_H * 2)
#define MAIN_ANIM_MAGIC 0x4E49414Du

int main_anim_get_frame(const uint8_t *bin, uint32_t bin_size, int idx,
                         const uint8_t **jpeg, uint32_t *jpeg_len);
