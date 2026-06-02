#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Vane/VaneAPI.h"
#include "Vane/VaneConfig.h"

using int32 = int32_t;
using int64 = int64_t;

// Media Foundation 编码器（P2 兜底，双模式：H.264 / WMV）
// H.264 模式：IMFTransform → Annex B → 外部 MP4Writer 封装
// WMV 模式：MFSinkWriter 直接写 ASF/WMV 文件（容器+编码一体）
class VANE_API FMFEncoderNew
{
public:
    FMFEncoderNew();
    ~FMFEncoderNew();

    // Initialize: 自动探测 H.264 可用性，优先 H.264，不可用则降级到 WMV
    bool Initialize(void* pD3D11Device, const FEncoderConfig& Config, std::string& OutError);
    bool EncodeFrame(const void* pInputNV12, int32 Width, int32 Height,
                     std::vector<uint8_t>& OutAnnexB, bool& OutIsKeyFrame);
    // 排空 MFT 内部缓冲（应在 MP4Writer::Finalize 之前调用）
    bool Drain(std::vector<uint8_t>& OutAnnexB);
    bool Finalize(std::string& OutError);
    const char* GetCodecName() const;
    void* GetEncoderHandle() const { return nullptr; }

    static bool IsAvailable();
    static bool IsH264Available();
    static bool IsWMVAvailable();

    // 查询当前是否使用 WMV 模式（调用者需据此跳过 MP4Writer）
    bool IsWMVMode() const { return bWMVMode; }

    // WMV 模式：在 Initialize 之前设置输出路径（SinkWriter 创建时需要）
    void SetOutputPath(const char* Path) { OutputPath = Path; }

private:
    // ---- H.264 IMFTransform 路径 ----
    bool InitializeH264(void* pD3D11Device, const FEncoderConfig& Config, std::string& OutError);
    bool EncodeFrameH264(const void* pInputNV12, int32 Width, int32 Height,
                         std::vector<uint8_t>& OutAnnexB, bool& OutIsKeyFrame);

    // ---- WMV MFSinkWriter 路径 ----
    bool InitializeWMV(const FEncoderConfig& Config, std::string& OutError);
    bool EncodeFrameWMV(const void* pInputNV12, int32 Width, int32 Height);

    void FinalizeH264();
    void FinalizeWMV();

    // ---- 公共 ----
    void* DeviceManager = nullptr;
    void* EncoderMFT = nullptr;     // H.264: IMFTransform
    void* SinkWriter = nullptr;     // WMV: IMFSinkWriter
    void* D3D11Device = nullptr;
    unsigned long StreamIndex = 0;  // WMV: SinkWriter stream index

    std::string OutputPath;         // WMV: 输出文件路径

    bool   bInitialized = false;
    bool   bWMVMode = false;        // true = WMV SinkWriter 路径
    int32  EncodeWidth = 0;
    int32  EncodeHeight = 0;
    int32  FrameRate = 0;
    int32  BitRate = 0;
    int32  ResetToken = 0;
    int32  InputNV12Size = 0;
    int64  SampleTime = 0;
};
