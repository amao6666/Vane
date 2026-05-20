#include "FMFEncoderNew.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <d3d11.h>
#include <cstring>

FMFEncoderNew::FMFEncoderNew() = default;

FMFEncoderNew::~FMFEncoderNew()
{
    std::string err;
    Finalize(err);
}

// ============================================================================
// MFStartup / MFShutdown 引用计数
// ============================================================================
static int32 s_MFRefCount = 0;
static bool  s_MFInitialized = false;

static bool EnsureMFStartup()
{
    if (!s_MFInitialized)
    {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        if (FAILED(hr)) return false;
        s_MFInitialized = true;
    }
    s_MFRefCount++;
    return true;
}

static void ReleaseMFStartup()
{
    if (--s_MFRefCount <= 0)
    {
        s_MFRefCount = 0;
        if (s_MFInitialized)
        {
            MFShutdown();
            s_MFInitialized = false;
        }
    }
}

// ============================================================================
// 可用性检测（结果缓存为 static bool）
// ============================================================================

bool FMFEncoderNew::IsAvailable()
{
    return IsH264Available() || IsWMVAvailable();
}

static bool s_MFH264Cached = false;
static bool s_MFH264Checked = false;

bool FMFEncoderNew::IsH264Available()
{
    if (!s_MFH264Checked)
    {
        MFT_REGISTER_TYPE_INFO inInfo  = { MFMediaType_Video, MFVideoFormat_NV12 };
        MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, MFVideoFormat_H264 };

        IMFActivate** ppActivate = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                               MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
                               &inInfo, &outInfo, &ppActivate, &count);

        s_MFH264Cached = SUCCEEDED(hr) && count > 0;

        if (ppActivate)
        {
            for (UINT32 i = 0; i < count; i++) ppActivate[i]->Release();
            CoTaskMemFree(ppActivate);
        }

        s_MFH264Checked = true;
    }
    return s_MFH264Cached;
}

bool FMFEncoderNew::IsWMVAvailable()
{
    return true; // WMV 编码器在 Windows 上通过 MFSinkWriter 始终可用
}

const char* FMFEncoderNew::GetCodecName() const
{
    return bWMVMode ? "MF WMV (MFSinkWriter)" : "MF H.264";
}

// ============================================================================
// Initialize: 自动选择 H.264 或 WMV 路径
// ============================================================================

bool FMFEncoderNew::Initialize(void* pD3D11Device, const FEncoderConfig& Config, std::string& OutError)
{
    if (bInitialized) return true;

    EncodeWidth  = Config.Width;
    EncodeHeight = Config.Height;
    FrameRate    = Config.FrameRate;
    BitRate      = Config.BitRate;
    D3D11Device  = pD3D11Device;

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrCom))
    {
        OutError = "MF: CoInitializeEx 失败 (HR=0x" + std::to_string(static_cast<int>(hrCom)) + ")";
        return false;
    }
    EnsureMFStartup();

    bool bOk = false;

    if (IsH264Available())
    {
        if (InitializeH264(pD3D11Device, Config, OutError))
        {
            bWMVMode = false;
            bInitialized = true;
            bOk = true;
        }
    }

    if (!bOk && InitializeWMV(Config, OutError))
    {
        bWMVMode = true;
        bInitialized = true;
        bOk = true;
    }

    if (!bOk)
    {
        ReleaseMFStartup();
        CoUninitialize();
    }
    else
    {
        CoUninitialize();
    }

    return bOk;
}

// ============================================================================
// H.264 IMFTransform 路径
// ============================================================================

static bool DrainOutputInternal(IMFTransform* pMFT, std::vector<uint8_t>& OutAnnexB)
{
    bool bGotOutput = false;
    MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
    outputBuffer.dwStreamID = 0;

    while (true)
    {
        DWORD status = 0;
        HRESULT hr = pMFT->ProcessOutput(0, 1, &outputBuffer, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(hr)) break;

        if (outputBuffer.pSample)
        {
            IMFMediaBuffer* pEncodedBuffer = nullptr;
            hr = outputBuffer.pSample->GetBufferByIndex(0, &pEncodedBuffer);
            if (SUCCEEDED(hr) && pEncodedBuffer)
            {
                BYTE* pEncData = nullptr;
                DWORD encSize = 0;
                hr = pEncodedBuffer->Lock(&pEncData, nullptr, &encSize);
                if (SUCCEEDED(hr) && pEncData && encSize > 0)
                {
                    OutAnnexB.insert(OutAnnexB.end(), pEncData, pEncData + encSize);
                    bGotOutput = true;
                }
                pEncodedBuffer->Unlock();
                pEncodedBuffer->Release();
            }
            outputBuffer.pSample->Release();
            outputBuffer.pSample = nullptr;
        }
    }
    return bGotOutput;
}

