#include "FVAEncoder.h"
#include "MP4Muxer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <unistd.h>

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
    int32 Width       = 0;
    int32 Height      = 0;
    int32 FrameCount  = 0;
    int32 BitRate     = 10000000;
    int32 KeyInterval = 120;
    int32 FlushInterval = 60;

    // VA-API 对象
    int          DrmFd       = -1;
    VADisplay    VaDisplay   = nullptr;
    VAConfigID   ConfigId    = VA_INVALID_ID;
    VAContextID  ContextId   = VA_INVALID_ID;

    // Surface 池（NV12 格式，用于硬件编码输入）
    static constexpr int32 SurfaceCount = 4;
    VASurfaceID Surfaces[SurfaceCount];

    // 输出
    MP4Muxer  Muxer;
    std::string OutputPath;

    bool bInitialized = false;
    bool bRecording   = false;
    bool bHasError    = false;

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

    if (Impl->ContextId != VA_INVALID_ID)
    {
        vaDestroyContext(Impl->VaDisplay, Impl->ContextId);
    }
    if (Impl->ConfigId != VA_INVALID_ID)
    {
        vaDestroyConfig(Impl->VaDisplay, Impl->ConfigId);
    }
    if (Impl->VaDisplay)
    {
        vaTerminate(Impl->VaDisplay);
    }
    if (Impl->DrmFd >= 0)
    {
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

    Impl->Width      = Config.Width;
    Impl->Height     = Config.Height;
    Impl->BitRate       = Config.BitRate;
    Impl->KeyInterval   = Config.KeyframeInterval;
    Impl->FlushInterval  = Config.FlushIntervalFrames;

    // ---- 打开 DRM 设备 ----
    Impl->DrmFd = open("/dev/dri/renderD128", O_RDWR);
    if (Impl->DrmFd < 0)
    {
        Impl->LastError = "无法打开 DRM 设备 /dev/dri/renderD128";
        return false;
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
                             Width, Height,
                             Impl->Surfaces, Impl->SurfaceCount,
                             &surfAttrib, 1);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        Impl->LastError = "vaCreateSurfaces 失败（VAStatus=" + std::to_string(vaErr) + ")";
        return false;
    }

    // ---- 创建编码上下文 ----
    vaErr = vaCreateContext(Impl->VaDisplay, Impl->ConfigId,
                            Width, Height, VA_PROGRESSIVE,
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

    Impl->OutputPath      = OutputPath;
    if (!Impl->Muxer.Start(OutputPath, Impl->Width, Impl->Height))
    {
        Impl->LastError = "无法创建输出文件";
        return false;
    }
    Impl->FrameCount      = 0;
    Impl->bHasError       = false;
    Impl->bRecording      = true;

    return true;
}

