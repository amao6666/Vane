# Vane

跨平台视频编码抽象库，为各平台硬件编码器提供统一的 C++17 接口，附带 C ABI 导出层用于 UE 等引擎集成。

## 平台支持

| 平台 | 底层 API | 输出格式 | 状态 |
|------|---------|----------|------|
| macOS | VideoToolbox | H.264 + MP4 (fMP4) | ✅ 已实现 |
| Windows | Media Foundation | H.264 + MP4 | ✅ 已实现 |
| Linux | VA-API | H.264 + MP4 (内置复用器) | ✅ 已实现 |

## 架构设计

```
┌─────────────────────────────────────────────────┐
│                  公开接口层                       │
│  C++: IVideoEncoder    C ABI: VaneAPI.h          │
│  Initialize / StartRecording / EncodeFrame       │
│  StopRecording / RequestStop / GetLastError      │
│  SetStateCallback / SetErrorCallback / ...       │
├─────────────────────────────────────────────────┤
│                  平台实现层                       │
│  FVTEncoder.mm (VideoToolbox)                    │
│  FMFEncoder.cpp (Media Foundation)               │
│  FVAEncoder.cpp (VA-API + MP4Muxer)              │
├─────────────────────────────────────────────────┤
│                 异步管线层                        │
│  FAsyncEncodingPipeline + FFrameQueue (SPSC)     │
│  帧采样节流 / 丢旧保新 / LastFrame续命            │
│  cv唤醒 / atomic无锁 / 周期刷盘                   │
└─────────────────────────────────────────────────┘
```

### 帧流转路径

```
游戏线程                         编码线程                       系统 API
───────                         ────────                       ────────
EncodeFrame()                   EncodeLoop()
  │                               │
  ├─ PushFrameRaw() ──memcpy──→  FFrameQueue (ring buffer)
  │  (0.5ms)                      │
  └─ notify_one() ──────────────→ cv.wake ── TryPop (零拷贝指针)
                                  │
                                  ├─ CVPixelBufferCreate (10μs)
                                  ├─ memcpy → pixelBuf (0.5ms)
                                  ├─ VTCompressionSessionEncodeFrame (0.2ms)
                                  ├─ CompressionOutputCallback
                                  │   └─ AVAssetWriter appendSampleBuffer
                                  ├─ ProgressCallback (每秒)
                                  └─ FlushCallback (每60帧)
```

### 回调系统

| 回调 | 触发线程 | 频率 | 用途 |
|------|---------|------|------|
| `StateCallback` | 调用线程 / 编码线程 | 状态变化时 | 录制生命周期通知 (Idle→Recording→Stopping→Idle) |
| `ErrorCallback` | 编码线程 | 出错时 | 错误级别 + 描述信息 |
| `ProgressCallback` | 编码线程 | 1 Hz | 已编码帧数 / 字节数 / 时间戳 |
| `FrameDropCallback` | 游戏线程 | 丢帧时 | 累计丢帧数 |

### 线程模型

| 组件 | 线程 | 阻塞 | 关键操作 |
|------|------|------|---------|
| `EncodeFrame` | 游戏线程 | 否 | memcpy + atomic push + notify |
| `RequestStop` | 调用线程 | 否 | 设置停止信号，立即返回 |
| `StopRecording` | 调用线程 | 是 | 等待编码线程排空后同步收尾 |
| 编码回调 | 编码线程 | — | CVPixelBuffer + VT encode + AVAssetWriter append |
| 状态回调 | 编码/调用线程 | — | UE 集成需 `AsyncTask(GameThread)` 转发 |
| 进度回调 | 编码线程 | — | UE 集成需 `AsyncTask(GameThread)` 转发 |
| 丢帧回调 | 游戏线程 | — | 可直接操作 UE 对象 |

## API

### C++ 接口 (IVideoEncoder)

```cpp
#include "Vane/IVideoEncoder.h"
#include "Vane/VaneConfig.h"

FEncoderConfig cfg;
cfg.Width = 1920; cfg.Height = 1080; cfg.FrameRate = 60;
cfg.BitRate = 10'000'000;

FVTEncoder encoder;
encoder.Initialize(cfg);
encoder.SetStateCallback(OnState, &userData);
encoder.StartRecording("output.mp4");

// 游戏线程，每帧调用
encoder.EncodeFrame(bgraData, dataSize, timestampSeconds);

encoder.RequestStop();  // 异步，不阻塞
```

### C ABI (跨 DLL / FFI)

