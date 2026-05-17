#pragma once

#include <cstdint>
#include <vector>
#include <cstddef>
#include <cstdio>

// fMP4 (Fragmented MP4) 复用器
// 将 H.264 Annex B 裸流封装为 avc1 格式的 fMP4 文件
// 支持周期刷盘：每 N 帧写一个 moof+mdat fragment，崩溃后已写入部分可播
class MP4Muxer
{
public:
    MP4Muxer();
    ~MP4Muxer();

    // 开始录制：打开文件，写入 ftyp + moov（含 mvex）
    bool Start(const char* OutputPath, int32 Width, int32 Height);

    // 添加一帧 H.264 Annex B 编码数据
    void AddFrame(const uint8_t* AnnexBData, size_t Size, bool bIsKeyFrame);

    // 将当前累积帧作为一个 fragment 刷写到磁盘（moof + mdat + fflush）
    void FlushFragment();

    // 完成录制：写入最后 fragment，关闭文件
    bool Finish();

    int32 GetFrameCount() const { return TotalFrames; }

private:
    struct FrameData
    {
        std::vector<uint8_t> Avc1Data;
        bool                 bKeyFrame;
    };

    FILE*  OutputFile = nullptr;
    int32  Width = 0;
    int32  Height = 0;
    int32  TotalFrames = 0;
    int32  FragmentSequence = 0;

    // SPS/PPS
    std::vector<uint8_t> SPS;
    std::vector<uint8_t> PPS;
    bool                 bHeadersExtracted = false;

    // 当前 fragment 的待刷写帧
    std::vector<FrameData> PendingFrames;

    // ---- 辅助 ----
    void ExtractHeaders(const uint8_t* Data, size_t Size);
    void ConvertAndCacheFrame(const uint8_t* AnnexBData, size_t Size, bool bIsKeyFrame);

    // Box 写入（大端序）
    static void WriteU32(std::vector<uint8_t>& Buf, uint32_t V);
    static void WriteU16(std::vector<uint8_t>& Buf, uint16_t V);
    static void WriteU8 (std::vector<uint8_t>& Buf, uint8_t  V);
    static void WriteFourCC(std::vector<uint8_t>& Buf, const char* CC);
    static void WriteBytes(std::vector<uint8_t>& Buf, const uint8_t* Data, size_t Len);
    static void WriteZero(std::vector<uint8_t>& Buf, size_t Len);

    void WriteFtyp();
    void WriteMoov();
    void WriteAvcC(std::vector<uint8_t>& Out);
    void WriteMoofAndMdat();

    // 时间刻度
    static constexpr uint32_t Timescale     = 600;
    static constexpr uint32_t FrameDuration = 10;
    static constexpr uint32_t TrackID       = 1;
};
