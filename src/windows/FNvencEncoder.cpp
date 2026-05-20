#include "FNvencEncoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <cstring>

#include "nvEncodeAPI.h"

// 官方 SDK 不提供此 typedef，本地定义
typedef NVENCSTATUS (NVENCAPI* PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);

FNvencEncoder::FNvencEncoder()
{
    NvEnc = new NV_ENCODE_API_FUNCTION_LIST();
    memset(NvEnc, 0, sizeof(NV_ENCODE_API_FUNCTION_LIST));
}

FNvencEncoder::~FNvencEncoder()
{
    std::string err;
    Finalize(err);
    delete NvEnc;
    NvEnc = nullptr;
}

bool FNvencEncoder::IsAvailable()
{
    HMODULE h = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (h) { FreeLibrary(h); return true; }
    return false;
}

// ============================================================================
// 真实验证：打开临时 NVENC 会话，查询 GPU 是否支持指定 codec
// 结果缓存为 static bool，只探测一次
// ============================================================================

static bool ProbeNvencCodec(void* pD3D11Device, const GUID& CodecGuid)
{
    if (!pD3D11Device) return false;

    HMODULE hDll = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!hDll) return false;

    auto pfnCreate = (PFN_NvEncodeAPICreateInstance)GetProcAddress(hDll, "NvEncodeAPICreateInstance");
    if (!pfnCreate) { FreeLibrary(hDll); return false; }

    NV_ENCODE_API_FUNCTION_LIST nvEnc = {};
    nvEnc.version = NV_ENCODE_API_FUNCTION_LIST_VER;

    NVENCSTATUS status = pfnCreate(&nvEnc);
    if (status != NV_ENC_SUCCESS) { FreeLibrary(hDll); return false; }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {};
    sessionParams.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.device     = pD3D11Device;
    sessionParams.apiVersion = NVENCAPI_VERSION;

    void* encoder = nullptr;
    status = nvEnc.nvEncOpenEncodeSessionEx(&sessionParams, &encoder);
    if (status != NV_ENC_SUCCESS)
    {
        fprintf(stderr, "[NVENC probe] nvEncOpenEncodeSessionEx failed: %d\n", (int)status);
        FreeLibrary(hDll);
        return false;
    }

    uint32_t count = 0;
    status = nvEnc.nvEncGetEncodeGUIDCount(encoder, &count);
    if (status != NV_ENC_SUCCESS || count == 0)
    {
        fprintf(stderr, "[NVENC probe] nvEncGetEncodeGUIDCount: status=%d count=%u\n", (int)status, count);
        nvEnc.nvEncDestroyEncoder(encoder);
        FreeLibrary(hDll);
        return false;
    }

    std::vector<GUID> guids(count);
    status = nvEnc.nvEncGetEncodeGUIDs(encoder, guids.data(), count, &count);
    if (status != NV_ENC_SUCCESS)
    {
        fprintf(stderr, "[NVENC probe] nvEncGetEncodeGUIDs failed: %d\n", (int)status);
        nvEnc.nvEncDestroyEncoder(encoder);
        FreeLibrary(hDll);
        return false;
    }

    fprintf(stderr, "[NVENC probe] %u codec GUIDs returned:\n", count);
    for (uint32_t i = 0; i < count; i++)
    {
        fprintf(stderr, "  [%u] {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
                i, guids[i].Data1, guids[i].Data2, guids[i].Data3,
                guids[i].Data4[0], guids[i].Data4[1], guids[i].Data4[2], guids[i].Data4[3],
                guids[i].Data4[4], guids[i].Data4[5], guids[i].Data4[6], guids[i].Data4[7]);
    }

    bool bFound = false;
    for (uint32_t i = 0; i < count; i++)
    {
        if (IsEqualGUID(guids[i], CodecGuid))
        {
            bFound = true;
            break;
        }
    }

    fprintf(stderr, "[NVENC probe] Looking for {%08lX-...}, found=%d\n",
            CodecGuid.Data1, (int)bFound);

    nvEnc.nvEncDestroyEncoder(encoder);
    FreeLibrary(hDll);
    return bFound;
}

static void*    s_NvencH264Dev   = nullptr;
static bool     s_NvencH264Cached = false;
static void*    s_NvencHEVCDev   = nullptr;
static bool     s_NvencHEVCCached = false;

bool FNvencEncoder::IsH264Supported(void* pD3D11Device)
{
    if (!pD3D11Device) return false;
    if (s_NvencH264Dev != pD3D11Device)
    {
        s_NvencH264Dev    = pD3D11Device;
        s_NvencH264Cached = ProbeNvencCodec(pD3D11Device, NV_ENC_CODEC_H264_GUID);
    }
    return s_NvencH264Cached;
}

