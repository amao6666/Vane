// ============================================================================
// 全编码器验证测试
// 遍历当前设备上所有可用编码器，各编码 60 帧并验证输出文件可解码
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <iostream>
#include <cstring>
#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>

#include "Vane/VaneAPI.h"
#include "windows/FNvencEncoder.h"
#include "windows/FMFEncoderNew.h"

using int32 = int32_t;
using uint8 = uint8_t;

static int g_Failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cerr << "  FAIL: " << msg << std::endl; ++g_Failures; } \
    else { std::cout << "  OK: " << msg << std::endl; } \
} while(0)

// ============================================================================
// D3D11 设备
// ============================================================================
static ID3D11Device* CreateD3D11Device(bool bPreferNvidia)
{
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pCtx = nullptr;
    D3D_FEATURE_LEVEL fl;

    IDXGIFactory* pFactory = nullptr;
    CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory);

    IDXGIAdapter* pChosenAdapter = nullptr;
    for (UINT i = 0; pFactory->EnumAdapters(i, &pChosenAdapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        if (!pChosenAdapter) continue;
        DXGI_ADAPTER_DESC desc = {};
        pChosenAdapter->GetDesc(&desc);
        std::wcout << L"  Adapter[" << i << L"]: " << desc.Description << std::endl;
        if (bPreferNvidia && wcsstr(desc.Description, L"NVIDIA"))
            break;
        if (!bPreferNvidia && !wcsstr(desc.Description, L"NVIDIA"))
            break;
        pChosenAdapter->Release();
        pChosenAdapter = nullptr;
    }

    HRESULT hr = D3D11CreateDevice(pChosenAdapter,
                                    pChosenAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                    nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    nullptr, 0, D3D11_SDK_VERSION,
                                    &pDevice, &fl, &pCtx);

    if (pChosenAdapter) pChosenAdapter->Release();
    if (pCtx) pCtx->Release();
    if (pFactory) pFactory->Release();
    if (FAILED(hr)) return nullptr;
    return pDevice;
}

// ============================================================================
// 填充测试帧（蓝色 + 移动白条）
// ============================================================================
static void FillTestFrame(uint8_t* bgra, int32 W, int32 H, int32 frameIdx)
{
    int barX = (frameIdx * 4) % W;
    int barH = 16;
    int barY = H / 2 - barH / 2;

    for (int32 y = 0; y < H; ++y)
    {
        for (int32 x = 0; x < W; ++x)
        {
            size_t idx = (static_cast<size_t>(y) * W + x) * 4;
            bool inBar = (x >= barX && x < barX + 120 && y >= barY && y < barY + barH);
            bgra[idx + 0] = 200;
            bgra[idx + 1] = inBar ? 255 : 0;
            bgra[idx + 2] = inBar ? 255 : 0;
            bgra[idx + 3] = 255;
        }
    }
}

// ============================================================================
// ffprobe / ffmpeg 验证
// ============================================================================
static bool VerifyWithFfprobe(const char* path)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "D:/Apps/ffmpeg-2026-05-18-git-b4d11dffbf-essentials_build/bin/ffprobe.exe "
        "-v error -show_entries stream=codec_name,width,height,nb_frames "
        "-of default=noprint_wrappers=1 \"%s\" 2>&1", path);
    FILE* f = _popen(cmd, "r");
    if (!f) return false;
    char buf[256] = {};
    fread(buf, 1, sizeof(buf) - 1, f);
    _pclose(f);
    return strstr(buf, "h264") || strstr(buf, "wmv");
}

static bool DecodeWithFfmpeg(const char* path)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "D:/Apps/ffmpeg-2026-05-18-git-b4d11dffbf-essentials_build/bin/ffmpeg.exe "
        "-v error -i \"%s\" -f null NUL 2>&1", path);
    return system(cmd) == 0;
}

