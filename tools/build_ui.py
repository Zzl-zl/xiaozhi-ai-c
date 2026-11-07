# -*- coding: utf-8 -*-
"""
从 ui_source/ 素材重新生成 LVGL C 数组（main/whale_anim.c、main/whale_assets.c）

用法：
    python tools/build_ui.py

改素材流程：
    1. 直接编辑 ui_source/assets/*.png 或 ui_source/frames/f*.png
       （保持尺寸：assets 里各图尺寸见 README；frames 必须 160x160 且 8 张）
    2. 运行本脚本
    3. idf.py build / 双击 build.bat
"""
import glob
import os
import sys

from PIL import Image

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(BASE, 'ui_source')
OUT = os.path.join(BASE, 'main')

FRAME_SIZE = 160
FRAME_COUNT = 8
ICON_SIZE = 64
BG_SIZE = 240
EMO_SIZE = 64
MAIN_SRC_SIZE = 160


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def to_rgb565(im):
    im = im.convert('RGB')
    w, h = im.size
    px = im.load()
    out = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = rgb565(r, g, b)
            out.append(v & 0xFF)
            out.append((v >> 8) & 0xFF)
    return bytes(out)


def to_rgb565a8(im):
    """LVGL RGB565A8: 先 w*h*2 RGB565(小端)，再 w*h alpha"""
    im = im.convert('RGBA')
    w, h = im.size
    px = im.load()
    rgb = bytearray()
    alpha = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            v = rgb565(r, g, b)
            rgb.append(v & 0xFF)
            rgb.append((v >> 8) & 0xFF)
            alpha.append(a)
    return bytes(rgb) + bytes(alpha)


def write_arr(f, name, data, per=32):
    f.write('const uint8_t %s[%d] = {\n' % (name, len(data)))
    for i in range(0, len(data), per):
        f.write(','.join('0x%02X' % b for b in data[i:i + per]) + ',\n')
    f.write('};\n\n')


def load(path, size, what):
    if not os.path.exists(path):
        sys.exit('缺少素材: %s' % path)
    im = Image.open(path)
    if im.size != (size, size):
        sys.exit('%s 尺寸应为 %dx%d，实际 %s' % (what, size, size, im.size))
    return im


def build_anim():
    files = sorted(glob.glob(os.path.join(UI, 'frames', 'f*.png')))
    if len(files) != FRAME_COUNT:
        sys.exit('frames 目录应有 %d 张 f*.png，实际 %d 张' % (FRAME_COUNT, len(files)))
    frames = [to_rgb565a8(load(p, FRAME_SIZE, os.path.basename(p))) for p in files]
    icon = to_rgb565a8(load(os.path.join(UI, 'assets', 'whale_icon_64.png'),
                            ICON_SIZE, 'whale_icon_64.png'))

    h_path = os.path.join(OUT, 'whale_anim.h')
    c_path = os.path.join(OUT, 'whale_anim.c')
    with open(h_path, 'w', encoding='utf-8') as f:
        f.write('#pragma once\n#include <stdint.h>\n\n')
        f.write('#define WHALE_ANIM_SZ %d\n#define WHALE_ANIM_FRAMES %d\n#define WHALE_ICON_SZ %d\n\n'
                % (FRAME_SIZE, FRAME_COUNT, ICON_SIZE))
        for i in range(FRAME_COUNT):
            f.write('extern const uint8_t whale_frame_%d[];\n' % i)
        f.write('extern const uint8_t whale_icon_data[];\n')

    with open(c_path, 'w', encoding='utf-8') as f:
        f.write('/* 鲸鱼娘 %d 帧动画（%dx%d RGB565A8）+ 鲸鱼图标 —— 由 tools/build_ui.py 生成 */\n'
                % (FRAME_COUNT, FRAME_SIZE, FRAME_SIZE))
        f.write('#include "whale_anim.h"\n\n')
        for i, fr in enumerate(frames):
            f.write('/* frame %d */\n' % i)
            write_arr(f, 'whale_frame_%d' % i, fr)
        f.write('/* whale icon */\n')
        write_arr(f, 'whale_icon_data', icon)
    print('whale_anim.c  %d 帧 + 图标 -> %.1f MB'
          % (FRAME_COUNT, os.path.getsize(c_path) / 1048576.0))


def build_assets():
    A = os.path.join(UI, 'assets')
    main = to_rgb565a8(load(os.path.join(A, 'main_char_160.png'), MAIN_SRC_SIZE, 'main_char_160.png'))
    emos = ['neutral', 'happy', 'laugh', 'pout']
    emo_data = {k: to_rgb565a8(load(os.path.join(A, 'emo_%s_64.png' % k), EMO_SIZE, 'emo_%s_64.png' % k))
                for k in emos}
    chatbg = to_rgb565(load(os.path.join(A, 'chatbg_240.png'), BG_SIZE, 'chatbg_240.png'))
    boot = to_rgb565(load(os.path.join(A, 'boot_240.png'), BG_SIZE, 'boot_240.png'))

    h_path = os.path.join(OUT, 'whale_assets.h')
    c_path = os.path.join(OUT, 'whale_assets.c')
    with open(h_path, 'w', encoding='utf-8') as f:
        f.write('#pragma once\n#include <stdint.h>\n\n')
        f.write('#define WHALE_MAIN_W %d\n#define WHALE_MAIN_H %d\n' % (MAIN_SRC_SIZE, MAIN_SRC_SIZE))
        f.write('#define WHALE_EMO_W %d\n#define WHALE_EMO_H %d\n' % (EMO_SIZE, EMO_SIZE))
        f.write('#define WHALE_BG_W %d\n#define WHALE_BG_H %d\n\n' % (BG_SIZE, BG_SIZE))
        f.write('extern const uint8_t whale_main_data[];\n')
        for k in emos:
            f.write('extern const uint8_t whale_emo_%s_data[];\n' % k)
        f.write('extern const uint8_t whale_chatbg_data[];\n')
        f.write('extern const uint8_t whale_boot_data[];\n')

    with open(c_path, 'w', encoding='utf-8') as f:
        f.write('/* 鲸鱼娘静态素材 —— 由 tools/build_ui.py 生成 */\n')
        f.write('#include "whale_assets.h"\n\n')
        f.write('/* 主界面角色基础图 */\n')
        write_arr(f, 'whale_main_data', main)
        for k in emos:
            f.write('/* 表情 %s */\n' % k)
            write_arr(f, 'whale_emo_%s_data' % k, emo_data[k])
        f.write('/* 聊天背景 */\n')
        write_arr(f, 'whale_chatbg_data', chatbg)
        f.write('/* 开机持剑图 */\n')
        write_arr(f, 'whale_boot_data', boot)
    print('whale_assets.c -> %.1f MB' % (os.path.getsize(c_path) / 1048576.0))


if __name__ == '__main__':
    build_anim()
    build_assets()
    print('完成。接下来编译：D:\\xiaozhi-c\\build.ps1')
