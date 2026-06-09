#include "FVAEncoder.h"
#include "core/AsyncPipeline.h"
#include "MP4Muxer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>

// ============================================================================
// BGRA -> NV12 颜色转换（BT.601 标准）
// NV12 布局：全分辨率 Y 平面 + 半分辨率交错 UV 平面
// ============================================================================
static void ConvertBGRAToNV12(const uint8* bgra, int32 width, int32 height,
                               uint8* yPlane, uint8* uvPlane,
                               int32 yStride, int32 uvStride)
{
    for (int32 y = 0; y < height; ++y)
    {
        for (int32 x = 0; x < width; ++x)
        {
            const int32 srcIdx = (y * width + x) * 4;
            int32 B = bgra[srcIdx];
            int32 G = bgra[srcIdx + 1];
            int32 R = bgra[srcIdx + 2];

            // BT.601 全范围
            int32 Y  = (( 66 * R + 129 * G +  25 * B + 128) >> 8) + 16;
            int32 U  = ((-38 * R -  74 * G + 112 * B + 128) >> 8) + 128;
            int32 V  = ((112 * R -  94 * G -  18 * B + 128) >> 8) + 128;

            Y  = (Y  < 0) ? 0 : ((Y  > 255) ? 255 : Y );
            U  = (U  < 0) ? 0 : ((U  > 255) ? 255 : U );
            V  = (V  < 0) ? 0 : ((V  > 255) ? 255 : V );

            yPlane[y * yStride + x] = static_cast<uint8>(Y);

            // UV 半分辨率采样（每 2x2 块共用一个 UV）
            if ((x & 1) == 0 && (y & 1) == 0)
            {
                int32 uvX = x / 2;
                int32 uvY = y / 2;
                uvPlane[uvY * uvStride + uvX * 2]     = static_cast<uint8>(U);
                uvPlane[uvY * uvStride + uvX * 2 + 1] = static_cast<uint8>(V);
            }
        }
    }
}

// ============================================================================
// 平台实现结构体
// ============================================================================
struct FVAEncoderImpl
{
    int32 Width        = 0;
    int32 Height       = 0;
    int32 FrameRate    = 60;
    int32 RecordFrameRate = 60;
    int32 BitRate      = 10000000;
    int32 KeyInterval  = 120;
    int32 QueueSize    = 32;
    int32 FlushInterval = 60;
    bool  bContinueLastFrame = false;
    int32 LastDroppedCount   = 0;

    // VA-API 对象
    int          DrmFd       = -1;
    VADisplay    VaDisplay   = nullptr;
    VAConfigID   ConfigId    = VA_INVALID_ID;
    VAContextID  ContextId   = VA_INVALID_ID;

    // Surface 池（NV12 格式，用于硬件编码输入）
    static constexpr int32 SurfaceCount = 4;
    VASurfaceID Surfaces[SurfaceCount];

    // 异步管线（编码线程执行 VA-API 操作）
    FAsyncEncodingPipeline Pipeline;

    // 输出（编码线程持有）
    MP4Muxer  Muxer;
    std::string OutputPath;

    // 编码线程帧计数器（取代主线程 FrameCount，用于 surface 轮转）
    std::atomic<int64_t> EncodedCount{0};

    // 回调
    VaneStateCallback    StateCb    = nullptr;
    VaneErrorCallback    ErrorCb    = nullptr;
    VaneProgressCallback ProgressCb = nullptr;
    VaneFrameDropCallback FrameDropCb = nullptr;
    void* StateUserData    = nullptr;
    void* ErrorUserData    = nullptr;
    void* ProgressUserData = nullptr;
    void* FrameDropUserData = nullptr;

    // 进度统计（编码线程更新，单线程无需原子）
    int64_t TotalFramesEncoded = 0;
    std::chrono::steady_clock::time_point LastProgressTime;
    double  LastProgressTimestamp = 0.0;

    bool bInitialized = false;
    bool bRecording   = false;
    bool bHasError    = false;
    bool bFakeMode    = false;  // DRM 不可用时启用模拟编码（开发/测试）

    std::string LastError;
};

// ============================================================================
// FVAEncoder 公开接口实现
// ============================================================================

