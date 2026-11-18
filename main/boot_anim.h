#pragma once
#include <stdint.h>

/* 由 tools/pack_boot_anim.py 生成，勿手改（JPEG 帧资源） */
#define BOOT_ANIM_W 240
#define BOOT_ANIM_H 240
#define BOOT_ANIM_N 50
#define BOOT_ANIM_RGB_BYTES (BOOT_ANIM_W * BOOT_ANIM_H * 2)
#define BOOT_ANIM_MAGIC 0x504A4F42u

/* 从映射后的 bin 里取第 idx 帧的 JPEG 指针和长度，成功返回 0 */
int boot_anim_get_frame(const uint8_t *bin, uint32_t bin_size, int idx,
                        const uint8_t **jpeg, uint32_t *jpeg_len);
