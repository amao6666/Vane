#pragma once

#include <cstdint>

// 编码器类型（Windows 平台）
enum class VaneEncoderType
{
    None  = -1,
    NVENC = 0,
    AMF   = 1,
    MF    = 2,
};

// 编码器能力检测结果 — 固定大小字符数组，跨 DLL 边界安全
struct FEncoderCapability
{
    bool bH264Available         = false;
    bool bH264SoftwareAvailable = false;
    bool bHEVCAvailable         = false;
    bool bWMVAvailable          = true;
    char H264EncoderName[256]   = {};
    char RecommendedFormat[64]  = {};
    char DiagnosticInfo[2048]   = {};
};

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
    bool        bAllowCodecFallback = true; // 允许降级到备选编码器（含 WMV）
    bool        bUserPreferH264     = true; // 用户偏好 H.264，默认 true
    bool        bAllowFormatFallback = true; // H.264 不可用时是否自动降级到 WMV
    bool        bForceSoftwareConversion = false; // 强制使用 CPU 颜色转换，跳过 D3D11Converter
};
