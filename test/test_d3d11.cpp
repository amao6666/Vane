// ============================================================================
// D3D11 最小测试程序
// 创建 D3D11Device → 填充 BGRA 测试图案 → 调用 Vane API 编码
// 自动适配输出格式（H.264/MP4 或 WMV/ASF）
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

using int32 = int32_t;
using uint8 = uint8_t;

// ---- 回调 ----
static void OnState(ERecordingState s, void*)
{
    const char* names[] = {"Idle","Starting","Recording","Paused","Stopping","Error"};
    std::cout << "  [State] " << names[(int)s] << std::endl;
}

static void OnError(EErrorLevel lv, const char* msg, void*)
{
    std::cout << "  [Error Lv=" << (int)lv << "] " << (msg ? msg : "") << std::endl;
}

static void OnProgress(int64_t frames, int64_t bytes, double dur, void*)
{
    std::cout << "  [Progress] frames=" << frames
              << " bytes=" << bytes
              << " duration=" << dur << "s" << std::endl;
}

static void OnFrameDrop(int32_t dropped, void*)
{
    std::cout << "  [Drop] totalDropped=" << dropped << std::endl;
}

// ---- 失败计数 ----
static int g_Failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cerr << "  FAIL: " << msg << std::endl; ++g_Failures; } \
} while(0)

// ============================================================================
// 创建最小 D3D11 设备
// ============================================================================
static ID3D11Device* CreateMinimalD3D11Device()
{
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pCtx = nullptr;
    D3D_FEATURE_LEVEL fl;

    // 枚举适配器，优先选择 NVIDIA（NVENC 需要 NVIDIA GPU）
    IDXGIFactory* pFactory = nullptr;
    CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory);

    IDXGIAdapter* pNvidiaAdapter = nullptr;
    for (UINT i = 0; pFactory->EnumAdapters(i, &pNvidiaAdapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        if (!pNvidiaAdapter) continue;
        DXGI_ADAPTER_DESC desc = {};
        pNvidiaAdapter->GetDesc(&desc);
        std::wcout << L"  Adapter[" << i << L"]: " << desc.Description
                   << L" VRAM=" << (desc.DedicatedVideoMemory >> 20) << L"MB" << std::endl;
        if (wcsstr(desc.Description, L"NVIDIA"))
            break; // 找到 NVIDIA 适配器
        pNvidiaAdapter->Release();
        pNvidiaAdapter = nullptr;
    }

    HRESULT hr = D3D11CreateDevice(pNvidiaAdapter,
                                    pNvidiaAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                    nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    nullptr, 0, D3D11_SDK_VERSION,
                                    &pDevice, &fl, &pCtx);

    if (pNvidiaAdapter) pNvidiaAdapter->Release();
    if (pCtx) pCtx->Release();
    if (pFactory) pFactory->Release();
    if (FAILED(hr)) return nullptr;
    return pDevice;
}

