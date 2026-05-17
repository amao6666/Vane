#include "FMFEncoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <cstdio>
#include <string>
#include <cstring>

// ============================================================================
// 平台实现结构体
// ============================================================================
struct FMFEncoderImpl
{
    int32 Width       = 0;
    int32 Height      = 0;
    int32 FrameCount  = 0;
    int32 FrameRate   = 60;
    int32 BitRate     = 10000000;

    IMFSinkWriter* SinkWriter = nullptr;
    DWORD          StreamIndex = 0;

    bool bInitialized = false;
    bool bRecording   = false;
    bool bHasError    = false;

    std::string LastError;

    // 60fps: 每帧 = 10000000 / 60 ≈ 166667（100ns 单位）
    static constexpr LONGLONG FrameDuration = 166667;
};

// MF 引用计数（进程级别，多个编码器实例共享）
int32 FMFEncoder::MFRefCount = 0;

// ============================================================================
// COM 初始化的 RAII 辅助
// ============================================================================
static void AddMFRef()
{
    if (FMFEncoder::MFRefCount == 0)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION);
    }
    ++FMFEncoder::MFRefCount;
}

static void ReleaseMFRef()
{
    --FMFEncoder::MFRefCount;
    if (FMFEncoder::MFRefCount == 0)
    {
        MFShutdown();
        CoUninitialize();
    }
}

// ============================================================================
// 将 UTF-8 路径转为宽字符路径（Sink Writer 需要宽字符 URL）
// ============================================================================
static wchar_t* ToWideString(const char* utf8)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    auto* wide = new wchar_t[len];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, len);
    return wide;
}

// ============================================================================
// FMFEncoder 公开接口实现
// ============================================================================

FMFEncoder::FMFEncoder()
    : Impl(new FMFEncoderImpl())
{
    AddMFRef();
}

FMFEncoder::~FMFEncoder()
{
    StopRecording();
    delete Impl;
    ReleaseMFRef();
}

bool FMFEncoder::Initialize(const FEncoderConfig& Config)
{
    if (Impl->bInitialized) return true;
    if (Config.Width <= 0 || Config.Height <= 0)
    {
        Impl->LastError = "分辨率无效：宽度和高度必须大于 0";
        return false;
    }

    Impl->Width     = Config.Width;
    Impl->Height    = Config.Height;
    Impl->FrameRate   = Config.FrameRate;
    Impl->BitRate     = Config.BitRate;
    Impl->FlushInterval = Config.FlushIntervalFrames;

    // 检查 Codec
    if (Config.Codec && strcmp(Config.Codec, "h264") != 0)
    {
        Impl->LastError = "不支持的编码格式：当前仅支持 h264";
        return false;
    }

    Impl->bInitialized = true;
    return true;
}

bool FMFEncoder::StartRecording(const char* OutputPath)
{
    if (!Impl->bInitialized)
    {
        Impl->LastError = "编码器未初始化：请先调用 Initialize()";
        return false;
    }
    if (Impl->bRecording)
    {
        Impl->LastError = "编码器已在录制中";
        return false;
    }

    // 删除已存在的文件
    wchar_t* widePath = ToWideString(OutputPath);
    DeleteFileW(widePath);

    // ---- 创建 Sink Writer，输出 MP4（低延迟 + fMP4 自动刷新）----
    IMFAttributes* pAttr = nullptr;
    MFCreateAttributes(&pAttr, 3);
    pAttr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    pAttr->SetUINT32(MF_LOW_LATENCY, TRUE);
    pAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    HRESULT hr = MFCreateSinkWriterFromURL(widePath, nullptr, pAttr, &Impl->SinkWriter);
    pAttr->Release();
    delete[] widePath;

    if (FAILED(hr) || !Impl->SinkWriter)
    {
        Impl->LastError = "创建 Sink Writer 失败（HRESULT=" + std::to_string(static_cast<int>(hr)) + ")";
        return false;
    }

    // ---- 配置输入媒体类型（BGRA32）----
    IMFMediaType* pInputType = nullptr;
    MFCreateMediaType(&pInputType);
    pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_BGRA32);
    pInputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, Impl->Width, Impl->Height);
    MFSetAttributeRatio(pInputType, MF_MT_FRAME_RATE, Impl->FrameRate, 1);
    MFSetAttributeRatio(pInputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = Impl->SinkWriter->AddStream(pInputType, &Impl->StreamIndex);
    pInputType->Release();

    if (FAILED(hr))
    {
        Impl->SinkWriter->Release();
        Impl->SinkWriter = nullptr;
        return false;
    }

    // ---- 配置输出媒体类型（H.264）----
    IMFMediaType* pOutputType = nullptr;
    MFCreateMediaType(&pOutputType);
    pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    pOutputType->SetUINT32(MF_MT_AVG_BITRATE, Impl->BitRate);
    MFSetAttributeRatio(pOutputType, MF_MT_FRAME_RATE, Impl->FrameRate, 1);
    MFSetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, Impl->Width, Impl->Height);

    hr = Impl->SinkWriter->SetInputMediaType(Impl->StreamIndex, pOutputType, nullptr);
    pOutputType->Release();

    if (FAILED(hr))
    {
        Impl->LastError = "设置输出媒体类型失败（HRESULT=" + std::to_string(static_cast<int>(hr)) + ")";
        Impl->SinkWriter->Release();
        Impl->SinkWriter = nullptr;
        return false;
    }

    // ---- 开始写入 ----
    hr = Impl->SinkWriter->BeginWriting();
    if (FAILED(hr))
    {
        Impl->LastError = "开始写入失败（HRESULT=" + std::to_string(static_cast<int>(hr)) + ")";
        Impl->SinkWriter->Release();
        Impl->SinkWriter = nullptr;
        return false;
    }

    Impl->FrameCount = 0;
    Impl->bHasError  = false;
    Impl->bRecording = true;
    return true;
}