FVAEncoder::FVAEncoder()
    : Impl(new FVAEncoderImpl())
{
}

FVAEncoder::~FVAEncoder()
{
    StopRecording();

    if (!Impl->bFakeMode)
    {
        if (Impl->ContextId != VA_INVALID_ID)
            vaDestroyContext(Impl->VaDisplay, Impl->ContextId);
        if (Impl->ConfigId != VA_INVALID_ID)
            vaDestroyConfig(Impl->VaDisplay, Impl->ConfigId);
        if (Impl->VaDisplay)
            vaTerminate(Impl->VaDisplay);
        if (Impl->DrmFd >= 0)
            close(Impl->DrmFd);
    }

    delete Impl;
}

bool FVAEncoder::Initialize(const FEncoderConfig& Config)
{
    if (Impl->bInitialized) return true;
    if (Config.Width <= 0 || Config.Height <= 0)
    {
        Impl->LastError = "分辨率无效：宽度和高度必须大于 0";
        return false;
    }

    // 检查 Codec
    if (Config.Codec && strcmp(Config.Codec, "h264") != 0)
    {
        Impl->LastError = "不支持的编码格式：当前仅支持 h264";
        return false;
    }

    Impl->Width        = Config.Width;
    Impl->Height       = Config.Height;
    Impl->FrameRate    = Config.FrameRate;
    Impl->BitRate      = Config.BitRate;
    Impl->KeyInterval  = Config.KeyframeInterval;
    Impl->FlushInterval = Config.FlushIntervalFrames;
    Impl->RecordFrameRate = Config.RecordFrameRate > 0 ? Config.RecordFrameRate : Config.FrameRate;
    Impl->QueueSize    = Config.FrameQueueSize;
    Impl->bContinueLastFrame = Config.bContinueLastFrame;

    // ---- 打开 DRM 设备 ----
    Impl->DrmFd = open("/dev/dri/renderD128", O_RDWR);
    if (Impl->DrmFd < 0)
    {
        // 尝试备选设备
        Impl->DrmFd = open("/dev/dri/renderD129", O_RDWR);
    }
    if (Impl->DrmFd < 0)
    {
        Impl->DrmFd = open("/dev/dri/card0", O_RDWR);
    }
    if (Impl->DrmFd < 0)
    {
#ifdef VANE_FAKE_VAAPI
        // DRM 不可用，进入模拟编码模式（仅用于开发/测试管线逻辑）
        Impl->bFakeMode    = true;
        Impl->bInitialized = true;
        return true;
#else
        Impl->LastError = "无法打开 DRM 设备 /dev/dri/renderD128（请确认 GPU 驱动已安装或安装 mesa-va-drivers 启用软件编码）";
        return false;
#endif
    }

    // ---- 初始化 VA-API ----
    Impl->VaDisplay = vaGetDisplayDRM(Impl->DrmFd);
    if (!Impl->VaDisplay)
    {
        Impl->LastError = "vaGetDisplayDRM 失败";
        return false;
    }

    int majorVer = 0, minorVer = 0;
    VAStatus vaErr = vaInitialize(Impl->VaDisplay, &majorVer, &minorVer);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        Impl->LastError = "vaInitialize 失败（VAStatus=" + std::to_string(vaErr) + ")";
        return false;
    }

    // ---- 创建编码配置（H.264 High Profile，含 packed headers）----
    VAConfigAttrib attribs[2];
    attribs[0].type = VAConfigAttribRateControl;
    attribs[1].type = VAConfigAttribEncPackedHeaders;
    vaGetConfigAttributes(Impl->VaDisplay, VAProfileH264High,
                          VAEntrypointEncSlice, attribs, 2);
    // 启用自动生成 SPS/PPS 等头部
    attribs[1].value = VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE;

    vaErr = vaCreateConfig(Impl->VaDisplay, VAProfileH264High,
                           VAEntrypointEncSlice, attribs, 2, &Impl->ConfigId);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        Impl->LastError = "vaCreateConfig 失败（VAStatus=" + std::to_string(vaErr) + ")";
        return false;
    }

    // ---- 创建 Surface 池 ----
    VASurfaceAttrib surfAttrib;
    surfAttrib.type  = VASurfaceAttribPixelFormat;
    surfAttrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    surfAttrib.value.type = VAGenericValueTypeInteger;
    surfAttrib.value.value.i = VA_FOURCC_NV12;

    vaErr = vaCreateSurfaces(Impl->VaDisplay, VA_RT_FORMAT_YUV420,
                             Impl->Width, Impl->Height,
                             Impl->Surfaces, Impl->SurfaceCount,
                             &surfAttrib, 1);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        Impl->LastError = "vaCreateSurfaces 失败（VAStatus=" + std::to_string(vaErr) + ")";
        return false;
    }

    // ---- 创建编码上下文 ----
    vaErr = vaCreateContext(Impl->VaDisplay, Impl->ConfigId,
                            Impl->Width, Impl->Height, VA_PROGRESSIVE,
                            Impl->Surfaces, Impl->SurfaceCount, &Impl->ContextId);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        Impl->LastError = "vaCreateContext 失败（VAStatus=" + std::to_string(vaErr) + ")";
        return false;
    }

    Impl->bInitialized = true;
    return true;
}