bool FMFEncoderNew::InitializeH264(void* pD3D11Device, const FEncoderConfig& Config, std::string& OutError)
{
    ID3D11Device* pDevice = static_cast<ID3D11Device*>(pD3D11Device);

    UINT resetToken = 0;
    IMFDXGIDeviceManager* pDevMgr = nullptr;
    if (pDevice)
    {
        HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, &pDevMgr);
        if (SUCCEEDED(hr))
        {
            hr = pDevMgr->ResetDevice(pDevice, resetToken);
            if (FAILED(hr))
            {
                pDevMgr->Release();
                pDevMgr = nullptr;
            }
        }
    }

    MFT_REGISTER_TYPE_INFO inInfo  = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** ppActivate = nullptr;
    UINT32 encCount = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
                           &inInfo, &outInfo, &ppActivate, &encCount);

    if (FAILED(hr) || encCount == 0)
    {
        if (ppActivate) CoTaskMemFree(ppActivate);
        if (pDevMgr) pDevMgr->Release();
        OutError = "MF H.264: 未找到 H.264 硬件编码器 MFT";
        return false;
    }

    // 遍历所有返回的 MFT，找第一个能成功初始化的
    IMFTransform* pMFT = nullptr;
    for (UINT32 i = 0; i < encCount; i++)
    {
        hr = ppActivate[i]->ActivateObject(IID_PPV_ARGS(&pMFT));
        if (FAILED(hr) || !pMFT) continue;

        if (pDevMgr)
        {
            pMFT->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                  reinterpret_cast<ULONG_PTR>(pDevMgr));
        }

        IMFMediaType* pOutType = nullptr;
        MFCreateMediaType(&pOutType);
        pOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pOutType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        pOutType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(BitRate));
        pOutType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(pOutType, MF_MT_FRAME_SIZE, static_cast<UINT32>(EncodeWidth), static_cast<UINT32>(EncodeHeight));
        MFSetAttributeRatio(pOutType, MF_MT_FRAME_RATE, static_cast<UINT32>(FrameRate), 1);

        hr = pMFT->SetOutputType(0, pOutType, 0);
        pOutType->Release();

        if (FAILED(hr)) { pMFT->Release(); pMFT = nullptr; continue; }

        IMFMediaType* pInType = nullptr;
        MFCreateMediaType(&pInType);
        pInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pInType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        pInType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(pInType, MF_MT_FRAME_SIZE, static_cast<UINT32>(EncodeWidth), static_cast<UINT32>(EncodeHeight));
        MFSetAttributeRatio(pInType, MF_MT_FRAME_RATE, static_cast<UINT32>(FrameRate), 1);

        hr = pMFT->SetInputType(0, pInType, 0);
        pInType->Release();

        if (FAILED(hr)) { pMFT->Release(); pMFT = nullptr; continue; }

        break;
    }

    for (UINT32 i = 0; i < encCount; i++)
        ppActivate[i]->Release();
    CoTaskMemFree(ppActivate);

    if (!pMFT)
    {
        if (pDevMgr) pDevMgr->Release();
        OutError = "MF H.264: 所有编码器 MFT 初始化失败（已尝试 " + std::to_string(encCount) + " 个）";
        return false;
    }

    DeviceManager = pDevMgr;
    ResetToken = static_cast<int32>(resetToken);
    EncoderMFT = pMFT;

    InputNV12Size = EncodeWidth * EncodeHeight * 3 / 2;
    SampleTime = 0;

    pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);

    // 预热：调用 ProcessOutput 让硬件 MFT 完成异步资源分配
    {
        MFT_OUTPUT_DATA_BUFFER preBuffer = {};
        preBuffer.dwStreamID = 0;
        for (int warmup = 0; warmup < 10; warmup++)
        {
            DWORD status = 0;
            HRESULT hrWarm = pMFT->ProcessOutput(0, 1, &preBuffer, &status);
            if (hrWarm == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (SUCCEEDED(hrWarm) && preBuffer.pSample)
            {
                preBuffer.pSample->Release();
                preBuffer.pSample = nullptr;
            }
            if (FAILED(hrWarm)) break;
        }
    }

    return true;
}

