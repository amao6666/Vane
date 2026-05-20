#if defined(PLATFORM_MAC)
    #include "FVTEncoder.h"
    using EncoderType = FVTEncoder;
#elif defined(PLATFORM_WINDOWS)
    #include "windows/FWindowsEncoder.h"
    using EncoderType = FWindowsEncoder;
    #include <d3d11.h>
    #include <dxgi.h>
    #pragma comment(lib, "d3d11.lib")
#elif defined(PLATFORM_LINUX)
    #include "FVAEncoder.h"
    using EncoderType = FVAEncoder;
#endif
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>

#ifdef _WIN32
static void* CreateTestD3D11Device()
{
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pCtx = nullptr;
    D3D_FEATURE_LEVEL fl;

    IDXGIFactory* pFactory = nullptr;
    CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory);

    IDXGIAdapter* pNvidiaAdapter = nullptr;
    for (UINT i = 0; pFactory->EnumAdapters(i, &pNvidiaAdapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        if (!pNvidiaAdapter) continue;
        DXGI_ADAPTER_DESC desc = {};
        pNvidiaAdapter->GetDesc(&desc);
        if (wcsstr(desc.Description, L"NVIDIA")) break;
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

static void SetupD3D11Device(IVideoEncoder& enc)
{
    static void* s_Device = nullptr;
    if (!s_Device) s_Device = CreateTestD3D11Device();
    if (s_Device)
    {
        enc.SetD3D11Device(s_Device);
        std::cout << "  [D3D11] 设备已设置" << std::endl;
    }
    else
    {
        std::cout << "  [D3D11] 无法创建设备（可能是远程桌面或无 GPU）" << std::endl;
    }
}
#endif

static const int32 Width  = 1920;
static const int32 Height = 1080;
static const int32 FrameSize = Width * Height * 4;

struct FTestStats
{
    std::atomic<int> StateChanges{0};
    std::atomic<int> Errors{0};
    std::atomic<int> ProgressCalls{0};
    std::atomic<int> DropCalls{0};
    std::atomic<int> EncodedFrames{0};
    std::atomic<int> DroppedFrames{0};
    ERecordingState   LastState{ERecordingState_Idle};
    std::atomic<bool> IdleReceived{false};
};

static void OnState(ERecordingState s, void* p)
{
    auto* st = static_cast<FTestStats*>(p);
    st->StateChanges++;
    st->LastState = s;
    if (s == ERecordingState_Idle) st->IdleReceived = true;
}
static void OnError(EErrorLevel Lv, const char* Msg, void* p) {
    std::cerr << "  [ERR Lv="<<Lv<<"] " << (Msg?Msg:"") << std::endl;
    if (Lv >= EErrorLevel_Error) static_cast<FTestStats*>(p)->Errors++;
}
static void OnProgress(int64_t f, int64_t, double, void* p)
{
    auto* st = static_cast<FTestStats*>(p);
    st->ProgressCalls++;
    st->EncodedFrames = (int)f;
}
static void OnDrop(int32_t d, void* p)
{
    auto* st = static_cast<FTestStats*>(p);
    st->DropCalls++;
    st->DroppedFrames = d;
}

static uint8_t* CreateBlueFrame()
{
    auto* img = new uint8_t[FrameSize];
    for (int32 y = 0; y < Height; ++y)
        for (int32 x = 0; x < Width; ++x) {
            size_t o = ((size_t)y*Width+x)*4;
            img[o]=255; img[o+1]=0; img[o+2]=0; img[o+3]=255;
        }
    return img;
}

static void Separator(const char* t)
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "  " << t << std::endl;
    std::cout << "========================================" << std::endl;
}

static void RegisterCallbacks(IVideoEncoder& enc, FTestStats* s)
{
    enc.SetStateCallback(OnState, s);
    enc.SetErrorCallback(OnError, s);
    enc.SetProgressCallback(OnProgress, s);
    enc.SetFrameDropCallback(OnDrop, s);
}

// ============================================================================
static bool RunScenario1()
{
#ifndef VANE_TEST_ENCODING_DELAY_MS
    Separator("场景一：丢帧验证（跳过 — 需要 VANE_TEST_ENCODING_DELAY_MS 编译选项）");
    std::cout << "  SKIP: 此测试需要注入人工编码延迟才能触发丢帧。" << std::endl;
    std::cout << "  使用: cmake -DVANE_TEST_ENCODING_DELAY_MS=80 && cmake --build" << std::endl;
    return true;
#else
    Separator("场景一：120fps 输入 vs 30ms 编码延迟（验证丢帧）");
    FTestStats s;
    uint8_t* f = CreateBlueFrame();

    FEncoderConfig c;
    c.Width=Width; c.Height=Height; c.FrameRate=60; c.RecordFrameRate=60;
    c.BitRate=Width*Height*4/2; c.bRealTime=true; c.FrameQueueSize=16;

    EncoderType enc;
#ifdef _WIN32
    SetupD3D11Device(enc);
#endif
    if (!enc.Initialize(c)) { delete[] f; return false; }
    RegisterCallbacks(enc, &s);
    if (!enc.StartRecording("test_scene1.mp4")) { delete[] f; return false; }

    auto t0 = std::chrono::steady_clock::now();
    int32 pushed=0, dropped=0;
    for (int32 i=0; i<240; ++i) {
        double ts = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        if (enc.EncodeFrame(f,FrameSize,ts)) ++pushed; else ++dropped;
        std::this_thread::sleep_for(std::chrono::microseconds(8300));
    }
    enc.StopRecording();

    bool pass = (s.DroppedFrames>0) && (s.DropCalls>0) && (s.Errors==0);
    std::cout << "  Push: "<<pushed<<"  drop(API): "<<dropped<<"  encoded: "<<s.EncodedFrames<<std::endl;
    std::cout << "  drop(CB): "<<s.DroppedFrames<<"  state:"<<s.StateChanges
              <<"  err:"<<s.Errors<<"  prog:"<<s.ProgressCalls<<"  dropCB:"<<s.DropCalls<<std::endl;
    std::cout << "  => " << (pass?"PASS":"FAIL") << std::endl;
    delete[] f;
    return pass;
#endif
}

// ============================================================================
static bool RunScenario2()
{
    Separator("场景二：主线程卡顿 5s（验证 LastFrame 续命）");
    FTestStats s;
    uint8_t* f = CreateBlueFrame();

    FEncoderConfig c;
    c.Width=Width; c.Height=Height; c.FrameRate=60; c.RecordFrameRate=60;
    c.BitRate=Width*Height*4/2; c.bRealTime=true; c.FrameQueueSize=32;
    c.bContinueLastFrame = true;

    EncoderType enc;
#ifdef _WIN32
    SetupD3D11Device(enc);
#endif
    if (!enc.Initialize(c)) { delete[] f; return false; }
    RegisterCallbacks(enc, &s);
    if (!enc.StartRecording("test_scene2.mp4")) { delete[] f; return false; }

    auto t0 = std::chrono::steady_clock::now();
    int32 pushed=0;

    for (int32 i=0; i<120; ++i) {
        double ts = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        if (enc.EncodeFrame(f,FrameSize,ts)) ++pushed;
        auto trg = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>((i+1)/60.0));
        while (std::chrono::steady_clock::now()<trg) std::this_thread::yield();
    }
    int32 pre = s.EncodedFrames;
    int32 preDrop = s.DropCalls;
    std::cout << "  [Push 2s] pushed:"<<pushed<<"  encoded:"<<pre<<"  drops:"<<preDrop<<std::endl;

    std::cout << "  [Stall 5s] ..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));
    int32 stallFrames = s.EncodedFrames - pre;
    int32 stallDrops   = s.DropCalls - preDrop;
    std::cout << "  [Stall 5s] 续命: +"<<stallFrames<<" 帧  drops:"<<stallDrops<<std::endl;

    for (int32 i=0; i<180; ++i) {
        double ts = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        if (enc.EncodeFrame(f,FrameSize,ts)) ++pushed;
        auto trg = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(7.0+(i+1)/60.0));
        while (std::chrono::steady_clock::now()<trg) std::this_thread::yield();
    }
    enc.StopRecording();

    bool pass = (stallFrames>0) && (stallDrops==0) && (s.Errors==0);
    std::cout << "  pushed:"<<pushed<<"  encoded:"<<s.EncodedFrames<<std::endl;
    std::cout << "  state:"<<s.StateChanges<<"  err:"<<s.Errors
              <<"  prog:"<<s.ProgressCalls<<"  dropCB:"<<s.DropCalls<<std::endl;
    std::cout << "  => " << (pass?"PASS":"FAIL") << std::endl;
    delete[] f;
    return pass;
}

