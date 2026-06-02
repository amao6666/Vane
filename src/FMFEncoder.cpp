#include "FMFEncoder.h"
#include "core/AsyncPipeline.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <codecapi.h>
#include <cstdio>
#include <string>
#include <cstring>
#include <vector>
#include <algorithm>
#include <chrono>

// ============================================================================
// 编码器候选结构体
// ============================================================================
struct FEncoderCandidate
{
    IMFActivate* pActivate  = nullptr;   // MFT 激活对象（拥有所有权）
    GUID         CodecSubtype = {};      // MFVideoFormat_H264 / HEVC / WMV3
    bool         bHardware   = false;
    bool         bDiscreteGPU = false;   // NVIDIA 或 AMD 独显
    int32        Priority    = 999;      // 越小越优先
    WCHAR        FriendlyName[256] = {};
};

// ============================================================================
// 平台实现结构体
// ============================================================================
struct FMFEncoderImpl
{
    int32 Width          = 0;
    int32 Height         = 0;
    int32 FrameRate      = 60;
    int32 BitRate        = 10000000;
    int32 KeyframeInterval = 120;
    int32 FlushInterval  = 60;
    int32 QueueSize      = 32;
    int32 RecordFrameRate = 60;
    bool  bContinueLastFrame   = false;
    bool  bAllowCodecFallback   = true;
    bool  bUserPreferH264       = true;
    bool  bAllowFormatFallback  = true;

    IMFSinkWriter* SinkWriter = nullptr;
    DWORD          StreamIndex = 0;

    bool bInitialized = false;
    bool bRecording   = false;
    bool bHasError    = false;

    std::string LastError;

    // 实际使用的编码器信息
    GUID    ActiveCodec = {};
    bool    bActiveIsHW = false;
    bool    bActiveIsDiscrete = false;
    std::string CodecDescription;

    // 异步编码管线
    FAsyncEncodingPipeline Pipeline;
    int32 LastDroppedCount = 0;

    // NV12 转换缓冲区（预分配，仅编码线程访问）
    std::vector<uint8_t> Nv12Buf;
    int32 Nv12BufSize = 0;

    // 进度回调计时（仅编码线程访问）
    int64_t TotalFramesEncoded  = 0;
    int64_t TotalBytesWritten   = 0;
    std::chrono::steady_clock::time_point LastProgressTime;

    // 回调存储
    VaneStateCallback    StateCb    = nullptr;
    void*                StateUserData = nullptr;
    VaneErrorCallback    ErrorCb    = nullptr;
    void*                ErrorUserData = nullptr;
    VaneProgressCallback ProgressCb = nullptr;
    void*                ProgressUserData = nullptr;
    VaneFrameDropCallback FrameDropCb = nullptr;
    void*                FrameDropUserData = nullptr;

    static constexpr LONGLONG FrameDuration = 166667; // 60fps, 100ns 单位
};

// MF 引用计数
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
// 将 UTF-8 路径转为宽字符路径
// ============================================================================
static wchar_t* ToWideString(const char* utf8)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    auto* wide = new wchar_t[len];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, len);
    return wide;
}

// ============================================================================
// BGRA → NV12 颜色空间转换（ITU-R BT.601）
// ============================================================================
static void ConvertBGRAToNV12(const uint8_t* BGRA, int32 Width, int32 Height, uint8_t* NV12)
{
    const int32 FrameSize = Width * Height;
    uint8_t* Y  = NV12;
    uint8_t* UV = NV12 + FrameSize;

    for (int32 y = 0; y < Height; y++)
    {
        for (int32 x = 0; x < Width; x++)
        {
            const uint8_t* p = BGRA + ((size_t)y * Width + x) * 4;
            int32 B = p[0], G = p[1], R = p[2];

            int32 YVal  = (( 66 * R + 129 * G +  25 * B + 128) >> 8) + 16;
            int32 UVal  = ((-38 * R -  74 * G + 112 * B + 128) >> 8) + 128;
            int32 VVal  = ((112 * R -  94 * G -  18 * B + 128) >> 8) + 128;

            Y[(size_t)y * Width + x] = (uint8_t)(YVal < 0 ? 0 : (YVal > 255 ? 255 : YVal));

            if ((y & 1) == 0 && (x & 1) == 0)
            {
                size_t uvIdx = ((size_t)(y / 2) * Width + (x & ~1));
                UV[uvIdx]     = (uint8_t)(UVal < 0 ? 0 : (UVal > 255 ? 255 : UVal));
                UV[uvIdx + 1] = (uint8_t)(VVal < 0 ? 0 : (VVal > 255 ? 255 : VVal));
            }
        }
    }
}

