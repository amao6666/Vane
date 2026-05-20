# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

Vane 是一个跨平台视频编码抽象库，为 macOS (VideoToolbox)、Windows (Media Foundation / NVENC / AMF)、Linux (VA-API) 提供统一的 C++17 接口，附带 C ABI 导出层供 UE 等引擎集成。MIT 许可证。

## 构建与测试

```bash
# 构建（仅当前平台）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 带编码延迟的压力测试
cmake -S . -B build -DVANE_TEST_ENCODING_DELAY_MS=80 && cmake --build build

# 运行主测试（四场景：丢帧/卡顿/异步Stop/吞吐量）
./build/test/VaneTest

# Windows 上还有两个独立测试
./build/test/Mp4WriterTest
./build/test/D3D11Test
```

| CMake 选项 | 默认值 | 说明 |
|---|---|---|
| `VANE_BUILD_SHARED` | ON | ON=动态库, OFF=静态库 |
| `VANE_TEST_ENCODING_DELAY_MS` | 0 | 注入人工编码延迟(ms)，0=关闭 |
| `VANE_DEBUG_TIMING` | OFF | 编码管线计时诊断日志 |

## 架构

```
include/Vane/
  IVideoEncoder.h      # C++ 抽象接口（所有平台编码器实现此接口）
  VaneConfig.h         # FEncoderConfig 配置 + FEncoderCapability 能力检测
  VaneCallbacks.h      # C 风格回调类型定义（状态/错误/进度/丢帧）
  VaneAPI.h            # C ABI 导出接口（跨 DLL / FFI）

src/
  VaneAPI.cpp          # C ABI 实现（门面模式，通过 #if PLATFORM_XXX 创建对应编码器）
  AsyncPipeline.cpp    # SPSC 无锁环形队列 + 异步编码管线（所有平台共用）

  core/                # 跨平台核心组件（始终编译）
    MP4Writer.cpp/h    # 普通 MP4 封装器（Annex B → avc1 + moov），Windows NVENC/AMF 路径使用
    ColorSpaceConverter.cpp/h  # BGRA → NV12 色彩空间转换

  FVTEncoder.h/.mm     # macOS VideoToolbox（AVAssetWriter 直接写 fMP4）
  FVAEncoder.h/.cpp    # Linux VA-API（输出裸流 + MP4Muxer 封装 fMP4）
  FMFEncoder.h/.cpp    # 旧版 Windows MF 编码器（不再编译，仅参考）

  windows/             # Windows 新架构（当前编译）
    FWindowsEncoder.cpp/h  # 门面类：运行时探测硬件 → NVENC > AMF > MF
    FNvencEncoder.cpp/h    # NVENC 原生编码器（P0 首选，LoadLibrary 动态加载）
    FAmfEncoder.cpp/h      # AMD AMF 编码器（P1 备选）
    FMFEncoderNew.cpp/h    # Media Foundation 编码器（P2 备选，含 WMV 兜底）
    MFUtils.h              # MF 公共工具（SafeRelease, 宽字符转换, 编码器枚举）

test/
  main.cpp             # 跨平台测试（#if PLATFORM_XXX 选择编码器类）
  test_mp4writer.cpp   # MP4Writer 单元测试
  test_d3d11.cpp       # D3D11 设备创建最小测试（仅 Windows）
```

### 关键设计决策

- **PIMPL 模式**：所有编码器类头文件只暴露接口，实现细节在 .cpp/.mm 中（`Impl` 结构体）
- **门面模式 (Windows)**：`FWindowsEncoder` 运行时选择最佳编码器（NVENC > AMF > MF），对外统一 `IVideoEncoder` 接口
- **动态加载 (NVENC)**：通过 `LoadLibrary("nvEncodeAPI64.dll")` 加载，编译期零 SDK 依赖
- **MF 引用计数**：`MFStartup`/`MFShutdown` 通过静态引用计数管理，防止进程内多次创建/销毁导致重复初始化
- **异步管线**：`FAsyncEncodingPipeline` + `FFrameQueue` 提供 SPSC 无锁环形队列，`condition_variable` 唤醒编码线程，`alignas(64)` 消除伪共享

### 各平台输出封装方式

| 平台 | 编码器 | 封装方式 |
|---|---|---|
| macOS | VideoToolbox | AVAssetWriter 直接写 fMP4 |
| Windows NVENC/AMF | NVENC/AMF SDK | 输出 Annex B 裸流 → `MP4Writer` 封装 MP4 |
| Windows MF | Media Foundation | MFSinkWriter 直接写 MP4 |
| Linux | VA-API | 输出 Annex B 裸流 → `MP4Muxer` 封装 fMP4 |

### 回调实现状态

- **macOS (FVTEncoder)**：全部实现（State / Error / Progress / FrameDrop）
- **Windows (FWindowsEncoder)**：全部实现（通过门面转发到内部编码器的回调）
- **Linux (FVAEncoder)**：回调为 TODO（`FVAEncoder.cpp:511`），当前继承空默认实现

## 平台隔离

CMake 定义的预处理器宏（`src/CMakeLists.txt` 通过 `target_compile_definitions` 设置）：

```cpp
#if PLATFORM_MAC       // VideoToolbox
#elif PLATFORM_WINDOWS  // Media Foundation / NVENC / AMF
#elif PLATFORM_LINUX   // VA-API
#endif
```

各平台链接库：
- **macOS**: VideoToolbox, CoreMedia, CoreVideo, AVFoundation, Foundation
- **Windows**: mfplat, mfreadwrite, mfuuid, ole32, d3d11
- **Linux**: libva, libva-drm（pkg-config 查找）

## 命名约定

- 类名：`F` 前缀 + PascalCase（`FVTEncoder`, `FWindowsEncoder`）
- 接口：`I` 前缀（`IVideoEncoder`）
- 函数：PascalCase（`EncodeFrame()`）
- bool 成员：`b` 前缀（`bIsRecording`）
- 常量：`k` 前缀（`kDefaultBitRate`）

## 行为规则

1. **先想后问**：对平台 API 不确定时查阅文档或提出疑问，不要凭猜测写代码
2. **极简优先**：不添加未要求的功能，不过度抽象，用最少代码解决问题
3. **精准修改**：修改某平台实现时不顺手"优化"其他平台；修改接口时必须同步更新所有平台实现
4. **目标驱动**：代码能编译通过，测试程序能生成有效视频文件

## 禁止事项

- 不引入 FFmpeg 或其他第三方视频库
- 不实现网络流媒体功能
- 不添加 GUI 界面
- 不修改 C++17 标准