// ============================================================================
static bool RunScenario3()
{
    Separator("场景三：异步 RequestStop（验证不阻塞）");
    FTestStats s;
    uint8_t* f = CreateBlueFrame();

    FEncoderConfig c;
    c.Width=Width; c.Height=Height; c.FrameRate=60; c.RecordFrameRate=60;
    c.BitRate=Width*Height*4/2; c.bRealTime=true; c.FrameQueueSize=32;

    EncoderType enc;
#ifdef _WIN32
    SetupD3D11Device(enc);
#endif
    if (!enc.Initialize(c)) { delete[] f; return false; }
    RegisterCallbacks(enc, &s);
    if (!enc.StartRecording("test_scene3.mp4")) { delete[] f; return false; }

    auto t0 = std::chrono::steady_clock::now();
    for (int32 i=0; i<180; ++i) {
        double ts = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        enc.EncodeFrame(f,FrameSize,ts);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    auto s1 = std::chrono::steady_clock::now();
    enc.RequestStop();
    auto s2 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double,std::milli>(s2-s1).count();

    int wait=0;
    while (!s.IdleReceived && wait<300) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); ++wait; }

    bool pass = (ms<5.0) && s.IdleReceived && (s.Errors==0);
    std::cout << "  RequestStop: "<<ms<<" ms  idle:"<<(s.IdleReceived?"YES":"NO")<<std::endl;
    std::cout << "  encoded:"<<s.EncodedFrames<<"  state:"<<s.StateChanges
              <<"  err:"<<s.Errors<<"  prog:"<<s.ProgressCalls<<std::endl;
    std::cout << "  => " << (pass?"PASS":"FAIL") << std::endl;
    delete[] f;
    return pass;
}

