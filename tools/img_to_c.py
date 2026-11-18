#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
通用图片 → LVGL C 数组生成器

把目录里的 PNG/JPG 图片批量转成 LVGL 可用的 C 数组（.c / .h）。
参考 HPT_707_ui_make.py 的资源生成思路，但输出 LVGL 原生格式。

特点
----
- 自动识别 PNG / JPG / JPEG / WEBP / BMP（按文件内容识别，不看扩展名）
- PNG 带透明通道 → RGB565A8（3 字节/像素：先 RGB565 再 8bit alpha）
- JPG / 不透明图  → RGB565（2 字节/像素，小端）
- 文件名自动转成合法 C 符号
- 可选缩放到统一尺寸
- 可强制输出格式

用法
----
    python img_to_c.py <图片目录> [选项]

选项
----
    -o, --out DIR    输出目录（默认 <图片目录>/generated_c）
    --prefix STR     符号前缀（如 ui_），默认空
    --size N         缩放到 N×N 正方形（默认保持原尺寸）
    --opaque         强制 RGB565（丢弃透明通道）
    --alpha          强制 RGB565A8（不透明处 alpha 补 255）
    --recursive      递归子目录

输出
----
    <out>/<name>.c   uint8_t 数组
    <out>/<name>.h   宽高宏 + extern 声明

示例
----
    python img_to_c.py ../ui_source/assets -o ../main/generated --prefix ui_
    python img_to_c.py ./icons --size 64 --opaque
"""
import argparse
import os
import re
import sys

from PIL import Image

EXTS = ('.png', '.jpg', '.jpeg', '.webp', '.bmp', '.gif')


def sanitize(name):
    base = os.path.splitext(os.path.basename(name))[0]
    s = re.sub(r'[^0-9a-zA-Z_]', '_', base).strip('_').lower()
    s = re.sub(r'_+', '_', s)
    if not s:
        s = 'img'
    if s[0].isdigit():
        s = 'img_' + s
    return s


def has_alpha(im):
    if im.mode in ('RGBA', 'LA', 'PA'):
        return True
    if im.mode == 'P' and 'transparency' in im.info:
        return True
    return False


def to_rgb565_bytes(im):
    im = im.convert('RGB')
    w, h = im.size
    px = im.load()
    out = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out.append(v & 0xFF)
            out.append((v >> 8) & 0xFF)
    return bytes(out)


def to_rgb565a8_bytes(im):
    im = im.convert('RGBA')
    w, h = im.size
    px = im.load()
    rgb = bytearray()
    alpha = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            rgb.append(v & 0xFF)
            rgb.append((v >> 8) & 0xFF)
            alpha.append(a)
    return bytes(rgb) + bytes(alpha)


def write_c_array(fp, sym, data, per_line=32):
    fp.write('const uint8_t %s[%d] = {\n' % (sym, len(data)))
    for i in range(0, len(data), per_line):
        fp.write(','.join('0x%02X' % b for b in data[i:i + per_line]) + ',\n')
    fp.write('};\n')


def guard_of(sym):
    return ('IMG_%s_H' % sym).upper()


def process(path, out_dir, prefix, size, force_opaque, force_alpha):
    im = Image.open(path)
    fmt_src = im.format or '?'
    if size:
        im = im.resize((size, size), Image.LANCZOS)
    use_alpha = has_alpha(im)
    if force_opaque:
        use_alpha = False
    if force_alpha:
        use_alpha = True

    if use_alpha:
        data = to_rgb565a8_bytes(im)
        cf = 'RGB565A8'
        bytes_per_px = 3
    else:
        data = to_rgb565_bytes(im)
        cf = 'RGB565'
        bytes_per_px = 2

    w, h = im.size
    sym = prefix + sanitize(path)
    c_path = os.path.join(out_dir, sym + '.c')
    h_path = os.path.join(out_dir, sym + '.h')

    with open(c_path, 'w', encoding='utf-8') as fp:
        fp.write('/* %s  (%s, %dx%d, %s) —— 由 tools/img_to_c.py 生成 */\n'
                 % (os.path.basename(path), fmt_src, w, h, cf))
        fp.write('#include "%s.h"\n\n' % sym)
        write_c_array(fp, sym + '_data', data)

    g = guard_of(sym)
    with open(h_path, 'w', encoding='utf-8') as fp:
        fp.write('#pragma once\n#include <stdint.h>\n\n')
        fp.write('#ifndef %s\n#define %s\n\n' % (g, g))
        fp.write('#define %s_W %d\n' % (sym.upper(), w))
        fp.write('#define %s_H %d\n' % (sym.upper(), h))
        fp.write('#define %s_BPP %d\n' % (sym.upper(), bytes_per_px))
        fp.write('extern const uint8_t %s_data[];\n\n' % sym)
        fp.write('#endif /* %s */\n' % g)

    return sym, fmt_src, w, h, cf, len(data)


def main():
    ap = argparse.ArgumentParser(
        description='PNG/JPG → LVGL C 数组生成器',
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input', help='图片目录')
    ap.add_argument('-o', '--out', help='输出目录（默认 <输入>/generated_c）')
    ap.add_argument('--prefix', default='', help='符号前缀（如 ui_）')
    ap.add_argument('--size', type=int, default=0, help='缩放到 N×N')
    ap.add_argument('--opaque', action='store_true', help='强制 RGB565')
    ap.add_argument('--alpha', action='store_true', help='强制 RGB565A8')
    ap.add_argument('--recursive', action='store_true', help='递归子目录')
    args = ap.parse_args()

    if not os.path.isdir(args.input):
        sys.exit('不是目录: %s' % args.input)
    out_dir = args.out or os.path.join(args.input, 'generated_c')
    os.makedirs(out_dir, exist_ok=True)

    files = []
    if args.recursive:
        for root, _, names in os.walk(args.input):
            for n in names:
                if n.lower().endswith(EXTS):
                    files.append(os.path.join(root, n))
    else:
        for n in os.listdir(args.input):
            p = os.path.join(args.input, n)
            if os.path.isfile(p) and n.lower().endswith(EXTS):
                files.append(p)
    files.sort()
    if not files:
        sys.exit('目录里没有图片: %s' % args.input)

    total = 0
    print('%-28s %-8s %-10s %-10s %s' % ('文件', '格式', '尺寸', '输出格式', '字节'))
    print('-' * 78)
    for p in files:
        try:
            sym, fmt, w, h, cf, n = process(
                p, out_dir, args.prefix, args.size, args.opaque, args.alpha)
            total += n
            print('%-28s %-8s %-10s %-10s %d' % (
                os.path.basename(p)[:28], fmt, '%dx%d' % (w, h), cf, n))
        except Exception as e:
            print('%-28s 失败: %s' % (os.path.basename(p)[:28], e))
    print('-' * 78)
    print('共 %d 个文件，数据 %d 字节（%.1f KB）' % (len(files), total, total / 1024.0))
    print('输出目录: %s' % os.path.abspath(out_dir))


if __name__ == '__main__':
    main()
