/* JPEG 帧表解析（对应 tools/pack_boot_anim.py 的 bin 布局） */
#include "boot_anim.h"

int boot_anim_get_frame(const uint8_t *bin, uint32_t bin_size, int idx,
                        const uint8_t **jpeg, uint32_t *jpeg_len)
{
    if(bin == NULL || bin_size < 16 + 4) {
        return -1;
    }
    const uint32_t *h = (const uint32_t *)bin;
    if(h[0] != BOOT_ANIM_MAGIC || (int)h[1] != BOOT_ANIM_N) {
        return -2;
    }
    if(idx < 0 || idx >= BOOT_ANIM_N) {
        return -3;
    }
    const uint32_t *off = (const uint32_t *)(bin + 16);
    uint32_t a = off[idx];
    uint32_t b = off[idx + 1];
    if(a >= b || b > bin_size) {
        return -4;
    }
    *jpeg = bin + a;
    *jpeg_len = b - a;
    return 0;
}