// ============================================================================
// GPU 检测：判断是否为独显（NVIDIA/AMD）
// ============================================================================
static bool IsDiscreteGPU(IMFAttributes* pMFTAttributes)
{
    // 方法1：Vendor ID 检测（最可靠）
    WCHAR vendorId[64] = {};
    if (SUCCEEDED(pMFTAttributes->GetString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, vendorId, 64, nullptr)))
    {
        if (_wcsicmp(vendorId, L"VEN_10DE") == 0) return true; // NVIDIA
        if (_wcsicmp(vendorId, L"VEN_1002") == 0) return true; // AMD
        if (_wcsicmp(vendorId, L"VEN_8086") == 0) return false; // Intel 核显
    }

    // 方法2：FriendlyName 辅助判断
    WCHAR friendlyName[256] = {};
    if (SUCCEEDED(pMFTAttributes->GetString(MFT_FRIENDLY_NAME_Attribute, friendlyName, 256, nullptr)))
    {
        if (wcsstr(friendlyName, L"NVIDIA") || wcsstr(friendlyName, L"AMD")) return true;
        if (wcsstr(friendlyName, L"Intel")) return false;
    }

    return false; // 无法判断时默认视为核显
}

// ============================================================================
// 编码器枚举：按优先级从优到次排列
// 优先级：独显 H.264 > 独显 HEVC > 核显 H.264 > 软 H.264 > 软 HEVC > WMV
// ============================================================================
static std::vector<FEncoderCandidate> EnumerateAndRankEncoders()
{
    std::vector<FEncoderCandidate> result;

    // 硬件和软件编码器在 MF 中是分开枚举的，需要两次调用
    IMFActivate** ppHWActivate = nullptr;
    IMFActivate** ppSWActivate = nullptr;
    UINT32 hwCount = 0, swCount = 0;

    // 枚举硬件编码器
    HRESULT hrHW = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
                   nullptr, nullptr, &ppHWActivate, &hwCount);
    fprintf(stderr, "[DIAG] HW encoders: hr=0x%08X count=%u\n", (int)hrHW, hwCount);

    // 枚举软件编码器
    HRESULT hrSW = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
                   nullptr, nullptr, &ppSWActivate, &swCount);
    fprintf(stderr, "[DIAG] SW encoders: hr=0x%08X count=%u\n", (int)hrSW, swCount);

    if (hwCount == 0 && swCount == 0)
    {
        if (ppHWActivate) CoTaskMemFree(ppHWActivate);
        if (ppSWActivate) CoTaskMemFree(ppSWActivate);
        fprintf(stderr, "[DIAG] No encoders found at all!\n");
        return result;
    }

    // 合并硬件 + 软件编码器
    for (UINT32 i = 0; i < hwCount; i++)
    {
        FEncoderCandidate cand;
        cand.pActivate = ppHWActivate[i];
        ppHWActivate[i]->AddRef();
        cand.bHardware = true;
        result.push_back(cand);
    }
    for (UINT32 i = 0; i < swCount; i++)
    {
        FEncoderCandidate cand;
        cand.pActivate = ppSWActivate[i];
        ppSWActivate[i]->AddRef();
        cand.bHardware = false;
        result.push_back(cand);
    }
    if (ppHWActivate) CoTaskMemFree(ppHWActivate);
    if (ppSWActivate) CoTaskMemFree(ppSWActivate);

    // 为每个候选填充附加信息并计算优先级
    for (auto& cand : result)
    {
        // 获取 FriendlyName
        cand.pActivate->GetString(MFT_FRIENDLY_NAME_Attribute, cand.FriendlyName, 256, nullptr);
        fprintf(stderr, "[DIAG] Candidate: %ls (HW=%d)\n", cand.FriendlyName, cand.bHardware);

        // 判断独显
        if (cand.bHardware)
            cand.bDiscreteGPU = IsDiscreteGPU(cand.pActivate);

        // 获取输出编码格式
        IMFTransform* pMFT = nullptr;
        if (SUCCEEDED(cand.pActivate->ActivateObject(IID_PPV_ARGS(&pMFT))) && pMFT)
        {
            IMFMediaType* pType = nullptr;
            if (SUCCEEDED(pMFT->GetOutputAvailableType(0, 0, &pType)) && pType)
            {
                pType->GetGUID(MF_MT_SUBTYPE, &cand.CodecSubtype);
                fprintf(stderr, "[DIAG]   Subtype: {%08X-%04X-%04X...}\n",
                       cand.CodecSubtype.Data1, cand.CodecSubtype.Data2, cand.CodecSubtype.Data3);
                pType->Release();
            }
            else
            {
                fprintf(stderr, "[DIAG]   GetOutputAvailableType failed\n");
            }
            pMFT->Release();
        }
        else
        {
            fprintf(stderr, "[DIAG]   ActivateObject failed\n");
        }

        // 计算优先级
        int32 priority = 50;
        if (cand.CodecSubtype == MFVideoFormat_H264)
        {
            if (cand.bHardware && cand.bDiscreteGPU)
                priority = 10;
            else if (cand.bHardware)
                priority = 12;
            else
                priority = 14;
        }
        else if (cand.CodecSubtype == MFVideoFormat_HEVC || cand.CodecSubtype == MFVideoFormat_H265)
        {
            if (cand.bHardware && cand.bDiscreteGPU)
                priority = 11;
            else
                priority = 15;
        }
        else if (cand.CodecSubtype == MFVideoFormat_WMV3 || cand.CodecSubtype == MFVideoFormat_WMV2)
        {
            priority = 20;
        }

        cand.Priority = priority;
        fprintf(stderr, "[DIAG]   Priority=%d DiscreteGPU=%d\n", priority, cand.bDiscreteGPU);
    }

    fprintf(stderr, "[DIAG] Total candidates: %zu\n", result.size());

    // 按 Priority 升序排序
    std::sort(result.begin(), result.end(),
              [](const FEncoderCandidate& a, const FEncoderCandidate& b) {
                  return a.Priority < b.Priority;
              });

    return result;
}