// ============================================================================
// 通过 Vane C API 编码并验证
// ============================================================================
static bool RunEncoderTest(const char* encoderName, const char* outputPath,
                            void* pD3D11Device, bool bPreferH264,
                            int32 W, int32 H, int32 FPS,
                            int32 totalFrames, int32 bitRate)
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "  " << encoderName << std::endl;
    std::cout << "========================================" << std::endl;
    DeleteFileA(outputPath);

    int32 frameSize = W * H * 4;
    std::vector<uint8_t> bgraBuf(frameSize);

    void* enc = VaneEncoder_Create();
    if (!enc) { CHECK(false, "VaneEncoder_Create 失败"); return false; }

#ifdef _WIN32
    if (pD3D11Device)
        VaneEncoder_SetD3D11Device(enc, pD3D11Device);
#endif

    FEncoderConfig cfg = {};
    cfg.Width  = W;
    cfg.Height = H;
    cfg.FrameRate = FPS;
    cfg.RecordFrameRate = FPS;
    cfg.BitRate = bitRate;
    cfg.bRealTime = true;
    cfg.FrameQueueSize = 8;
    cfg.FlushIntervalFrames = 30;
    cfg.KeyframeInterval = 30;
    cfg.bUserPreferH264 = bPreferH264;
    cfg.bAllowFormatFallback = true;

    if (VaneEncoder_Initialize(enc, &cfg) != 0)
    {
        std::cout << "  Initialize 失败: " << VaneEncoder_GetLastError(enc) << std::endl;
        VaneEncoder_Destroy(enc);
        CHECK(false, "Initialize 失败");
        return false;
    }
    std::cout << "  Initialize: OK" << std::endl;

    // 查询实际使用的编码器
#ifdef _WIN32
    int encType = VaneEncoder_GetEncoderType(enc);
    const char* typeNames[] = {"NVENC", "AMF", "MF"};
    std::cout << "  编码器类型: " << (encType >= 0 ? typeNames[encType] : "None") << std::endl;
#endif

    if (VaneEncoder_StartRecording(enc, outputPath) != 0)
    {
        std::cout << "  StartRecording 失败: " << VaneEncoder_GetLastError(enc) << std::endl;
        VaneEncoder_Destroy(enc);
        CHECK(false, "StartRecording 失败");
        return false;
    }
    std::cout << "  StartRecording: OK" << std::endl;

    auto t0 = std::chrono::steady_clock::now();
    int encodedOk = 0;
    for (int32 i = 0; i < totalFrames; ++i)
    {
        FillTestFrame(bgraBuf.data(), W, H, i);
        double ts = static_cast<double>(i) / FPS;
        if (VaneEncoder_EncodeFrame(enc, bgraBuf.data(), frameSize, ts) == 0) // 0=success
            ++encodedOk;
        auto trg = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>((i + 1) / static_cast<double>(FPS)));
        while (std::chrono::steady_clock::now() < trg)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto endTime = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(endTime - t0).count();

    VaneEncoder_StopRecording(enc);
    VaneEncoder_Destroy(enc);

    std::cout << "  推送: " << encodedOk << "/" << totalFrames
              << "  耗时: " << elapsed << "s" << std::endl;

    // 检查输出文件
    FILE* f = fopen(outputPath, "rb");
    if (!f) { CHECK(false, "输出文件不存在"); return false; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fclose(f);
    std::cout << "  文件大小: " << fsize << " 字节" << std::endl;
    CHECK(fsize > 1000, "文件大小正常");

    if (fsize <= 1000) return false;

    // ffprobe + ffmpeg 解码验证
    bool bProbeOk = VerifyWithFfprobe(outputPath);
    CHECK(bProbeOk, "ffprobe 识别");

    if (bProbeOk)
    {
        bool bDecodeOk = DecodeWithFfmpeg(outputPath);
        CHECK(bDecodeOk, "ffmpeg 解码");
        return bDecodeOk;
    }
    return false;
}

