# -*- coding: utf-8 -*-
"""
开机动画帧压缩打包器（JPEG 帧方案）

参考 HPT_707 的资源生成思路：图片 → 压缩 → 生成 .c/.h + 数据文件。
杰理的私有压缩 DLL 在 ESP32 上不可用，这里改用标准 JPEG：
240x240 质量 85 约 10~16KB/帧，50 帧不到 1MB，配合项目已有的 esp_jpeg 组件解码。

输入：ui_source/frames_boot/b*.png（240x240 RGB）
输出：
  main/boot_anim.bin    JPEG 帧数据（烧到 assets 分区 0xB00000）
  main/boot_anim.h      帧数/尺寸接口
  main/boot_anim.c      帧表辅助函数（解析 bin 头）

bin 布局（小端）：
  u32 magic        'BOJP'
  u32 frame_count
  u32 width
  u32 height
  u32 offsets[N+1] 每帧 JPEG 在 bin 内的偏移
  u8  data[]       JPEG 数据
"""
import glob
import io
import os
import struct
import sys

from PIL import Image

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(BASE, 'ui_source')
OUT = os.path.join(BASE, 'main')
SIZE = 240
QUALITY = 85
MAGIC = 0x504A4F42  # 'BOJP' little-endian


def main():
    files = sorted(glob.glob(os.path.join(UI, 'frames_boot', 'b*.png')))
    if not files:
        sys.exit('缺少 ui_source/frames_boot/b*.png')
    blobs = []
    for p in files:
        im = Image.open(p)
        if im.size != (SIZE, SIZE):
            sys.exit('%s 尺寸应为 %dx%d，实际 %s' % (os.path.basename(p), SIZE, SIZE, im.size))
        buf = io.BytesIO()
        im.convert('RGB').save(buf, 'JPEG', quality=QUALITY, optimize=True, subsampling=2)
        blobs.append(buf.getvalue())

    n = len(blobs)
    head = struct.pack('<IIII', MAGIC, n, SIZE, SIZE)
    offsets = [0] * (n + 1)
    body = bytearray()
    base = 16 + 4 * (n + 1)
    for i, b in enumerate(blobs):
        offsets[i] = base + len(body)
        body += b
    offsets[n] = base + len(body)

    bin_path = os.path.join(OUT, 'boot_anim.bin')
    with open(bin_path, 'wb') as fp:
        fp.write(head)
        fp.write(struct.pack('<%dI' % (n + 1), *offsets))
        fp.write(body)
    total = os.path.getsize(bin_path)

    h_path = os.path.join(OUT, 'boot_anim.h')
    with open(h_path, 'w', encoding='utf-8') as fp:
        fp.write('#pragma once\n#include <stdint.h>\n\n')
        fp.write('/* 由 tools/pack_boot_anim.py 生成，勿手改（JPEG 帧资源） */\n')
        fp.write('#define BOOT_ANIM_W %d\n' % SIZE)
        fp.write('#define BOOT_ANIM_H %d\n' % SIZE)
        fp.write('#define BOOT_ANIM_N %d\n' % n)
        fp.write('#define BOOT_ANIM_RGB_BYTES (BOOT_ANIM_W * BOOT_ANIM_H * 2)\n')
        fp.write('#define BOOT_ANIM_MAGIC 0x504A4F42u\n\n')
        fp.write('/* 从映射后的 bin 里取第 idx 帧的 JPEG 指针和长度，成功返回 0 */\n')
        fp.write('int boot_anim_get_frame(const uint8_t *bin, uint32_t bin_size, int idx,\n')
        fp.write('                        const uint8_t **jpeg, uint32_t *jpeg_len);\n')

    c_path = os.path.join(OUT, 'boot_anim.c')
    with open(c_path, 'w', encoding='utf-8') as fp:
        fp.write('/* JPEG 帧表解析（对应 tools/pack_boot_anim.py 的 bin 布局） */\n')
        fp.write('#include "boot_anim.h"\n\n')
        fp.write('int boot_anim_get_frame(const uint8_t *bin, uint32_t bin_size, int idx,\n')
        fp.write('                        const uint8_t **jpeg, uint32_t *jpeg_len)\n{\n')
        fp.write('    if(bin == NULL || bin_size < 16 + 4) {\n')
        fp.write('        return -1;\n')
        fp.write('    }\n')
        fp.write('    const uint32_t *h = (const uint32_t *)bin;\n')
        fp.write('    if(h[0] != BOOT_ANIM_MAGIC || (int)h[1] != BOOT_ANIM_N) {\n')
        fp.write('        return -2;\n')
        fp.write('    }\n')
        fp.write('    if(idx < 0 || idx >= BOOT_ANIM_N) {\n')
        fp.write('        return -3;\n')
        fp.write('    }\n')
        fp.write('    const uint32_t *off = (const uint32_t *)(bin + 16);\n')
        fp.write('    uint32_t a = off[idx];\n')
        fp.write('    uint32_t b = off[idx + 1];\n')
        fp.write('    if(a >= b || b > bin_size) {\n')
        fp.write('        return -4;\n')
        fp.write('    }\n')
        fp.write('    *jpeg = bin + a;\n')
        fp.write('    *jpeg_len = b - a;\n')
        fp.write('    return 0;\n')
        fp.write('}\n')

    avg = sum(len(b) for b in blobs) / float(n) / 1024.0
    print('帧数        : %d' % n)
    print('每帧 JPEG   : 平均 %.1f KB（质量 %d）' % (avg, QUALITY))
    print('boot_anim.bin: %.2f MB（assets 分区 5.00 MB，余量 %.2f MB）' % (
        total / 1048576.0, (5 * 1048576 - total) / 1048576.0))
    print('生成: main/boot_anim.bin / boot_anim.h / boot_anim.c')


if __name__ == '__main__':
    main()
