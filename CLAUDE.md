# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## AI Behavior Rules (Highest Priority)

These rules override all other instructions in this file. Violating any of them will produce unacceptable code.

**RULE 1: Think Before Coding**
- Stop and ask when requirements are ambiguous. Do NOT guess or make assumptions about business logic, API contracts, or user intent.
- If a task can be interpreted multiple ways, list the options and let the developer choose before implementing.
- When asked a question about the codebase, first explain your understanding, then ask clarifying questions before proposing solutions.
- NEVER silently correct what you think is a mistake — flag it and ask.

**RULE 2: Simplicity First**
- Write the minimum code needed to satisfy the exact requirement. No more.
- Do NOT add features, abstractions, configuration options, or extensibility points unless explicitly requested.
- Do NOT refactor "while you're at it" unless the task explicitly includes refactoring.
- If a single function will do, do NOT create a class. If a simple script will do, do NOT create a module hierarchy.
- Reuse existing patterns in the codebase — do NOT introduce new patterns just because they're "better practice."

**RULE 3: Surgical Changes**
- Only touch code directly related to the task. Do NOT reformat, fix style, reorganize imports, or rename things outside the change scope.
- Your diff should show ONLY the lines relevant to the change. A clean diff is more valuable than a "cleaned up" file.

**RULE 4: Goal-Driven Execution**
- First confirm the acceptance criteria (what must be true for the task to be "done").
- Work iteratively: implement → test/verify → fix → repeat until criteria are met.
- If you hit a blocker, report it immediately with specific details. Do NOT silently work around a problem.

**RULE 5: Explicit Reasoning**
- Before writing or modifying code, first briefly state your plan (bullet points are fine).
- After implementing, briefly verify against the acceptance criteria.

---

## Project Overview

Vane is a cross-platform video encoding abstraction library. It provides a unified C++17 interface over hardware encoders on macOS (VideoToolbox), Windows (NVENC / AMF / Media Foundation), and Linux (VA-API). A C ABI export layer enables integration with engines like Unreal Engine via dynamic linking. MIT license.

**Platform support:** macOS (VideoToolbox + H.264 fMP4) | Windows (NVENC → AMF → MF auto-select + H.264 MP4) | Linux (VA-API + H.264 fMP4)

## Build & Test

```bash
# Build (current platform only)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests
./build/test/VaneTest                         # Main test (4 scenarios: drop/stall/async-stop/throughput)
./build/test/Release/AllEncodersTest.exe      # Windows: all-encoder validation
./build/test/Release/Mp4WriterTest.exe        # Windows: MP4Writer unit test
./build/test/Release/D3D11Test.exe            # Windows: D3D11 device creation test

# Stress test with encoding delay
cmake -S . -B build -DVANE_TEST_ENCODING_DELAY_MS=80 && cmake --build build
```

| CMake option | Default | Description |
|---|---|---|
| `VANE_BUILD_SHARED` | ON | ON=dynamic library, OFF=static library |
| `VANE_TEST_ENCODING_DELAY_MS` | 0 | Inject artificial encoding delay (ms), 0=off |
| `VANE_DEBUG_TIMING` | OFF | Encoding pipeline timing diagnostic logs |
| `VANE_FAKE_VAAPI` | OFF | Linux: fake encoding mode without DRM (dev/test only) |

## Architecture

```
                         ┌──────────────────────┐
                         │   FEncoderConfig      │
                         │ (resolution/fps/bitrate..)│
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
    │  (VideoToolbox)│    │ (facade: runtime   │    │  (VA-API)      │
    └───────────────┘    │  detection)        │    └───────────────┘
                         └─────────┬─────────┘
                                   │
                    ┌──────────────┼──────────────┐
                    │              │              │
            ┌───────┴──────┐ ┌────┴─────┐ ┌──────┴──────┐
            │ FNvencEncoder │ │FAmfEncoder│ │FMFEncoderNew│
            │ (NVENC SDK,   │ │ (AMF SDK) │ │  (MF H.264  │
            │  LoadLibrary) │ │           │ │  + WMV fallback)│
            └───────────────┘ └───────────┘ └─────────────┘
                    │
            ┌───────┴───────┐
            │ D3D11Converter │
            │ (BGRA→NV12 GPU)│
            └───────────────┘

                     Cross-Platform Core
            ┌─────────────────────┐
            │  AsyncPipeline       │
            │  (SPSC lock-free ring buffer)│
            ├─────────────────────┤
            │  MP4Writer           │
            │  (Annex B → avc1)    │
            ├─────────────────────┤
            │  ColorSpaceConverter │
            │  (BGRA → NV12 CPU)   │
            └─────────────────────┘
```

## Source Tree