// ============================================================================
int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "  全编码器验证测试" << std::endl;
    std::cout << "========================================" << std::endl;

    // ---- 编码器可用性概览 ----
    std::cout << "\n=== 编码器可用性 ===" << std::endl;
    std::cout << "  NVENC DLL:  " << (FNvencEncoder::IsAvailable() ? "已找到" : "未找到") << std::endl;
    std::cout << "  MF H.264:   " << (FMFEncoderNew::IsH264Available() ? "可用" : "不可用") << std::endl;
    std::cout << "  MF WMV:     " << (FMFEncoderNew::IsWMVAvailable() ? "可用" : "不可用") << std::endl;

    // 创建 D3D11 设备（用于 NVENC 探测和编码）
    ID3D11Device* pNvDevice = CreateD3D11Device(true);
    if (pNvDevice)
    {
        bool bH264 = FNvencEncoder::IsH264Supported(pNvDevice);
        bool bHEVC = FNvencEncoder::IsHEVCSupported(pNvDevice);
        std::cout << "  NVENC H.264: " << (bH264 ? "可用" : "不可用") << std::endl;
        std::cout << "  NVENC HEVC:  " << (bHEVC ? "可用" : "不可用") << std::endl;
        std::cout << "  设备类型:     RTX 5060 Laptop GPU" << std::endl;
    }

    int tested = 0, passed = 0;

    // ---- 测试 1: NVENC (D3D11 NVIDIA + bPreferH264=true) ----
    if (pNvDevice && FNvencEncoder::IsH264Supported(pNvDevice))
    {
        ++tested;
        if (RunEncoderTest("NVENC H.264 (RTX 5060)", "test_enc_nvenc.mp4",
                            pNvDevice, true,
                            640, 480, 30, 60, 640 * 480 * 2))
            ++passed;
    }
    else
        std::cout << "\n--- NVENC: 跳过 ---" << std::endl;

    // ---- 测试 2: NVENC 1080p (高性能) ----
    if (pNvDevice && FNvencEncoder::IsH264Supported(pNvDevice))
    {
        ++tested;
        if (RunEncoderTest("NVENC H.264 1080p (RTX 5060)", "test_enc_nvenc_1080p.mp4",
                            pNvDevice, true,
                            1920, 1080, 60, 120, 1920 * 1080 * 2))
            ++passed;
    }

    // 为 MF 路径创建非 NVIDIA 设备（Intel UHD / MS Basic Render）
    ID3D11Device* pMFDevice = CreateD3D11Device(false);

    // ---- 测试 3: MF H.264 (非NVIDIA GPU → 强制走 MF 路径) ----
    if (FMFEncoderNew::IsH264Available())
    {
        ++tested;
        // 用非 NVIDIA D3D11 设备，NVENC probe 不过，自动回退到 MF
        if (RunEncoderTest("MF H.264 (Intel UHD)", "test_enc_mfh264.mp4",
                            pMFDevice, true,
                            640, 480, 30, 60, 640 * 480 * 2))
            ++passed;
    }

    // ---- 测试 4: MF WMV (bPreferH264=false → WMV 兜底) ----
    if (FMFEncoderNew::IsWMVAvailable())
    {
        ++tested;
        if (RunEncoderTest("MF WMV (ASF容器)", "test_enc_wmv.wmv",
                            pMFDevice, false,
                            640, 480, 30, 60, 640 * 480 * 2))
            ++passed;
    }

    // ---- 清理 ----
    if (pNvDevice) pNvDevice->Release();
    if (pMFDevice) pMFDevice->Release();

    // ---- 汇总 ----
    std::cout << "\n========================================" << std::endl;
    std::cout << "           汇总报告" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  测试: " << tested << "  通过: " << passed
              << "  失败: " << (tested - passed) << std::endl;
    std::cout << "========================================" << std::endl;

    return (passed == tested) ? 0 : 1;
}