// ============================================================================
// 创建 SinkWriter 并配置媒体类型（让 MF 自动选择编码器）
// ============================================================================
static bool TryCreateSinkWriter(
    FMFEncoderImpl* Impl,
    const wchar_t* WidePath,
    const GUID& ContainerType,
    const GUID& CodecSubtype,
    std::string& OutError)
{
    IMFAttributes* pAttr = nullptr;
    MFCreateAttributes(&pAttr, 3);
    pAttr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, ContainerType);
    pAttr->SetUINT32(MF_LOW_LATENCY, TRUE);
    pAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    HRESULT hr = MFCreateSinkWriterFromURL(WidePath, nullptr, pAttr, &Impl->SinkWriter);
    pAttr->Release();

    if (FAILED(hr) || !Impl->SinkWriter)
    {
        OutError = "创建 Sink Writer 失败（HRESULT=" + std::to_string(static_cast<int>(hr)) + ")";
        return false;
    }

    // 配置输入媒体类型（BGRA32 — 保持与已验证配置一致）
    IMFMediaType* pInputType = nullptr;
    MFCreateMediaType(&pInputType);
    pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    pInputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, Impl->Width, Impl->Height);
    MFSetAttributeRatio(pInputType, MF_MT_FRAME_RATE, Impl->FrameRate, 1);
    MFSetAttributeRatio(pInputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = Impl->SinkWriter->AddStream(pInputType, &Impl->StreamIndex);
    pInputType->Release();

    if (FAILED(hr))
    {
        OutError = "添加输入流失败（HRESULT=" + std::to_string(static_cast<int>(hr)) + ")";
        Impl->SinkWriter->Release();
        Impl->SinkWriter = nullptr;
        return false;
    }

    // 配置输出格式 — 先尝试不传编码器，让 SinkWriter 自动选择
    IMFMediaType* pOutputType = nullptr;
    MFCreateMediaType(&pOutputType);
    pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutputType->SetGUID(MF_MT_SUBTYPE, CodecSubtype);
    pOutputType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)Impl->BitRate);
    MFSetAttributeRatio(pOutputType, MF_MT_FRAME_RATE, Impl->FrameRate, 1);
    MFSetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, Impl->Width, Impl->Height);

    hr = Impl->SinkWriter->SetInputMediaType(Impl->StreamIndex, pOutputType, nullptr);
    pOutputType->Release();

    // 如果自动选择失败，尝试手动枚举编码器并注入
    if (FAILED(hr))
    {
        fprintf(stderr, "[DIAG] SetInputMediaType(auto) failed: 0x%08X, trying manual encoder injection...\n", (int)hr);

        MFT_REGISTER_TYPE_INFO inInfo  = { MFMediaType_Video, MFVideoFormat_NV12 };
        MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, CodecSubtype };
        IMFActivate** ppActivate = nullptr;
        UINT32 encCount = 0;
        HRESULT hrEnum = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
                            &inInfo, &outInfo, &ppActivate, &encCount);

        fprintf(stderr, "[DIAG] MFTEnumEx(NV12->H264): hr=0x%08X count=%u\n", (int)hrEnum, encCount);

        bool bInjected = false;
        for (UINT32 i = 0; i < encCount && !bInjected; i++)
        {
            WCHAR encName[256] = {};
            ppActivate[i]->GetString(MFT_FRIENDLY_NAME_Attribute, encName, 256, nullptr);
            fprintf(stderr, "[DIAG]   Trying: %ls\n", encName);

            IMFTransform* pEncoder = nullptr;
            if (SUCCEEDED(ppActivate[i]->ActivateObject(IID_PPV_ARGS(&pEncoder))) && pEncoder)
            {
                // 重新创建 SinkWriter（因为之前的 AddStream 已绑定失败的拓扑）
                Impl->SinkWriter->Release();
                Impl->SinkWriter = nullptr;

                IMFAttributes* pAttr2 = nullptr;
                MFCreateAttributes(&pAttr2, 3);
                pAttr2->SetGUID(MF_TRANSCODE_CONTAINERTYPE, ContainerType);
                pAttr2->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
                pAttr2->SetUINT32(MF_LOW_LATENCY, TRUE);

                hr = MFCreateSinkWriterFromURL(WidePath, nullptr, pAttr2, &Impl->SinkWriter);
                pAttr2->Release();

                if (FAILED(hr) || !Impl->SinkWriter)
                {
                    pEncoder->Release();
                    ppActivate[i]->Release();
                    continue;
                }

                // 重新 AddStream
                IMFMediaType* pInputType2 = nullptr;
                MFCreateMediaType(&pInputType2);
                pInputType2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                pInputType2->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                pInputType2->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                MFSetAttributeSize(pInputType2, MF_MT_FRAME_SIZE, Impl->Width, Impl->Height);
                MFSetAttributeRatio(pInputType2, MF_MT_FRAME_RATE, Impl->FrameRate, 1);
                MFSetAttributeRatio(pInputType2, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

                hr = Impl->SinkWriter->AddStream(pInputType2, &Impl->StreamIndex);
                pInputType2->Release();

                if (FAILED(hr))
                {
                    pEncoder->Release();
                    ppActivate[i]->Release();
                    Impl->SinkWriter->Release();
                    Impl->SinkWriter = nullptr;
                    continue;
                }

                // 设置编码器输出类型
                IMFMediaType* pOutType2 = nullptr;
                MFCreateMediaType(&pOutType2);
                pOutType2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                pOutType2->SetGUID(MF_MT_SUBTYPE, CodecSubtype);
                pOutType2->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)Impl->BitRate);
                MFSetAttributeRatio(pOutType2, MF_MT_FRAME_RATE, Impl->FrameRate, 1);
                MFSetAttributeSize(pOutType2, MF_MT_FRAME_SIZE, Impl->Width, Impl->Height);

                HRESULT hrSetOut = pEncoder->SetOutputType(0, pOutType2, 0);
                fprintf(stderr, "[DIAG]     SetOutputType on encoder: 0x%08X\n", (int)hrSetOut);

                // 通过 MF_SINK_WRITER_ENCODER_CONFIG 注入编码器
                IMFAttributes* pEncCfg = nullptr;
                MFCreateAttributes(&pEncCfg, 1);
                pEncCfg->SetUnknown(MF_SINK_WRITER_ENCODER_CONFIG, pEncoder);

                hr = Impl->SinkWriter->SetInputMediaType(Impl->StreamIndex, pOutType2, pEncCfg);
                fprintf(stderr, "[DIAG]     SetInputMediaType(with encoder): 0x%08X\n", (int)hr);

                pEncCfg->Release();
                pOutType2->Release();
                pEncoder->Release();

                bInjected = SUCCEEDED(hr);
            }
            ppActivate[i]->Release();
        }

        if (ppActivate) CoTaskMemFree(ppActivate);

        if (!bInjected)
        {
            OutError = "SetInputMediaType 失败（HRESULT=" + std::to_string(static_cast<int>(hr)) + "），手动注入编码器也未成功";
            Impl->SinkWriter->Release();
            Impl->SinkWriter = nullptr;
            return false;
        }
    }

    // 开始写入
    hr = Impl->SinkWriter->BeginWriting();
    if (FAILED(hr))
    {
        OutError = "BeginWriting 失败（HRESULT=" + std::to_string(static_cast<int>(hr)) + ")";
        Impl->SinkWriter->Release();
        Impl->SinkWriter = nullptr;
        return false;
    }

    return true;
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

    Impl->Width              = Config.Width;
    Impl->Height             = Config.Height;
    Impl->FrameRate          = Config.FrameRate;
    Impl->BitRate            = Config.BitRate;
    Impl->KeyframeInterval   = Config.KeyframeInterval;
    Impl->FlushInterval      = Config.FlushIntervalFrames;
    Impl->QueueSize          = Config.FrameQueueSize;
    Impl->RecordFrameRate    = Config.RecordFrameRate;
    Impl->bContinueLastFrame   = Config.bContinueLastFrame;
    Impl->bAllowCodecFallback   = Config.bAllowCodecFallback;
    Impl->bUserPreferH264       = Config.bUserPreferH264;
    Impl->bAllowFormatFallback  = Config.bAllowFormatFallback;

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

    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Starting, Impl->StateUserData);

    // 环境检测
    FEncoderCapability cap = CheckCapability();
    fprintf(stderr, "[DIAG] Capability: H264=%d H264SW=%d HEVC=%d WMV=%d Recommended=%s\n",
           cap.bH264Available, cap.bH264SoftwareAvailable, cap.bHEVCAvailable,
           cap.bWMVAvailable, cap.RecommendedFormat.c_str());

    // 枚举所有编码器（诊断输出）
    std::vector<FEncoderCandidate> candidates = EnumerateAndRankEncoders();

    wchar_t* widePath = ToWideString(OutputPath);
    DeleteFileW(widePath);

    std::string actualOutputPath(OutputPath);

    bool   bSuccess = false;
    int32  codecLevel = 0; // 1=H264, 2=HEVC, 3=WMV

    // 决定编码策略
    bool bTryH264 = Impl->bUserPreferH264;
    bool bTryWMV  = !Impl->bUserPreferH264;

    // 如果用户偏好 H.264 但系统不支持
    if (bTryH264 && !cap.bH264Available && !cap.bH264SoftwareAvailable)
    {
        if (Impl->bAllowFormatFallback)
        {
            bTryWMV = true;
            if (Impl->ErrorCb)
                Impl->ErrorCb(EErrorLevel_Warning,
                    "您的系统不支持 H.264 编码，已自动切换为 WMV 格式。"
                    "建议安装显卡驱动或媒体功能包以获得最佳体验。", Impl->ErrorUserData);
        }
        else
        {
            Impl->LastError = "系统不支持 H.264 编码，请在设置中允许 WMV 格式或安装 H.264 编码器";
            for (auto& cand : candidates) { if (cand.pActivate) cand.pActivate->Release(); }
            if (Impl->StateCb) Impl->StateCb(ERecordingState_Error, Impl->StateUserData);
            delete[] widePath;
            return false;
        }
    }

    if (bTryH264)
    {
        std::string err;
        fprintf(stderr, "[DIAG] Trying H.264 (MP4)...\n");
        bSuccess = TryCreateSinkWriter(Impl, widePath, MFTranscodeContainerType_MPEG4, MFVideoFormat_H264, err);
        fprintf(stderr, "[DIAG]   %s (err=%s)\n", bSuccess ? "OK" : "FAIL", err.c_str());
        if (bSuccess) codecLevel = 1;
    }

    // HEVC 仅在 H.264 首选且失败时尝试
    if (!bSuccess && bTryH264)
    {
        std::string err;
        fprintf(stderr, "[DIAG] Trying HEVC (MP4)...\n");
        bSuccess = TryCreateSinkWriter(Impl, widePath, MFTranscodeContainerType_MPEG4, MFVideoFormat_HEVC, err);
        fprintf(stderr, "[DIAG]   %s (err=%s)\n", bSuccess ? "OK" : "FAIL", err.c_str());
        if (bSuccess) codecLevel = 2;
    }

    // WMV
    if (!bSuccess && bTryWMV)
    {
        // 将输出路径后缀改为 .wmv
        {
            size_t lastDot = actualOutputPath.rfind('.');
            size_t lastSep = actualOutputPath.find_last_of("\\/");
            if (lastDot != std::string::npos && (lastSep == std::string::npos || lastDot > lastSep))
                actualOutputPath = actualOutputPath.substr(0, lastDot);
            actualOutputPath += ".wmv";
        }

        wchar_t* wmvPath = ToWideString(actualOutputPath.c_str());
        DeleteFileW(wmvPath);

        std::string err;
        fprintf(stderr, "[DIAG] Trying WMV9 (ASF)...\n");
        bSuccess = TryCreateSinkWriter(Impl, wmvPath, MFTranscodeContainerType_ASF, MFVideoFormat_WMV3, err);
        fprintf(stderr, "[DIAG]   %s (err=%s)\n", bSuccess ? "OK" : "FAIL", err.c_str());
        if (bSuccess) codecLevel = 3;

        delete[] wmvPath;
    }

    // 兜底：无压缩 NV12
    if (!bSuccess)
    {
        IMFAttributes* pAttr2 = nullptr;
        MFCreateAttributes(&pAttr2, 1);
        pAttr2->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);

        HRESULT hr = MFCreateSinkWriterFromURL(widePath, nullptr, pAttr2, &Impl->SinkWriter);
        pAttr2->Release();

        if (SUCCEEDED(hr) && Impl->SinkWriter)
        {
            IMFMediaType* pInputType = nullptr;
            MFCreateMediaType(&pInputType);
            pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            pInputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, Impl->Width, Impl->Height);
            MFSetAttributeRatio(pInputType, MF_MT_FRAME_RATE, Impl->FrameRate, 1);
            MFSetAttributeRatio(pInputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

            hr = Impl->SinkWriter->AddStream(pInputType, &Impl->StreamIndex);
            pInputType->Release();

            if (SUCCEEDED(hr))
            {
                hr = Impl->SinkWriter->BeginWriting();
                if (SUCCEEDED(hr))
                {
                    Impl->CodecDescription = "未压缩 NV12（兜底）";
                    Impl->LastError = "所有编码器均不可用，写入未压缩数据。检查显卡驱动和媒体功能包";
                    bSuccess = true;
                    codecLevel = 0;
                    if (Impl->ErrorCb)
                        Impl->ErrorCb(EErrorLevel_Error, Impl->LastError.c_str(), Impl->ErrorUserData);
                }
                else { Impl->SinkWriter->Release(); Impl->SinkWriter = nullptr; }
            }
            else { Impl->SinkWriter->Release(); Impl->SinkWriter = nullptr; }
        }
    }

    for (auto& cand : candidates)
    {
        if (cand.pActivate) cand.pActivate->Release();
    }

    if (!bSuccess)
    {
        if (Impl->StateCb)
            Impl->StateCb(ERecordingState_Error, Impl->StateUserData);
        delete[] widePath;
        return false;
    }

    if (codecLevel == 1)      Impl->CodecDescription = "H.264";
    else if (codecLevel == 2) Impl->CodecDescription = "H.265/HEVC";
    else if (codecLevel == 3) Impl->CodecDescription = "WMV";

    if (codecLevel >= 2)
    {
        if (codecLevel == 2)
        {
            if (Impl->ErrorCb)
                Impl->ErrorCb(EErrorLevel_Warning, "H.264 不可用，已切换为 HEVC", Impl->ErrorUserData);
        }
        else
        {
            std::string msg = "H.264/H.265 编解码器不可用，已降级为 WMV 格式\n";
            msg += "实际输出文件: ";
            msg += actualOutputPath;
            msg += "\n播放器 (如 Windows Media Player) 可直接播放此 .wmv 文件";
            if (Impl->ErrorCb)
                Impl->ErrorCb(EErrorLevel_Error, msg.c_str(), Impl->ErrorUserData);
        }
    }

    delete[] widePath;

    Impl->Nv12BufSize = Impl->Width * Impl->Height * 3 / 2;
    Impl->Nv12Buf.resize(Impl->Nv12BufSize);

    // ---- 启动异步编码管线 ----
    FMFEncoderImpl* p = Impl;

    FEncodeCallback encCb = [p](const FFrameBuffer& Frame) {
        thread_local bool bComInit = false;
        if (!bComInit) { CoInitializeEx(nullptr, COINIT_MULTITHREADED); bComInit = true; }

        if (!Frame.bValid || p->bHasError) return;

        int32 nv12Size = p->Nv12BufSize;
        ConvertBGRAToNV12(Frame.Data, p->Width, p->Height, p->Nv12Buf.data());

        IMFSample* pSample = nullptr;
        if (FAILED(MFCreateSample(&pSample))) { p->bHasError = true; return; }

        IMFMediaBuffer* pBuffer = nullptr;
        if (FAILED(MFCreateMemoryBuffer(nv12Size, &pBuffer))) { pSample->Release(); p->bHasError = true; return; }

        BYTE* pData = nullptr;
        if (FAILED(pBuffer->Lock(&pData, nullptr, nullptr))) { pBuffer->Release(); pSample->Release(); p->bHasError = true; return; }

        memcpy(pData, p->Nv12Buf.data(), nv12Size);
        pBuffer->Unlock();
        pBuffer->SetCurrentLength(nv12Size);
        pSample->AddBuffer(pBuffer);
        pBuffer->Release();

        int64_t frameIdx = p->TotalFramesEncoded;
        LONGLONG rtStart    = static_cast<LONGLONG>(frameIdx) * p->FrameDuration;
        LONGLONG rtDuration = p->FrameDuration;
        pSample->SetSampleTime(rtStart);
        pSample->SetSampleDuration(rtDuration);

        HRESULT hr = p->SinkWriter->WriteSample(p->StreamIndex, pSample);
        pSample->Release();

        if (SUCCEEDED(hr))
        {
            p->TotalFramesEncoded = frameIdx + 1;
            p->TotalBytesWritten += nv12Size;

            if (p->ProgressCb)
            {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - p->LastProgressTime).count();
                if (elapsed >= 1000)
                {
                    p->ProgressCb(p->TotalFramesEncoded, p->TotalBytesWritten,
                                  static_cast<double>(p->TotalFramesEncoded) / p->FrameRate, p->ProgressUserData);
                    p->LastProgressTime = now;
                }
            }
        }
        else
        {
            p->bHasError = true;
        }
    };

    // 刷盘回调（MF SinkWriter 内部管理缓冲，无需手动 flush）
    FFlushCallback flushCb = []() {};

    Impl->Pipeline.Start(std::move(encCb), std::move(flushCb), Impl->QueueSize, Impl->FlushInterval,
                         Impl->bContinueLastFrame, Impl->FrameRate);

    // 初始化进度统计
    Impl->TotalFramesEncoded = 0;
    Impl->TotalBytesWritten  = 0;
    Impl->LastProgressTime   = std::chrono::steady_clock::now();
    Impl->LastDroppedCount   = 0;
    Impl->bHasError          = false;
    Impl->bRecording         = true;

    // 通知状态：Recording
    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Recording, Impl->StateUserData);

    return true;
}

