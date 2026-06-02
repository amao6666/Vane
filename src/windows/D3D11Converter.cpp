#include "D3D11Converter.h"

#ifdef _WIN32

#include <cstring>
#include <cstdio>

D3D11Converter::D3D11Converter(ID3D11Device* pDevice, int32 InWidth, int32 InHeight)
    : Device(pDevice)
    , Width(InWidth)
    , Height(InHeight)
{
    if (!Device) return;

    Device->GetImmediateContext(&Context);
    if (!Context) return;

    // 获取视频设备接口
    HRESULT hr = Device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&VideoDevice);
    if (FAILED(hr) || !VideoDevice) { Context->Release(); Context = nullptr; return; }

    hr = Context->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&VideoContext);
    if (FAILED(hr) || !VideoContext)
    {
        VideoDevice->Release(); VideoDevice = nullptr;
        Context->Release(); Context = nullptr;
        return;
    }

    if (!CreateTextures(InWidth, InHeight)) return;
    if (!CreateVideoProcessor(InWidth, InHeight)) return;

    bInitialized = true;
}

D3D11Converter::~D3D11Converter()
{
    if (OutputView)    { OutputView->Release(); }
    if (InputView)     { InputView->Release(); }
    if (VP)            { VP->Release(); }
    if (VPEnum)        { VPEnum->Release(); }
    if (StagingBGRA)   { StagingBGRA->Release(); }
    if (BGRATexture)   { BGRATexture->Release(); }
    if (NV12Texture)   { NV12Texture->Release(); }
    if (VideoContext)  { VideoContext->Release(); }
    if (VideoDevice)   { VideoDevice->Release(); }
    if (Context)       { Context->Release(); }
}

bool D3D11Converter::CreateTextures(int32 W, int32 H)
{
    // 输出 NV12 纹理
    D3D11_TEXTURE2D_DESC descNV12 = {};
    descNV12.Width            = static_cast<UINT>(W);
    descNV12.Height           = static_cast<UINT>(H);
    descNV12.MipLevels        = 1;
    descNV12.ArraySize        = 1;
    descNV12.Format           = DXGI_FORMAT_NV12;
    descNV12.SampleDesc.Count = 1;
    descNV12.Usage            = D3D11_USAGE_DEFAULT;
    descNV12.BindFlags        = D3D11_BIND_RENDER_TARGET;

    HRESULT hr = Device->CreateTexture2D(&descNV12, nullptr, &NV12Texture);
    if (FAILED(hr)) return false;

    // 输入 BGRA 纹理
    D3D11_TEXTURE2D_DESC descBGRA = {};
    descBGRA.Width            = static_cast<UINT>(W);
    descBGRA.Height           = static_cast<UINT>(H);
    descBGRA.MipLevels        = 1;
    descBGRA.ArraySize        = 1;
    descBGRA.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    descBGRA.SampleDesc.Count = 1;
    descBGRA.Usage            = D3D11_USAGE_DEFAULT;
    descBGRA.BindFlags        = D3D11_BIND_RENDER_TARGET;

    hr = Device->CreateTexture2D(&descBGRA, nullptr, &BGRATexture);
    if (FAILED(hr)) return false;

    // Staging BGRA 纹理（CPU → GPU 上传中转）
    D3D11_TEXTURE2D_DESC descStaging = {};
    descStaging.Width            = static_cast<UINT>(W);
    descStaging.Height           = static_cast<UINT>(H);
    descStaging.MipLevels        = 1;
    descStaging.ArraySize        = 1;
    descStaging.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    descStaging.SampleDesc.Count = 1;
    descStaging.Usage            = D3D11_USAGE_STAGING;
    descStaging.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;

    hr = Device->CreateTexture2D(&descStaging, nullptr, &StagingBGRA);
    return SUCCEEDED(hr);
}

bool D3D11Converter::CreateVideoProcessor(int32 W, int32 H)
{
    // 创建 VideoProcessor 枚举器
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat       = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputFrameRate         = { 1, 1 };
    contentDesc.InputWidth             = static_cast<UINT>(W);
    contentDesc.InputHeight            = static_cast<UINT>(H);
    contentDesc.OutputFrameRate        = { 1, 1 };
    contentDesc.OutputWidth            = static_cast<UINT>(W);
    contentDesc.OutputHeight           = static_cast<UINT>(H);
    contentDesc.Usage                  = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = VideoDevice->CreateVideoProcessorEnumerator(&contentDesc, &VPEnum);
    if (FAILED(hr) || !VPEnum) { fprintf(stderr, "[D3D11Conv] CreateVideoProcessorEnumerator failed: 0x%lx\n", hr); return false; }

    hr = VideoDevice->CreateVideoProcessor(VPEnum, 0, &VP);
    if (FAILED(hr) || !VP) { fprintf(stderr, "[D3D11Conv] CreateVideoProcessor failed: 0x%lx\n", hr); return false; }

    // 创建输入/输出视图（绑定纹理，可跨帧复用）
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
    inputDesc.ViewDimension       = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.MipSlice  = 0;
    inputDesc.Texture2D.ArraySlice = 0;

    hr = VideoDevice->CreateVideoProcessorInputView(
        (ID3D11Resource*)BGRATexture, VPEnum, &inputDesc, &InputView);
    if (FAILED(hr) || !InputView) { fprintf(stderr, "[D3D11Conv] CreateInputView failed: 0x%lx\n", hr); return false; }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
    outputDesc.ViewDimension       = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputDesc.Texture2D.MipSlice  = 0;

    hr = VideoDevice->CreateVideoProcessorOutputView(
        (ID3D11Resource*)NV12Texture, VPEnum, &outputDesc, &OutputView);
    if (FAILED(hr) || !OutputView) { fprintf(stderr, "[D3D11Conv] CreateOutputView failed: 0x%lx\n", hr); return false; }

    return true;
}

ID3D11Texture2D* D3D11Converter::Convert(const uint8_t* BGRAData)
{
    if (!bInitialized || !BGRAData) return nullptr;

    // 1. 上传 BGRA 到 staging 纹理
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = Context->Map(StagingBGRA, 0, D3D11_MAP_WRITE, 0, &mapped);
    if (FAILED(hr))
    {
        fprintf(stderr, "[D3D11Conv] Map failed: 0x%lx\n", hr);
        return nullptr;
    }

    const size_t rowBytes = Width * 4;
    for (int32 y = 0; y < Height; y++)
    {
        memcpy(static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
               BGRAData + y * rowBytes, rowBytes);
    }
    Context->Unmap(StagingBGRA, 0);

    // 2. Staging → BGRA default 纹理
    Context->CopyResource(BGRATexture, StagingBGRA);

    // 3. VideoProcessorBlt: BGRA → NV12
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable        = TRUE;
    stream.pInputSurface = InputView;

    hr = VideoContext->VideoProcessorBlt(VP, OutputView, 0, 1, &stream);

    if (FAILED(hr))
    {
        fprintf(stderr, "[D3D11Conv] VideoProcessorBlt failed: 0x%lx\n", hr);
        return nullptr;
    }

    return NV12Texture;
}

#endif // _WIN32
