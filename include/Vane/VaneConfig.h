#pragma once

#include <cstdint>

// 编码器配置（所有平台通用）
struct FEncoderConfig
{
    int32_t     Width             = 1920;
    int32_t     Height            = 1080;
    int32_t     FrameRate         = 60;
    int32_t     BitRate           = 10000000;
    int32_t     KeyframeInterval  = 120;
    bool        bRealTime         = true;
    const char* Codec             = "h264";
    int32_t     RecordFrameRate   = 60;   // 录制目标帧率，0 表示和输入一致
    int32_t     FrameQueueSize    = 32;   // 异步编码队列大小
    int32_t     FlushIntervalFrames = 60; // fMP4 fragment 间隔（崩溃恢复）
    bool        bContinueLastFrame = false; // 队列空时用最后一帧续命（CFR场景）
};
