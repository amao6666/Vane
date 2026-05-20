#include "FWindowsEncoder.h"
#include "windows/FNvencEncoder.h"
#include "windows/FAmfEncoder.h"
#include "windows/FMFEncoderNew.h"
#include "core/D3D11Converter.h"

#include <cstring>
#include <thread>
#include <d3d11.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

FWindowsEncoder::FWindowsEncoder()
    : Ptr(std::make_unique<Impl>())
{
}

FWindowsEncoder::~FWindowsEncoder()
{
    StopRecording();
}

// ============================================================================
// 硬件探测（真实验证）
// ============================================================================

EWindowsEncoderType FWindowsEncoder::ProbeHardware() const
{
    // P0 修复：使用真实编码会话探测，而非仅检查 DLL 存在
    bool bNvencDll = FNvencEncoder::IsAvailable();
    bool bNvencReal = bNvencDll && Ptr->D3D11Device
                      && FNvencEncoder::IsH264Supported(Ptr->D3D11Device);

    if (bNvencReal)
        return EWindowsEncoderType::NVENC;

    if (FAmfEncoder::IsAvailable())
        return EWindowsEncoderType::AMF;

    // MF：优先 H.264，兜底 WMV
    return EWindowsEncoderType::MF;
}

void FWindowsEncoder::SetD3D11Device(void* pDevice)
{
    Ptr->D3D11Device = pDevice;
}

EWindowsEncoderType FWindowsEncoder::GetEncoderType() const
{
    if (Ptr->EncoderType != EWindowsEncoderType::None)
        return Ptr->EncoderType;
    return ProbeHardware();
}

// ============================================================================
// 统一内部编码器清理
// ============================================================================
static void DestroyInternalEncoder(EWindowsEncoderType T, void* Enc)
{
    if (!Enc) return;
    std::string dummy;
    switch (T)
    {
    case EWindowsEncoderType::NVENC:
        static_cast<FNvencEncoder*>(Enc)->Finalize(dummy);
        delete static_cast<FNvencEncoder*>(Enc);
        break;
    case EWindowsEncoderType::AMF:
        static_cast<FAmfEncoder*>(Enc)->Finalize(dummy);
        delete static_cast<FAmfEncoder*>(Enc);
        break;
    case EWindowsEncoderType::MF:
    default:
        static_cast<FMFEncoderNew*>(Enc)->Finalize(dummy);
        delete static_cast<FMFEncoderNew*>(Enc);
        break;
    }
}

// ============================================================================
// 创建内部编码器
// ============================================================================
static bool CreateInternalEncoder(EWindowsEncoderType T, void* D3D11Device,
                                   const FEncoderConfig& Cfg,
                                   void*& OutEnc, std::string& OutErr)
{
    switch (T)
    {
    case EWindowsEncoderType::NVENC:
    {
        auto* enc = new FNvencEncoder();
        if (enc->Initialize(D3D11Device, Cfg, OutErr)) { OutEnc = enc; return true; }
        delete enc;
        return false;
    }
    case EWindowsEncoderType::AMF:
    {
        auto* enc = new FAmfEncoder();
        if (enc->Initialize(D3D11Device, Cfg, OutErr)) { OutEnc = enc; return true; }
        delete enc;
        return false;
    }
    case EWindowsEncoderType::MF:
    default:
    {
        auto* enc = new FMFEncoderNew();
        if (enc->Initialize(D3D11Device, Cfg, OutErr)) { OutEnc = enc; return true; }
        delete enc;
        return false;
    }
    }
}

// ============================================================================
// 创建 WMV 编码器（MFSinkWriter 路径）
// ============================================================================
static bool CreateWMVEncoder(const char* OutputPath, void* D3D11Device,
                              const FEncoderConfig& Cfg,
                              void*& OutEnc, std::string& OutErr)
{
    auto* enc = new FMFEncoderNew();
    enc->SetOutputPath(OutputPath);

    // WMV Initialize 不需要 D3D11 device（SinkWriter 内部管理）
    if (enc->Initialize(nullptr, Cfg, OutErr))
    {
        OutEnc = enc;
        return true;
    }

    delete enc;
    return false;
}

// ============================================================================
// IVideoEncoder 实现
// ============================================================================

