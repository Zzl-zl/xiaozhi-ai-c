# -*- coding: utf-8 -*-
"""最终版开机 30 帧：长发平滑飘动 + 眼线渐变眨眼（用原持剑图）"""
import math
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

D = r'D:\deepseek个人规划\themes\whale'
N = 30
base = Image.open(os.path.join(D, 'boot_240.png')).convert('RGB')
W, H = base.size

a = np.array(base).astype(np.float32)
yy = np.arange(H)[:, None].astype(np.float32)
xx = np.arange(W)[None, :].astype(np.float32)
mid = W / 2.0
side = np.clip((np.abs(xx - mid) - 30) / 60.0, 0, 1) ** 0.8
vert = np.clip((yy - 55) / 120.0, 0, 1)
WGT = (side * vert).astype(np.float32)
center_keep = ((xx > 62) & (xx < 150)).astype(np.float32)
WGT = WGT * (1.0 - 0.85 * center_keep)

# 眼睛区域（原图坐标）
EX0, EY0, EX1, EY1 = 137, 99, 181, 130
ECX, ECY = (EX0 + EX1) // 2, (EY0 + EY1) // 2

# 眨眼：帧 -> 眼线粗细（0 = 睁眼）
BLINK = {9: 3, 10: 6, 11: 8, 12: 8, 13: 6, 14: 4}


def make_wave(t):
    phase = 2.0 * math.pi * t / N
    dy_phase = 2.0 * math.pi * (yy / H) * 0.55
    dx = 4.0 * WGT * np.sin(phase + dy_phase)
    src_x = np.clip(xx - dx, 0, W - 1).astype(np.int32)
    idx = (yy.astype(np.int32) * W + src_x).ravel()
    out = a.reshape(-1, 3)[idx].reshape(H, W, 3)
    return Image.fromarray(out.clip(0, 255).astype(np.uint8))


def add_blink(img, thick):
    layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.arc([EX0, ECY - 10, EX1, ECY + 10], start=18, end=162,
          fill=(18, 22, 38, 255), width=thick)
    d.arc([EX0 + 3, ECY - 2, EX1 - 3, ECY + 12], start=200, end=340,
          fill=(22, 26, 42, 230), width=max(2, thick // 3))
    layer = layer.filter(ImageFilter.GaussianBlur(0.8))
    out = img.convert('RGBA')
    out.alpha_composite(layer)
    return out.convert('RGB')


frames = []
for t in range(N):
    im = make_wave(t)
    thick = BLINK.get(t, 0)
    if thick:
        im = add_blink(im, thick)
    frames.append(im)

outdir = os.path.join(D, 'boot_frames3')
os.makedirs(outdir, exist_ok=True)
for i, f in enumerate(frames):
    f.save(os.path.join(outdir, 'd%02d.png' % i))

gif = [f.resize((256, 256), Image.LANCZOS) for f in frames]
gif[0].save(os.path.join(D, 'preview_boot_anim3.gif'), save_all=True,
            append_images=gif[1:], duration=120, loop=0)

# 关键帧拼图
key = [0, 5, 10, 15, 20, 25]
sheet = Image.new('RGB', (3 * 250, 2 * 270), (10, 12, 18))
for i, k in enumerate(key):
    im = frames[k].resize((240, 240), Image.LANCZOS)
    sheet.paste(im, ((i % 3) * 250 + 5, (i // 3) * 270 + 20))
sheet.save(os.path.join(D, 'preview_boot3_keyframes.png'))
print('done', len(frames))
