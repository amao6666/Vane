#include "Vane/VaneAPI.h"

#if defined(PLATFORM_MAC)
    #include "FVTEncoder.h"
#elif defined(PLATFORM_WINDOWS)
    #include "FMFEncoder.h"
#elif defined(PLATFORM_LINUX)
    #include "FVAEncoder.h"
#endif

extern "C" {

VANE_API void* VaneEncoder_Create(void)
{
#if defined(PLATFORM_MAC)
    return new FVTEncoder();
#elif defined(PLATFORM_WINDOWS)
    return new FMFEncoder();
#elif defined(PLATFORM_LINUX)
    return new FVAEncoder();
#else
    return nullptr;
#endif
}

VANE_API void VaneEncoder_Destroy(void* Encoder)
{
    delete static_cast<IVideoEncoder*>(Encoder);
}

VANE_API int VaneEncoder_Initialize(void* Encoder, const FEncoderConfig* Config)
{
    auto* enc = static_cast<IVideoEncoder*>(Encoder);
    return enc && Config && enc->Initialize(*Config) ? 1 : 0;
}

VANE_API int VaneEncoder_StartRecording(void* Encoder, const char* OutputPath)
{
    auto* enc = static_cast<IVideoEncoder*>(Encoder);
    return enc && enc->StartRecording(OutputPath) ? 1 : 0;
}

VANE_API int VaneEncoder_EncodeFrame(void* Encoder, const uint8_t* RawBGRA, int DataSize, double TimestampSeconds)
{
    auto* enc = static_cast<IVideoEncoder*>(Encoder);
    return enc && enc->EncodeFrame(RawBGRA, DataSize, TimestampSeconds) ? 1 : 0;
}

VANE_API void VaneEncoder_StopRecording(void* Encoder)
{
    auto* enc = static_cast<IVideoEncoder*>(Encoder);
    if (enc) enc->StopRecording();
}

VANE_API void VaneEncoder_RequestStop(void* Encoder)
{
    auto* enc = static_cast<IVideoEncoder*>(Encoder);
    if (enc) enc->RequestStop();
}

VANE_API int VaneEncoder_IsRecording(void* Encoder)
{
    auto* enc = static_cast<IVideoEncoder*>(Encoder);
    return enc && enc->IsRecording() ? 1 : 0;
}

VANE_API const char* VaneEncoder_GetLastError(void* Encoder)
{
    auto* enc = static_cast<IVideoEncoder*>(Encoder);
    return enc ? enc->GetLastError() : "Encoder handle is null";
}

VANE_API void VaneEncoder_SetStateCallback(void* Encoder, VaneStateCallback Cb, void* UserData)
{
    auto* enc = static_cast<IVideoEncoder*>(Encoder);
    if (enc) enc->SetStateCallback(Cb, UserData);
}

VANE_API void VaneEncoder_SetErrorCallback(void* Encoder, VaneErrorCallback Cb, void* UserData)
{
    auto* enc = static_cast<IVideoEncoder*>(Encoder);
    if (enc) enc->SetErrorCallback(Cb, UserData);
}

VANE_API void VaneEncoder_SetProgressCallback(void* Encoder, VaneProgressCallback Cb, void* UserData)
{
    auto* enc = static_cast<IVideoEncoder*>(Encoder);
    if (enc) enc->SetProgressCallback(Cb, UserData);
}

VANE_API void VaneEncoder_SetFrameDropCallback(void* Encoder, VaneFrameDropCallback Cb, void* UserData)
{
    auto* enc = static_cast<IVideoEncoder*>(Encoder);
    if (enc) enc->SetFrameDropCallback(Cb, UserData);
}

} // extern "C"
