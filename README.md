# Vane

跨平台视频编码抽象库，为各平台硬件编码器提供统一的 C++17 接口，附带 C ABI 导出层用于 UE 等引擎集成。MIT 许可证。

## 平台支持

| 平台 | 底层 API | 输出格式 | 状态 |
|------|---------|----------|------|
| macOS | VideoToolbox | H.264 + fMP4 (AVAssetWriter) | ✅ |
| Windows | NVENC → AMF → MF | H.264 + MP4 | ✅ |
| Linux | VA-API | H.264 + fMP4 (内置复用器) | ✅ |

Windows 采用运行时硬件探测，按优先级自动选择：NVIDIA NVENC > AMD AMF > Media Foundation。MF 路径还提供 WMV 格式兜底。

## 架构

```
                         ┌──────────────────────┐
                         │   FEncoderConfig      │
                         │ (分辨率/帧率/码率/..)  │
                         └──────────┬───────────┘
                                    │
                    ┌───────────────┴───────────────┐
                    │      VaneAPI.cpp (C ABI)       │
                    │  Create / Initialize / Encode  │
                    └───────────────┬───────────────┘
                                    │
            ┌───────────────────────┼───────────────────────┐
            │                       │                       │
    ┌───────┴───────┐    ┌─────────┴─────────┐    ┌───────┴───────┐
    │  FVTEncoder    │    │ FWindowsEncoder   │    │  FVAEncoder    │
    │  (VideoToolbox)│    │ (门面: 运行时探测) │    │  (VA-API)      │
    └───────────────┘    └─────────┬─────────┘    └───────────────┘
                                   │
                    ┌──────────────┼──────────────┐
                    │              │              │
            ┌───────┴──────┐ ┌────┴─────┐ ┌──────┴──────┐
            │ FNvencEncoder │ │FAmfEncoder│ │FMFEncoderNew│
            │ (NVENC SDK)   │ │ (AMF SDK) │ │  (MF H.264  │
            │ LoadLibrary   │ │           │ │  + WMV 兜底)│
            └───────────────┘ └───────────┘ └─────────────┘
                    │
            ┌───────┴───────┐
            │ D3D11Converter │
            │ (BGRA→NV12 GPU)│
            └───────────────┘

                    跨平台核心
            ┌─────────────────────┐
            │  AsyncPipeline       │
            │  (SPSC 无锁环形队列)  │
            ├─────────────────────┤
            │  MP4Writer           │
            │  (Annex B → avc1)    │
            ├─────────────────────┤
            │  ColorSpaceConverter │
            │  (BGRA → NV12 CPU)   │
            └─────────────────────┘
```

## 源码结构

```
include/Vane/          # 公开头文件
  IVideoEncoder.h      # C++ 抽象接口
  VaneConfig.h         # 配置 + 能力检测
  VaneCallbacks.h      # C 风格回调类型
  VaneAPI.h            # C ABI 导出接口

src/
  VaneAPI.cpp          # C ABI 实现（#if PLATFORM_XXX 创建编码器）
  AsyncPipeline.cpp    # 异步管线（所有平台共用）
  core/                # 跨平台核心
    MP4Writer.cpp/h    # MP4 封装器
    ColorSpaceConverter.cpp/h  # 色彩空间转换
    D3D11Converter.cpp/h       # GPU BGRA→NV12

  FVTEncoder.h/.mm     # macOS VideoToolbox
  FVAEncoder.h/.cpp    # Linux VA-API
  windows/             # Windows 多编码器
    FWindowsEncoder.cpp/h    # 门面（NVENC > AMF > MF）
    FNvencEncoder.cpp/h      # NVENC 原生编码器
    FAmfEncoder.cpp/h        # AMD AMF 编码器
    FMFEncoderNew.cpp/h      # Media Foundation 编码器
    MFUtils.h                # MF 公共工具

test/
  main.cpp             # 主测试（四场景：丢帧/卡顿/异步Stop/吞吐量）
  test_mp4writer.cpp   # MP4Writer 单元测试
  test_d3d11.cpp       # D3D11 设备测试
  test_all_encoders.cpp # 全编码器遍历验证
```

## 构建与测试

```bash
# 构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 运行主测试
./build/test/VaneTest

# Windows 上的专项测试
./build/test/Release/AllEncodersTest.exe  # 全编码器验证
./build/test/Release/Mp4WriterTest.exe    # MP4Writer 单元测试
./build/test/Release/D3D11Test.exe        # D3D11 设备测试

# 带编码延迟的压力测试
cmake -S . -B build -DVANE_TEST_ENCODING_DELAY_MS=80 && cmake --build build
```

| CMake 选项 | 默认值 | 说明 |
|---|---|---|
| `VANE_BUILD_SHARED` | ON | ON=动态库, OFF=静态库 |
| `VANE_TEST_ENCODING_DELAY_MS` | 0 | 注入人工编码延迟(ms)，0=关闭 |
| `VANE_DEBUG_TIMING` | OFF | 编码管线计时诊断日志 |

## C ABI 接口

```c
#include "Vane/VaneAPI.h"

void* h = VaneEncoder_Create();
#ifdef _WIN32
VaneEncoder_SetD3D11Device(h, pD3D11Device);  // 可选：指定 GPU
#endif
VaneEncoder_Initialize(h, &cfg);
VaneEncoder_StartRecording(h, "output.mp4");
VaneEncoder_EncodeFrame(h, bgra, size, timestamp);
VaneEncoder_RequestStop(h);  // 异步，回调通知完成
VaneEncoder_Destroy(h);
```

## 回调系统

| 回调 | 线程 | 频率 | 用途 |
|------|------|------|------|
| `StateCallback` | 编码线程 | 状态变化时 | Idle→Starting→Recording→Stopping→Idle |
| `ErrorCallback` | 编码线程 | 出错时 | 错误/警告级别 + 描述 |
| `ProgressCallback` | 编码线程 | 1 Hz | 已编码帧数/字节数/时间戳 |
| `FrameDropCallback` | 游戏线程 | 丢帧时 | 累计丢帧数 |

## 关键设计决策

- **PIMPL**：所有编码器类头文件只暴露接口，实现细节在 .cpp 中
- **动态加载 (NVENC)**：通过 `LoadLibrary("nvEncodeAPI64.dll")` 加载，编译期零 SDK 依赖
- **门面模式 (Windows)**：`FWindowsEncoder` 运行时探测硬件 → 选择最佳编码器
- **真实编码会话探测 (NVENC)**：`IsH264Supported()` 打开临时编码会话验证，按 D3D11 设备指针缓存
- **MF 引用计数**：`MFStartup`/`MFShutdown` 通过静态引用计数管理
- **异步管线**：SPSC 无锁环形队列 + `condition_variable` 唤醒 + `alignas(64)` 消除伪共享

## 各平台封装方式

| 平台 | 编码器 | 封装 |
|---|---|---|
| macOS | VideoToolbox | AVAssetWriter 直接写 fMP4 |
| Windows NVENC/AMF | NVENC/AMF SDK | 输出 Annex B → MP4Writer 封装 |
| Windows MF H.264 | Media Foundation MFT | 输出 Annex B → MP4Writer 封装 |
| Windows MF WMV | Media Foundation SinkWriter | 直接写 ASF 文件 |
| Linux | VA-API | 输出 Annex B → MP4Muxer 封装 fMP4 |

## 许可证

MIT License
