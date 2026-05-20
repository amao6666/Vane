#pragma once

#include "Vane/IVideoEncoder.h"

// 前向声明，隐藏平台相关实现细节（PIMPL 模式）
struct FMFEncoderImpl;

// Windows Media Foundation 硬件编码器封装
class FMFEncoder : public IVideoEncoder
{
public:
    FMFEncoder();
    virtual ~FMFEncoder();

    bool Initialize(const FEncoderConfig& Config) override;
    bool StartRecording(const char* OutputPath) override;
    bool EncodeFrame(const uint8* RawBGRA, int32 DataSize, double TimestampSeconds) override;
    void StopRecording() override;
    void RequestStop() override;
    bool IsRecording() const override;
    const char* GetLastError() const override;
    FEncoderCapability CheckCapability() const override;

    void SetStateCallback(VaneStateCallback Cb, void* UserData) override;
    void SetErrorCallback(VaneErrorCallback Cb, void* UserData) override;
    void SetProgressCallback(VaneProgressCallback Cb, void* UserData) override;
    void SetFrameDropCallback(VaneFrameDropCallback Cb, void* UserData) override;

    // MF 引用计数，确保 MFStartup / MFShutdown 只调用一次
    static int32 MFRefCount;

private:
    FMFEncoderImpl* Impl;
};