bool FMFEncoder::EncodeFrame(const uint8* RawBGRA, int32 DataSize, double TimestampSeconds)
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

    Impl->Pipeline.PushFrame(RawBGRA, DataSize, TimestampSeconds, Impl->RecordFrameRate);

    int32 dropped = static_cast<int32>(Impl->Pipeline.GetDroppedFrames());
    if (dropped > Impl->LastDroppedCount)
    {
        Impl->LastDroppedCount = dropped;
        if (Impl->FrameDropCb)
            Impl->FrameDropCb(dropped, Impl->FrameDropUserData);
    }

    return true;
}

void FMFEncoder::StopRecording()
{
    if (!Impl->bRecording) return;

    Impl->bRecording = false;

    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Stopping, Impl->StateUserData);

    Impl->Pipeline.Stop();

    if (Impl->SinkWriter)
    {
        Impl->SinkWriter->Finalize();
        Impl->SinkWriter->Release();
        Impl->SinkWriter = nullptr;
    }

    // 最终进度回调
    if (Impl->ProgressCb && Impl->TotalFramesEncoded > 0)
    {
        Impl->ProgressCb(Impl->TotalFramesEncoded, Impl->TotalBytesWritten,
                         static_cast<double>(Impl->TotalFramesEncoded) / Impl->FrameRate, Impl->ProgressUserData);
    }

    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Idle, Impl->StateUserData);
}