bool FVAEncoder::StartRecording(const char* OutputPath)
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

    // ---- 打开 MP4Muxer（fake 模式使用与码流一致的 16x16，并预注入 SPS/PPS）----
    {
        int32 muxW = Impl->bFakeMode ? 320 : Impl->Width;
        int32 muxH = Impl->bFakeMode ? 240 : Impl->Height;
#ifdef VANE_FAKE_VAAPI
        if (Impl->bFakeMode)
        {
            // 预注入 SPS/PPS，确保 moov 中的 avcC 正确
            // 由 x264 生成的真正可解码的 Baseline Profile CAVLC H.264
            // 320x240 CAVLC Baseline H.264 (x264 生成，真正可解码)
            static const uint8_t kFakeSPS[] = {
                0x67,0x42,0xC0,0x1E,0xDC,0x14,0x1F,0xB0,0x11,0x00,0x00,0x03,
                0x00,0x01,0x00,0x00,0x03,0x00,0x02,0x0F,0x16,0x2F,0x80
            };
            static const uint8_t kFakePPS[] = {
                0x68,0xCE,0x0F,0x2C,0x80
            };
            Impl->Muxer.SetHeaders(kFakeSPS, sizeof(kFakeSPS),
                                    kFakePPS, sizeof(kFakePPS));
        }
#endif
        if (!Impl->Muxer.Start(OutputPath, muxW, muxH))
        {
            Impl->LastError = "无法创建输出文件";
            return false;
        }
    }

    Impl->OutputPath  = OutputPath;
    Impl->bHasError   = false;
    Impl->bRecording  = true;

    // 触发状态回调
    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Starting, Impl->StateUserData);

    // ---- 构建编码回调（在编码线程中执行 VA-API 编码）----
    FVAEncoderImpl* pImpl = Impl;
    FEncodeCallback encodeCb = [pImpl](const FFrameBuffer& Frame) {
        if (pImpl->bHasError) return;

        int64_t frameIdx = pImpl->EncodedCount.load(std::memory_order_relaxed);

#ifdef VANE_FAKE_VAAPI
        if (pImpl->bFakeMode)
        {
            // ---- x264 生成的 320×240 CAVLC Baseline 黑色帧 ----
            bool bIsIDR = ((frameIdx % pImpl->KeyInterval) == 0);
            static const uint8_t kStartCode[] = { 0x00, 0x00, 0x00, 0x01 };
            static const uint8_t kFakeIDR[] = {
                0x65,0x88,0x84,0x04,0xBC,0x98,0xA0,0x00,0x38,0xA3,0x27,0x27,0x27,0x27,0x27,0x27,
                0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x5D,0x75,0xD7,
                0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,
                0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,
                0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,
                0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,
                0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,
                0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,
                0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,
                0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,
                0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,
                0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,
                0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,
                0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,
                0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x5D,0x75,0xD7,0x80,
            };

            std::vector<uint8_t> annexb;
            if (bIsIDR)
            {
                static const uint8_t kFakeSPS[] = {
                    0x67,0x42,0xC0,0x1E,0xDC,0x14,0x1F,0xB0,0x11,0x00,0x00,0x03,
                    0x00,0x01,0x00,0x00,0x03,0x00,0x02,0x0F,0x16,0x2F,0x80
                };
                static const uint8_t kFakePPS[] = {
                    0x68,0xCE,0x0F,0x2C,0x80
                };
                annexb.insert(annexb.end(), kStartCode, kStartCode + 4);
                annexb.insert(annexb.end(), kFakeSPS, kFakeSPS + sizeof(kFakeSPS));
                annexb.insert(annexb.end(), kStartCode, kStartCode + 4);
                annexb.insert(annexb.end(), kFakePPS, kFakePPS + sizeof(kFakePPS));
                annexb.insert(annexb.end(), kStartCode, kStartCode + 4);
                annexb.insert(annexb.end(), kFakeIDR, kFakeIDR + sizeof(kFakeIDR));
            }
            else
            {
                annexb.insert(annexb.end(), kStartCode, kStartCode + 4);
                annexb.insert(annexb.end(), kFakeIDR, kFakeIDR + sizeof(kFakeIDR));
            }

            pImpl->Muxer.AddFrame(annexb.data(), annexb.size(), bIsIDR);

            pImpl->EncodedCount.fetch_add(1, std::memory_order_relaxed);
            ++pImpl->TotalFramesEncoded;

            // ---- 进度回调（每秒一次）----
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - pImpl->LastProgressTime).count();
            if (elapsed >= 1000 && pImpl->ProgressCb)
            {
                pImpl->ProgressCb(pImpl->TotalFramesEncoded, 0,
                                  Frame.TimestampSeconds, pImpl->ProgressUserData);
                pImpl->LastProgressTime = now;
            }
            pImpl->LastProgressTimestamp = Frame.TimestampSeconds;

            // ---- fMP4 周期刷盘 ----
            if (pImpl->FlushInterval > 0 && (pImpl->TotalFramesEncoded % pImpl->FlushInterval == 0))
                pImpl->Muxer.FlushFragment();

            return;
        }