bool FMFEncoderNew::EncodeFrameH264(const void* pInputNV12, int32 Width, int32 Height,
                                     std::vector<uint8_t>& OutAnnexB, bool& /*OutIsKeyFrame*/)
{
    if (!EncoderMFT || !pInputNV12) return false;

    IMFTransform* pMFT = static_cast<IMFTransform*>(EncoderMFT);
    HRESULT hr;

    int32 nv12Size = Width * Height * 3 / 2;
    LONGLONG rtDuration = 10000000LL / FrameRate;

    IMFSample* pSample = nullptr;
    hr = MFCreateSample(&pSample);
    if (FAILED(hr)) return false;

    IMFMediaBuffer* pBuffer = nullptr;
    hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12Size), &pBuffer);
    if (FAILED(hr)) { pSample->Release(); return false; }

    BYTE* pData = nullptr;
    hr = pBuffer->Lock(&pData, nullptr, nullptr);
    if (FAILED(hr)) { pBuffer->Release(); pSample->Release(); return false; }

    memcpy(pData, pInputNV12, static_cast<size_t>(nv12Size));
    pBuffer->Unlock();
    pBuffer->SetCurrentLength(static_cast<DWORD>(nv12Size));
    pSample->AddBuffer(pBuffer);
    pBuffer->Release();

    pSample->SetSampleTime(SampleTime);
    pSample->SetSampleDuration(rtDuration);

    hr = pMFT->ProcessInput(0, pSample, 0);
    pSample->Release();

    if (hr == MF_E_NOTACCEPTING)
    {
        DrainOutputInternal(pMFT, OutAnnexB);

        // 重建 sample 并重试
        IMFSample* pRetrySample = nullptr;
        if (FAILED(MFCreateSample(&pRetrySample))) return false;
        IMFMediaBuffer* pRetryBuf = nullptr;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12Size), &pRetryBuf)))
        { pRetrySample->Release(); return false; }
        BYTE* pRetryData = nullptr;
        if (FAILED(pRetryBuf->Lock(&pRetryData, nullptr, nullptr)))
        { pRetryBuf->Release(); pRetrySample->Release(); return false; }
        memcpy(pRetryData, pInputNV12, static_cast<size_t>(nv12Size));
        pRetryBuf->Unlock();
        pRetryBuf->SetCurrentLength(static_cast<DWORD>(nv12Size));
        pRetrySample->AddBuffer(pRetryBuf);
        pRetryBuf->Release();
        pRetrySample->SetSampleTime(SampleTime);
        pRetrySample->SetSampleDuration(rtDuration);
        hr = pMFT->ProcessInput(0, pRetrySample, 0);
        pRetrySample->Release();
        if (FAILED(hr)) return false;
    }
    else if (FAILED(hr))
    {
        return false;
    }

    SampleTime += rtDuration;
    DrainOutputInternal(pMFT, OutAnnexB);

    return !OutAnnexB.empty();
}

bool FMFEncoderNew::Drain(std::vector<uint8_t>& OutAnnexB)
{
    if (!EncoderMFT) return false;

    IMFTransform* pMFT = static_cast<IMFTransform*>(EncoderMFT);

    pMFT->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    bool bGot = DrainOutputInternal(pMFT, OutAnnexB);
    pMFT->ProcessMessage(static_cast<MFT_MESSAGE_TYPE>(0x10000001), 0);

    return bGot;
}

void FMFEncoderNew::FinalizeH264()
{
    if (EncoderMFT)
    {
        IMFTransform* pMFT = static_cast<IMFTransform*>(EncoderMFT);
        pMFT->ProcessMessage(static_cast<MFT_MESSAGE_TYPE>(0x10000001), 0);
        pMFT->Release();
        EncoderMFT = nullptr;
    }
    if (DeviceManager)
    {
        static_cast<IMFDXGIDeviceManager*>(DeviceManager)->Release();
        DeviceManager = nullptr;
    }
}

// ============================================================================
// WMV MFSinkWriter 路径（兜底：SinkWriter 直接写 ASF/WMV 文件）
// ============================================================================

