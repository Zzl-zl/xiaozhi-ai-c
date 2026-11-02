# xiaozhi-c

小智 AI 语音助手 —— ESP32-S3 的 **C 语言移植版**（原版为 C++ / xiaozhi-esp32）。

## 硬件
- 面包板 DIY ESP32-S3-CAM（16MB Flash / 8MB OCT PSRAM）
- 麦克风 I2S 输入 @16kHz，扬声器 I2S 输出（MAX98357 类功放）
- ST7789 240x240 SPI 屏（SPI 80MHz）
- OV3660 摄像头（拍照识图）

## 功能
- 语音唤醒（esp-sr WakeNet9「你好小智」）+ ASR/LLM/TTS 对话
- 连续对话（说完自动续听 6 秒）
- 赛博朋克 UI：纯黑背景 + 全息机器人脸 + 2.5D 星球魔方 + 时钟/天气 + 表情
- 独立音乐界面（呼吸圆环 + 频谱条），支持：
  - 语音点歌（`self.music.play` / `self.music.search`）+ 手机网页推流/上传
  - 音量控制（`self.music.volume` / `volume_up` / `volume_down`）、停止
  - 手机控制网页 `http://<设备IP>/`
- 摄像头拍照识图（MCP 工具 `self.camera.take_photo`，照片显示在聊天框）
- MCP 工具：`self.led.set` / `self.camera.take_photo` / `self.music.*`

## 目录
- `main/` 源码：display.c（UI）、mic/speaker/opus_codec（音频）、wake_word.c（唤醒）、
  xiaozhi_app.c（对话业务）、music.c（在线音乐）、camera.c（拍照识图）、mcp_tools.c、weather.c 等
- `components/` 本地组件：esp32-camera、esp_jpeg、esp_xiaozhi
- `docs/开发进度.md` 开发日志（含踩坑记录）
- `build.ps1` / `flash.ps1` / `monitor.ps1` 一键脚本（默认 COM5）

## 编译 / 烧录
```powershell
# 需要 ESP-IDF v6.1.0（D:\Espressif\esp-idf）
.\build.ps1
.\flash.ps1      # COM5
.\monitor.ps1
```

首次使用前：复制 `main/wifi_config.example.h` 为 `main/wifi_config.h` 并填入 Wi-Fi。

## 已知问题（进行中）
- 音乐播放中唤醒偶发卡死（暂停/停止状态机死锁，排查中）
- 空闲界面约 20~30fps，可继续优化渲染
