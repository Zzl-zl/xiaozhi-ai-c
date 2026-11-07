# -*- coding: utf-8 -*-
"""生成 160x160 的 8 帧动画：摇摆 + 浮动 + 眨眼（用设定集闭眼贴图）"""
import os
from PIL import Image

D = r'D:\deepseek个人规划\themes\whale'
SZ = 160
base = Image.open(os.path.join(D, 'main_char_160.png')).convert('RGBA')
assert base.size == (SZ, SZ), base.size

# ---- 尝试用设定集 Laughing（闭眼）做眨眼帧 ----
laugh = Image.open(os.path.join(D, 'crops', 'emo_laugh_cut.png')).convert('RGBA')
print('laugh size', laugh.size)

# 主角色 160 尺寸下，脸部/眼睛位置按 200->160 比例：x*0.8, y*0.8
# 200 图眼睛中心 (82,97) (118,97) -> 160 图 (66,78) (94,78)
EYES = [(66, 78), (94, 78)]
EYE_BOX_160 = (48, 60, 112, 96)   # 160 坐标下的眼部区域

# 从 laugh 图里裁闭眼区域（laugh 是 188x236，闭眼在脸部中上）
# 闭眼大约在他的 y 95-140, x 35-155
eye_cut = laugh.crop((28, 88, 162, 148))
scale_w = EYE_BOX_160[2] - EYE_BOX_160[0]
scale_h = EYE_BOX_160[3] - EYE_BOX_160[1]
eye_cut = eye_cut.resize((scale_w, scale_h), Image.LANCZOS)

blink = base.copy()
blink.alpha_composite(eye_cut, (EYE_BOX_160[0], EYE_BOX_160[1]))
blink.save(os.path.join(D, 'main_char_160_blink.png'))

cmp = Image.new('RGB', (360, 200), (14, 24, 42))
a = base.resize((160, 160), Image.LANCZOS)
b = blink.resize((160, 160), Image.LANCZOS)
cmp.paste(a, (10, 20), a)
cmp.paste(b, (190, 20), b)
cmp.save(os.path.join(D, 'preview_blink4.png'))

# ---- 8 帧 ----
PLAN = [
    (0.0, 0, 0, False),
    (-0.8, -1, 0, False),
    (-1.6, -2, 1, False),
    (-1.0, -1, 1, False),
    (0.0, 0, 0, True),     # 眨眼
    (0.8, 1, 0, False),
    (1.6, 2, 1, False),
    (1.0, 1, 1, False),
]
os.makedirs(os.path.join(D, 'frames160'), exist_ok=True)
frames = []
for i, (ang, dx, dy, do_blink) in enumerate(PLAN):
    src = blink if do_blink else base
    pad = 8
    canvas = Image.new('RGBA', (SZ + pad * 2, SZ + pad * 2), (0, 0, 0, 0))
    canvas.alpha_composite(src, (pad, pad))
    rot = canvas.rotate(ang, resample=Image.BICUBIC, center=(SZ // 2 + pad, SZ // 2 + pad))
    out = Image.new('RGBA', (SZ, SZ), (0, 0, 0, 0))
    out.alpha_composite(rot.crop((pad, pad, pad + SZ, pad + SZ)))
    if dx or dy:
        sh = Image.new('RGBA', (SZ, SZ), (0, 0, 0, 0))
        sh.alpha_composite(out, (dx, dy))
        out = sh
    out.save(os.path.join(D, 'frames160', 'f%02d.png' % i))
    frames.append(out)

gif = []
for f in frames:
    bgs = Image.new('RGB', (256, 256), (12, 26, 46))
    ff = f.resize((256, 256), Image.LANCZOS)
    bgs.paste(ff, (0, 0), ff)
    gif.append(bgs)
gif[0].save(os.path.join(D, 'preview_anim160.gif'), save_all=True,
            append_images=gif[1:], duration=280, loop=0)
print('done', len(frames))