bool FMFEncoder::EncodeFrame(const uint8* RawBGRA, int32 DataSize, double /*TimestampSeconds*/)
{
    if (!Impl->bRecording)
    {
        Impl->LastError = "编码器未在录制状态";
        return false;
    }
    if (!RawBGRA || DataSize <= 0)
    {
        Impl->LastError = "输入帧数据为空";
        return false;
    }

    int32  bytesPerRow = Impl->Width * 4;
    DWORD  bufferSize  = Impl->Height * bytesPerRow;

    // ---- 创建 IMFSample 并填充 BGRA 数据 ----
    IMFSample*      pSample = nullptr;
    IMFMediaBuffer* pBuffer = nullptr;

    HRESULT hr = MFCreateSample(&pSample);
    if (FAILED(hr))
    {
        Impl->LastError = "创建 IMFSample 失败";
        return false;
    }

    hr = MFCreateMemoryBuffer(bufferSize, &pBuffer);
    if (FAILED(hr))
    {
        Impl->LastError = "创建内存缓冲区失败";
        pSample->Release();
        return false;
    }

    BYTE* pData = nullptr;
    hr = pBuffer->Lock(&pData, nullptr, nullptr);
    if (FAILED(hr))
    {
        Impl->LastError = "锁定内存缓冲区失败";
        pBuffer->Release();
        pSample->Release();
        return false;
    }

    BYTE* pData = nullptr;
    hr = pBuffer->Lock(&pData, nullptr, nullptr);
    if (FAILED(hr))
    {
        Impl->LastError = "添加输入流失败（HRESULT=" + std::to_string(static_cast<int>(hr)) + ")";
        Impl->SinkWriter->Release();
        Impl->SinkWriter = nullptr;
        return false;
    }
    memcpy(pData, RawBGRA, bufferSize);
    pBuffer->Unlock();
    pBuffer->SetCurrentLength(bufferSize);

    pSample->AddBuffer(pBuffer);
    pBuffer->Release();

    // ---- 设置时间戳（100ns 单位）----
    LONGLONG rtStart    = static_cast<LONGLONG>(Impl->FrameCount) * Impl->FrameDuration;
    LONGLONG rtDuration = Impl->FrameDuration;
    pSample->SetSampleTime(rtStart);
    pSample->SetSampleDuration(rtDuration);

    // ---- 写入编码管线 ----
    hr = Impl->SinkWriter->WriteSample(Impl->StreamIndex, pSample);
    pSample->Release();

    if (FAILED(hr))
    {
        Impl->LastError = "写入编码帧失败";
        return false;
    }
    if (Impl->bHasError)
    {
        return false;
    }

    ++Impl->FrameCount;
    return true;
}

void FMFEncoder::StopRecording()
{
    if (!Impl->bRecording) return;

    Impl->bRecording = false;

    if (Impl->SinkWriter)
    {
        Impl->SinkWriter->Finalize();
        Impl->SinkWriter->Release();
        Impl->SinkWriter = nullptr;
    }
}

void FMFEncoder::RequestStop()
{
    // TODO: 实现异步停止（当前委托同步版本）
    StopRecording();
}

bool FMFEncoder::IsRecording() const
{
    return Impl->bRecording;
}

const char* FMFEncoder::GetLastError() const
{
    return Impl->LastError.c_str();
}

// TODO: 实现回调通知（SetStateCallback / SetErrorCallback / SetProgressCallback / SetFrameDropCallback）
// 当前使用 IVideoEncoder 基类默认空实现，后续需要覆盖并在适当位置触发回调。
