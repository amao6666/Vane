#include "AsyncPipeline.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

static int32 NextPowerOf2(int32 v)
{
    if (v <= 0) return 2;
    --v;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

// ============================================================================
// FFrameQueue
// ============================================================================

FFrameQueue::FFrameQueue(int32 InCapacity)
    : Capacity(NextPowerOf2(std::max(2, InCapacity)))
    , Mask(Capacity - 1)
    , Slots(Capacity)
    , WriteIdx(0)
    , ReadIdx(0)
{
}

int32 FFrameQueue::PushRaw(const uint8_t* Data, size_t Size, double TimestampSeconds)
{
    int32 dropped = 0;
    int32 cur  = WriteIdx.load(std::memory_order_relaxed);
    int32 next = (cur + 1) & Mask;

    if (next == ReadIdx.load(std::memory_order_acquire))
    {
        // 队列满：丢弃最旧帧，释放一个槽位
        int32 oldRead = ReadIdx.load(std::memory_order_relaxed);
        Slots[oldRead].bValid = false;
        ReadIdx.store((oldRead + 1) & Mask, std::memory_order_release);
        dropped = 1;
    }

    // 写入新帧数据到 slot（唯一的 memcpy）
    cur  = WriteIdx.load(std::memory_order_relaxed);
    next = (cur + 1) & Mask;

    FSlot& slot = Slots[cur];
    if (slot.Data.size() != Size)
        slot.Data.resize(Size);
    memcpy(slot.Data.data(), Data, Size);
    slot.TimestampSeconds = TimestampSeconds;
    slot.bValid           = true;

    WriteIdx.store(next, std::memory_order_release);
    return dropped;
}

bool FFrameQueue::TryPop(FFrameBuffer& OutFrame)
{
    int32 cur = ReadIdx.load(std::memory_order_relaxed);
    if (cur == WriteIdx.load(std::memory_order_acquire))
        return false;

    FSlot& slot = Slots[cur];
    // 零拷贝：OutFrame 指针直接指向 slot 内部缓冲区
    OutFrame.Data            = slot.Data.data();
    OutFrame.DataSize        = slot.Data.size();
    OutFrame.TimestampSeconds = slot.TimestampSeconds;
    OutFrame.bValid          = slot.bValid;

    slot.bValid = false;
    ReadIdx.store((cur + 1) & Mask, std::memory_order_release);
    return true;
}

int32 FFrameQueue::GetBacklog() const
{
    int32 write = WriteIdx.load(std::memory_order_acquire);
    int32 read  = ReadIdx.load(std::memory_order_acquire);
    if (write >= read) return write - read;
    return Capacity - read + write;
}

// ============================================================================
// FAsyncEncodingPipeline
// ============================================================================

FAsyncEncodingPipeline::FAsyncEncodingPipeline()
    : QueuePtr(std::make_unique<FFrameQueue>(32))
    , bStopRequested(false), bRunning(false)
    , DroppedCount(0), EncodedCount(0)
{
}

FAsyncEncodingPipeline::~FAsyncEncodingPipeline() { Stop(); }

void FAsyncEncodingPipeline::Start(FEncodeCallback Callback, FFlushCallback FlushCb,
                                    int32 QueueSize, int32 FlushIntervalFrames,
                                    bool bContinueLastFrame, int32 FPSForLastFrame)
{
    if (bRunning.load(std::memory_order_acquire)) return;
    QueuePtr = std::make_unique<FFrameQueue>(QueueSize);
    bStopRequested.store(false, std::memory_order_release);
    bRunning.store(true, std::memory_order_release);
    DroppedCount.store(0, std::memory_order_release);
    EncodedCount.store(0, std::memory_order_release);
    LastSampledTime = -1.0;

    Thread = std::thread([this, cb = std::move(Callback), fc = std::move(FlushCb),
                          interval = FlushIntervalFrames, lf = bContinueLastFrame,
                          fps = FPSForLastFrame]() {
        EncodeLoop(cb, fc, interval, lf, fps);
    });
}

bool FAsyncEncodingPipeline::PushFrame(const uint8_t* RawData, size_t DataSize,
                                        double TimestampSeconds, int32 TargetFPS)
{
    if (!bRunning.load(std::memory_order_acquire)) return false;

    // 帧采样节流
    if (TargetFPS > 0 && LastSampledTime >= 0.0)
    {
        double minInterval = 1.0 / TargetFPS;
        if (TimestampSeconds - LastSampledTime < minInterval * 0.95)
            return true; // 跳过，不丢帧
    }
    LastSampledTime = TimestampSeconds;

    // 直接写入 slot（唯一一次数据拷贝）
    int32 dropped = QueuePtr->PushRaw(RawData, DataSize, TimestampSeconds);
    if (dropped > 0)
        DroppedCount.fetch_add(dropped, std::memory_order_relaxed);

    // 唤醒编码线程（有新帧可消费）
    CvWake.notify_one();

    return true;
}

void FAsyncEncodingPipeline::RequestStop(FFinalizeCallback FinalizeCb)
{
    if (!bRunning.load(std::memory_order_acquire)) return;
    bStopRequested.store(true, std::memory_order_release);
    bRunning.store(false, std::memory_order_release);

    std::thread finalizer([this, cb = std::move(FinalizeCb)]() {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(30))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (Thread.joinable()) Thread.join();
        if (cb) cb();
    });
    finalizer.detach();
}

