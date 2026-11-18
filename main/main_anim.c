/* 主界面 JPEG 帧表解析 */
#include "main_anim.h"

int main_anim_get_frame(const uint8_t *bin, uint32_t bin_size, int idx,
                         const uint8_t **jpeg, uint32_t *jpeg_len)
{
    if(bin == NULL || bin_size < 20u) {
        return -1;
    }
    const uint32_t *h = (const uint32_t *)bin;
    if(h[0] != MAIN_ANIM_MAGIC || (int)h[1] != MAIN_ANIM_N) {
        return -2;
    }
    if(idx < 0 || idx >= MAIN_ANIM_N) {
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