bool FWindowsEncoder::Initialize(const FEncoderConfig& Config)
{
    if (Ptr->bInitialized) return true;
    if (Config.Width <= 0 || Config.Height <= 0)
    {
        Ptr->LastError = "分辨率无效：宽度和高度必须大于 0";
        return false;
    }

    Ptr->Config = Config;
    Ptr->EncoderType = ProbeHardware();

    const char* typeNames[] = { "NVENC", "AMF", "MF" };
    int typeIdx = static_cast<int>(Ptr->EncoderType);
    Ptr->CodecDescription = (typeIdx >= 0 && typeIdx < 3) ? typeNames[typeIdx] : "Unknown";

    Ptr->bInitialized = true;
    return true;
}

bool FWindowsEncoder::StartRecording(const char* OutputPath)
{
    if (!Ptr->bInitialized)
    {
        Ptr->LastError = "编码器未初始化：请先调用 Initialize()";
        return false;
    }
    if (Ptr->bRecording)
    {
        Ptr->LastError = "编码器已在录制中";
        return false;
    }

    if (Ptr->StateCb)
        Ptr->StateCb(ERecordingState_Starting, Ptr->StateUserData);

    // 按优先级创建内部编码器：NVENC → AMF → MF H.264 → WMV
    std::string err;
    void* enc = nullptr;
    EWindowsEncoderType actualType = Ptr->EncoderType;
    bool bUseWMVPath = false;

    // 回退链标记
    bool bTried[4] = {};
    // 标记初始优先类型为"已尝试"，后续回退时会跳过
    bTried[static_cast<int>(actualType)] = true;

    // 尝试初始优先类型
    bool bEncoderReady = false;
    if (CreateInternalEncoder(actualType, Ptr->D3D11Device, Ptr->Config, enc, err))
    {
        // NVENC 需额外验证 D3D11Converter（GPU BGRA→NV12 转换必须可用）
        if (actualType == EWindowsEncoderType::NVENC)
        {
            if (Ptr->D3D11Device)
            {
                ID3D11Device* dev = static_cast<ID3D11Device*>(Ptr->D3D11Device);
                auto* conv = new D3D11Converter(dev, Ptr->Config.Width, Ptr->Config.Height);
                if (conv->IsValid())
                {
                    Ptr->D3D11Converter = conv;
                    bEncoderReady = true;
                }
                else
                {
                    delete conv;
                    err = "NVENC: D3D11 VideoProcessor 不可用（GPU 不支持 DXVA BGRA→NV12）";
                    DestroyInternalEncoder(EWindowsEncoderType::NVENC, enc);
                    enc = nullptr;
                }
            }
            else
            {
                err = "NVENC: D3D11 Device 为空，无法创建 GPU 色彩转换器";
                DestroyInternalEncoder(EWindowsEncoderType::NVENC, enc);
                enc = nullptr;
            }
        }
        else
        {
            bEncoderReady = true;
        }
    }
    else
    {
        err.clear(); // 初始失败的错误不阻塞回退链
    }

    if (Ptr->ErrorCb && !err.empty())
        Ptr->ErrorCb(EErrorLevel_Warning, err.c_str(), Ptr->ErrorUserData);

    // 回退链：按优先级依次尝试
    if (!bEncoderReady)
    {
        // NVENC
        if (!bTried[0])
        {
            bTried[0] = true;
            err.clear();
            if (CreateInternalEncoder(EWindowsEncoderType::NVENC, Ptr->D3D11Device, Ptr->Config, enc, err))
            {
                if (Ptr->D3D11Device)
                {
                    ID3D11Device* dev = static_cast<ID3D11Device*>(Ptr->D3D11Device);
                    auto* conv = new D3D11Converter(dev, Ptr->Config.Width, Ptr->Config.Height);
                    if (conv->IsValid())
                    {
                        Ptr->D3D11Converter = conv;
                        actualType = EWindowsEncoderType::NVENC;
                        bEncoderReady = true;
                    }
                    else
                    {
                        delete conv;
                        err = "NVENC: D3D11 VideoProcessor 不可用（GPU 不支持 DXVA BGRA→NV12）";
                        DestroyInternalEncoder(EWindowsEncoderType::NVENC, enc);
                        enc = nullptr;
                    }
                }
                else
                {
                    err = "NVENC: D3D11 Device 为空，无法创建 GPU 色彩转换器";
                    DestroyInternalEncoder(EWindowsEncoderType::NVENC, enc);
                    enc = nullptr;
                }
            }
            if (Ptr->ErrorCb && !err.empty())
                Ptr->ErrorCb(EErrorLevel_Warning, err.c_str(), Ptr->ErrorUserData);
        }

        // AMF
        if (!bEncoderReady && !bTried[1])
        {
            bTried[1] = true;
            err.clear();
            if (CreateInternalEncoder(EWindowsEncoderType::AMF, Ptr->D3D11Device, Ptr->Config, enc, err))
            {
                actualType = EWindowsEncoderType::AMF;
                bEncoderReady = true;
            }
            if (Ptr->ErrorCb && !err.empty())
                Ptr->ErrorCb(EErrorLevel_Warning, err.c_str(), Ptr->ErrorUserData);
        }

        // MF H.264
        if (!bEncoderReady && !bTried[2] && FMFEncoderNew::IsH264Available())
        {
            bTried[2] = true;
            err.clear();
            if (CreateInternalEncoder(EWindowsEncoderType::MF, Ptr->D3D11Device, Ptr->Config, enc, err))
            {
                actualType = EWindowsEncoderType::MF;
                bEncoderReady = true;
            }
            if (Ptr->ErrorCb && !err.empty())
                Ptr->ErrorCb(EErrorLevel_Warning, err.c_str(), Ptr->ErrorUserData);
        }

        // WMV 最终兜底（MFSinkWriter 直接写 ASF 文件）
        if (!bEncoderReady && !bTried[3] && FMFEncoderNew::IsWMVAvailable())
        {
            err.clear();
            if (CreateWMVEncoder(OutputPath, Ptr->D3D11Device, Ptr->Config, enc, err))
            {
                actualType = EWindowsEncoderType::MF;
                bUseWMVPath = true;
                bEncoderReady = true;
                if (Ptr->ErrorCb)
                    Ptr->ErrorCb(EErrorLevel_Warning,
                        "H.264 编码器不可用，已自动降级为 WMV 格式。"
                        "建议安装显卡驱动或媒体功能包。", Ptr->ErrorUserData);
            }
            if (Ptr->ErrorCb && !err.empty())
                Ptr->ErrorCb(EErrorLevel_Warning, err.c_str(), Ptr->ErrorUserData);
        }
    }

    if (!bEncoderReady || !enc)
    {
        Ptr->LastError = "所有编码器均不可用。最后错误: " + err;
        if (Ptr->StateCb)
            Ptr->StateCb(ERecordingState_Error, Ptr->StateUserData);
        return false;
    }

    Ptr->InternalEncoder = enc;
    Ptr->EncoderType = actualType;
    Ptr->bUseWMVPath = bUseWMVPath;

    // ========================================================================
    // 创建 MP4Writer（仅 H.264 路径需要；WMV 由 MFSinkWriter 内部管理文件）
    // ========================================================================
    if (!bUseWMVPath)
    {
        DeleteFileA(OutputPath);

        Ptr->Muxer = std::make_unique<MP4Writer>();
        if (!Ptr->Muxer->Create(OutputPath, Ptr->Config.Width, Ptr->Config.Height, Ptr->Config.FrameRate))
        {
            Ptr->LastError = "创建输出文件失败: ";
            Ptr->LastError += OutputPath;

            DestroyInternalEncoder(Ptr->EncoderType, Ptr->InternalEncoder);
            Ptr->InternalEncoder = nullptr;

            if (Ptr->StateCb)
                Ptr->StateCb(ERecordingState_Error, Ptr->StateUserData);
            return false;
        }
    }

    // 预分配 NV12 转换缓冲（所有路径共用）
    Ptr->Nv12BufSize = Ptr->Config.Width * Ptr->Config.Height * 3 / 2;
    Ptr->Nv12Buf.resize(Ptr->Nv12BufSize);

    // NVENC 路径：创建 DXVA GPU 色彩转换器（BGRA→NV12，避免 CPU 8MB 转换）
    // 注意：可能在 fallback 链中已提前创建（并验证通过），此处仅作未创建时的兜底
    if (actualType == EWindowsEncoderType::NVENC && Ptr->D3D11Device && !Ptr->D3D11Converter)
    {
        ID3D11Device* dev = static_cast<ID3D11Device*>(Ptr->D3D11Device);
        auto* conv = new D3D11Converter(dev, Ptr->Config.Width, Ptr->Config.Height);
        if (conv->IsValid())
        {
            Ptr->D3D11Converter = conv;
        }
        else
        {
            delete conv;
            // D3D11Converter 无效：销毁 NVENC 编码器，报错
            DestroyInternalEncoder(EWindowsEncoderType::NVENC, Ptr->InternalEncoder);
            Ptr->InternalEncoder = nullptr;
            Ptr->LastError = "NVENC: D3D11 VideoProcessor 不可用（GPU 不支持 DXVA）";
            if (Ptr->StateCb)
                Ptr->StateCb(ERecordingState_Error, Ptr->StateUserData);
            return false;
        }
    }

    // ========================================================================
    // 启动异步编码管线
    // ========================================================================
    Impl* p = Ptr.get();

    FEncodeCallback encCb = [p](const FFrameBuffer& Frame) {
        bool bNeedCom = (p->EncoderType == EWindowsEncoderType::MF);
        if (bNeedCom) CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (!Frame.bValid || p->bHasError)
        {
            if (bNeedCom) CoUninitialize();
            return;
        }

#ifdef VANE_TEST_ENCODING_DELAY_MS
        std::this_thread::sleep_for(std::chrono::milliseconds(VANE_TEST_ENCODING_DELAY_MS));
#endif

        int32 w = p->Config.Width;
        int32 h = p->Config.Height;

        // 编码
        std::vector<uint8_t> annexB;
        bool bIsKeyFrame = false;
        bool bEncoded = false;

        switch (p->EncoderType)
        {
        case EWindowsEncoderType::NVENC:
        {
            auto* enc = static_cast<FNvencEncoder*>(p->InternalEncoder);
            auto* conv = static_cast<D3D11Converter*>(p->D3D11Converter);

            if (conv)
            {
                // P2: GPU 加速 BGRA→NV12（DXVA VideoProcessBlt，避免 CPU 8MB 转换）
                ID3D11Texture2D* nv12Tex = conv->Convert(Frame.Data);
                if (nv12Tex)
                {
                    bEncoded = enc->EncodeFrame(nv12Tex, w, h, annexB, bIsKeyFrame);
                }
            }
            break;
        }
        case EWindowsEncoderType::AMF:
        {
            auto* enc = static_cast<FAmfEncoder*>(p->InternalEncoder);
            ColorSpaceConverter::ConvertBGRAToNV12_CPU(Frame.Data, w, h, p->Nv12Buf.data());
            bEncoded = enc->EncodeFrame(p->Nv12Buf.data(), w, h, annexB, bIsKeyFrame);
            break;
        }
        case EWindowsEncoderType::MF:
        default:
        {
            auto* enc = static_cast<FMFEncoderNew*>(p->InternalEncoder);
            ColorSpaceConverter::ConvertBGRAToNV12_CPU(Frame.Data, w, h, p->Nv12Buf.data());
            bEncoded = enc->EncodeFrame(p->Nv12Buf.data(), w, h, annexB, bIsKeyFrame);
            break;
        }
        }

        // 写入 MP4Writer（仅 H.264 路径；WMV 由 SinkWriter 内部管理）
        if (bEncoded)
        {
            if (p->bUseWMVPath)
            {
                // WMV 模式：EncodeFrame 已将 NV12 写入 SinkWriter，无需额外操作
                p->TotalFramesEncoded += 1;
                // WMV 路径不统计 BytesWritten（由 SinkWriter 内部管理）
            }
            else if (!annexB.empty())
            {
                int64 frameIdx = p->TotalFramesEncoded;
                int64 pts = frameIdx;
                p->Muxer->WriteFrame(annexB.data(), annexB.size(), pts, bIsKeyFrame);

                p->TotalFramesEncoded = frameIdx + 1;
                p->TotalBytesWritten += static_cast<int64_t>(annexB.size());
            }

            // 进度回调（每秒一次）
            if (p->ProgressCb)
            {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - p->LastProgressTime).count();
                if (elapsed >= 1000)
                {
                    p->ProgressCb(p->TotalFramesEncoded, p->TotalBytesWritten,
                                  static_cast<double>(p->TotalFramesEncoded) / p->Config.FrameRate,
                                  p->ProgressUserData);
                    p->LastProgressTime = now;
                }
            }
        }

        if (bNeedCom) CoUninitialize();
    };

    FFlushCallback flushCb = []() {};

    p->Pipeline.Start(std::move(encCb), std::move(flushCb),
                      p->Config.FrameQueueSize, p->Config.FlushIntervalFrames,
                      p->Config.bContinueLastFrame, p->Config.RecordFrameRate);

    // 初始化统计
    p->TotalFramesEncoded = 0;
    p->TotalBytesWritten  = 0;
    p->LastProgressTime   = std::chrono::steady_clock::now();
    p->LastDroppedCount   = 0;
    p->bHasError          = false;
    p->bRecording         = true;

    if (p->StateCb)
        p->StateCb(ERecordingState_Recording, p->StateUserData);

    return true;
}

