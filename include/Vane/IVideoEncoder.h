#pragma once

#include <cstdint>

#include "Vane/VaneConfig.h"
#include "Vane/VaneCallbacks.h"
#include "Vane/VaneAPI.h"

// 类型别名
using int32 = int32_t;
using uint8 = uint8_t;

// 平台无关的视频编码器抽象接口
class VANE_API IVideoEncoder
{
public:
    virtual ~IVideoEncoder() = default;

    // 使用配置结构体初始化编码器
    // 返回 true 表示初始化成功
    virtual bool Initialize(const FEncoderConfig& Config) = 0;

    // 开始录制，指定输出文件路径
    // 返回 true 表示成功开始录制
    virtual bool StartRecording(const char* OutputPath) = 0;

    // 编码一帧 BGRA 格式的原始像素数据
    // RawBGRA: 像素数据指针，每像素 4 字节（B, G, R, A）
    // DataSize: 数据字节数（用于校验）
    // TimestampSeconds: 真实墙上时钟秒数（首帧记为 0.0）
    virtual bool EncodeFrame(const uint8* RawBGRA, int32 DataSize, double TimestampSeconds) = 0;

    // 停止录制，完成文件写入（同步，阻塞调用线程）
    virtual void StopRecording() = 0;

    // 异步停止：发送停止信号后立即返回，不阻塞
    // 编码线程处理完剩余帧后自动完成文件收尾并通过 StateCallback 通知 Idle
    virtual void RequestStop() { StopRecording(); }

    // 查询是否正在录制
    virtual bool IsRecording() const = 0;

    // 获取最后一次错误的描述信息
    virtual const char* GetLastError() const = 0;

    // 检测当前系统编码器能力（Initialize 之前即可调用）
    virtual FEncoderCapability CheckCapability() const = 0;

    // 设置 D3D11 Device（Windows 平台，非 Windows 平台无操作）
    // 必须在 Initialize 之前调用；传入 nullptr 表示取消
    virtual void SetD3D11Device(void* /*pDevice*/) {}

    // ---- 回调注册接口 ----
    // 回调触发线程说明见 VaneCallbacks.h，UE 用户必须将回调转发到 GameThread 才能操作 UI
    // UserData 指针必须保证在录制期间有效
    virtual void SetStateCallback(VaneStateCallback Cb, void* UserData) {}
    virtual void SetErrorCallback(VaneErrorCallback Cb, void* UserData) {}
    virtual void SetProgressCallback(VaneProgressCallback Cb, void* UserData) {}
    virtual void SetFrameDropCallback(VaneFrameDropCallback Cb, void* UserData) {}
};
