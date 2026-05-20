#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Vane/VaneConfig.h"

using int32 = int32_t;

// AMD AMF 原生编码器（P1）
// 通过 LoadLibrary("amfrt64.dll") 动态加载
// 当前占位：接口声明完整，实现为 TODO
class FAmfEncoder
{
public:
    FAmfEncoder();
    ~FAmfEncoder();

    bool Initialize(void* pD3D11Device, const FEncoderConfig& Config, std::string& OutError);
    bool EncodeFrame(const void* pInputTexture, int32 Width, int32 Height,
                     std::vector<uint8_t>& OutAnnexB, bool& OutIsKeyFrame);
    bool Finalize(std::string& OutError);
    const char* GetCodecName() const { return "AMD AMF H.264"; }
    void* GetEncoderHandle() const { return nullptr; }

    static bool IsAvailable();

private:
    void* AmfDLL = nullptr;
    bool  bInitialized = false;
    int32 EncodeWidth = 0;
    int32 EncodeHeight = 0;
};
