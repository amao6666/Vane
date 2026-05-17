#import <VideoToolbox/VideoToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

// 取消 Foundation 宏与参数名冲突
#undef Height

#include "FVTEncoder.h"
#include "AsyncPipeline.h"
#include <dispatch/dispatch.h>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>

// ============================================================================
// 平台实现结构体（Objective-C / C 类型混用）
// ============================================================================
struct FVTEncoderImpl
{
    int32 Width       = 0;
    int32 Height      = 0;
    int32 FrameCount  = 0;
    int32 FrameRate   = 60;
    int32 RecordFrameRate = 60;
    int32 QueueSize   = 32;
    int32 FlushInterval = 60;
    bool  bContinueLastFrame = false;
    int32 LastDroppedCount   = 0;

    VTCompressionSessionRef CompressionSession = nullptr;

    AVAssetWriter*      AssetWriter      = nil;
    AVAssetWriterInput* AssetWriterInput = nil;

    FAsyncEncodingPipeline Pipeline;

    // 回调
    VaneStateCallback    StateCb    = nullptr;
    VaneErrorCallback    ErrorCb    = nullptr;
    VaneProgressCallback ProgressCb = nullptr;
    VaneFrameDropCallback FrameDropCb = nullptr;
    void* StateUserData    = nullptr;
    void* ErrorUserData    = nullptr;
    void* ProgressUserData = nullptr;
    void* FrameDropUserData = nullptr;

    // 进度统计（编码线程更新 + 读取，单线程无需原子）
    int64_t      TotalBytesWritten  = 0;
    int64_t      TotalFramesEncoded = 0;
    std::chrono::steady_clock::time_point LastProgressTime;

    bool bInitialized = false;
    bool bRecording   = false;
    bool bFirstFrame  = true;
    bool bHasError    = false;

    std::string LastError;

    static constexpr int32 TimeScale = 600; // 时间刻度（60fps 下每帧 10 单位）
};

// ============================================================================
// VTCompressionSession 输出回调 —— VideoToolbox 每编码完一帧就会调用此函数
// ============================================================================
static void CompressionOutputCallback(
    void*               outputCallbackRefCon,
    void*               sourceFrameRefCon,
    OSStatus            status,
    VTEncodeInfoFlags   infoFlags,
    CMSampleBufferRef   sampleBuffer)
{
    auto* impl = static_cast<FVTEncoderImpl*>(outputCallbackRefCon);

    if (status != noErr)
    {
        impl->bHasError = true;
        impl->LastError = "VideoToolbox 编码回调返回错误";
        if (impl->ErrorCb)
            impl->ErrorCb(EErrorLevel_Error, impl->LastError.c_str(), impl->ErrorUserData);
        if (impl->StateCb)
            impl->StateCb(ERecordingState_Error, impl->StateUserData);
        return;
    }

    if (!sampleBuffer || !impl->AssetWriter)
    {
        return;
    }

    // 第一帧：从压缩数据中提取格式描述，创建 AVAssetWriterInput（直通模式）
    if (impl->bFirstFrame)
    {
        CMFormatDescriptionRef fmtDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (!fmtDesc)
        {
            impl->bHasError = true;
            impl->LastError = "无法获取帧格式描述";
            return;
        }

        // nil outputSettings = 直通模式，AVAssetWriter 只负责容器封装
        impl->AssetWriterInput = [[AVAssetWriterInput alloc]
            initWithMediaType:AVMediaTypeVideo
               outputSettings:nil
             sourceFormatHint:fmtDesc];

        impl->AssetWriterInput.expectsMediaDataInRealTime = NO;

        if ([impl->AssetWriter canAddInput:impl->AssetWriterInput])
        {
            [impl->AssetWriter addInput:impl->AssetWriterInput];
        }
        else
        {
            impl->bHasError = true;
            impl->LastError = "无法将输入添加到 AVAssetWriter";
            return;
        }

        // 启动写入会话
        if (![impl->AssetWriter startWriting])
        {
            impl->bHasError = true;
            impl->LastError = "AVAssetWriter 启动写入失败";
            return;
        }

        CMTime startTime = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
        [impl->AssetWriter startSessionAtSourceTime:startTime];

        impl->bFirstFrame = false;
    }

    // 累加编码后字节数
    size_t sampleSize = CMSampleBufferGetTotalSampleSize(sampleBuffer);
    if (sampleSize > 0)
        impl->TotalBytesWritten += static_cast<int64_t>(sampleSize);

    // 将压缩后的 CMSampleBuffer 写入 MP4 容器
    if (impl->AssetWriterInput && impl->AssetWriterInput.readyForMoreMediaData)
    {
        if (![impl->AssetWriterInput appendSampleBuffer:sampleBuffer])
        {
            impl->bHasError = true;
            impl->LastError = "写入 CMSampleBuffer 到 MP4 容器失败";

            // 错误回调（编码线程触发）
            if (impl->ErrorCb)
                impl->ErrorCb(EErrorLevel_Error, impl->LastError.c_str(), impl->ErrorUserData);
            if (impl->StateCb)
                impl->StateCb(ERecordingState_Error, impl->StateUserData);
        }
    }
}

