#pragma once

#include <cstdint>
#include <vector>

using int32 = int32_t;
using uint8 = uint8_t;

// BGRA ↔ NV12 色彩空间转换器
// 内部使用 D3D11 Compute Shader（Windows）或 CPU 回退
class ColorSpaceConverter
{
public:
    ColorSpaceConverter();
    ~ColorSpaceConverter();

    // 初始化 D3D11 设备（需引擎传入，不自行创建 GPU 上下文）
    // 非 Windows 平台始终返回 false
    bool InitializeDX11(void* pD3D11Device);

    // CPU 路径：BGRA32 → NV12（始终可用，无需 D3D11 初始化）
    static bool ConvertBGRAToNV12_CPU(const uint8_t* BGRA, int32 Width, int32 Height, uint8_t* NV12Out);

    // D3D11 路径：BGRA32 纹理 → NV12 纹理
    bool ConvertBGRAToNV12_GPU(void* pBGRAInputTexture, void* pNV12OutputTexture, int32 Width, int32 Height);

    bool IsDX11Available() const;

private:
    void* D3D11Device = nullptr;
    void* D3D11Context = nullptr;
    void* ComputeShader = nullptr;
    void* InputSRV = nullptr;
    void* OutputUAV = nullptr;
    bool bDX11Initialized = false;
};