bool FVAEncoder::EncodeFrame(const uint8* RawBGRA, int32 DataSize, double /*TimestampSeconds*/)
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

    VAStatus vaErr;

    // ---- 从 Surface 池中选择一个空闲 surface ----
    int32 surfIdx = Impl->FrameCount % Impl->SurfaceCount;
    VASurfaceID surface = Impl->Surfaces[surfIdx];

    // ---- 将 BGRA 数据转换为 NV12 并写入 surface ----
    VAImage surfaceImg;
    vaErr = vaDeriveImage(Impl->VaDisplay, surface, &surfaceImg);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        // 如果 vaDeriveImage 不支持，尝试用 vaCreateImage + vaPutImage
        vaErr = vaCreateImage(Impl->VaDisplay, &surfaceImg, VA_FOURCC_NV12,
                              Impl->Width, Impl->Height);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        vaEndPicture(Impl->VaDisplay, Impl->ContextId);
        Impl->bHasError = true;
        Impl->LastError = "创建序列参数 buffer 失败";
        return false;
    }
    }

    void* imgData = nullptr;
    vaErr = vaMapBuffer(Impl->VaDisplay, surfaceImg.buf, &imgData);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        vaDestroyImage(Impl->VaDisplay, surfaceImg.image_id);
        Impl->bHasError = true;
        Impl->LastError = "vaMapBuffer（surface 图像）失败";
        return false;
    }

    uint8* yPlane  = static_cast<uint8*>(imgData) + surfaceImg.offsets[0];
    uint8* uvPlane = static_cast<uint8*>(imgData) + surfaceImg.offsets[1];
    ConvertBGRAToNV12(RawBGRA, Impl->Width, Impl->Height,
                      yPlane, uvPlane,
                      surfaceImg.pitches[0], surfaceImg.pitches[1]);

    vaUnmapBuffer(Impl->VaDisplay, surfaceImg.buf);
    vaDestroyImage(Impl->VaDisplay, surfaceImg.image_id);

    // ---- 开始编码帧 ----
    vaErr = vaBeginPicture(Impl->VaDisplay, Impl->ContextId, surface);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        vaEndPicture(Impl->VaDisplay, Impl->ContextId);
        Impl->bHasError = true;
        Impl->LastError = "创建编码输出 buffer 失败";
        return false;
    }

    // ---- 构造编码参数 ----
    int32 alignedWidth  = ((Impl->Width  + 15) / 16) * 16;
    int32 alignedHeight = ((Impl->Height + 15) / 16) * 16;

    // 序列参数集
    VAEncSequenceParameterBufferH264 seqParam = {};
    seqParam.level_idc        = 42;   // Level 4.2（支持 1080p）
    seqParam.picture_width_in_mbs  = alignedWidth  / 16;
    seqParam.picture_height_in_mbs = alignedHeight / 16;
    seqParam.bits_per_second       = Impl->BitRate;
    seqParam.intra_period          = Impl->KeyInterval;  // I 帧间隔
    seqParam.intra_idr_period      = Impl->KeyInterval;
    seqParam.ip_period             = 1;   // IPPPPP...
    seqParam.max_num_ref_frames    = 1;
    seqParam.seq_fields.bits.chroma_format_idc                 = 1;
    seqParam.seq_fields.bits.frame_mbs_only_flag               = 1;
    seqParam.seq_fields.bits.log2_max_frame_num_minus4         = 0;
    seqParam.seq_fields.bits.pic_order_cnt_type                = 0;
    seqParam.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;

    VABufferID seqBuf;
    vaErr = vaCreateBuffer(Impl->VaDisplay, Impl->ContextId,
                           VAEncSequenceParameterBufferType,
                           sizeof(seqParam), 1, &seqParam, &seqBuf);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        vaEndPicture(Impl->VaDisplay, Impl->ContextId);
        Impl->bHasError = true;
        return false;
    }
    vaRenderPicture(Impl->VaDisplay, Impl->ContextId, &seqBuf, 1);
    vaDestroyBuffer(Impl->VaDisplay, seqBuf);

    // ---- 创建编码输出缓冲区 ----
    // 缓冲区大小按最坏情况（原始帧大小）
    VABufferID codedBuf = VA_INVALID_ID;
    vaErr = vaCreateBuffer(Impl->VaDisplay, Impl->ContextId,
                           VAEncCodedBufferType,
                            Impl->Width * Impl->Height * 3, 1, nullptr, &codedBuf);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        vaEndPicture(Impl->VaDisplay, Impl->ContextId);
        Impl->bHasError = true;
        Impl->LastError = "创建编码输出 buffer 失败";
        return false;
    }

    // 图像参数集
    bool bIsIDR = ((Impl->FrameCount % Impl->KeyInterval) == 0);

    VAEncPictureParameterBufferH264 picParam = {};
    picParam.picture_width  = Impl->Width;
    picParam.picture_height = Impl->Height;
    picParam.reconstructed_picture = surface;
    picParam.coded_buf     = codedBuf;
    picParam.last_picture  = VA_INVALID_ID;
    picParam.pic_init_qp   = 26;
    picParam.ref_pic_list0[0] = VA_INVALID_SURFACE;
    picParam.ref_pic_list1[0] = VA_INVALID_SURFACE;
    picParam.pic_fields.bits.idr_pic_flag       = bIsIDR ? 1 : 0;
    picParam.pic_fields.bits.reference_pic_flag = 1;
    picParam.pic_fields.bits.entropy_coding_mode_flag = 1;
    picParam.pic_fields.bits.deblocking_filter_control_present_flag = 1;

    VABufferID picBuf;
    vaErr = vaCreateBuffer(Impl->VaDisplay, Impl->ContextId,
                           VAEncPictureParameterBufferType,
                            sizeof(picParam), 1, &picParam, &picBuf);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        vaDestroyBuffer(Impl->VaDisplay, codedBuf);
        vaEndPicture(Impl->VaDisplay, Impl->ContextId);
        Impl->bHasError = true;
        Impl->LastError = "创建图像参数 buffer 失败";
        return false;
    }
    vaRenderPicture(Impl->VaDisplay, Impl->ContextId, &picBuf, 1);
    vaDestroyBuffer(Impl->VaDisplay, picBuf);

    // Slice 参数
    VAEncSliceParameterBufferH264 sliceParam = {};
    sliceParam.macroblock_address = 0;
    sliceParam.num_macroblocks    = (alignedWidth / 16) * (alignedHeight / 16);
    sliceParam.slice_type         = bIsIDR ? 2 : 0; // I or P
    sliceParam.slice_alpha_c0_offset_div2 = 0;
    sliceParam.slice_beta_offset_div2      = 0;
    sliceParam.direct_spatial_mv_pred_flag = 1;

    VABufferID sliceBuf;
    vaErr = vaCreateBuffer(Impl->VaDisplay, Impl->ContextId,
                           VAEncSliceParameterBufferType,
                            sizeof(sliceParam), 1, &sliceParam, &sliceBuf);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        vaDestroyBuffer(Impl->VaDisplay, codedBuf);
        vaEndPicture(Impl->VaDisplay, Impl->ContextId);
        Impl->bHasError = true;
        Impl->LastError = "创建 Slice 参数 buffer 失败";
        return false;
    }
    vaRenderPicture(Impl->VaDisplay, Impl->ContextId, &sliceBuf, 1);
    vaDestroyBuffer(Impl->VaDisplay, sliceBuf);

    // 码率控制参数
    VAEncMiscParameterRateControl rateCtrl = {};
    rateCtrl.bits_per_second = Impl->BitRate;
    rateCtrl.target_percentage = 70;
    rateCtrl.quality_factor    = 26;
    rateCtrl.rc_flags.bits.reset = 0;
    rateCtrl.rc_flags.bits.disable_frame_skip = 1;

    VAEncMiscParameterBuffer miscParam = {};
    miscParam.type = VAEncMiscParameterTypeRateControl;
    memcpy(miscParam.data, &rateCtrl, sizeof(rateCtrl));

    VABufferID miscBuf;
    vaErr = vaCreateBuffer(Impl->VaDisplay, Impl->ContextId,
                           VAEncMiscParameterBufferType,
                           sizeof(miscParam), 1, &miscParam, &miscBuf);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        vaDestroyBuffer(Impl->VaDisplay, codedBuf);
        vaEndPicture(Impl->VaDisplay, Impl->ContextId);
        Impl->bHasError = true;
        Impl->LastError = "创建码率控制参数 buffer 失败";
        return false;
    }
    vaRenderPicture(Impl->VaDisplay, Impl->ContextId, &miscBuf, 1);
    vaDestroyBuffer(Impl->VaDisplay, miscBuf);

    // ---- 提交编码 ----
    vaErr = vaEndPicture(Impl->VaDisplay, Impl->ContextId);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        vaDestroyBuffer(Impl->VaDisplay, codedBuf);
        Impl->bHasError = true;
        Impl->LastError = "vaEndPicture 失败";
        return false;
    }

    // ---- 等待编码完成 ----
    vaErr = vaSyncSurface(Impl->VaDisplay, surface);
    if (vaErr != VA_STATUS_SUCCESS)
    {
        vaDestroyBuffer(Impl->VaDisplay, codedBuf);
        Impl->bHasError = true;
        Impl->LastError = "vaSyncSurface 失败";
        return false;
    }

    // ---- 获取编码数据并送入 MP4 复用器 ----
    VACodedBufferSegment* codedSeg = nullptr;
    vaErr = vaMapBuffer(Impl->VaDisplay, codedBuf, (void**)&codedSeg);
    if (vaErr != VA_STATUS_SUCCESS || !codedSeg)
    {
        vaDestroyBuffer(Impl->VaDisplay, codedBuf);
        Impl->bHasError = true;
        Impl->LastError = "vaMapBuffer（编码数据）失败";
        return false;
    }

    if (codedSeg->size > 0)
    {
        Impl->Muxer.AddFrame(codedSeg->buf, codedSeg->size, bIsIDR);
    }

    vaUnmapBuffer(Impl->VaDisplay, codedBuf);
    vaDestroyBuffer(Impl->VaDisplay, codedBuf);

    ++Impl->FrameCount;

    // fMP4 周期刷盘
    if (Impl->FlushInterval > 0 && (Impl->FrameCount % Impl->FlushInterval == 0))
        Impl->Muxer.FlushFragment();

    return true;
}

void FVAEncoder::StopRecording()
{
    if (!Impl->bRecording) return;

    Impl->bRecording = false;

    if (Impl->Muxer.GetFrameCount() > 0)
    {
        Impl->Muxer.Finish();
    }
}

void FVAEncoder::RequestStop()
{
    // TODO: 实现异步停止（当前委托同步版本）
    StopRecording();
}

bool FVAEncoder::IsRecording() const
{
    return Impl->bRecording;
}

const char* FVAEncoder::GetLastError() const
{
    return Impl->LastError.c_str();
}

// TODO: 实现回调通知（SetStateCallback / SetErrorCallback / SetProgressCallback / SetFrameDropCallback）
// 当前使用 IVideoEncoder 基类默认空实现，后续需要覆盖并在适当位置触发回调。
