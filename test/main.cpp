#if defined(__APPLE__)
    #include "FVTEncoder.h"
    using FPlatformEncoder = FVTEncoder;
#elif defined(_WIN32)
    #include "FMFEncoder.h"
    using FPlatformEncoder = FMFEncoder;
#elif defined(__linux__)
    #include "FVAEncoder.h"
    using FPlatformEncoder = FVAEncoder;
#else
    #error "Unsupported platform"
#endif

#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdint>

using int32 = int32_t;

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
static void OnError(EErrorLevel, const char*, void* p) { static_cast<FTestStats*>(p)->Errors++; }
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

static void RegisterCallbacks(FPlatformEncoder& enc, FTestStats* s)
{
    enc.SetStateCallback(OnState, s);
    enc.SetErrorCallback(OnError, s);
    enc.SetProgressCallback(OnProgress, s);
    enc.SetFrameDropCallback(OnDrop, s);
}

// ============================================================================
// 场景一：丢帧验证
// ============================================================================
static bool RunScenario1()
{
    Separator("场景一：丢帧验证（push 120帧 into 16-slot queue）");
    FTestStats s;
    uint8_t* f = CreateBlueFrame();

    FEncoderConfig c;
    c.Width=Width; c.Height=Height; c.FrameRate=60; c.RecordFrameRate=60;
    c.BitRate=Width*Height*4/2; c.bRealTime=true; c.FrameQueueSize=16;

    FPlatformEncoder enc;
    if (!enc.Initialize(c)) { std::cout << "  Initialize 失败: " << enc.GetLastError() << std::endl; delete[] f; return false; }
    RegisterCallbacks(enc, &s);
    if (!enc.StartRecording("test_scene1.mp4")) { std::cout << "  StartRecording 失败" << std::endl; delete[] f; return false; }

    auto t0 = std::chrono::steady_clock::now();
    int32 pushed=0, dropped=0;
    for (int32 i=0; i<120; ++i) {
        double ts = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        if (enc.EncodeFrame(f,FrameSize,ts)) ++pushed; else ++dropped;
        std::this_thread::sleep_for(std::chrono::microseconds(8300));
    }
    enc.StopRecording();

    bool pass = (pushed >= 100) && (dropped == 0) && (s.Errors==0);
    std::cout << "  Push: "<<pushed<<"  drop(API): "<<dropped<<"  encoded(CB): "<<s.EncodedFrames<<std::endl;
    std::cout << "  drop(CB): "<<s.DroppedFrames<<"  state:"<<s.StateChanges
              <<"  err:"<<s.Errors<<"  prog:"<<s.ProgressCalls<<"  dropCB:"<<s.DropCalls<<std::endl;
    std::cout << "  => " << (pass?"PASS":"FAIL") << std::endl;
    delete[] f;
    return pass;
}

// ============================================================================
// 场景二：主线程卡顿，LastFrame 续命
// ============================================================================
static bool RunScenario2()
{
    Separator("场景二：主线程卡顿 5s（LastFrame 续命）");
    FTestStats s;
    uint8_t* f = CreateBlueFrame();

    FEncoderConfig c;
    c.Width=Width; c.Height=Height; c.FrameRate=60; c.RecordFrameRate=60;
    c.BitRate=Width*Height*4/2; c.bRealTime=true; c.FrameQueueSize=32;
    c.bContinueLastFrame = true;

    FPlatformEncoder enc;
    if (!enc.Initialize(c)) { std::cout << "  Initialize 失败: " << enc.GetLastError() << std::endl; delete[] f; return false; }
    RegisterCallbacks(enc, &s);
    if (!enc.StartRecording("test_scene2.mp4")) { std::cout << "  StartRecording 失败" << std::endl; delete[] f; return false; }

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
    std::cout << "  [Push 2s] pushed:"<<pushed<<"  encoded(CB):"<<pre<<std::endl;

    std::cout << "  [Stall 5s] ..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));
    int32 stallFrames = s.EncodedFrames - pre;
    std::cout << "  [Stall 5s] 续命: +"<<stallFrames<<" 帧"<<std::endl;

    for (int32 i=0; i<180; ++i) {
        double ts = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        if (enc.EncodeFrame(f,FrameSize,ts)) ++pushed;
        auto trg = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(7.0+(i+1)/60.0));
        while (std::chrono::steady_clock::now()<trg) std::this_thread::yield();
    }
    enc.StopRecording();

    bool pass = (pushed >= 280) && (s.Errors==0);
    std::cout << "  pushed:"<<pushed<<"  encoded(CB):"<<s.EncodedFrames<<std::endl;
    std::cout << "  state:"<<s.StateChanges<<"  err:"<<s.Errors
              <<"  prog:"<<s.ProgressCalls<<"  dropCB:"<<s.DropCalls<<std::endl;
    std::cout << "  => " << (pass?"PASS":"FAIL") << std::endl;
    delete[] f;
    return pass;
}