// ============================================================================
// FVTEncoder 公开接口实现
// ============================================================================

FVTEncoder::FVTEncoder()
    : Impl(new FVTEncoderImpl())
{
}

FVTEncoder::~FVTEncoder()
{
    StopRecording();

    if (Impl->CompressionSession)
    {
        VTCompressionSessionInvalidate(Impl->CompressionSession);
        CFRelease(Impl->CompressionSession);
    }

    delete Impl;
}

bool FVTEncoder::Initialize(const FEncoderConfig& Config)
{
    if (Impl->bInitialized) return true;
    if (Config.Width <= 0 || Config.Height <= 0)
    {
        Impl->LastError = "分辨率无效：宽度和高度必须大于 0";
        return false;
    }

    Impl->Width           = Config.Width;
    Impl->Height          = Config.Height;
    Impl->FrameRate       = Config.FrameRate;
    Impl->RecordFrameRate = Config.RecordFrameRate > 0 ? Config.RecordFrameRate : Config.FrameRate;
    Impl->QueueSize       = Config.FrameQueueSize;
    Impl->FlushInterval   = Config.FlushIntervalFrames;
    Impl->bContinueLastFrame = Config.bContinueLastFrame;

    // 根据 Codec 选择编码格式
    CMVideoCodecType codecType = kCMVideoCodecType_H264;
    if (Config.Codec && strcmp(Config.Codec, "hevc") == 0)
    {
        codecType = kCMVideoCodecType_HEVC;
    }

    // ---- 构建源图像属性字典（告知 VT 输入格式为 BGRA）----
    CFMutableDictionaryRef srcAttrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 3,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    int32 pixelFmt = kCVPixelFormatType_32BGRA;
    CFNumberRef cfPixFmt = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixelFmt);
    CFDictionarySetValue(srcAttrs, kCVPixelBufferPixelFormatTypeKey, cfPixFmt);
    CFRelease(cfPixFmt);

    int32 w = Config.Width, h = Config.Height;
    CFNumberRef cfW = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &w);
    CFNumberRef cfH = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &h);
    CFDictionarySetValue(srcAttrs, kCVPixelBufferWidthKey, cfW);
    CFDictionarySetValue(srcAttrs, kCVPixelBufferHeightKey, cfH);
    CFRelease(cfW);
    CFRelease(cfH);

    // ---- 创建压缩会话 ----
    OSStatus err = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        Config.Width, Config.Height,
        codecType,
        nullptr,                   // encoderSpecification: 使用默认编码器
        srcAttrs,
        nullptr,                   // compressedDataAllocator: 使用默认分配器
        CompressionOutputCallback,
        Impl,                      // refCon: 在回调中获取 impl 指针
        &Impl->CompressionSession);

    CFRelease(srcAttrs);

    if (err != noErr)
    {
        Impl->LastError = "创建 VTCompressionSession 失败（OSStatus=" + std::to_string(err) + ")";
        return false;
    }

    // ---- 配置编码参数 ----
    CFBooleanRef realTime = Config.bRealTime ? kCFBooleanTrue : kCFBooleanFalse;
    VTSessionSetProperty(Impl->CompressionSession,
        kVTCompressionPropertyKey_RealTime, realTime);

    VTSessionSetProperty(Impl->CompressionSession,
        kVTCompressionPropertyKey_ProfileLevel,
        kVTProfileLevel_H264_High_AutoLevel);

    int32 bitRate = Config.BitRate;
    CFNumberRef cfBitRate = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bitRate);
    VTSessionSetProperty(Impl->CompressionSession,
        kVTCompressionPropertyKey_AverageBitRate, cfBitRate);
    CFRelease(cfBitRate);

    int32 fps = Config.FrameRate;
    CFNumberRef cfFPS = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &fps);
    VTSessionSetProperty(Impl->CompressionSession,
        kVTCompressionPropertyKey_ExpectedFrameRate, cfFPS);
    CFRelease(cfFPS);

    int32 keyInterval = Config.KeyframeInterval;
    CFNumberRef cfKeyInt = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &keyInterval);
    VTSessionSetProperty(Impl->CompressionSession,
        kVTCompressionPropertyKey_MaxKeyFrameInterval, cfKeyInt);
    CFRelease(cfKeyInt);

    // 禁用 B 帧重排序，保证输出顺序与输入顺序一致
    VTSessionSetProperty(Impl->CompressionSession,
        kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    // 通知 VT 准备好编码
    VTCompressionSessionPrepareToEncodeFrames(Impl->CompressionSession);

    Impl->bInitialized = true;
    return true;
}

