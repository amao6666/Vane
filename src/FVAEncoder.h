#pragma once

#include "Vane/IVideoEncoder.h"

// 前向声明，隐藏平台相关实现细节（PIMPL 模式）
struct FVAEncoderImpl;

// Linux VA-API 硬件编码器封装（输出 H.264 裸流）
class FVAEncoder : public IVideoEncoder
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

private:
    FVAEncoderImpl* Impl;
};
