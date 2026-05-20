#include "FAmfEncoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

FAmfEncoder::FAmfEncoder() = default;

FAmfEncoder::~FAmfEncoder()
{
    std::string err;
    Finalize(err);
}

bool FAmfEncoder::IsAvailable()
{
    HMODULE h = LoadLibraryW(L"amfrt64.dll");
    if (h)
    {
        FreeLibrary(h);
        return true;
    }
    return false;
}

bool FAmfEncoder::Initialize(void* pD3D11Device, const FEncoderConfig& Config, std::string& OutError)
{
    if (!pD3D11Device)
    {
        OutError = "AMF: D3D11 Device 为空";
        return false;
    }

    AmfDLL = LoadLibraryW(L"amfrt64.dll");
    if (!AmfDLL)
    {
        OutError = "AMF: 无法加载 amfrt64.dll（未安装 AMD 驱动）";
        return false;
    }

    EncodeWidth  = Config.Width;
    EncodeHeight = Config.Height;

    // TODO: 实现 AMF 编码器初始化
    // 1. AMFInit → AMFFactory
    // 2. factory->CreateContext → InitDX11(device)
    // 3. factory->CreateComponent(AMFVideoEncoderHW_AVC, &encoder)
    // 4. encoder->Init(NV12, width, height)
    // 5. 配置码率/GOP

    OutError = "AMF: 尚未实现（TODO）";
    return false;
}

bool FAmfEncoder::EncodeFrame(const void* /*pInputTexture*/, int32 /*Width*/, int32 /*Height*/,
                               std::vector<uint8_t>& /*OutAnnexB*/, bool& /*OutIsKeyFrame*/)
{
    return false;
}

bool FAmfEncoder::Finalize(std::string& /*OutError*/)
{
    if (AmfDLL)
    {
        FreeLibrary((HMODULE)AmfDLL);
        AmfDLL = nullptr;
    }
    bInitialized = false;
    return true;
}