```
include/Vane/              # Public headers
  IVideoEncoder.h          # C++ abstract interface (+ VANE_API export macro)
  VaneConfig.h             # FEncoderConfig + FEncoderCapability
  VaneCallbacks.h          # C-style callback typedefs
  VaneAPI.h                # C ABI export interface + platform export macros

src/
  VaneAPI.cpp              # C ABI implementation (#if PLATFORM_XXX dispatch)
  core/                    # Cross-platform core (always compiled)
    AsyncPipeline.cpp/h    # Async encoding pipeline (SPSC lock-free queue)
    MP4Writer.cpp/h        # MP4 muxer (Annex B → avc1)
    ColorSpaceConverter.cpp/h  # Color space conversion (BGRA → NV12 CPU)
  mac/                     # macOS VideoToolbox
    FVTEncoder.h/.mm
  linux/                   # Linux VA-API
    FVAEncoder.h/.cpp
    MP4Muxer.cpp/h         # fMP4 muxer
  windows/                 # Windows multi-encoder
    FWindowsEncoder.cpp/h  # Facade (NVENC > AMF > MF)
    FNvencEncoder.cpp/h    # NVENC via LoadLibrary (zero SDK dependency at build)
    FAmfEncoder.cpp/h      # AMD AMF encoder
    FMFEncoderNew.cpp/h    # Media Foundation encoder (H.264 + WMV fallback)
    MFUtils.h              # MF utilities
    D3D11Converter.cpp/h   # GPU BGRA→NV12 conversion
  FMFEncoder.cpp/h         # Legacy MF encoder (preserved for reference, not compiled)

test/
  main.cpp                 # Cross-platform main test
  test_mp4writer.cpp       # MP4Writer unit test (Windows)
  test_d3d11.cpp           # D3D11 device creation test (Windows)
  test_all_encoders.cpp    # All-encoder validation (Windows)
```

## C ABI Interface (Primary Integration Point)

```c
#include "Vane/VaneAPI.h"

void* h = VaneEncoder_Create();
#ifdef _WIN32
VaneEncoder_SetD3D11Device(h, pD3D11Device);  // Optional: specify GPU
#endif
VaneEncoder_Initialize(h, &cfg);
VaneEncoder_StartRecording(h, "output.mp4");
VaneEncoder_EncodeFrame(h, bgra, size, timestamp);
VaneEncoder_RequestStop(h);  // Async, callback notifies completion
VaneEncoder_Destroy(h);
```

Callbacks (`StateCallback`, `ErrorCallback`, `ProgressCallback`, `FrameDropCallback`) fire from the **encoding thread**. UE integrators must forward them to GameThread before touching UI.

## Key Design Decisions

- **PIMPL**: All encoder class headers expose only the interface; implementation details live in `.cpp`/`.mm` (`Impl` struct)
- **Facade pattern (Windows)**: `FWindowsEncoder` probes hardware at runtime → selects best encoder (NVENC > AMF > MF), exposes unified `IVideoEncoder` interface
- **Dynamic loading (NVENC)**: `LoadLibrary("nvEncodeAPI64.dll")` at runtime, zero SDK dependency at build time; headers auto-downloaded by CMake
- **Real encoding session probing (NVENC)**: `IsH264Supported()` opens a temporary encoding session to verify, cached per D3D11 device pointer
- **MF reference counting**: `MFStartup`/`MFShutdown` managed via static refcount to prevent repeated init/shutdown within a process
- **Async pipeline**: SPSC lock-free ring buffer + `condition_variable` wakeup + `alignas(64)` to eliminate false sharing
- **VANE_API export macro**: Cross-platform `__declspec(dllexport/dllimport)` / `visibility("default")` ensures correct symbol export from DLL/dylib/so

## Platform Encapsulation

| Platform | Encoder | Muxer |
|---|---|---|
| macOS | VideoToolbox | AVAssetWriter writes fMP4 directly |
| Windows NVENC/AMF | NVENC/AMF SDK | Output Annex B → MP4Writer muxes MP4 |
| Windows MF H.264 | Media Foundation MFT | Output Annex B → MP4Writer muxes MP4 |
| Windows MF WMV | Media Foundation SinkWriter | Writes ASF directly |
| Linux | VA-API | Output Annex B → MP4Muxer muxes fMP4 |

## Platform Macros

CMake sets `PLATFORM_MAC`, `PLATFORM_WINDOWS`, `PLATFORM_LINUX` via `target_compile_definitions` in `src/CMakeLists.txt`. Code uses `#if PLATFORM_XXX` to isolate platform-specific paths.

**Link libraries:**
- macOS: VideoToolbox, CoreMedia, CoreVideo, AVFoundation, Foundation
- Windows: mfplat, mfreadwrite, mfuuid, ole32, d3d11
- Linux: libva, libva-drm (via pkg-config)

## Naming Conventions

- Classes: `F` prefix + PascalCase (`FVTEncoder`, `FWindowsEncoder`)
- Interfaces: `I` prefix (`IVideoEncoder`)
- Functions: PascalCase (`EncodeFrame()`)
- bool members: `b` prefix (`bIsRecording`)
- Constants: `k` prefix (`kDefaultBitRate`)

## Callback Implementation Status

- **macOS (FVTEncoder)**: All implemented (State / Error / Progress / FrameDrop)
- **Windows (FWindowsEncoder)**: All implemented (forwarded through facade to internal encoder callbacks)
- **Linux (FVAEncoder)**: Callbacks are TODO (`FVAEncoder.cpp:511`), currently inheriting empty default implementations

## Constraints

- Do NOT introduce FFmpeg or other third-party video libraries
- Do NOT implement network streaming functionality
- Do NOT add GUI interfaces
- Do NOT change the C++17 standard
- When modifying one platform's implementation, do NOT casually "optimize" other platforms
- When modifying the interface, MUST synchronize updates across all platform implementations
