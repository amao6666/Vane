#pragma once

#ifdef _WIN32

#include <cstdint>
#include <d3d11.h>

using int32 = int32_t;

// DXVA GPU 加速 BGRA→NV12 色彩空间转换
// 使用 ID3D11VideoContext::VideoProcessorBlt，避免 CPU 做 8MB 转换
class D3D11Converter
{
public:
    D3D11Converter(ID3D11Device* pDevice, int32 Width, int32 Height);
    ~D3D11Converter();

    D3D11Converter(const D3D11Converter&) = delete;
    D3D11Converter& operator=(const D3D11Converter&) = delete;

    // 上传 BGRA CPU 数据并 GPU 转换为 NV12 纹理，返回输出纹理（调用者不拥有所有权）
    ID3D11Texture2D* Convert(const uint8_t* BGRAData);

    bool  IsValid() const { return bInitialized; }
    int32 GetWidth()  const { return Width; }
    int32 GetHeight() const { return Height; }

private:
    ID3D11Device*        Device = nullptr;
    ID3D11DeviceContext* Context = nullptr;
    ID3D11VideoDevice*   VideoDevice = nullptr;
    ID3D11VideoContext*  VideoContext = nullptr;

    ID3D11VideoProcessorEnumerator* VPEnum = nullptr;
    ID3D11VideoProcessor*           VP     = nullptr;

    ID3D11Texture2D*     BGRATexture = nullptr;  // 输入：BGRA (D3D11_USAGE_DEFAULT)
    ID3D11Texture2D*     NV12Texture = nullptr;  // 输出：NV12 (D3D11_USAGE_DEFAULT)
    ID3D11Texture2D*     StagingBGRA = nullptr;  // 上传中转：BGRA (D3D11_USAGE_STAGING)

    ID3D11VideoProcessorInputView*  InputView  = nullptr;
    ID3D11VideoProcessorOutputView* OutputView = nullptr;

    int32 Width  = 0;
    int32 Height = 0;
    bool  bInitialized = false;

    bool CreateTextures(int32 InWidth, int32 InHeight);
    bool CreateVideoProcessor(int32 InWidth, int32 InHeight);
};

#endif // _WIN32