bool FWindowsEncoder::EncodeFrame(const uint8* RawBGRA, int32 DataSize, double TimestampSeconds)
{
    if (!Ptr->bRecording)
    {
        Ptr->LastError = "编码器未在录制状态";
        return false;
    }
    if (!RawBGRA || DataSize <= 0)
    {
        Ptr->LastError = "输入帧数据为空";
        return false;
    }

    Ptr->Pipeline.PushFrame(RawBGRA, static_cast<size_t>(DataSize), TimestampSeconds,
                            Ptr->Config.RecordFrameRate);

    int32 dropped = static_cast<int32>(Ptr->Pipeline.GetDroppedFrames());
    if (dropped > Ptr->LastDroppedCount)
    {
        Ptr->LastDroppedCount = dropped;
        if (Ptr->FrameDropCb)
            Ptr->FrameDropCb(dropped, Ptr->FrameDropUserData);
    }

    return true;
}

void FWindowsEncoder::StopRecording()
{
    if (!Ptr->bRecording) return;

    Ptr->bRecording = false;

    if (Ptr->StateCb)
        Ptr->StateCb(ERecordingState_Stopping, Ptr->StateUserData);

    Ptr->Pipeline.Stop();

    // MF H.264 路径：排空 MFT 内部缓冲，将剩余编码数据写入 MP4Writer
    if (!Ptr->bUseWMVPath && Ptr->EncoderType == EWindowsEncoderType::MF && Ptr->Muxer)
    {
        auto* mfEnc = static_cast<FMFEncoderNew*>(Ptr->InternalEncoder);
        if (mfEnc && !mfEnc->IsWMVMode())
        {
            std::vector<uint8_t> drainedData;
            if (mfEnc->Drain(drainedData) && !drainedData.empty())
            {
                int64 frameIdx = Ptr->TotalFramesEncoded;
                Ptr->Muxer->WriteFrame(drainedData.data(), drainedData.size(), frameIdx, false);
                Ptr->TotalFramesEncoded = frameIdx + 1;
                Ptr->TotalBytesWritten += static_cast<int64_t>(drainedData.size());
            }
        }
    }

    // WMV 模式：SinkWriter 内部管理文件，跳过 MP4Writer
    if (!Ptr->bUseWMVPath && Ptr->Muxer)
    {
        Ptr->Muxer->Finalize();
        Ptr->Muxer.reset();
    }

    // 最终化内部编码器
    if (Ptr->InternalEncoder)
    {
        std::string err;
        switch (Ptr->EncoderType)
        {
        case EWindowsEncoderType::NVENC:
            static_cast<FNvencEncoder*>(Ptr->InternalEncoder)->Finalize(err);
            delete static_cast<FNvencEncoder*>(Ptr->InternalEncoder);
            break;
        case EWindowsEncoderType::AMF:
            static_cast<FAmfEncoder*>(Ptr->InternalEncoder)->Finalize(err);
            delete static_cast<FAmfEncoder*>(Ptr->InternalEncoder);
            break;
        case EWindowsEncoderType::MF:
        default:
            static_cast<FMFEncoderNew*>(Ptr->InternalEncoder)->Finalize(err);
            delete static_cast<FMFEncoderNew*>(Ptr->InternalEncoder);
            break;
        }
        Ptr->InternalEncoder = nullptr;
    }

    // 释放 D3D11 GPU 转换器
    if (Ptr->D3D11Converter)
    {
        delete static_cast<D3D11Converter*>(Ptr->D3D11Converter);
        Ptr->D3D11Converter = nullptr;
    }

    // 最终进度
    if (Ptr->ProgressCb && Ptr->TotalFramesEncoded > 0)
    {
        Ptr->ProgressCb(Ptr->TotalFramesEncoded, Ptr->TotalBytesWritten,
                        static_cast<double>(Ptr->TotalFramesEncoded) / Ptr->Config.FrameRate,
                        Ptr->ProgressUserData);
    }

    if (Ptr->StateCb)
        Ptr->StateCb(ERecordingState_Idle, Ptr->StateUserData);
}