// ============================================================================
// 测试 1: 能力检测（需要 D3D11 设备才能准确探测 NVENC）
// ============================================================================
static void TestCapability()
{
    std::cout << "[TEST] 编码器能力检测" << std::endl;

    ID3D11Device* pDevice = CreateMinimalD3D11Device();

    void* enc = VaneEncoder_Create();
    CHECK(enc != nullptr, "VaneEncoder_Create 返回 null");

#ifdef _WIN32
    if (pDevice)
    {
        VaneEncoder_SetD3D11Device(enc, pDevice);
    }
#endif

    FEncoderCapability cap;
    int ok = VaneEncoder_CheckCapability(enc, &cap);
    CHECK(ok != 0, "CheckCapability 失败");

    std::cout << "  H.264: " << (cap.bH264Available ? "可用" : "不可用") << std::endl;
    std::cout << "  HEVC:  " << (cap.bHEVCAvailable ? "可用" : "不可用") << std::endl;
    std::cout << "  WMV:   " << (cap.bWMVAvailable ? "可用" : "不可用") << std::endl;
    std::cout << "  推荐:  " << cap.RecommendedFormat << std::endl;
    std::cout << "  编码器: " << cap.H264EncoderName << std::endl;
    std::cout << cap.DiagnosticInfo;

#ifdef _WIN32
    int encType = VaneEncoder_GetEncoderType(enc);
    const char* typeNames[] = {"NVENC", "AMF", "MF"};
    std::cout << "  运行时编码器类型: "
              << (encType >= 0 ? typeNames[encType] : "None") << std::endl;
#endif

    VaneEncoder_Destroy(enc);
    if (pDevice) pDevice->Release();
    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// 填充 BGRA 测试图案（彩色条纹 + 移动方块）
// ============================================================================
static void FillBGRATestFrame(uint8_t* bgra, int32 W, int32 H, int32 frameIdx)
{
    int blockX = frameIdx % W;
    int blockY = H / 2;
    int blockW = 64, blockH = 64;

    for (int32 y = 0; y < H; ++y)
    {
        for (int32 x = 0; x < W; ++x)
        {
            size_t idx = (static_cast<size_t>(y) * W + x) * 4;
            int r, g, b;

            bool inBlock = (x >= blockX && x < blockX + blockW &&
                           y >= blockY && y < blockY + blockH);

            if (inBlock)
            {
                r = 255; g = 0; b = 0;
            }
            else
            {
                int stripe = (x / 32) % 3;
                switch (stripe)
                {
                case 0: r = 255; g = 255; b = 0; break; // 黄
                case 1: r = 0;   g = 255; b = 255; break; // 青
                default: r = 255; g = 0;   b = 255; break; // 品红
                }
            }

            bgra[idx + 0] = static_cast<uint8_t>(b);
            bgra[idx + 1] = static_cast<uint8_t>(g);
            bgra[idx + 2] = static_cast<uint8_t>(r);
            bgra[idx + 3] = 255;
        }
    }
}

// ============================================================================
// 测试 2: 完整编码流程（D3D11 设备 + BGRA → H.264/WMV）
// ============================================================================
static void TestEncodeWithD3D11()
{
    std::cout << "[TEST] D3D11 编码流程" << std::endl;

    // 1. 创建 D3D11 设备
    ID3D11Device* pDevice = CreateMinimalD3D11Device();
    if (!pDevice)
    {
        std::cout << "  跳过：无法创建 D3D11 设备（可能无 GPU 或远程桌面）" << std::endl;
        return;
    }
    std::cout << "  D3D11 设备已创建" << std::endl;

    // 2. 创建编码器
    void* enc = VaneEncoder_Create();
    CHECK(enc != nullptr, "Create 失败");

    // 注册回调
    VaneEncoder_SetStateCallback(enc, OnState, nullptr);
    VaneEncoder_SetErrorCallback(enc, OnError, nullptr);
    VaneEncoder_SetProgressCallback(enc, OnProgress, nullptr);
    VaneEncoder_SetFrameDropCallback(enc, OnFrameDrop, nullptr);

#ifdef _WIN32
    // 设置 D3D11 设备
    int setDev = VaneEncoder_SetD3D11Device(enc, pDevice);
    std::cout << "  SetD3D11Device: " << (setDev ? "OK" : "FAIL") << std::endl;
#endif

    // 3. 初始化
    const int32 W = 640, H = 480, FPS = 30;
    FEncoderConfig cfg = {};
    cfg.Width  = W;
    cfg.Height = H;
    cfg.FrameRate = FPS;
    cfg.RecordFrameRate = FPS;
    cfg.BitRate = W * H * 2;
    cfg.bRealTime = true;
    cfg.FrameQueueSize = 8;
    cfg.FlushIntervalFrames = 30;
    cfg.KeyframeInterval = 30;

    int init = VaneEncoder_Initialize(enc, &cfg);
    std::cout << "  Initialize: " << (init == 0 ? "OK" : "FAIL") << std::endl;
    if (init != 0)
    {
        std::cout << "  LastError: " << VaneEncoder_GetLastError(enc) << std::endl;
        VaneEncoder_Destroy(enc);
        pDevice->Release();
        return;
    }

    // 4. 查询编码器能力，判断输出格式
    FEncoderCapability cap;
    VaneEncoder_CheckCapability(enc, &cap);
    bool bIsWMV = (strcmp(cap.RecommendedFormat, "wmv") == 0);
    const char* outputPath = bIsWMV ? "test_d3d11_output.wmv" : "test_d3d11_output.mp4";
    std::cout << "  输出格式: " << (bIsWMV ? "WMV (ASF)" : "H.264 (MP4)") << std::endl;

    // 清理旧文件（两种格式都尝试删除，避免残留干扰）
    DeleteFileA("test_d3d11_output.mp4");
    DeleteFileA("test_d3d11_output.wmv");

    // 5. 开始录制
    int start = VaneEncoder_StartRecording(enc, outputPath);
    std::cout << "  StartRecording(" << outputPath << "): " << (start == 0 ? "OK" : "FAIL") << std::endl;
    CHECK(start == 0, "StartRecording 失败");
    if (start != 0)
    {
        std::cout << "  LastError: " << VaneEncoder_GetLastError(enc) << std::endl;
        VaneEncoder_Destroy(enc);
        pDevice->Release();
        return;
    }

    // 6. 编码若干帧（BGRA 格式 → Vane 内部转换为编码器所需格式）
    int32 totalFrames = 60;
    int32 frameSize = W * H * 4; // BGRA
    std::vector<uint8_t> bgraBuf(frameSize);

    for (int32 i = 0; i < totalFrames; ++i)
    {
        FillBGRATestFrame(bgraBuf.data(), W, H, i);

        double ts = static_cast<double>(i) / FPS;
        int encOk = VaneEncoder_EncodeFrame(enc, bgraBuf.data(), frameSize, ts);
        if (encOk != 0)
        {
            std::cerr << "  EncodeFrame #" << i << " 失败: "
                      << VaneEncoder_GetLastError(enc) << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // 7. 停止录制
    VaneEncoder_StopRecording(enc);
    std::cout << "  StopRecording 完成" << std::endl;

    // 8. 检查输出文件
    FILE* f = fopen(outputPath, "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fclose(f);
        std::cout << "  输出文件大小: " << fsize << " 字节" << std::endl;
        CHECK(fsize > 1000, "输出文件太小（可能编码失败）");

        // 验证容器签名
        uint8_t header[12];
        f = fopen(outputPath, "rb");
        size_t readBytes = fread(header, 1, sizeof(header), f);
        fclose(f);

        if (bIsWMV)
        {
            // ASF header: 30 26 B2 75 8E 66 CF 11 A6 D9 00 AA 00 62 CE 6C
            static const uint8_t asfGuid[16] = {
                0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
            };
            CHECK(readBytes >= 12, "无法读取 ASF 文件头");
            CHECK(memcmp(header, asfGuid, 12) == 0, "文件头应为 ASF GUID");
            std::cout << "  WMV/ASF 验证通过" << std::endl;
        }
        else
        {
            // ftyp box: [00 00 00 xx 66 74 79 70]
            CHECK(readBytes >= 8, "无法读取 MP4 文件头");
            CHECK(memcmp(header + 4, "ftyp", 4) == 0, "文件头应为 ftyp");
            std::cout << "  MP4 验证通过（可尝试用 ffplay " << outputPath << " 播放）" << std::endl;
        }
    }
    else
    {
        std::cerr << "  输出文件不存在: " << outputPath << std::endl;
        ++g_Failures;
    }

    VaneEncoder_Destroy(enc);
    pDevice->Release();
    std::cout << "  PASS" << std::endl;
}

// ============================================================================
int main()
{
    std::cout << "=== D3D11 Vane API 测试 ===" << std::endl;
    std::cout << std::endl;

    TestCapability();
    std::cout << std::endl;
    TestEncodeWithD3D11();

    std::cout << std::endl;
    if (g_Failures == 0)
        std::cout << "=== 全部通过 ===" << std::endl;
    else
        std::cout << "=== " << g_Failures << " 个测试失败 ===" << std::endl;

    return g_Failures > 0 ? 1 : 0;
}
