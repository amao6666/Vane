#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <atomic>
#include <chrono>

#include "Vane/IVideoEncoder.h"
#include "Vane/VaneConfig.h"
#include "Vane/VaneCallbacks.h"
#include "windows/MFUtils.h"
#include "core/MP4Writer.h"
#include "core/ColorSpaceConverter.h"
#include "AsyncPipeline.h"

// 编码器类型
enum class EWindowsEncoderType
{
    None   = -1,
    NVENC  = 0,
    AMF    = 1,
    MF     = 2,
};

// Windows 平台编码器门面
// 运行时探测硬件 → 选择最佳编码器（NVENC > AMF > MF）
class FWindowsEncoder : public IVideoEncoder
{
public:
    FWindowsEncoder();
    ~FWindowsEncoder() override;

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

    // Windows 专属：设置 D3D11 Device
    void SetD3D11Device(void* pDevice);
    EWindowsEncoderType GetEncoderType() const;

private:
    struct Impl
    {
        FEncoderConfig              Config;
        EWindowsEncoderType         EncoderType = EWindowsEncoderType::None;
        void*                       D3D11Device = nullptr;

        // 内部编码器
        void* InternalEncoder = nullptr; // FNvencEncoder* / FAmfEncoder* / FMFEncoderNew*

        // MP4Writer
        std::unique_ptr<MP4Writer> Muxer;

        // 异步管线
        FAsyncEncodingPipeline     Pipeline;

        // NV12 转换缓冲
        std::vector<uint8_t> Nv12Buf;
        int32 Nv12BufSize = 0;

        // D3D11 GPU 色彩转换器（NVENC 路径用，避免 CPU BGRA→NV12）
        void* D3D11Converter = nullptr;

        // 统计
        std::atomic<int64_t> TotalFramesEncoded{0};
        std::atomic<int64_t> TotalBytesWritten{0};
        std::chrono::steady_clock::time_point LastProgressTime;
        int32 LastDroppedCount = 0;

        // 回调
        VaneStateCallback    StateCb = nullptr;
        void*                StateUserData = nullptr;
        VaneErrorCallback    ErrorCb = nullptr;
        void*                ErrorUserData = nullptr;
        VaneProgressCallback ProgressCb = nullptr;
        void*                ProgressUserData = nullptr;
        VaneFrameDropCallback FrameDropCb = nullptr;
        void*                FrameDropUserData = nullptr;

        // 状态
        std::string LastError;
        bool bInitialized = false;
        bool bRecording = false;
        bool bHasError = false;
        bool bUseWMVPath = false;    // true = WMV MFSinkWriter 路径，跳过 MP4Writer
        std::string CodecDescription;
    };

    std::unique_ptr<Impl> Ptr;

    EWindowsEncoderType ProbeHardware() const;
};
