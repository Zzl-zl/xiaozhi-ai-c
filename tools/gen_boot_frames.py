# -*- coding: utf-8 -*-
"""为开机持剑图生成 30 帧：头发波浪飘动 + 眨眼"""
import math
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

D = r'D:\deepseek个人规划\themes\whale'
N = 30
src = Image.open(os.path.join(D, 'boot_240.png')).convert('RGB')
W, H = src.size
assert (W, H) == (240, 240), src.size


def find_eye(im):
    """粗略找可见的眼睛（右半脸浅色区域内的深色椭圆）"""
    a = np.array(im.convert('RGB')).astype(int)
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    # 皮肤：偏白
    skin = (r > 170) & (g > 150) & (b > 150)
    # 眼睛：深蓝/黑，位于皮肤包围内
    dark = (r < 90) & (g < 110) & (b < 160)
    ys, xs = np.nonzero(skin)
    if len(xs) == 0:
        return None
    return (xs.min(), xs.max(), ys.min(), ys.max())


print('skin/eye probe:', find_eye(src))

# ---- 头发波浪位移 ----
a = np.array(src).astype(np.float32)
h, w = a.shape[:2]
yy = np.arange(h)[:, None]
xx = np.arange(w)[None, :]

# 头发区域权重：越靠上、越靠两侧，位移越大；中间脸部和剑少动
def region_weight():
    top = np.clip((200 - yy) / 160.0, 0, 1)      # 上部多动
    side = np.clip((np.abs(xx - w / 2) - 25) / 70.0, 0, 1)  # 两侧多动
    return (top * side).astype(np.float32)

WGT = region_weight()


def make_frame(t):
    phase = 2 * math.pi * t / N
    amp = 3.2
    dx = amp * WGT * np.sin(phase + yy * 0.045)
    src_x = np.clip(xx - dx, 0, w - 1).astype(np.int32)
    idx = (yy.astype(np.int32) * w + src_x)
    out = a.reshape(-1, 3)[idx.ravel()].reshape(h, w, 3)
    return out


frames = []
for t in range(N):
    out = make_frame(t)
    im = Image.fromarray(out.clip(0, 255).astype(np.uint8))
    frames.append(im)

# ---- 眨眼：用暗色覆盖眼睛并画闭合线 ----
# 眼睛大致位置（由 probe 结果人工微调）
EYE = (165, 112, 18, 13)   # cx, cy, rx, ry
blink_src = frames[0].copy()
# 用局部高斯模糊消除眼睛细节（过渡自然），再画闭合弧线
box = (EYE[0] - EYE[2] - 2, EYE[1] - EYE[3] - 2,
       EYE[0] + EYE[2] + 2, EYE[1] + EYE[3] + 2)
region = blink_src.crop(box).filter(ImageFilter.GaussianBlur(6))
blink_src.paste(region, box)
bd = ImageDraw.Draw(blink_src)
bd.arc([EYE[0] - EYE[2] + 2, EYE[1] - 7, EYE[0] + EYE[2] - 2, EYE[1] + 9],
       start=15, end=165, fill=(40, 44, 66), width=3)
blink_src.save(os.path.join(D, 'boot_blink_frame.png'))

# 眨眼插在 12/13 帧
BLINK_AT = {12, 13}
for i in BLINK_AT:
    frames[i] = blink_src.copy()

os.makedirs(os.path.join(D, 'boot_frames'), exist_ok=True)
for i, f in enumerate(frames):
    f.save(os.path.join(D, 'boot_frames', 'b%02d.png' % i))

# GIF 预览
gif = [f.resize((256, 256), Image.LANCZOS) for f in frames]
gif[0].save(os.path.join(D, 'preview_boot_anim.gif'), save_all=True,
            append_images=gif[1:], duration=100, loop=0)
print('frames', len(frames))