void FWindowsEncoder::RequestStop()
{
    if (!Ptr->bRecording) return;

    Ptr->bRecording = false;

    if (Ptr->StateCb)
        Ptr->StateCb(ERecordingState_Stopping, Ptr->StateUserData);

    Impl* p = Ptr.get();
    FFinalizeCallback finalizeCb = [p]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // MF H.264 路径：排空 MFT，写入剩余数据到 MP4Writer
        if (!p->bUseWMVPath && p->EncoderType == EWindowsEncoderType::MF && p->Muxer)
        {
            auto* mfEnc = static_cast<FMFEncoderNew*>(p->InternalEncoder);
            if (mfEnc && !mfEnc->IsWMVMode())
            {
                std::vector<uint8_t> drainedData;
                if (mfEnc->Drain(drainedData) && !drainedData.empty())
                {
                    int64 frameIdx = p->TotalFramesEncoded;
                    p->Muxer->WriteFrame(drainedData.data(), drainedData.size(), frameIdx, false);
                    p->TotalFramesEncoded = frameIdx + 1;
                    p->TotalBytesWritten += static_cast<int64_t>(drainedData.size());
                }
            }
        }

        // WMV 模式：SinkWriter 内部管理文件，跳过 MP4Writer
        if (!p->bUseWMVPath && p->Muxer)
        {
            p->Muxer->Finalize();
            p->Muxer.reset();
        }

        if (p->InternalEncoder)
        {
            std::string err;
            switch (p->EncoderType)
            {
            case EWindowsEncoderType::NVENC:
                static_cast<FNvencEncoder*>(p->InternalEncoder)->Finalize(err);
                delete static_cast<FNvencEncoder*>(p->InternalEncoder);
                break;
            case EWindowsEncoderType::AMF:
                static_cast<FAmfEncoder*>(p->InternalEncoder)->Finalize(err);
                delete static_cast<FAmfEncoder*>(p->InternalEncoder);
                break;
            case EWindowsEncoderType::MF:
            default:
                static_cast<FMFEncoderNew*>(p->InternalEncoder)->Finalize(err);
                delete static_cast<FMFEncoderNew*>(p->InternalEncoder);
                break;
            }
            p->InternalEncoder = nullptr;
        }

        if (p->D3D11Converter)
        {
            delete static_cast<D3D11Converter*>(p->D3D11Converter);
            p->D3D11Converter = nullptr;
        }

        if (p->ProgressCb && p->TotalFramesEncoded > 0)
        {
            p->ProgressCb(p->TotalFramesEncoded, p->TotalBytesWritten,
                          static_cast<double>(p->TotalFramesEncoded) / p->Config.FrameRate,
                          p->ProgressUserData);
        }

        if (p->StateCb)
            p->StateCb(ERecordingState_Idle, p->StateUserData);

        CoUninitialize();
    };

    Ptr->Pipeline.RequestStop(std::move(finalizeCb));
}

