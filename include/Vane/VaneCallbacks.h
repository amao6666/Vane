#pragma once

#include <cstdint>

// 兼容 C 调用者
#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 录制状态
// ============================================================================
typedef enum
{
    ERecordingState_Idle      = 0,
    ERecordingState_Starting  = 1,
    ERecordingState_Recording = 2,
    ERecordingState_Paused    = 3,
    ERecordingState_Stopping  = 4,
    ERecordingState_Error     = 5
} ERecordingState;

// ============================================================================
// 错误级别
// ============================================================================
typedef enum
{
    EErrorLevel_Warning = 0,
    EErrorLevel_Error   = 1,
    EErrorLevel_Fatal   = 2
} EErrorLevel;

// ============================================================================
// 回调函数指针类型（C 风格，兼容跨 DLL 调用）
// ============================================================================

// 状态变化回调
// 触发线程：调用线程（StartRecording/StopRecording）或编码线程（出错时）
// 注意：回调内不应阻塞或执行长时间计算；UE 用户须转发到 GameThread 操作 UI
typedef void (*VaneStateCallback)(ERecordingState NewState, void* UserData);

// 错误回调
// 触发线程：编码线程（编码失败时）或调用线程
// 注意：ErrorLevel_Fatal 后编码器已不可用；Message 指针生命周期为本次回调
typedef void (*VaneErrorCallback)(EErrorLevel Level, const char* Message, void* UserData);

// 进度回调（每秒触发一次）
// 触发线程：编码线程
// 注意：FramesEncoded/BytesWritten/DurationSeconds 基于编码线程的瞬时快照
typedef void (*VaneProgressCallback)(int64_t FramesEncoded, int64_t BytesWritten, double DurationSeconds, void* UserData);

// 丢帧回调
// 触发线程：主线程（EncodeFrame 中队列满时）
// 注意：FramesDropped 为累计值；UE 用户建议用此回调调整推送频率
typedef void (*VaneFrameDropCallback)(int32_t FramesDropped, void* UserData);

#ifdef __cplusplus
}
#endif
