# -*- coding: utf-8 -*-
"""
主界面动画帧打包器（JPEG，黑底 240x240）

输入：ui_source/frames_main/m*.png（240x240 RGB，黑底）
输出：
  main/main_anim.bin    JPEG 帧数据（烧到 assets 分区 0xC00000）
  main/main_anim.h      帧数/尺寸接口
  main/main_anim.c      帧表解析函数

bin 布局与 boot_anim.bin 相同：
  u32 magic 'MAIN'
  u32 frame_count
  u32 width
  u32 height
  u32 offsets[N+1]
  u8  data[]
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
MAGIC = 0x4E49414D  # 'MAIN' little-endian


def main():
    files = sorted(glob.glob(os.path.join(UI, 'frames_main', 'm*.png')))
    if not files:
        sys.exit('缺少 ui_source/frames_main/m*.png')
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

    bin_path = os.path.join(OUT, 'main_anim.bin')
    with open(bin_path, 'wb') as fp:
        fp.write(head)
        fp.write(struct.pack('<%dI' % (n + 1), *offsets))
        fp.write(body)
    total = os.path.getsize(bin_path)

    h_path = os.path.join(OUT, 'main_anim.h')
    with open(h_path, 'w', encoding='utf-8') as fp:
        fp.write('#pragma once\n#include <stdint.h>\n\n')
        fp.write('/* 由 tools/pack_main_anim.py 生成（主界面 JPEG 帧） */\n')
        fp.write('#define MAIN_ANIM_W %d\n' % SIZE)
        fp.write('#define MAIN_ANIM_H %d\n' % SIZE)
        fp.write('#define MAIN_ANIM_N %d\n' % n)
        fp.write('#define MAIN_ANIM_RGB_BYTES (MAIN_ANIM_W * MAIN_ANIM_H * 2)\n')
        fp.write('#define MAIN_ANIM_MAGIC 0x4E49414Du\n\n')
        fp.write('int main_anim_get_frame(const uint8_t *bin, uint32_t bin_size, int idx,\n')
        fp.write('                         const uint8_t **jpeg, uint32_t *jpeg_len);\n')

    c_path = os.path.join(OUT, 'main_anim.c')
    with open(c_path, 'w', encoding='utf-8') as fp:
        fp.write('/* 主界面 JPEG 帧表解析 */\n')
        fp.write('#include "main_anim.h"\n\n')
        fp.write('int main_anim_get_frame(const uint8_t *bin, uint32_t bin_size, int idx,\n')
        fp.write('                         const uint8_t **jpeg, uint32_t *jpeg_len)\n{\n')
        fp.write('    if(bin == NULL || bin_size < 20u) {\n        return -1;\n    }\n')
        fp.write('    const uint32_t *h = (const uint32_t *)bin;\n')
        fp.write('    if(h[0] != MAIN_ANIM_MAGIC || (int)h[1] != MAIN_ANIM_N) {\n        return -2;\n    }\n')
        fp.write('    if(idx < 0 || idx >= MAIN_ANIM_N) {\n        return -3;\n    }\n')
        fp.write('    const uint32_t *off = (const uint32_t *)(bin + 16);\n')
        fp.write('    uint32_t a = off[idx];\n')
        fp.write('    uint32_t b = off[idx + 1];\n')
        fp.write('    if(a >= b || b > bin_size) {\n        return -4;\n    }\n')
        fp.write('    *jpeg = bin + a;\n')
        fp.write('    *jpeg_len = b - a;\n')
        fp.write('    return 0;\n}\n')

    avg = sum(len(b) for b in blobs) / float(n) / 1024.0
    print('主界面帧数  : %d' % n)
    print('每帧 JPEG   : 平均 %.1f KB' % avg)
    print('main_anim.bin: %.2f MB（烧到 0xC00000）' % (total / 1048576.0))
    print('生成: main/main_anim.bin / .h / .c')


if __name__ == '__main__':
    main()