bool FNvencEncoder::IsHEVCSupported(void* pD3D11Device)
{
    if (!pD3D11Device) return false;
    if (s_NvencHEVCDev != pD3D11Device)
    {
        s_NvencHEVCDev    = pD3D11Device;
        s_NvencHEVCCached = ProbeNvencCodec(pD3D11Device, NV_ENC_CODEC_HEVC_GUID);
    }
    return s_NvencHEVCCached;
}

bool FNvencEncoder::Initialize(void* pD3D11Device, const FEncoderConfig& Config, std::string& OutError)
{
    if (bInitialized) return true;
    if (!pD3D11Device) { OutError = "NVENC: D3D11 Device 为空"; return false; }

    EncodeWidth  = Config.Width;
    EncodeHeight = Config.Height;
    FrameRate    = Config.FrameRate;
    BitRate      = Config.BitRate;

    NvEncDLL = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!NvEncDLL)
    {
        OutError = "NVENC: 无法加载 nvEncodeAPI64.dll";
        return false;
    }

    auto pfnCreate = (PFN_NvEncodeAPICreateInstance)GetProcAddress((HMODULE)NvEncDLL, "NvEncodeAPICreateInstance");
    if (!pfnCreate)
    {
        OutError = "NVENC: 找不到 NvEncodeAPICreateInstance 入口点";
        FreeLibrary((HMODULE)NvEncDLL); NvEncDLL = nullptr;
        return false;
    }

    memset(NvEnc, 0, sizeof(NV_ENCODE_API_FUNCTION_LIST));
    NvEnc->version = NV_ENCODE_API_FUNCTION_LIST_VER;

    NVENCSTATUS status = pfnCreate(NvEnc);
    if (status != NV_ENC_SUCCESS)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "NVENC: NvEncodeAPICreateInstance 失败 (err=%d)", (int)status);
        OutError = buf;
        FreeLibrary((HMODULE)NvEncDLL); NvEncDLL = nullptr;
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {};
    sessionParams.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.device     = pD3D11Device;
    sessionParams.apiVersion = NVENCAPI_VERSION;

    status = NvEnc->nvEncOpenEncodeSessionEx(&sessionParams, &EncoderHandle);
    if (status != NV_ENC_SUCCESS)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "NVENC: 打开编码会话失败 (err=%d)", (int)status);
        OutError = buf;
        FreeLibrary((HMODULE)NvEncDLL); NvEncDLL = nullptr;
        return false;
    }

    GUID codecGuid  = NV_ENC_CODEC_H264_GUID;
    GUID presetGuid = NV_ENC_PRESET_P4_GUID;

    // 按照 NVIDIA SDK 参考模式初始化预设配置
    // 关键：presetCfg.presetCfg.version 必须设为 NV_ENC_CONFIG_VER
    NV_ENC_PRESET_CONFIG presetCfg = { NV_ENC_PRESET_CONFIG_VER, 0, { NV_ENC_CONFIG_VER } };
    NVENCSTATUS presetStatus = NvEnc->nvEncGetEncodePresetConfigEx(
        EncoderHandle, codecGuid, presetGuid, NV_ENC_TUNING_INFO_LOW_LATENCY, &presetCfg);

    NV_ENC_CONFIG encConfig = {};
    encConfig.version = NV_ENC_CONFIG_VER;

    if (presetStatus == NV_ENC_SUCCESS)
    {
        memcpy(&encConfig, &presetCfg.presetCfg, sizeof(NV_ENC_CONFIG));
    }

    // 覆盖用户指定的关键参数
    encConfig.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    encConfig.gopLength   = static_cast<uint32_t>(Config.KeyframeInterval);
    encConfig.frameIntervalP = 1;
    encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    encConfig.rcParams.averageBitRate  = static_cast<uint32_t>(BitRate);
    encConfig.rcParams.maxBitRate      = static_cast<uint32_t>(BitRate);
    // CBR 必须设置 VBV 参数；预设不填具体码率，不设则编码器不产出期望码率
    encConfig.rcParams.vbvBufferSize   = static_cast<uint32_t>(BitRate);
    encConfig.rcParams.vbvInitialDelay = static_cast<uint32_t>(BitRate);
    // 强制 CBR 严格按 GOP 目标，低延迟模式下关闭重排序延迟
    encConfig.rcParams.strictGOPTarget = 1;
    encConfig.rcParams.zeroReorderDelay = 1;
    // QP 范围：允许编码器压低 QP 以消耗码率预算
    encConfig.rcParams.enableMinQP = 1;
    encConfig.rcParams.minQP = { 1, 1, 1 };
    encConfig.rcParams.enableMaxQP = 1;
    encConfig.rcParams.maxQP = { 35, 35, 35 };
    encConfig.encodeCodecConfig.h264Config.repeatSPSPPS  = 1;
    encConfig.encodeCodecConfig.h264Config.disableSPSPPS = 0;
    encConfig.encodeCodecConfig.h264Config.idrPeriod     = encConfig.gopLength;
    // CBR 填充：使用合法 H.264 filler NAL (type 12) 而非 raw 0xFF 字节
    encConfig.encodeCodecConfig.h264Config.enableFillerDataInsertion = 1;

    NV_ENC_INITIALIZE_PARAMS initParams = {};
    initParams.version              = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID           = codecGuid;
    initParams.presetGUID           = presetGuid;
    initParams.encodeWidth          = static_cast<uint32_t>(EncodeWidth);
    initParams.encodeHeight         = static_cast<uint32_t>(EncodeHeight);
    initParams.darWidth             = static_cast<uint32_t>(EncodeWidth);
    initParams.darHeight            = static_cast<uint32_t>(EncodeHeight);
    initParams.frameRateNum         = static_cast<uint32_t>(FrameRate);
    initParams.frameRateDen         = 1;
    initParams.enablePTD            = 1;
    initParams.maxEncodeWidth       = static_cast<uint32_t>(EncodeWidth);
    initParams.maxEncodeHeight      = static_cast<uint32_t>(EncodeHeight);
    initParams.enableOutputInVidmem = 0;
    initParams.tuningInfo           = NV_ENC_TUNING_INFO_LOW_LATENCY;
    initParams.encodeConfig         = &encConfig;

    status = NvEnc->nvEncInitializeEncoder(EncoderHandle, &initParams);
    if (status != NV_ENC_SUCCESS)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "NVENC: 初始化编码器失败 (err=%d)", (int)status);
        OutError = buf;
        NvEnc->nvEncDestroyEncoder(EncoderHandle); EncoderHandle = nullptr;
        FreeLibrary((HMODULE)NvEncDLL); NvEncDLL = nullptr;
        return false;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER bsb = {};
    bsb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    bsb.size    = static_cast<uint32_t>(EncodeWidth * EncodeHeight * 3 / 2);

    status = NvEnc->nvEncCreateBitstreamBuffer(EncoderHandle, &bsb);
    if (status != NV_ENC_SUCCESS)
    {
        OutError = "NVENC: 创建比特流缓冲区失败";
        NvEnc->nvEncDestroyEncoder(EncoderHandle); EncoderHandle = nullptr;
        FreeLibrary((HMODULE)NvEncDLL); NvEncDLL = nullptr;
        return false;
    }

    OutputBitstreamBuffer = bsb.bitstreamBuffer;
    FrameIndex = 0;
    bInitialized = true;

    return true;
}