bool FWindowsEncoder::IsRecording() const
{
    return Ptr->bRecording;
}

const char* FWindowsEncoder::GetLastError() const
{
    return Ptr->LastError.c_str();
}

FEncoderCapability FWindowsEncoder::CheckCapability() const
{
    FEncoderCapability cap;
    cap.DiagnosticInfo.reserve(1024);

    // P0 修复：使用真实编码会话探测
    // IsH264Supported(nullptr) 安全检查静态缓存，不写入（需有效 Device 才会写入缓存）
    bool bNvencDll  = FNvencEncoder::IsAvailable();
    bool bNvencH264 = bNvencDll && FNvencEncoder::IsH264Supported(Ptr->D3D11Device);
    bool bNvencHEVC = bNvencDll && FNvencEncoder::IsHEVCSupported(Ptr->D3D11Device);
    bool bAmf       = FAmfEncoder::IsAvailable();
    bool bMFH264    = FMFEncoderNew::IsH264Available();
    bool bMFWMV     = FMFEncoderNew::IsWMVAvailable();

    cap.bH264Available         = bNvencH264 || bAmf || bMFH264;
    cap.bH264SoftwareAvailable = bMFH264;
    cap.bHEVCAvailable         = bNvencHEVC;
    cap.bWMVAvailable          = bMFWMV;

    cap.DiagnosticInfo += "=== 编码器探测（真实会话验证） ===\n";
    cap.DiagnosticInfo += "  [P0] NVIDIA NVENC:\n";
    cap.DiagnosticInfo += "    DLL: ";
    cap.DiagnosticInfo += bNvencDll ? "已找到\n" : "未找到\n";
    if (bNvencDll)
    {
        cap.DiagnosticInfo += "    H.264 编码: ";
        if (bNvencH264)
            cap.DiagnosticInfo += "可用\n";
        else if (!Ptr->D3D11Device)
            cap.DiagnosticInfo += "无法检测（D3D11 Device 未设置）\n";
        else
            cap.DiagnosticInfo += "不可用（GPU/驱动不支持）\n";
        cap.DiagnosticInfo += "    H.265 编码: ";
        if (bNvencHEVC)
            cap.DiagnosticInfo += "可用\n";
        else if (!Ptr->D3D11Device)
            cap.DiagnosticInfo += "无法检测（D3D11 Device 未设置）\n";
        else
            cap.DiagnosticInfo += "不可用\n";
    }
    cap.DiagnosticInfo += "  [P1] AMD AMF: ";
    cap.DiagnosticInfo += bAmf ? "DLL 已找到（实现 TODO）\n" : "未找到\n";
    cap.DiagnosticInfo += "  [P2] Media Foundation H.264: ";
    cap.DiagnosticInfo += bMFH264 ? "硬件编码器可用\n" : "不可用（缺少系统 codec）\n";
    cap.DiagnosticInfo += "  [P3] Media Foundation WMV: ";
    cap.DiagnosticInfo += bMFWMV ? "始终可用（兜底）\n" : "不可用\n";

    if (bNvencH264)
    {
        cap.H264EncoderName = "NVIDIA NVENC (原生 H.264)";
        cap.RecommendedFormat = "h264";
        cap.DiagnosticInfo += "  选择: NVENC H.264 硬件编码\n";
    }
    else if (bAmf)
    {
        cap.H264EncoderName = "AMD AMF (原生)";
        cap.RecommendedFormat = "h264";
        cap.DiagnosticInfo += "  选择: AMF H.264 硬件编码\n";
    }
    else if (bMFH264)
    {
        cap.H264EncoderName = "Media Foundation H.264 硬件编码";
        cap.RecommendedFormat = "h264";
        cap.DiagnosticInfo += "  选择: MF H.264 硬件编码\n";
    }
    else if (bMFWMV)
    {
        cap.H264EncoderName = "Windows Media Video (WMV)";
        cap.RecommendedFormat = "wmv";
        cap.DiagnosticInfo += "  选择: WMV（无 H.264 编码器可用）\n";
    }
    else
    {
        cap.H264EncoderName = "无可用编码器";
        cap.RecommendedFormat = "none";
        cap.DiagnosticInfo += "  警告: 未找到任何可用视频编码器\n";
    }

    return cap;
}

// ---- 回调注册 ----

void FWindowsEncoder::SetStateCallback(VaneStateCallback Cb, void* UserData)
{
    Ptr->StateCb = Cb;
    Ptr->StateUserData = UserData;
}

void FWindowsEncoder::SetErrorCallback(VaneErrorCallback Cb, void* UserData)
{
    Ptr->ErrorCb = Cb;
    Ptr->ErrorUserData = UserData;
}

void FWindowsEncoder::SetProgressCallback(VaneProgressCallback Cb, void* UserData)
{
    Ptr->ProgressCb = Cb;
    Ptr->ProgressUserData = UserData;
}

void FWindowsEncoder::SetFrameDropCallback(VaneFrameDropCallback Cb, void* UserData)
{
    Ptr->FrameDropCb = Cb;
    Ptr->FrameDropUserData = UserData;
}