bool FVTEncoder::StartRecording(const char* OutputPath)
{
    if (!Impl->bInitialized)
    {
        Impl->LastError = "编码器未初始化：请先调用 Initialize()";
        return false;
    }
    if (Impl->bRecording)
    {
        Impl->LastError = "编码器已在录制中";
        return false;
    }

    // ---- 创建 AVAssetWriter，输出 MP4 ----
    NSString* path = [NSString stringWithUTF8String:OutputPath];
    NSURL*    url  = [NSURL fileURLWithPath:path];

    // 删除已存在的文件
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];

    NSError* error = nil;
    Impl->AssetWriter = [[AVAssetWriter alloc] initWithURL:url
                                                  fileType:AVFileTypeMPEG4
                                                     error:&error];
    if (error || !Impl->AssetWriter)
    {
        Impl->LastError = "创建 AVAssetWriter 失败";
        return false;
    }

    // fMP4 崩溃恢复：每秒写入一个 fragment，崩溃后已写入部分可播
    // AVAssetWriter 自动将 moov 放在文件头，后续写入 moof + mdat
    Impl->AssetWriter.movieFragmentInterval = CMTimeMake(1, 1);

    // 重置录制状态
    Impl->FrameCount  = 0;
    Impl->bFirstFrame = true;
    Impl->bHasError   = false;
    Impl->bRecording  = true;

    // 触发状态回调
    if (Impl->StateCb)
    {
        Impl->StateCb(ERecordingState_Starting, Impl->StateUserData);
    }

    // ---- 启动异步编码管线 ----
    FVTEncoderImpl* pImpl = Impl;
    FEncodeCallback cb = [pImpl](const FFrameBuffer& Frame) {
        int32 bytesPerRow = pImpl->Width * 4;
        size_t expectedSize = pImpl->Height * bytesPerRow;

        // 分配 CVPixelBuffer
        CVPixelBufferRef pixelBuffer = nullptr;
        CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(pImpl->CompressionSession);
        if (pool)
        {
            CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixelBuffer);
        }
        if (!pixelBuffer)
        {
            CVPixelBufferCreate(kCFAllocatorDefault,
                pImpl->Width, pImpl->Height,
                kCVPixelFormatType_32BGRA, nullptr, &pixelBuffer);
        }
        if (!pixelBuffer) return;

        // 拷贝 BGRA 数据
        CVPixelBufferLockBaseAddress(pixelBuffer, 0);
        void* baseAddr = CVPixelBufferGetBaseAddress(pixelBuffer);
        size_t dstBytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
        if (baseAddr)
        {
            if ((size_t)bytesPerRow == dstBytesPerRow)
            {
                memcpy(baseAddr, Frame.Data, Frame.DataSize);
            }
            else
            {
                const uint8* src = Frame.Data;
                uint8* dst = static_cast<uint8*>(baseAddr);
                for (int32 row = 0; row < pImpl->Height; ++row)
                {
                    memcpy(dst, src, bytesPerRow);
                    src += bytesPerRow;
                    dst += dstBytesPerRow;
                }
            }
        }
        CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

        // 使用真实时间戳计算 PTS
        CMTime pts      = CMTimeMake(Frame.TimestampSeconds * pImpl->TimeScale, pImpl->TimeScale);
        int32 frameDur  = pImpl->TimeScale / pImpl->FrameRate;
        CMTime duration = CMTimeMake(frameDur, pImpl->TimeScale);

        // 编码
        OSStatus err = VTCompressionSessionEncodeFrame(
            pImpl->CompressionSession,
            pixelBuffer,
            pts,
            duration,
            nullptr, nullptr, nullptr);

        CVPixelBufferRelease(pixelBuffer);

        // 测试用编码延迟注入（模拟复杂编码场景）
        #ifdef VANE_TEST_ENCODING_DELAY_MS
            std::this_thread::sleep_for(std::chrono::milliseconds(VANE_TEST_ENCODING_DELAY_MS));
        #endif

        if (err != noErr)
        {
            pImpl->bHasError = true;
            pImpl->LastError = "VideoToolbox 编码失败（OSStatus=" + std::to_string(err) + ")";
            // 错误回调（编码线程触发）
            if (pImpl->ErrorCb)
            {
                pImpl->ErrorCb(EErrorLevel_Error, pImpl->LastError.c_str(), pImpl->ErrorUserData);
            }
            if (pImpl->StateCb)
            {
                pImpl->StateCb(ERecordingState_Error, pImpl->StateUserData);
            }
            return;
        }

        // 进度统计（编码线程，单线程无竞争）
        ++pImpl->TotalFramesEncoded;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - pImpl->LastProgressTime).count();

        if (elapsed >= 1000 && pImpl->ProgressCb)
        {
            pImpl->ProgressCb(pImpl->TotalFramesEncoded, pImpl->TotalBytesWritten,
                              Frame.TimestampSeconds, pImpl->ProgressUserData);
            pImpl->LastProgressTime = now;
        }
    };

    // ---- 刷盘回调（编码线程触发，macOS 10.15+ 强制写入文件）----
    FFlushCallback flushCb = [pImpl]() {
        if (@available(macOS 10.15, *)) {
            if (pImpl->AssetWriter && pImpl->AssetWriter.status == AVAssetWriterStatusWriting)
                [(id)pImpl->AssetWriter flush];
        }
    };

    Impl->Pipeline.Start(std::move(cb), std::move(flushCb), Impl->QueueSize, Impl->FlushInterval,
                         Impl->bContinueLastFrame, Impl->FrameRate);

    // 初始化进度统计
    Impl->TotalBytesWritten  = 0;
    Impl->TotalFramesEncoded = 0;
    Impl->LastProgressTime   = std::chrono::steady_clock::now();

    if (Impl->StateCb)
    {
        Impl->StateCb(ERecordingState_Recording, Impl->StateUserData);
    }

    return true;
}

