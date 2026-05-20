#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// MF 头文件包含顺序必须严格
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>
#include <string>
#include <vector>

using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;

// MF 编码器候选（枚举与排序用）
struct FEncoderCandidate
{
    IMFActivate* pActivate = nullptr;
    WCHAR        FriendlyName[256] = {};
    GUID         CodecSubtype = {};
    bool         bHardware = false;
    bool         bDiscreteGPU = false;
    int32        Priority = 50;
};

// 宽字符转换工具
wchar_t* ToWideString(const char* Utf8);
char*    ToUtf8String(const wchar_t* Wide);

// ComPtr 辅助
template<typename T>
void SafeRelease(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

#endif // _WIN32
