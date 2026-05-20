#include "ColorSpaceConverter.h"
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#endif

ColorSpaceConverter::ColorSpaceConverter() = default;

ColorSpaceConverter::~ColorSpaceConverter()
{
}

bool ColorSpaceConverter::InitializeDX11(void* pD3D11Device)
{
#ifdef _WIN32
    if (!pD3D11Device) return false;
    D3D11Device = pD3D11Device;

    ID3D11Device* pDevice = static_cast<ID3D11Device*>(D3D11Device);
    pDevice->GetImmediateContext(reinterpret_cast<ID3D11DeviceContext**>(&D3D11Context));

    bDX11Initialized = true;
    return true;
#else
    (void)pD3D11Device;
    return false;
#endif
}

bool ColorSpaceConverter::IsDX11Available() const
{
    return bDX11Initialized && D3D11Device != nullptr;
}

// ============================================================================
// CPU 路径：BGRA32 → NV12
// ============================================================================
bool ColorSpaceConverter::ConvertBGRAToNV12_CPU(const uint8_t* BGRA, int32 Width, int32 Height, uint8_t* NV12Out)
{
    if (!BGRA || !NV12Out || Width <= 0 || Height <= 0) return false;

    int32 frameSize = Width * Height;
    uint8_t* Y  = NV12Out;
    uint8_t* UV = NV12Out + frameSize;

    for (int32 y = 0; y < Height; ++y)
    {
        for (int32 x = 0; x < Width; ++x)
        {
            const uint8_t* pixel = BGRA + (static_cast<size_t>(y) * Width + x) * 4;
            int32 B = pixel[0];
            int32 G = pixel[1];
            int32 R = pixel[2];

            int32 YVal  = (( 66 * R + 129 * G +  25 * B + 128) >> 8) + 16;
            int32 UVal  = ((-38 * R -  74 * G + 112 * B + 128) >> 8) + 128;
            int32 VVal  = ((112 * R -  94 * G -  18 * B + 128) >> 8) + 128;

            Y[static_cast<size_t>(y) * Width + x] = static_cast<uint8_t>(
                YVal < 0 ? 0 : (YVal > 255 ? 255 : YVal));

            if ((y & 1) == 0 && (x & 1) == 0)
            {
                size_t uvIdx = (static_cast<size_t>(y / 2) * Width + (x & ~1));
                UV[uvIdx]     = static_cast<uint8_t>(UVal < 0 ? 0 : (UVal > 255 ? 255 : UVal));
                UV[uvIdx + 1] = static_cast<uint8_t>(VVal < 0 ? 0 : (VVal > 255 ? 255 : VVal));
            }
        }
    }

    return true;
}

bool ColorSpaceConverter::ConvertBGRAToNV12_GPU(void* /*pBGRAInputTexture*/, void* /*pNV12OutputTexture*/,
                                                  int32 /*Width*/, int32 /*Height*/)
{
    // TODO: D3D11 Compute Shader BGRA → NV12 转换
    return false;
}