bool FVTEncoder::EncodeFrame(const uint8* RawBGRA, int32 DataSize, double TimestampSeconds)
{
    if (!Impl->bRecording)
    {
        Impl->LastError = "编码器未在录制状态";
        return false;
    }
    if (!RawBGRA || DataSize <= 0)
    {
        Impl->LastError = "输入帧数据为空";
        return false;
    }

    // ---- 推入异步队列（零拷贝：直接写入 slot）----
    Impl->Pipeline.PushFrame(RawBGRA, DataSize, TimestampSeconds, Impl->RecordFrameRate);

    int32 dropped = static_cast<int32>(Impl->Pipeline.GetDroppedFrames());
    if (dropped > Impl->LastDroppedCount)
    {
        Impl->LastDroppedCount = dropped;
        if (Impl->FrameDropCb)
            Impl->FrameDropCb(dropped, Impl->FrameDropUserData);
    }

    return true;
}

void FVTEncoder::RequestStop()
{
    if (!Impl->bRecording) return;

    Impl->bRecording = false;

    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Stopping, Impl->StateUserData);

    // 捕获 Impl 指针用于文件收尾
    FVTEncoderImpl* pImpl = Impl;
    FFinalizeCallback finalizeCb = [pImpl]() {
        VTCompressionSessionCompleteFrames(pImpl->CompressionSession, kCMTimeInvalid);

        if (pImpl->AssetWriterInput)
            [pImpl->AssetWriterInput markAsFinished];

        if (pImpl->AssetWriter)
        {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [pImpl->AssetWriter finishWritingWithCompletionHandler:^{
                dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

            pImpl->AssetWriter      = nil;
            pImpl->AssetWriterInput = nil;
        }

        pImpl->bFirstFrame = true;
        VTCompressionSessionPrepareToEncodeFrames(pImpl->CompressionSession);

        // 强制最终进度
        if (pImpl->ProgressCb && pImpl->TotalFramesEncoded > 0)
        {
            double finalSec = static_cast<double>(pImpl->TotalFramesEncoded) / pImpl->FrameRate;
            pImpl->ProgressCb(pImpl->TotalFramesEncoded, pImpl->TotalBytesWritten,
                             finalSec, pImpl->ProgressUserData);
        }

        if (pImpl->StateCb)
            pImpl->StateCb(ERecordingState_Idle, pImpl->StateUserData);
    };

    Impl->Pipeline.RequestStop(std::move(finalizeCb));
}

void FVTEncoder::StopRecording()
{
    if (!Impl->bRecording) return;

    Impl->bRecording = false;

    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Stopping, Impl->StateUserData);

    Impl->Pipeline.Stop();

    VTCompressionSessionCompleteFrames(Impl->CompressionSession, kCMTimeInvalid);

    if (Impl->AssetWriterInput)
        [Impl->AssetWriterInput markAsFinished];

    if (Impl->AssetWriter)
    {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [Impl->AssetWriter finishWritingWithCompletionHandler:^{
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        Impl->AssetWriter      = nil;
        Impl->AssetWriterInput = nil;
    }

    Impl->bFirstFrame = true;
    VTCompressionSessionPrepareToEncodeFrames(Impl->CompressionSession);

    // 停止时强制发送最终进度（绕过1秒节流）
    if (Impl->ProgressCb && Impl->TotalFramesEncoded > 0)
    {
        double finalSec = static_cast<double>(Impl->TotalFramesEncoded) / Impl->FrameRate;
        Impl->ProgressCb(Impl->TotalFramesEncoded, Impl->TotalBytesWritten,
                         finalSec, Impl->ProgressUserData);
    }

    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Idle, Impl->StateUserData);
}

bool FVTEncoder::IsRecording() const
{
    return Impl->bRecording;
}

const char* FVTEncoder::GetLastError() const
{
    return Impl->LastError.c_str();
}

void FVTEncoder::SetStateCallback(VaneStateCallback Cb, void* UserData)
{
    Impl->StateCb     = Cb;
    Impl->StateUserData = UserData;
}

void FVTEncoder::SetErrorCallback(VaneErrorCallback Cb, void* UserData)
{
    Impl->ErrorCb     = Cb;
    Impl->ErrorUserData = UserData;
}

void FVTEncoder::SetProgressCallback(VaneProgressCallback Cb, void* UserData)
{
    Impl->ProgressCb     = Cb;
    Impl->ProgressUserData = UserData;
}

void FVTEncoder::SetFrameDropCallback(VaneFrameDropCallback Cb, void* UserData)
{
    Impl->FrameDropCb     = Cb;
    Impl->FrameDropUserData = UserData;
}