#endif

        VAStatus vaErr;

        // 选择当前 surface（轮转）
        int32 surfIdx = frameIdx % pImpl->SurfaceCount;
        VASurfaceID surface = pImpl->Surfaces[surfIdx];

        // ---- BGRA → NV12 转换并写入 VA surface ----
        VAImage surfaceImg;
        memset(&surfaceImg, 0, sizeof(surfaceImg));
        vaErr = vaDeriveImage(pImpl->VaDisplay, surface, &surfaceImg);
        if (vaErr != VA_STATUS_SUCCESS)
        {
            VAImageFormat format;
            memset(&format, 0, sizeof(format));
            format.fourcc         = VA_FOURCC_NV12;
            format.byte_order     = VA_LSB_FIRST;
            format.bits_per_pixel = 12;

            vaErr = vaCreateImage(pImpl->VaDisplay, &format,
                                  pImpl->Width, pImpl->Height, &surfaceImg);
            if (vaErr != VA_STATUS_SUCCESS)
            {
                pImpl->bHasError = true;
                pImpl->LastError = "创建 VA 图像失败（VAStatus=" + std::to_string(vaErr) + ")";
                if (pImpl->ErrorCb)
                    pImpl->ErrorCb(EErrorLevel_Error, pImpl->LastError.c_str(), pImpl->ErrorUserData);
                return;
            }
        }

        void* imgData = nullptr;
        vaErr = vaMapBuffer(pImpl->VaDisplay, surfaceImg.buf, &imgData);
        if (vaErr != VA_STATUS_SUCCESS)
        {
            vaDestroyImage(pImpl->VaDisplay, surfaceImg.image_id);
            pImpl->bHasError = true;
            pImpl->LastError = "vaMapBuffer（surface 图像）失败";
            if (pImpl->ErrorCb)
                pImpl->ErrorCb(EErrorLevel_Error, pImpl->LastError.c_str(), pImpl->ErrorUserData);
            return;
        }

        uint8* yPlane  = static_cast<uint8*>(imgData) + surfaceImg.offsets[0];
        uint8* uvPlane = static_cast<uint8*>(imgData) + surfaceImg.offsets[1];
        ConvertBGRAToNV12(Frame.Data, pImpl->Width, pImpl->Height,
                          yPlane, uvPlane,
                          surfaceImg.pitches[0], surfaceImg.pitches[1]);

        vaUnmapBuffer(pImpl->VaDisplay, surfaceImg.buf);
        vaDestroyImage(pImpl->VaDisplay, surfaceImg.image_id);

        // ---- 开始编码帧 ----
        vaErr = vaBeginPicture(pImpl->VaDisplay, pImpl->ContextId, surface);
        if (vaErr != VA_STATUS_SUCCESS)
        {
            vaEndPicture(pImpl->VaDisplay, pImpl->ContextId);
            pImpl->bHasError = true;
            pImpl->LastError = "vaBeginPicture 失败（VAStatus=" + std::to_string(vaErr) + ")";
            if (pImpl->ErrorCb)
                pImpl->ErrorCb(EErrorLevel_Error, pImpl->LastError.c_str(), pImpl->ErrorUserData);
            return;
        }

        // ---- 序列参数集 ----
        int32 alignedWidth  = ((pImpl->Width  + 15) / 16) * 16;
        int32 alignedHeight = ((pImpl->Height + 15) / 16) * 16;

        VAEncSequenceParameterBufferH264 seqParam = {};
        seqParam.level_idc        = 42;
        seqParam.picture_width_in_mbs  = alignedWidth  / 16;
        seqParam.picture_height_in_mbs = alignedHeight / 16;
        seqParam.bits_per_second       = pImpl->BitRate;
        seqParam.intra_period          = pImpl->KeyInterval;
        seqParam.intra_idr_period      = pImpl->KeyInterval;
        seqParam.ip_period             = 1;
        seqParam.max_num_ref_frames    = 1;
        seqParam.seq_fields.bits.chroma_format_idc                 = 1;
        seqParam.seq_fields.bits.frame_mbs_only_flag               = 1;
        seqParam.seq_fields.bits.log2_max_frame_num_minus4         = 0;
        seqParam.seq_fields.bits.pic_order_cnt_type                = 0;
        seqParam.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;

        VABufferID seqBuf;
        vaErr = vaCreateBuffer(pImpl->VaDisplay, pImpl->ContextId,
                               VAEncSequenceParameterBufferType,
                               sizeof(seqParam), 1, &seqParam, &seqBuf);
        if (vaErr != VA_STATUS_SUCCESS)
        {
            vaEndPicture(pImpl->VaDisplay, pImpl->ContextId);
            pImpl->bHasError = true;
            pImpl->LastError = "创建序列参数 buffer 失败（VAStatus=" + std::to_string(vaErr) + ")";
            if (pImpl->ErrorCb)
                pImpl->ErrorCb(EErrorLevel_Error, pImpl->LastError.c_str(), pImpl->ErrorUserData);
            return;
        }
        vaRenderPicture(pImpl->VaDisplay, pImpl->ContextId, &seqBuf, 1);
        vaDestroyBuffer(pImpl->VaDisplay, seqBuf);

        // ---- 编码输出缓冲区 ----
        VABufferID codedBuf = VA_INVALID_ID;
        vaErr = vaCreateBuffer(pImpl->VaDisplay, pImpl->ContextId,
                               VAEncCodedBufferType,
                               pImpl->Width * pImpl->Height * 3, 1, nullptr, &codedBuf);
        if (vaErr != VA_STATUS_SUCCESS)
        {
            vaEndPicture(pImpl->VaDisplay, pImpl->ContextId);
            pImpl->bHasError = true;
            pImpl->LastError = "创建编码输出 buffer 失败（VAStatus=" + std::to_string(vaErr) + ")";
            if (pImpl->ErrorCb)
                pImpl->ErrorCb(EErrorLevel_Error, pImpl->LastError.c_str(), pImpl->ErrorUserData);
            return;
        }

        // ---- 图像参数集 ----
        bool bIsIDR = ((frameIdx % pImpl->KeyInterval) == 0);

        VAEncPictureParameterBufferH264 picParam = {};
        picParam.CurrPic.picture_id = surface;
        picParam.CurrPic.flags      = 0;
        picParam.coded_buf          = codedBuf;
        picParam.last_picture       = 0;
        picParam.pic_init_qp        = 26;
        for (int32 i = 0; i < 16; ++i)
            picParam.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
        picParam.num_ref_idx_l0_active_minus1 = 0;
        picParam.num_ref_idx_l1_active_minus1 = 0;
        picParam.pic_fields.bits.idr_pic_flag       = bIsIDR ? 1 : 0;
        picParam.pic_fields.bits.reference_pic_flag = 1;
        picParam.pic_fields.bits.entropy_coding_mode_flag = 1;
        picParam.pic_fields.bits.deblocking_filter_control_present_flag = 1;

        VABufferID picBuf;
        vaErr = vaCreateBuffer(pImpl->VaDisplay, pImpl->ContextId,
                               VAEncPictureParameterBufferType,
                               sizeof(picParam), 1, &picParam, &picBuf);
        if (vaErr != VA_STATUS_SUCCESS)
        {
            vaDestroyBuffer(pImpl->VaDisplay, codedBuf);
            vaEndPicture(pImpl->VaDisplay, pImpl->ContextId);
            pImpl->bHasError = true;
            pImpl->LastError = "创建图像参数 buffer 失败（VAStatus=" + std::to_string(vaErr) + ")";
            if (pImpl->ErrorCb)
                pImpl->ErrorCb(EErrorLevel_Error, pImpl->LastError.c_str(), pImpl->ErrorUserData);
            return;
        }
        vaRenderPicture(pImpl->VaDisplay, pImpl->ContextId, &picBuf, 1);
        vaDestroyBuffer(pImpl->VaDisplay, picBuf);

        // ---- Slice 参数 ----
        VAEncSliceParameterBufferH264 sliceParam = {};
        sliceParam.macroblock_address = 0;
        sliceParam.num_macroblocks    = (alignedWidth / 16) * (alignedHeight / 16);
        sliceParam.slice_type         = bIsIDR ? 2 : 0;
        sliceParam.slice_alpha_c0_offset_div2 = 0;
        sliceParam.slice_beta_offset_div2      = 0;
        sliceParam.direct_spatial_mv_pred_flag = 1;

        VABufferID sliceBuf;
        vaErr = vaCreateBuffer(pImpl->VaDisplay, pImpl->ContextId,
                               VAEncSliceParameterBufferType,
                               sizeof(sliceParam), 1, &sliceParam, &sliceBuf);
        if (vaErr != VA_STATUS_SUCCESS)
        {
            vaDestroyBuffer(pImpl->VaDisplay, codedBuf);
            vaEndPicture(pImpl->VaDisplay, pImpl->ContextId);
            pImpl->bHasError = true;
            pImpl->LastError = "创建 Slice 参数 buffer 失败（VAStatus=" + std::to_string(vaErr) + ")";
            if (pImpl->ErrorCb)
                pImpl->ErrorCb(EErrorLevel_Error, pImpl->LastError.c_str(), pImpl->ErrorUserData);
            return;
        }
        vaRenderPicture(pImpl->VaDisplay, pImpl->ContextId, &sliceBuf, 1);
        vaDestroyBuffer(pImpl->VaDisplay, sliceBuf);

        // ---- 码率控制参数 ----
        VAEncMiscParameterRateControl rateCtrl = {};
        rateCtrl.bits_per_second = (unsigned int)pImpl->BitRate;
        rateCtrl.target_percentage = 70;
        rateCtrl.quality_factor    = 26;
        rateCtrl.rc_flags.bits.reset = 0;
        rateCtrl.rc_flags.bits.disable_frame_skip = 1;

        size_t miscSize = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl);
        std::vector<uint8_t> miscData(miscSize);
        auto* miscBufHeader = reinterpret_cast<VAEncMiscParameterBuffer*>(miscData.data());
        miscBufHeader->type = VAEncMiscParameterTypeRateControl;
        memcpy(miscBufHeader->data, &rateCtrl, sizeof(rateCtrl));

        VABufferID miscBuf;
        vaErr = vaCreateBuffer(pImpl->VaDisplay, pImpl->ContextId,
                               VAEncMiscParameterBufferType,
                               miscSize, 1, miscData.data(), &miscBuf);
        if (vaErr != VA_STATUS_SUCCESS)
        {
            vaDestroyBuffer(pImpl->VaDisplay, codedBuf);
            vaEndPicture(pImpl->VaDisplay, pImpl->ContextId);
            pImpl->bHasError = true;
            pImpl->LastError = "创建码率控制参数 buffer 失败（VAStatus=" + std::to_string(vaErr) + ")";
            if (pImpl->ErrorCb)
                pImpl->ErrorCb(EErrorLevel_Error, pImpl->LastError.c_str(), pImpl->ErrorUserData);
            return;
        }
        vaRenderPicture(pImpl->VaDisplay, pImpl->ContextId, &miscBuf, 1);
        vaDestroyBuffer(pImpl->VaDisplay, miscBuf);

        // ---- 提交编码 ----
        vaErr = vaEndPicture(pImpl->VaDisplay, pImpl->ContextId);
        if (vaErr != VA_STATUS_SUCCESS)
        {
            vaDestroyBuffer(pImpl->VaDisplay, codedBuf);
            pImpl->bHasError = true;
            pImpl->LastError = "vaEndPicture 失败（VAStatus=" + std::to_string(vaErr) + ")";
            if (pImpl->ErrorCb)
                pImpl->ErrorCb(EErrorLevel_Error, pImpl->LastError.c_str(), pImpl->ErrorUserData);
            return;
        }

        // ---- 等待编码完成 ----
        vaErr = vaSyncSurface(pImpl->VaDisplay, surface);
        if (vaErr != VA_STATUS_SUCCESS)
        {
            vaDestroyBuffer(pImpl->VaDisplay, codedBuf);
            pImpl->bHasError = true;
            pImpl->LastError = "vaSyncSurface 失败（VAStatus=" + std::to_string(vaErr) + ")";
            if (pImpl->ErrorCb)
                pImpl->ErrorCb(EErrorLevel_Error, pImpl->LastError.c_str(), pImpl->ErrorUserData);
            return;
        }

        // ---- 获取编码数据并送入 MP4 复用器 ----
        VACodedBufferSegment* codedSeg = nullptr;
        vaErr = vaMapBuffer(pImpl->VaDisplay, codedBuf, (void**)&codedSeg);
        if (vaErr != VA_STATUS_SUCCESS || !codedSeg)
        {
            vaDestroyBuffer(pImpl->VaDisplay, codedBuf);
            pImpl->bHasError = true;
            pImpl->LastError = "vaMapBuffer（编码数据）失败";
            if (pImpl->ErrorCb)
                pImpl->ErrorCb(EErrorLevel_Error, pImpl->LastError.c_str(), pImpl->ErrorUserData);
            return;
        }

        if (codedSeg->size > 0)
            pImpl->Muxer.AddFrame(static_cast<const uint8_t*>(codedSeg->buf), codedSeg->size, bIsIDR);

        vaUnmapBuffer(pImpl->VaDisplay, codedBuf);
        vaDestroyBuffer(pImpl->VaDisplay, codedBuf);

        // 更新编码线程帧计数
        pImpl->EncodedCount.fetch_add(1, std::memory_order_relaxed);

        // ---- 进度回调（每秒一次）----
        ++pImpl->TotalFramesEncoded;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - pImpl->LastProgressTime).count();

        if (elapsed >= 1000 && pImpl->ProgressCb)
        {
            pImpl->ProgressCb(pImpl->TotalFramesEncoded, 0,
                              Frame.TimestampSeconds, pImpl->ProgressUserData);
            pImpl->LastProgressTime = now;
        }
        pImpl->LastProgressTimestamp = Frame.TimestampSeconds;

        // ---- fMP4 周期刷盘 ----
        if (pImpl->FlushInterval > 0 && (pImpl->TotalFramesEncoded % pImpl->FlushInterval == 0))
            pImpl->Muxer.FlushFragment();

        // 测试用编码延迟注入
        #ifdef VANE_TEST_ENCODING_DELAY_MS
            std::this_thread::sleep_for(std::chrono::milliseconds(VANE_TEST_ENCODING_DELAY_MS));
        #endif
    };

    // ---- 刷盘回调（编码线程触发）----
    FFlushCallback flushCb = [pImpl]() {
        pImpl->Muxer.FlushFragment();
    };

    // ---- 启动异步编码管线 ----
    Impl->Pipeline.Start(std::move(encodeCb), std::move(flushCb),
                         Impl->QueueSize, Impl->FlushInterval,
                         Impl->bContinueLastFrame, Impl->FrameRate);

    // 初始化进度统计
    Impl->TotalFramesEncoded = 0;
    Impl->LastProgressTime   = std::chrono::steady_clock::now();
    Impl->LastProgressTimestamp = 0.0;

    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Recording, Impl->StateUserData);

    return true;
}

