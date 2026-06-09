#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Vane/VaneAPI.h"
#include "Vane/VaneConfig.h"

using int32 = int32_t;
using int64 = int64_t;

// 前向声明 NVENC 结构体（实际定义在 nvEncodeAPI.h）
typedef struct _NV_ENCODE_API_FUNCTION_LIST NV_ENCODE_API_FUNCTION_LIST;

// NVENC 原生编码器（P0 首选）
// 通过 LoadLibrary("nvEncodeAPI64.dll") 动态加载，编译期零 NVENC SDK 库依赖
class VANE_API FNvencEncoder
{
public:
    FNvencEncoder();
    ~FNvencEncoder();

    bool Initialize(void* pD3D11Device, const FEncoderConfig& Config, std::string& OutError);
    bool EncodeFrame(const void* pInputTexture, int32 Width, int32 Height,
                     std::vector<uint8_t>& OutAnnexB, bool& OutIsKeyFrame);
    bool Finalize(std::string& OutError);
    const char* GetCodecName() const { return EncoderCodec == "h265" ? "NVENC H.265" : "NVENC H.264"; }
    void* GetEncoderHandle() const { return EncoderHandle; }

    // 检查 DLL 是否存在（快速检查，不保证 codec 可用）
    static bool IsAvailable();

    // 真实验证：打开临时编码会话查询 codec GUID（P0 关键修复）
    // 需要有效的 D3D11 Device 才能探测；调用一次即缓存结果
    static bool IsH264Supported(void* pD3D11Device);
    static bool IsHEVCSupported(void* pD3D11Device);

private:
    void* EncoderHandle = nullptr;
    void* NvEncDLL = nullptr;
    bool  bInitialized = false;

    NV_ENCODE_API_FUNCTION_LIST* NvEnc = nullptr;

    std::unordered_map<void*, void*> RegisteredResourceMap;  // D3D11 ptr → NVENC registered handle
    void* OutputBitstreamBuffer = nullptr;

    int32 EncodeWidth = 0;
    int32 EncodeHeight = 0;
    int32 FrameRate = 0;
    int32 BitRate = 0;
    int32 FrameIndex = 0;
    std::string EncoderCodec = "h264";

    bool CreateAndRegisterResource(void* pD3D11Texture, int32 Width, int32 Height);
    bool GetEncodedBitstream(std::vector<uint8_t>& OutAnnexB, bool& OutIsKeyFrame);
};