void FAsyncEncodingPipeline::Stop()
{
    if (!bRunning.load(std::memory_order_acquire)) return;
    bStopRequested.store(true, std::memory_order_release);
    bRunning.store(false, std::memory_order_release);
    if (Thread.joinable()) Thread.join();
}

bool FAsyncEncodingPipeline::IsRunning() const { return bRunning.load(std::memory_order_acquire); }
int64_t FAsyncEncodingPipeline::GetDroppedFrames() const { return DroppedCount.load(std::memory_order_acquire); }
int64_t FAsyncEncodingPipeline::GetEncodedFrames() const { return EncodedCount.load(std::memory_order_acquire); }

// ============================================================================
// 编码线程主循环
// ============================================================================

void FAsyncEncodingPipeline::EncodeLoop(FEncodeCallback Callback, FFlushCallback FlushCb,
                                         int32 FlushIntervalFrames, bool bContinueLastFrame, int32 FPSForLastFrame)
{
    FFrameBuffer frame;
    FFrameBuffer lastFrame;
    bool  hasLastFrame = false;
    auto  lastFrameTime = std::chrono::steady_clock::now();
    int32 lastIntervalMs = (FPSForLastFrame > 0) ? (1000 / FPSForLastFrame) : 16;
    int64_t frameCount = 0;

    while (!bStopRequested.load(std::memory_order_acquire))
    {
        #ifdef VANE_DEBUG_TIMING
            auto t0 = std::chrono::steady_clock::now();
        #endif

        if (QueuePtr->TryPop(frame))
        {
            #ifdef VANE_DEBUG_TIMING
                auto t1 = std::chrono::steady_clock::now();
                auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                if (waitUs > 2000)
                    fprintf(stderr, "[DEBUG] PopWithWait: %lld us (backlog=%d)\n",
                            (long long)waitUs, QueuePtr->GetBacklog());
            #endif

            #ifdef VANE_DEBUG_TIMING
                auto cbStart = std::chrono::steady_clock::now();
            #endif

            Callback(frame);
            EncodedCount.fetch_add(1, std::memory_order_relaxed);

            #ifdef VANE_DEBUG_TIMING
                auto cbEnd = std::chrono::steady_clock::now();
                auto cbUs = std::chrono::duration_cast<std::chrono::microseconds>(cbEnd - cbStart).count();
                static int debugCount = 0;
                if (++debugCount % 30 == 0)
                    fprintf(stderr, "[DEBUG] Frame#%d encCallback: %lld us (backlog=%d)\n",
                            debugCount, (long long)cbUs, QueuePtr->GetBacklog());
            #endif

            lastFrame = frame;
            hasLastFrame = true;
            lastFrameTime = std::chrono::steady_clock::now();

            ++frameCount;
            if (FlushIntervalFrames > 0 && (frameCount % FlushIntervalFrames == 0) && FlushCb)
                FlushCb();
        }
        else if (hasLastFrame && bContinueLastFrame)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime).count();
            if (elapsed >= lastIntervalMs)
            {
                lastFrame.TimestampSeconds += 1.0 / FPSForLastFrame;
                Callback(lastFrame);
                EncodedCount.fetch_add(1, std::memory_order_relaxed);
                lastFrameTime = now;

                ++frameCount;
                if (FlushIntervalFrames > 0 && (frameCount % FlushIntervalFrames == 0) && FlushCb)
                    FlushCb();
            }
            else
            {
                // cv wait 等待新帧或超时（1ms），替代 yield 自旋
                std::unique_lock<std::mutex> lk(CvMutex);
                CvWake.wait_for(lk, std::chrono::milliseconds(1));
            }
        }
        else
        {
            // cv wait 等待新帧或超时（1ms），替代 yield 自旋
            std::unique_lock<std::mutex> lk(CvMutex);
            CvWake.wait_for(lk, std::chrono::milliseconds(1));
        }
    }

    // 排空剩余帧
    while (QueuePtr->TryPop(frame))
    {
        Callback(frame);
        EncodedCount.fetch_add(1, std::memory_order_relaxed);
        ++frameCount;
        if (FlushIntervalFrames > 0 && (frameCount % FlushIntervalFrames == 0) && FlushCb)
            FlushCb();
    }

    if (FlushCb) FlushCb();
}