bool FVAEncoder::EncodeFrame(const uint8* RawBGRA, int32 DataSize, double TimestampSeconds)
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

    // ---- 推入异步队列 ----
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

void FVAEncoder::RequestStop()
{
    if (!Impl->bRecording) return;

    Impl->bRecording = false;

    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Stopping, Impl->StateUserData);

    FVAEncoderImpl* pImpl = Impl;
    FFinalizeCallback finalizeCb = [pImpl]() {
        // 完成 MP4 文件写入
        pImpl->Muxer.Finish();

        // 强制最终进度
        if (pImpl->ProgressCb && pImpl->TotalFramesEncoded > 0)
        {
            pImpl->ProgressCb(pImpl->TotalFramesEncoded, 0,
                              pImpl->LastProgressTimestamp, pImpl->ProgressUserData);
        }

        if (pImpl->StateCb)
            pImpl->StateCb(ERecordingState_Idle, pImpl->StateUserData);
    };

    Impl->Pipeline.RequestStop(std::move(finalizeCb));
}

void FVAEncoder::StopRecording()
{
    if (!Impl->bRecording) return;

    Impl->bRecording = false;

    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Stopping, Impl->StateUserData);

    // 同步停止：等待编码线程排空后收尾
    Impl->Pipeline.Stop();

    // 完成 MP4 文件写入
    Impl->Muxer.Finish();

    // 强制最终进度
    if (Impl->ProgressCb && Impl->TotalFramesEncoded > 0)
    {
        Impl->ProgressCb(Impl->TotalFramesEncoded, 0,
                         Impl->LastProgressTimestamp, Impl->ProgressUserData);
    }

    if (Impl->StateCb)
        Impl->StateCb(ERecordingState_Idle, Impl->StateUserData);
}

