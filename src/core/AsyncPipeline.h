#pragma once

#include "Vane/IVideoEncoder.h"
#include <atomic>
#include <vector>
#include <thread>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <memory>
#include <mutex>
#include <condition_variable>

// 帧数据（使用指针指向 slot 内部缓冲区，消除拷贝）
struct FFrameBuffer
{
    const uint8_t* Data       = nullptr;
    size_t         DataSize   = 0;
    double         TimestampSeconds = 0.0;
    bool           bValid     = false;
};

// 无锁环形帧队列（SPSC — 丢旧不丢新）
class FFrameQueue
{
public:
    explicit FFrameQueue(int32 Capacity = 32);

    // 直接写入源数据到 slot（1次 memcpy）
    int32 PushRaw(const uint8_t* Data, size_t Size, double TimestampSeconds);

    // 编码线程取帧（零拷贝：OutFrame 指针指向 slot 内部缓冲区）
    bool TryPop(FFrameBuffer& OutFrame);

    int32 GetBacklog() const;

private:
    struct FSlot
    {
        std::vector<uint8_t> Data;   // 预分配缓冲区（首次写入时分配）
        double               TimestampSeconds = 0.0;
        bool                 bValid = false;
    };

    int32                          Capacity;
    int32                          Mask;
    std::vector<FSlot>             Slots;
    alignas(64) std::atomic<int32> WriteIdx;
    alignas(64) std::atomic<int32> ReadIdx;
};

// 异步编码管线
using FEncodeCallback = std::function<void(const FFrameBuffer& Frame)>;
using FFlushCallback  = std::function<void()>;
using FFinalizeCallback = std::function<void()>;

class FAsyncEncodingPipeline
{
public:
    FAsyncEncodingPipeline();
    ~FAsyncEncodingPipeline();

    void Start(FEncodeCallback Callback, FFlushCallback FlushCb,
               int32 QueueSize, int32 FlushIntervalFrames,
               bool bContinueLastFrame = false, int32 FPSForLastFrame = 0);

    // 主线程塞帧（含采样节流，零拷贝：直接写入 slot）
    bool PushFrame(const uint8_t* RawData, size_t DataSize, double TimestampSeconds, int32 TargetFPS);

    void RequestStop(FFinalizeCallback FinalizeCb);
    void Stop();

    bool IsRunning() const;
    int64_t GetDroppedFrames() const;
    int64_t GetEncodedFrames() const;

private:
    void EncodeLoop(FEncodeCallback Callback, FFlushCallback FlushCb, int32 FlushIntervalFrames,
                    bool bContinueLastFrame, int32 FPSForLastFrame);

    std::unique_ptr<FFrameQueue> QueuePtr;
    std::thread          Thread;
    std::atomic<bool>    bStopRequested;
    std::atomic<bool>    bRunning;
    std::atomic<int64_t> DroppedCount;
    std::atomic<int64_t> EncodedCount;
    double               LastSampledTime = -1.0;

    // 条件变量：编码线程等待新帧（替代 yield 自旋）
    std::mutex              CvMutex;
    std::condition_variable CvWake;
};