void FMFEncoder::RequestStop()
{
    if (!Impl->bRecording) return;

    Impl->bRecording = false;

    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Stopping, Impl->StateUserData);

    FMFEncoderImpl* p = Impl;
    FFinalizeCallback finalizeCb = [p]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (p->SinkWriter)
        {
            p->SinkWriter->Finalize();
            p->SinkWriter->Release();
            p->SinkWriter = nullptr;
        }

        // 最终进度
        if (p->ProgressCb && p->TotalFramesEncoded > 0)
        {
            p->ProgressCb(p->TotalFramesEncoded, p->TotalBytesWritten,
                          static_cast<double>(p->TotalFramesEncoded) / p->FrameRate, p->ProgressUserData);
        }

        if (p->StateCb)
            p->StateCb(ERecordingState_Idle, p->StateUserData);

        CoUninitialize();
    };

    Impl->Pipeline.RequestStop(std::move(finalizeCb));
}

bool FMFEncoder::IsRecording() const
{
    return Impl->bRecording;
}

const char* FMFEncoder::GetLastError() const
{
    return Impl->LastError.c_str();
}

FEncoderCapability FMFEncoder::CheckCapability() const
{
    FEncoderCapability cap;
    std::string& diag = cap.DiagnosticInfo;
    diag.reserve(2048);

    // 临时初始化 COM（如果构造时未初始化则在此处补充）
    bool bNeedCleanup = false;
    if (MFRefCount == 0)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION);
        bNeedCleanup = true;
    }

    // ---- 检测 H.264 硬件编码器 ----
    diag += "=== H.264 硬件编码器 ===\n";
    {
        IMFActivate** ppAct = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                     MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
                     nullptr, nullptr, &ppAct, &count);

        for (UINT32 i = 0; i < count; i++)
        {
            WCHAR name[256] = {};
            ppAct[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr);

            IMFTransform* pMFT = nullptr;
            HRESULT hrAct = ppAct[i]->ActivateObject(IID_PPV_ARGS(&pMFT));
            if (SUCCEEDED(hrAct) && pMFT)
            {
                bool bHasH264Out = false;
                for (DWORD t = 0; t < 20; t++)
                {
                    IMFMediaType* pOut = nullptr;
                    if (FAILED(pMFT->GetOutputAvailableType(0, t, &pOut)) || !pOut) break;
                    GUID sub = {};
                    if (SUCCEEDED(pOut->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_H264)
                    {
                        bHasH264Out = true;
                        pOut->Release();
                        break;
                    }
                    pOut->Release();
                }

                if (bHasH264Out)
                {
                    cap.bH264Available = true;
                    int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
                    cap.H264EncoderName.resize(len > 0 ? len - 1 : 0);
                    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, name, -1, &cap.H264EncoderName[0], len, nullptr, nullptr);

                    // 检测是否独显
                    WCHAR ven[64] = {};
                    ppAct[i]->GetString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, ven, 64, nullptr);
                    const char* venLabel = "核显";
                    if (wcsstr(ven, L"10DE")) venLabel = "NVIDIA 独显";
                    else if (wcsstr(ven, L"1002")) venLabel = "AMD 独显";

                    char buf[512];
                    snprintf(buf, sizeof(buf), "  [可用] %ls (%s)\n", name, venLabel);
                    diag += buf;
                }
                else
                {
                    char buf[512];
                    snprintf(buf, sizeof(buf), "  [不可用] %ls（不支持 H.264 输出）\n", name);
                    diag += buf;
                }
                pMFT->Release();
            }
            else
            {
                char buf[512];
                snprintf(buf, sizeof(buf), "  [不可用] %ls（无法激活: 0x%08X，建议更新显卡驱动）\n", name, (int)hrAct);
                diag += buf;
            }
            ppAct[i]->Release();
        }
        if (ppAct) CoTaskMemFree(ppAct);
        if (count == 0) diag += "  (未找到硬件 H.264 编码器)\n";
    }

    // ---- 检测 H.264 软件编码器 ----
    diag += "\n=== H.264 软件编码器 ===\n";
    if (!cap.bH264Available)
    {
        IMFActivate** ppAct = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                     MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
                     nullptr, nullptr, &ppAct, &count);

        for (UINT32 i = 0; i < count; i++)
        {
            // 通过 Vendor ID 判断是否为硬件编码器（软件编码器无 Vendor ID）
            WCHAR ven[64] = {};
            bool bIsHW = SUCCEEDED(ppAct[i]->GetString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, ven, 64, nullptr)) && ven[0];
            if (bIsHW) { ppAct[i]->Release(); continue; }

            WCHAR name[256] = {};
            ppAct[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr);

            IMFTransform* pMFT = nullptr;
            HRESULT hrAct = ppAct[i]->ActivateObject(IID_PPV_ARGS(&pMFT));
            if (SUCCEEDED(hrAct) && pMFT)
            {
                bool bHasH264Out = false;
                for (DWORD t = 0; t < 20; t++)
                {
                    IMFMediaType* pOut = nullptr;
                    if (FAILED(pMFT->GetOutputAvailableType(0, t, &pOut)) || !pOut) break;
                    GUID sub = {};
                    if (SUCCEEDED(pOut->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_H264)
                    {
                        bHasH264Out = true;
                        pOut->Release();
                        break;
                    }
                    pOut->Release();
                }

                if (bHasH264Out)
                {
                    cap.bH264SoftwareAvailable = true;
                    char buf[512];
                    snprintf(buf, sizeof(buf), "  [可用] %ls\n", name);
                    diag += buf;
                }
                else
                {
                    char buf[512];
                    snprintf(buf, sizeof(buf), "  [不可用] %ls（不支持 H.264 输出）\n", name);
                    diag += buf;
                }
                pMFT->Release();
            }
            else
            {
                char buf[512];
                snprintf(buf, sizeof(buf), "  [不可用] %ls（无法激活: 0x%08X）\n", name, (int)hrAct);
                diag += buf;
            }
            ppAct[i]->Release();
        }
        if (ppAct) CoTaskMemFree(ppAct);
        if (count == 0) diag += "  (未找到软件 H.264 编码器)\n";
    }
    else
    {
        diag += "  (硬件编码器已可用，跳过软件检测)\n";
    }

    // ---- 检测 HEVC 硬件编码器 ----
    diag += "\n=== HEVC 硬件编码器 ===\n";
    {
        IMFActivate** ppAct = nullptr;
        UINT32 count = 0;
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                  MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
                  nullptr, nullptr, &ppAct, &count);

        for (UINT32 i = 0; i < count; i++)
        {
            WCHAR name[256] = {};
            ppAct[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr);

            IMFTransform* pMFT = nullptr;
            if (SUCCEEDED(ppAct[i]->ActivateObject(IID_PPV_ARGS(&pMFT))) && pMFT)
            {
                bool bHasHEVCOut = false;
                for (DWORD t = 0; t < 20; t++)
                {
                    IMFMediaType* pOut = nullptr;
                    if (FAILED(pMFT->GetOutputAvailableType(0, t, &pOut)) || !pOut) break;
                    GUID sub = {};
                    if (SUCCEEDED(pOut->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_HEVC)
                    {
                        bHasHEVCOut = true;
                        pOut->Release();
                        break;
                    }
                    pOut->Release();
                }
                if (bHasHEVCOut)
                {
                    cap.bHEVCAvailable = true;
                    char buf[512];
                    snprintf(buf, sizeof(buf), "  [可用] %ls\n", name);
                    diag += buf;
                }
                pMFT->Release();
            }
            ppAct[i]->Release();
        }
        if (ppAct) CoTaskMemFree(ppAct);
        if (!cap.bHEVCAvailable) diag += "  (未找到硬件 HEVC 编码器)\n";
    }

    // ---- 推荐格式 ----
    if (cap.bH264Available)
        cap.RecommendedFormat = "h264";
    else
        cap.RecommendedFormat = "wmv";

    diag += "\n=== 推荐 ===\n";
    diag += "  RecommendedFormat: " + cap.RecommendedFormat + "\n";

    if (bNeedCleanup)
    {
        MFShutdown();
        CoUninitialize();
    }

    return cap;
}

// ---- 回调注册 ----
void FMFEncoder::SetStateCallback(VaneStateCallback Cb, void* UserData)
{
    Impl->StateCb     = Cb;
    Impl->StateUserData = UserData;
}

void FMFEncoder::SetErrorCallback(VaneErrorCallback Cb, void* UserData)
{
    Impl->ErrorCb     = Cb;
    Impl->ErrorUserData = UserData;
}

void FMFEncoder::SetProgressCallback(VaneProgressCallback Cb, void* UserData)
{
    Impl->ProgressCb     = Cb;
    Impl->ProgressUserData = UserData;
}

void FMFEncoder::SetFrameDropCallback(VaneFrameDropCallback Cb, void* UserData)
{
    Impl->FrameDropCb     = Cb;
    Impl->FrameDropUserData = UserData;
}
