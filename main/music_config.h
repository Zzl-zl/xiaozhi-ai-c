#pragma once

/**
 * @file music_config.h
 * 在线音乐配置（WiFi 拉流 → 本地 MP3 解码 → 板载喇叭播放）
 *
 * 说明：
 *  - 默认曲目用网易云音乐外链：https://music.163.com/song/media/outer/url?id=<歌曲ID>.mp3
 *    把 id= 后面的数字换成你想听的歌即可（非 VIP/版权受限歌曲一般可用）
 *  - 也可以填任意直链 MP3（http/https 均支持）
 *  - 手机也可以打开设备网页（串口日志里会打印 IP）推送任意 URL 或上传本地文件
 */

#define MUSIC_STATION_COUNT   3

static const char *MUSIC_STATIONS[MUSIC_STATION_COUNT] = {
    "http://music.163.com/song/media/outer/url?id=447925558.mp3",
    "http://music.163.com/song/media/outer/url?id=1330348068.mp3",
    "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.mp3",
};

static const char *MUSIC_STATION_NAMES[MUSIC_STATION_COUNT] = {
    "默认曲目 1",
    "默认曲目 2",
    "测试音频",
};

/* 音量（百分比） */
#define MUSIC_VOLUME_PERCENT  40
