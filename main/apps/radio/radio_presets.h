#pragma once
#include "radio_player.h"

// Shared presets: one catalog failure must not leave the radio with one station.
inline constexpr RadioStation kRadioPresets[] = {
#ifdef PASSPORT_RADIO_HLS_TEST
    {"CNR Business", "HLS test · TS/AAC", "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8", 0},
    {"Chinese Hits", "HLS test · AAC segments", "http://ls.qingting.fm/live/3412131.m3u8?bitrate=64", 0},
    {"Phoenix Info", "HLS test · TS/AAC", "http://playtv-live.ifeng.com/live/06OLEEWQKN4_audio.m3u8", 0},
    {"SomaFM", "USA · MP3 test", "https://ice6.somafm.com/groovesalad-128-mp3", 0},
    {"MANGORADIO", "Germany · MP3 test", "https://mangoradio.stream.laut.fm/mangoradio", 0},
    {"CNR China Voice", "MP3 regression test", "http://lhttp.qingting.fm/live/15318317/64k.mp3", 0},
#else
    {"CNR China Voice", "Featured · News", "http://lhttp.qingting.fm/live/15318317/64k.mp3", 0},
    {"Nostalgia Music", "Featured · Classic oldies", "http://lhttp.qingting.fm/live/4804/64k.mp3", 0},
    {"Morning Music", "Featured · Easy melodies", "http://lhttp.qingting.fm/live/4915/64k.mp3", 0},
    {"Two-Guang Voice", "Featured · Cantonese music", "http://lhttp.qingting.fm/live/20500149/64k.mp3", 0},
    {"Yangcheng Traffic", "Featured · City info", "http://lhttp.qingting.fm/live/1262/64k.mp3", 0},
    {"Yicai Finance", "Featured · Finance news", "http://lhttp.qingting.fm/live/276/64k.mp3", 0},
#endif
};