```c
#include "Vane/VaneAPI.h"

void* h = VaneEncoder_Create();
VaneEncoder_Initialize(h, &config);
VaneEncoder_StartRecording(h, "output.mp4");
VaneEncoder_EncodeFrame(h, bgra, size, timestamp);
VaneEncoder_RequestStop(h);
VaneEncoder_Destroy(h);
```

### 配置结构体

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `Width` / `Height` | 1920 / 1080 | 输出分辨率 |
| `FrameRate` | 60 | 编码帧率 |
| `BitRate` | 10'000'000 | 目标码率 (bps) |
| `KeyframeInterval` | 120 | I 帧间隔 |
| `bRealTime` | true | 低延迟模式 |
| `RecordFrameRate` | 60 | 录制目标帧率（0=不节流） |
| `FrameQueueSize` | 32 | 异步队列槽位数 |
| `FlushIntervalFrames` | 60 | fMP4 分片间隔 |
| `bContinueLastFrame` | false | 队列空时用最后一帧续命 |

## 性能指标

> 测试环境：macOS, Apple Silicon M1, Release 编译, 1080p BGRA → H.264 60fps

| 指标 | 数值 |
|------|------|
| 单帧编码回调耗时 | **250–300 μs** |
| 主线程 EncodeFrame 耗时 | ~0.5 ms (1×memcpy 8MB) |
| 编码线程唤醒延迟 | <100 μs (condition_variable) |
| 600 帧连续录制 | 零丢帧，吞吐量 60 fps |
| `RequestStop` 返回时间 | <0.1 ms |
| 队列最大深度 | 32（可配置） |

## 已实现的优化

| 优化项 | 说明 |
|--------|------|
| 无锁环形队列 | SPSC，双 `std::atomic` 索引，`condition_variable` 即时唤醒 |
| 真实时间戳 | `double TimestampSeconds` 取代帧序号推导，卡顿后视频时间连续 |
| 帧采样节流 | 录制帧率与输入帧率解耦，避免无效帧入队 |
| 异步 RequestStop | 不阻塞调用线程，收尾完成通过 StateCallback(Idle) 通知 |
| LastFrame 续命 | 主线程卡顿时编码线程用最后一帧填充，视频画面定格不黑屏 |
| fMP4 分片写入 | `movieFragmentInterval=1s`，崩溃后已写入部分可播 |
| 丢旧保新 | 队列满时丢弃最旧帧，保证最新画面优先和视频时间连续 |
| 零拷贝出队 | TryPop 返回 slot 内部指针，消除一次 8MB memcpy |
| CAVLC + B 帧关闭 | `AllowFrameReordering=false`，保证输出顺序与输入一致 |
| 最终进度刷新 | 停止时强制发送最后一次 ProgressCallback |

## 待完成

| 项目 | 说明 |
|------|------|
| Windows 低延迟参数对齐 | `MF_LOW_LATENCY` + `RequestStop` 异步化 |
| Linux 低延迟参数对齐 | VA-API `RequestStop` + 编码参数调整 |
| 三平台统一压力测试 | 120fps 输入 / 长时间录制 / OOM 边界 |
| UE 插件层 | `Build.cs` + 游戏线程回调转发封装 |
| IOSurface 零拷贝 (v2) | 消除 memcpy 链，直接包装渲染纹理 |

## 构建与测试

### 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

| 选项 | 默认 | 说明 |
|------|------|------|
| `VANE_BUILD_SHARED` | ON | ON=动态库, OFF=静态库 |
| `VANE_TEST_ENCODING_DELAY_MS` | 0 | 测试用人工编码延迟 (ms)，0=关闭 |
| `VANE_DEBUG_TIMING` | OFF | 编码管线计时诊断日志 |

### 测试

```bash
# 基础测试（四场景：丢帧 / 卡顿 / 异步Stop / 吞吐量）
./build/test/VaneTest

# 带编码延迟的压力测试
cmake -S . -B build -DVANE_TEST_ENCODING_DELAY_MS=80 && cmake --build build && ./build/test/VaneTest
```

### UE 集成

```
MyPlugin/ThirdParty/Vane/
├── include/Vane/VaneAPI.h
├── lib/Win64/Vane.dll + Vane.lib
├── lib/Mac/libVane.dylib
└── lib/Linux/libVane.so
```

```csharp
// MyPlugin.Build.cs
if (Target.Platform == UnrealTargetPlatform.Win64) {
    PublicAdditionalLibraries.Add(Path.Combine(ModuleDir, "Vane/lib/Win64/Vane.lib"));
    RuntimeDependencies.Add("$(BinaryOutputDir)/Vane.dll", Path.Combine(ModuleDir, "Vane/lib/Win64/Vane.dll"));
}
```

## 许可证

MIT License
