# UI 素材源（鲸鱼娘主题）

这里的 PNG 是**可直接编辑的源素材**。改完后运行 `..\tools\build_ui.py`，
会自动重新生成 `main/whale_anim.c` 和 `main/whale_assets.c`，再编译即可。

## 目录

```
ui_source/
├── assets/     最终素材（脚本从这里读）
│   ├── main_char_160.png   主界面角色基础图   160x160  RGBA
│   ├── whale_icon_64.png   DeepSeek 鲸鱼图标   64x64    RGBA
│   ├── boot_240.png        开机界面持剑图     240x240  RGB
│   ├── chatbg_240.png      聊天界面背景       240x240  RGB（已压暗模糊）
│   └── emo_{neutral,happy,laugh,pout}_64.png  聊天表情  64x64 RGBA
├── frames/     主界面动画帧，必须 8 张 f00~f07.png，160x160 RGBA
├── frames_boot/ 开机动画帧，30 张 b00~b29.png，240x240 RGB（打包成 assets.bin）
├── raw/        原始素材（设定集、用户提供的大图），仅供重新裁切参考
└── preview/    效果预览图（不参与生成）
```

## 修改流程

1. 直接改 `assets/` 或 `frames/` 里的 PNG（保持上面的尺寸）
2. 运行：`python ..\tools\build_ui.py`（或 `D:\xiaozhi-c\tools\build_ui.py`）
3. 编译烧录：双击 `build.bat` → `flash.bat`

脚本会校验尺寸，尺寸不对会直接报错并指出是哪张图。

## 想调整动画

- **帧数**：改 `tools/build_ui.py` 顶部 `FRAME_COUNT`（同时 frames 目录要有对应张数），
  并同步改 `display.c` 里 `ANIM_DSC(0..7)` 的定义数量和 `s_anim_dsc[]` 数组
- **播放速度**：改 `display.c` 里 `now_ms - s_anim_last_ms >= 280` 的 280（毫秒）
- **角色大小**：改 `FRAME_SIZE` + 重新生成 + 改 `display.c` 里 `lv_obj_set_pos(s_whale, 40, 44)` 的坐标
- **背景/开机图**：直接替换同名 PNG 即可（240x240）

## RGB565 转换说明

脚本把 PNG 转成 LVGL 原始格式：
- 带透明的（角色/表情/图标）→ `RGB565A8`（先 RGB565 再 alpha，共 3 字节/像素）
- 不透明的（开机图/聊天背景）→ `RGB565`（2 字节/像素，小端）

flash 占用（当前配置）：帧动画 8×160×160×3 ≈ 614KB，其余素材 ≈ 220KB。
