#pragma once

#include <stdint.h>

#include "Vane/VaneConfig.h"
#include "Vane/VaneCallbacks.h"

// ============================================================================
// 导出宏：Windows 下 DLL 需要 __declspec(dllexport/dllimport)
// ============================================================================
#if defined(_WIN32)
    #if defined(VANE_EXPORTS)
        #define VANE_API __declspec(dllexport)
    #else
        #define VANE_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__) || defined(__clang__)
    #if defined(VANE_EXPORTS)
        #define VANE_API __attribute__((visibility("default")))
    #else
        #define VANE_API
    #endif
#else
    #define VANE_API
#endif

// ============================================================================
// C ABI 导出接口——可被 C / UE / 任意 FFI 直接调用
// ============================================================================
#ifdef __cplusplus
extern "C" {
#endif

// 创建编码器实例，返回不透明句柄
VANE_API void* VaneEncoder_Create(void);

// 销毁编码器实例
VANE_API void  VaneEncoder_Destroy(void* Encoder);

// 初始化编码器，传入配置结构体指针
VANE_API int   VaneEncoder_Initialize(void* Encoder, const FEncoderConfig* Config);

// 开始录制，指定输出文件路径（已存在则覆盖）
VANE_API int   VaneEncoder_StartRecording(void* Encoder, const char* OutputPath);

// 编码一帧 BGRA 原始像素数据
// DataSize: 数据字节数；TimestampSeconds: 真实墙上时钟秒数（首帧 0.0）
VANE_API int   VaneEncoder_EncodeFrame(void* Encoder, const uint8_t* RawBGRA, int DataSize, double TimestampSeconds);

// 同步停止录制
VANE_API void  VaneEncoder_StopRecording(void* Encoder);

// 异步停止：发送信号后立即返回，完成时通过 StateCallback(Idle) 通知
VANE_API void  VaneEncoder_RequestStop(void* Encoder);

// 查询是否正在录制
VANE_API int   VaneEncoder_IsRecording(void* Encoder);

// 获取最后一次错误的描述信息（返回字符串生命周期由编码器实例持有）
VANE_API const char* VaneEncoder_GetLastError(void* Encoder);

// ---- 回调注册 ----
VANE_API void VaneEncoder_SetStateCallback(void* Encoder, VaneStateCallback Cb, void* UserData);
VANE_API void VaneEncoder_SetErrorCallback(void* Encoder, VaneErrorCallback Cb, void* UserData);
VANE_API void VaneEncoder_SetProgressCallback(void* Encoder, VaneProgressCallback Cb, void* UserData);
VANE_API void VaneEncoder_SetFrameDropCallback(void* Encoder, VaneFrameDropCallback Cb, void* UserData);

#ifdef __cplusplus
}
#endif