// ============================================================================
// 场景四：正常 60fps 吞吐量测试
// ============================================================================
static bool RunScenario4()
{
#ifndef VANE_TEST_ENCODING_DELAY_MS
    Separator("场景四：正常 60fps 吞吐量（600帧/10s，无编码延迟）");
    FTestStats s;
    uint8_t* f = CreateBlueFrame();

    FEncoderConfig c;
    c.Width=Width; c.Height=Height; c.FrameRate=60; c.RecordFrameRate=0; // 不节流
    c.BitRate=Width*Height*4/2; c.bRealTime=true; c.FrameQueueSize=32;

    EncoderType enc;
#ifdef _WIN32
    SetupD3D11Device(enc);
#endif
    if (!enc.Initialize(c)) { delete[] f; return false; }
    RegisterCallbacks(enc, &s);
    if (!enc.StartRecording("test_scene4.mp4")) { delete[] f; return false; }

    auto t0 = std::chrono::steady_clock::now();
    int32 pushed=0;

    // 60fps 推送 10 秒，共 600 帧
    for (int32 i=0; i<600; ++i)
    {
        double ts = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        if (enc.EncodeFrame(f,FrameSize,ts)) ++pushed;
        auto trg = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>((i+1)/60.0));
        while (std::chrono::steady_clock::now()<trg) std::this_thread::yield();
    }

    auto pushEnd = std::chrono::steady_clock::now();
    double pushSec = std::chrono::duration<double>(pushEnd - t0).count();

    enc.StopRecording();
    auto stopEnd = std::chrono::steady_clock::now();
    double stopMs = std::chrono::duration<double,std::milli>(stopEnd - pushEnd).count();
    double totalSec = std::chrono::duration<double>(stopEnd - t0).count();

    bool pass = (pushed==600) && (s.EncodedFrames > 550) && (s.DroppedFrames==0) && (s.Errors==0);
    double avgEncMs = (s.EncodedFrames > 0) ? (totalSec / s.EncodedFrames * 1000.0) : 0.0;

    std::cout << "  Push: "<<pushed<<"  编码: "<<s.EncodedFrames<<"  丢帧: "<<s.DroppedFrames<<std::endl;
    std::cout << "  推送耗时: "<<pushSec<<"s  停止耗时: "<<stopMs<<"ms  总计: "<<totalSec<<"s"<<std::endl;
    std::cout << "  平均每帧编码: "<<avgEncMs<<" ms  吞吐量: "<<(s.EncodedFrames/totalSec)<<" fps"<<std::endl;
    std::cout << "  err:"<<s.Errors<<"  prog:"<<s.ProgressCalls<<"  dropCB:"<<s.DropCalls<<std::endl;
    std::cout << "  => " << (pass?"PASS":"FAIL") << std::endl;
    delete[] f;
    return pass;