bool FMFEncoderNew::InitializeWMV(const FEncoderConfig& Config, std::string& OutError)
{
    if (OutputPath.empty())
    {
        OutError = "WMV: 输出路径未设置（WMV 模式需要 StartRecording 时指定路径）";
        return false;
    }

    int len = MultiByteToWideChar(CP_UTF8, 0, OutputPath.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> widePath(len);
    MultiByteToWideChar(CP_UTF8, 0, OutputPath.c_str(), -1, widePath.data(), len);

    IMFAttributes* pAttr = nullptr;
    MFCreateAttributes(&pAttr, 3);
    pAttr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_ASF);
    pAttr->SetUINT32(MF_LOW_LATENCY, TRUE);
    pAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    HRESULT hr = MFCreateSinkWriterFromURL(widePath.data(), nullptr, pAttr, (IMFSinkWriter**)&SinkWriter);
    pAttr->Release();

    if (FAILED(hr) || !SinkWriter)
    {
        OutError = "WMV: 创建 SinkWriter 失败 (HR=0x" + std::to_string(static_cast<int>(hr)) + ")";
        return false;
    }

    IMFMediaType* pInputType = nullptr;
    MFCreateMediaType(&pInputType);
    pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    pInputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, static_cast<UINT32>(EncodeWidth), static_cast<UINT32>(EncodeHeight));
    MFSetAttributeRatio(pInputType, MF_MT_FRAME_RATE, static_cast<UINT32>(FrameRate), 1);

    hr = static_cast<IMFSinkWriter*>(SinkWriter)->AddStream(pInputType, &StreamIndex);
    pInputType->Release();

    if (FAILED(hr))
    {
        OutError = "WMV: AddStream 失败 (HR=0x" + std::to_string(static_cast<int>(hr)) + ")";
        static_cast<IMFSinkWriter*>(SinkWriter)->Release();
        SinkWriter = nullptr;
        return false;
    }

    IMFMediaType* pOutputType = nullptr;
    MFCreateMediaType(&pOutputType);
    pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_WMV3);
    pOutputType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(BitRate));
    MFSetAttributeRatio(pOutputType, MF_MT_FRAME_RATE, static_cast<UINT32>(FrameRate), 1);
    MFSetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, static_cast<UINT32>(EncodeWidth), static_cast<UINT32>(EncodeHeight));

    hr = static_cast<IMFSinkWriter*>(SinkWriter)->SetInputMediaType(StreamIndex, pOutputType, nullptr);
    pOutputType->Release();

    if (FAILED(hr))
    {
        OutError = "WMV: SetInputMediaType 失败 (HR=0x" + std::to_string(static_cast<int>(hr)) + ")，"
                   "系统可能缺少 WMV 编码器";
        static_cast<IMFSinkWriter*>(SinkWriter)->Release();
        SinkWriter = nullptr;
        return false;
    }

    hr = static_cast<IMFSinkWriter*>(SinkWriter)->BeginWriting();
    if (FAILED(hr))
    {
        OutError = "WMV: BeginWriting 失败 (HR=0x" + std::to_string(static_cast<int>(hr)) + ")";
        static_cast<IMFSinkWriter*>(SinkWriter)->Release();
        SinkWriter = nullptr;
        return false;
    }

    InputNV12Size = EncodeWidth * EncodeHeight * 3 / 2;
    SampleTime = 0;

    return true;
}

bool FMFEncoderNew::EncodeFrameWMV(const void* pInputNV12, int32 Width, int32 Height)
{
    if (!SinkWriter || !pInputNV12) return false;

    IMFSample* pSample = nullptr;
    if (FAILED(MFCreateSample(&pSample))) return false;

    int32 nv12Size = Width * Height * 3 / 2;
    IMFMediaBuffer* pBuffer = nullptr;
    if (FAILED(MFCreateMemoryBuffer(nv12Size, &pBuffer))) { pSample->Release(); return false; }

    BYTE* pData = nullptr;
    if (FAILED(pBuffer->Lock(&pData, nullptr, nullptr))) { pBuffer->Release(); pSample->Release(); return false; }

    memcpy(pData, pInputNV12, static_cast<size_t>(nv12Size));
    pBuffer->Unlock();
    pBuffer->SetCurrentLength(static_cast<DWORD>(nv12Size));
    pSample->AddBuffer(pBuffer);
    pBuffer->Release();

    LONGLONG rtDuration = 10000000LL / FrameRate;
    pSample->SetSampleTime(SampleTime);
    pSample->SetSampleDuration(rtDuration);

    HRESULT hr = static_cast<IMFSinkWriter*>(SinkWriter)->WriteSample(StreamIndex, pSample);
    pSample->Release();

    if (SUCCEEDED(hr))
    {
        SampleTime += rtDuration;
        return true;
    }

    return false;
}

void FMFEncoderNew::FinalizeWMV()
{
    if (SinkWriter)
    {
        auto* sw = static_cast<IMFSinkWriter*>(SinkWriter);
        sw->Finalize();
        sw->Release();
        SinkWriter = nullptr;
    }
}

// ============================================================================
// 统一接口（根据模式分发）
// ============================================================================

bool FMFEncoderNew::EncodeFrame(const void* pInputNV12, int32 Width, int32 Height,
                                 std::vector<uint8_t>& OutAnnexB, bool& OutIsKeyFrame)
{
    if (!bInitialized) return false;

    if (bWMVMode)
    {
        OutAnnexB.clear();
        OutIsKeyFrame = false;
        return EncodeFrameWMV(pInputNV12, Width, Height);
    }

    return EncodeFrameH264(pInputNV12, Width, Height, OutAnnexB, OutIsKeyFrame);
}

bool FMFEncoderNew::Finalize(std::string& /*OutError*/)
{
    if (!bInitialized) return true;

    if (bWMVMode)
        FinalizeWMV();
    else
        FinalizeH264();

    bInitialized = false;
    ReleaseMFStartup();
    return true;
}