bool FNvencEncoder::CreateAndRegisterResource(void* pD3D11Texture, int32 Width, int32 Height)
{
    if (!pD3D11Texture || !EncoderHandle) return false;

    NV_ENC_REGISTER_RESOURCE regRes = {};
    regRes.version           = NV_ENC_REGISTER_RESOURCE_VER;
    regRes.resourceType      = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    regRes.resourceToRegister = pD3D11Texture;
    regRes.width              = static_cast<uint32_t>(Width);
    regRes.height             = static_cast<uint32_t>(Height);
    regRes.bufferFormat       = NV_ENC_BUFFER_FORMAT_NV12;

    NVENCSTATUS status = NvEnc->nvEncRegisterResource(EncoderHandle, &regRes);
    if (status != NV_ENC_SUCCESS) return false;

    RegisteredResourceMap[pD3D11Texture] = regRes.registeredResource;
    return true;
}

bool FNvencEncoder::EncodeFrame(const void* pInputTexture, int32 Width, int32 Height,
                                 std::vector<uint8_t>& OutAnnexB, bool& OutIsKeyFrame)
{
    if (!bInitialized || !EncoderHandle || !pInputTexture) return false;

    void* pTexture = const_cast<void*>(pInputTexture);
    auto it = RegisteredResourceMap.find(pTexture);
    if (it == RegisteredResourceMap.end())
    {
        if (!CreateAndRegisterResource(pTexture, Width, Height))
            return false;
        it = RegisteredResourceMap.find(pTexture);
        if (it == RegisteredResourceMap.end()) return false;
    }

    NV_ENC_MAP_INPUT_RESOURCE mapRes = {};
    mapRes.version            = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapRes.registeredResource = it->second;

    NVENCSTATUS status = NvEnc->nvEncMapInputResource(EncoderHandle, &mapRes);
    if (status != NV_ENC_SUCCESS) return false;

    NV_ENC_PIC_PARAMS picParams = {};
    picParams.version        = NV_ENC_PIC_PARAMS_VER;
    picParams.inputBuffer    = mapRes.mappedResource;
    picParams.bufferFmt      = mapRes.mappedBufferFmt;
    picParams.inputWidth     = static_cast<uint32_t>(Width);
    picParams.inputHeight    = static_cast<uint32_t>(Height);
    picParams.pictureStruct  = NV_ENC_PIC_STRUCT_FRAME;
    picParams.frameIdx       = static_cast<uint32_t>(FrameIndex);
    picParams.inputTimeStamp = static_cast<uint64_t>(FrameIndex);
    picParams.inputDuration  = 1;
    picParams.outputBitstream = OutputBitstreamBuffer;

    status = NvEnc->nvEncEncodePicture(EncoderHandle, &picParams);
    if (status != NV_ENC_SUCCESS)
    {
        NvEnc->nvEncUnmapInputResource(EncoderHandle, mapRes.mappedResource);
        return false;
    }

    GetEncodedBitstream(OutAnnexB, OutIsKeyFrame);
    NvEnc->nvEncUnmapInputResource(EncoderHandle, mapRes.mappedResource);
    ++FrameIndex;

    return true;
}

