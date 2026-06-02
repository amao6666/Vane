#pragma once

#include <cstdint>
#include <vector>
#include <cstddef>
#include <cstdio>

// 标准 MP4 复用器（非分片）
// 将 H.264 Annex B 裸流封装为 avc1 格式的标准 MP4 文件
class MP4Muxer
{
public:
    MP4Muxer();
    ~MP4Muxer();

    // 开始录制：打开文件，写入 ftyp
    bool Start(const char* OutputPath, int32_t Width, int32_t Height);

    // 预注入 SPS/PPS（fake 模式下在 Start 前调用）
    void SetHeaders(const uint8_t* SpsData, size_t SpsLen,
                    const uint8_t* PpsData, size_t PpsLen);

    // 添加一帧 H.264 Annex B 编码数据
    void AddFrame(const uint8_t* AnnexBData, size_t Size, bool bIsKeyFrame);

    // 刷写缓冲区到磁盘（fflush，不写 box）
    void FlushFragment();

    // 完成录制：写入 mdat + moov（含完整 stts/stsz/stsc/stco），关闭文件
    bool Finish();

    int32_t GetFrameCount() const { return TotalFrames; }

private:
    struct FrameData
    {
        std::vector<uint8_t> Avc1Data;
        bool                 bKeyFrame;
    };

    FILE*  OutputFile = nullptr;
    int32_t Width  = 0;
    int32_t Height = 0;
    int32_t TotalFrames = 0;

    // SPS/PPS
    std::vector<uint8_t> SPS;
    std::vector<uint8_t> PPS;
    bool                 bHeadersExtracted = false;

    // 当前待刷写帧
    std::vector<FrameData> PendingFrames;
    // 所有帧（用于 Finish 时构建 mdat + moov）
    std::vector<FrameData> AllFrames;

    void ExtractHeaders(const uint8_t* Data, size_t Size);
    void ConvertAndCacheFrame(const uint8_t* AnnexBData, size_t Size, bool bIsKeyFrame);

    // Box 写入辅助（大端序）
    static void WriteU32(std::vector<uint8_t>& Buf, uint32_t V);
    static void WriteU16(std::vector<uint8_t>& Buf, uint16_t V);
    static void WriteU8 (std::vector<uint8_t>& Buf, uint8_t  V);
    static void WriteFourCC(std::vector<uint8_t>& Buf, const char* CC);
    static void WriteBytes(std::vector<uint8_t>& Buf, const uint8_t* Data, size_t Len);
    static void WriteZero(std::vector<uint8_t>& Buf, size_t Len);

    void WriteFtyp();
    void WriteMoov();
    void WriteMdat();

    // 时间刻度（60fps 下每帧 10 单位）
    static constexpr uint32_t Timescale     = 600;
    static constexpr uint32_t FrameDuration = 10;
    static constexpr uint32_t TrackID       = 1;
};
