#pragma once

#include "Vane/IVideoEncoder.h"

// 前向声明，隐藏平台相关实现细节（PIMPL 模式）
struct FVAEncoderImpl;

// Linux VA-API 硬件编码器封装（输出 H.264 裸流，内置 MP4Muxer 封装为 fMP4）
class VANE_API FVAEncoder : public IVideoEncoder
{
public:
    FVAEncoder();
    virtual ~FVAEncoder();

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

private:
    FVAEncoderImpl* Impl;
};
