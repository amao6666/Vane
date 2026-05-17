#pragma once

#include "Vane/IVideoEncoder.h"

// 前向声明，隐藏平台相关实现细节（PIMPL 模式）
struct FVTEncoderImpl;

// macOS VideoToolbox 硬件编码器封装
class FVTEncoder : public IVideoEncoder
{
public:
    FVTEncoder();
    virtual ~FVTEncoder();

    bool Initialize(const FEncoderConfig& Config) override;
    bool StartRecording(const char* OutputPath) override;
    bool EncodeFrame(const uint8* RawBGRA, int32 DataSize, double TimestampSeconds) override;
    void StopRecording() override;
    void RequestStop() override;
    bool IsRecording() const override;
    const char* GetLastError() const override;

    void SetStateCallback(VaneStateCallback Cb, void* UserData) override;
    void SetErrorCallback(VaneErrorCallback Cb, void* UserData) override;
    void SetProgressCallback(VaneProgressCallback Cb, void* UserData) override;
    void SetContinueLastFrame(bool bEnable, int32 FPS);

    void SetFrameDropCallback(VaneFrameDropCallback Cb, void* UserData) override;

private:
    FVTEncoderImpl* Impl;
};