#else
    Separator("场景四：吞吐量测试（跳过 — 编码延迟注入模式下无意义）");
    std::cout << "  SKIP: 编码延迟注入会严重降低吞吐量，此测试仅在无延迟模式下运行。" << std::endl;
    return true;
#endif
}

// ============================================================================
// 场景五：编码器环境检测与用户选择测试
// ============================================================================
static bool RunScenario5()
{
    Separator("场景五：编码器环境检测与用户选择");

    // ---- 5.1 检测并打印完整诊断信息 ----
    {
        EncoderType enc;
        FEncoderCapability cap = enc.CheckCapability();

        std::cout << "--- 编码器能力检测结果 ---" << std::endl;
        std::cout << "  H.264 硬件编码: " << (cap.bH264Available ? "可用" : "不可用") << std::endl;
        std::cout << "  H.264 软件编码: " << (cap.bH264SoftwareAvailable ? "可用" : "不可用") << std::endl;
        std::cout << "  HEVC 硬件编码:  " << (cap.bHEVCAvailable ? "可用" : "不可用") << std::endl;
        std::cout << "  WMV 编码:       " << (cap.bWMVAvailable ? "可用" : "不可用") << std::endl;
        std::cout << "  推荐格式:       " << cap.RecommendedFormat << std::endl;
        if (!cap.H264EncoderName.empty())
            std::cout << "  H.264 编码器:   " << cap.H264EncoderName << std::endl;
        std::cout << std::endl;
        std::cout << "--- 诊断明细 ---" << std::endl;
        std::cout << cap.DiagnosticInfo;
    }

    bool overallPass = true;
    uint8_t* f = CreateBlueFrame();

    // ---- 5.2 测试 H.264 首选 + 无降级 ----
    {
        FEncoderConfig c;
        c.Width=Width; c.Height=Height; c.FrameRate=60; c.RecordFrameRate=60;
        c.BitRate=Width*Height*4/2; c.bRealTime=true; c.FrameQueueSize=16;
        c.bUserPreferH264 = true;
        c.bAllowFormatFallback = false;

        EncoderType enc;
#ifdef _WIN32
        SetupD3D11Device(enc);
#endif
        FEncoderCapability capH264 = enc.CheckCapability();
        bool bH264Supported = capH264.bH264Available || capH264.bH264SoftwareAvailable;

        if (!enc.Initialize(c))
        {
            std::cout << "  5.2 初始化失败: " << enc.GetLastError() << std::endl;
            overallPass = false;
        }
        else if (bH264Supported)
        {
            FTestStats s;
            RegisterCallbacks(enc, &s);
            if (!enc.StartRecording("test_scene5a.mp4"))
            {
                std::cout << "  5.2 StartRecording 失败: " << enc.GetLastError() << std::endl;
                overallPass = false;
            }
            else
            {
                for (int32 i = 0; i < 60; ++i)
                {
                    enc.EncodeFrame(f, FrameSize, (double)i / 60.0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                enc.StopRecording();
                bool pass = (s.Errors == 0) && (s.EncodedFrames > 0);
                std::cout << "  5.2 H.264首选(无降级): encoded=" << s.EncodedFrames
                          << " err=" << s.Errors << " => " << (pass ? "PASS" : "FAIL") << std::endl;
                if (!pass) overallPass = false;
            }
        }
        else
        {
            // 系统不支持 H.264，应正确拒绝
            if (!enc.StartRecording("test_scene5a.mp4"))
            {
                std::cout << "  5.2 H.264不支持，正确拒绝: " << enc.GetLastError() << " => PASS" << std::endl;
            }
            else
            {
                std::cout << "  5.2 H.264不支持但未拒绝 => FAIL" << std::endl;
                enc.StopRecording();
                overallPass = false;
            }
        }
    }

    // ---- 5.3 测试用户选择 WMV ----
    {
        FEncoderConfig c;
        c.Width=Width; c.Height=Height; c.FrameRate=60; c.RecordFrameRate=60;
        c.BitRate=Width*Height*4/2; c.bRealTime=true; c.FrameQueueSize=16;
        c.bUserPreferH264 = false;

        EncoderType enc;
#ifdef _WIN32
        SetupD3D11Device(enc);
#endif
        if (!enc.Initialize(c))
        {
            std::cout << "  5.3 初始化失败: " << enc.GetLastError() << std::endl;
            overallPass = false;
        }
        else
        {
            FTestStats s;
            RegisterCallbacks(enc, &s);
            if (!enc.StartRecording("test_scene5b.wmv"))
            {
                std::cout << "  5.3 WMV StartRecording 失败: " << enc.GetLastError() << std::endl;
                overallPass = false;
            }
            else
            {
                for (int32 i = 0; i < 60; ++i)
                {
                    enc.EncodeFrame(f, FrameSize, (double)i / 60.0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                enc.StopRecording();
                bool pass = (s.Errors == 0) && (s.EncodedFrames > 0);
                std::cout << "  5.3 WMV选择: encoded=" << s.EncodedFrames
                          << " err=" << s.Errors << " => " << (pass ? "PASS" : "FAIL") << std::endl;
                if (!pass) overallPass = false;
            }
        }
    }

    delete[] f;
    std::cout << "  => " << (overallPass ? "PASS" : "FAIL") << std::endl;
    return overallPass;
}

// ============================================================================
int main()
{
    bool a=RunScenario1(), b=RunScenario2(), c=RunScenario3(), d=RunScenario4(), e=RunScenario5();
    std::cout << "\n========================================" << std::endl;
    std::cout << "           汇总报告" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  场景一（丢帧验证）: " << (a?"PASS":"FAIL") << std::endl;
    std::cout << "  场景二（卡顿续命）: " << (b?"PASS":"FAIL") << std::endl;
    std::cout << "  场景三（异步Stop）: " << (c?"PASS":"FAIL") << std::endl;
    std::cout << "  场景四（吞吐量）:   " << (d?"PASS":"FAIL") << std::endl;
    std::cout << "  场景五（环境检测）: " << (e?"PASS":"FAIL") << std::endl;
    std::cout << "========================================" << std::endl;
    return (a&&b&&c&&d&&e)?0:1;
}