bool FNvencEncoder::GetEncodedBitstream(std::vector<uint8_t>& OutAnnexB, bool& OutIsKeyFrame)
{
    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version        = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = OutputBitstreamBuffer;
    lock.doNotWait       = 0;

    NVENCSTATUS status = NvEnc->nvEncLockBitstream(EncoderHandle, &lock);
    if (status != NV_ENC_SUCCESS) return false;

    OutAnnexB.assign(
        static_cast<const uint8_t*>(lock.bitstreamBufferPtr),
        static_cast<const uint8_t*>(lock.bitstreamBufferPtr) + lock.bitstreamSizeInBytes
    );

    // NVENC CBR 会在 bitstream 末尾填充 0xFF 字节以达到目标码率；
    // 这些字节不是合法的 NAL 数据，必须截去，否则 MP4Writer 无法正确解析
    // 填充模式: ...<有效NAL数据> FF FF ... FF 80（末尾标记字节0x80）
    size_t trimEnd = OutAnnexB.size();
    if (trimEnd > 0 && OutAnnexB[trimEnd - 1] == 0x80)
        --trimEnd;  // 末尾 0x80 标记字节
    while (trimEnd > 0 && OutAnnexB[trimEnd - 1] == 0xFF)
        --trimEnd;  // CBR 填充字节
    if (trimEnd < OutAnnexB.size())
        OutAnnexB.resize(trimEnd);

    OutIsKeyFrame = (lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I);

    NvEnc->nvEncUnlockBitstream(EncoderHandle, lock.outputBitstream);
    return true;
}

bool FNvencEncoder::Finalize(std::string& /*OutError*/)
{
    if (!bInitialized) return true;

    if (EncoderHandle)
    {
        NV_ENC_PIC_PARAMS picParams = {};
        picParams.version        = NV_ENC_PIC_PARAMS_VER;
        picParams.encodePicFlags = 0x00000002;
        NvEnc->nvEncEncodePicture(EncoderHandle, &picParams);
    }

    for (auto& pair : RegisteredResourceMap)
        NvEnc->nvEncUnregisterResource(EncoderHandle, pair.second);
    RegisteredResourceMap.clear();

    if (OutputBitstreamBuffer && EncoderHandle)
    {
        NvEnc->nvEncDestroyBitstreamBuffer(EncoderHandle, OutputBitstreamBuffer);
        OutputBitstreamBuffer = nullptr;
    }

    if (EncoderHandle)
    {
        NvEnc->nvEncDestroyEncoder(EncoderHandle);
        EncoderHandle = nullptr;
    }

    if (NvEncDLL)
    {
        FreeLibrary((HMODULE)NvEncDLL);
        NvEncDLL = nullptr;
    }

    memset(NvEnc, 0, sizeof(NV_ENCODE_API_FUNCTION_LIST));
    bInitialized = false;

    return true;
}