bool FVAEncoder::IsRecording() const
{
    return Impl->bRecording;
}

const char* FVAEncoder::GetLastError() const
{
    return Impl->LastError.c_str();
}

FEncoderCapability FVAEncoder::CheckCapability() const
{
    FEncoderCapability cap = {};
    cap.bH264Available = true;
    cap.bHEVCAvailable = true;
    snprintf(cap.H264EncoderName, sizeof(cap.H264EncoderName), "VA-API (Linux)");
    snprintf(cap.RecommendedFormat, sizeof(cap.RecommendedFormat), "h264");
    snprintf(cap.DiagnosticInfo, sizeof(cap.DiagnosticInfo), "Linux VA-API: H.264/H.265 硬件编码（取决于驱动支持）");
    return cap;
}

void FVAEncoder::SetStateCallback(VaneStateCallback Cb, void* UserData)
{
    Impl->StateCb     = Cb;
    Impl->StateUserData = UserData;
}

void FVAEncoder::SetErrorCallback(VaneErrorCallback Cb, void* UserData)
{
    Impl->ErrorCb     = Cb;
    Impl->ErrorUserData = UserData;
}

void FVAEncoder::SetProgressCallback(VaneProgressCallback Cb, void* UserData)
{
    Impl->ProgressCb     = Cb;
    Impl->ProgressUserData = UserData;
}

void FVAEncoder::SetFrameDropCallback(VaneFrameDropCallback Cb, void* UserData)
{
    Impl->FrameDropCb     = Cb;
    Impl->FrameDropUserData = UserData;
}