// ============================================================================
// 场景三：异步 RequestStop
// ============================================================================
static bool RunScenario3()
{
    Separator("场景三：异步 RequestStop（验证不阻塞）");
    FTestStats s;
    uint8_t* f = CreateBlueFrame();

    FEncoderConfig c;
    c.Width=Width; c.Height=Height; c.FrameRate=60; c.RecordFrameRate=60;
    c.BitRate=Width*Height*4/2; c.bRealTime=true; c.FrameQueueSize=32;

    FPlatformEncoder enc;
    if (!enc.Initialize(c)) { std::cout << "  Initialize 失败: " << enc.GetLastError() << std::endl; delete[] f; return false; }
    RegisterCallbacks(enc, &s);
    if (!enc.StartRecording("test_scene3.mp4")) { std::cout << "  StartRecording 失败" << std::endl; delete[] f; return false; }

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

    bool pass = (ms < 10.0) && s.IdleReceived && (s.Errors==0);
    std::cout << "  RequestStop: "<<ms<<" ms  idle:"<<(s.IdleReceived?"YES":"NO")<<std::endl;
    std::cout << "  encoded(CB):"<<s.EncodedFrames<<"  state:"<<s.StateChanges
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
    Separator("场景四：正常 60fps 吞吐量（300帧/5s）");
    FTestStats s;
    uint8_t* f = CreateBlueFrame();

    FEncoderConfig c;
    c.Width=Width; c.Height=Height; c.FrameRate=60; c.RecordFrameRate=0;
    c.BitRate=Width*Height*4/2; c.bRealTime=true; c.FrameQueueSize=32;

    FPlatformEncoder enc;
    if (!enc.Initialize(c)) { std::cout << "  Initialize 失败: " << enc.GetLastError() << std::endl; delete[] f; return false; }
    RegisterCallbacks(enc, &s);
    if (!enc.StartRecording("test_scene4.mp4")) { std::cout << "  StartRecording 失败" << std::endl; delete[] f; return false; }

    auto t0 = std::chrono::steady_clock::now();
    int32 pushed=0;

    for (int32 i=0; i<300; ++i)
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

    bool pass = (pushed==300) && (s.Errors==0);
    double avgEncMs = (pushed > 0) ? (totalSec / pushed * 1000.0) : 0.0;

    std::cout << "  Push: "<<pushed<<"  丢帧: "<<s.DroppedFrames<<std::endl;
    std::cout << "  推送耗时: "<<pushSec<<"s  停止耗时: "<<stopMs<<"ms  总计: "<<totalSec<<"s"<<std::endl;
    std::cout << "  平均每帧编码: "<<avgEncMs<<" ms  吞吐量: "<<(pushed/totalSec)<<" fps"<<std::endl;
    std::cout << "  err:"<<s.Errors<<"  prog:"<<s.ProgressCalls<<"  dropCB:"<<s.DropCalls<<std::endl;
    std::cout << "  => " << (pass?"PASS":"FAIL") << std::endl;
    delete[] f;
    return pass;
}

// ============================================================================
int main()
{
    bool a=RunScenario1(), b=RunScenario2(), c=RunScenario3(), d=RunScenario4();
    std::cout << "\n========================================" << std::endl;
    std::cout << "           汇总报告" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  场景一（丢帧验证）: " << (a?"PASS":"FAIL") << std::endl;
    std::cout << "  场景二（卡顿续命）: " << (b?"PASS":"FAIL") << std::endl;
    std::cout << "  场景三（异步Stop）: " << (c?"PASS":"FAIL") << std::endl;
    std::cout << "  场景四（吞吐量）:   " << (d?"PASS":"FAIL") << std::endl;
    std::cout << "========================================" << std::endl;
    return (a&&b&&c&&d)?0:1;
}
