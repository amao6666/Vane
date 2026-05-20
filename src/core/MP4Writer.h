#pragma once

#include <cstdint>
#include <vector>
#include <cstdio>

using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;

// 普通 MP4 封装器（moov 在文件末尾，非 fMP4）
// 将 H.264 Annex B 裸流封装为 avc1 格式的 MP4 文件
// 与 src/MP4Muxer.h（Linux fMP4）并行存在，不修改现有代码
class MP4Writer
{
public:
    MP4Writer();
    ~MP4Writer();

    bool Create(const char* OutputPath, int32 Width, int32 Height, int32 FPS);
    bool WriteFrame(const uint8* AnnexBData, size_t Size, int64 PTS, bool bIsKeyFrame);
    bool Finalize();
    void Destroy();

private:
    struct SampleInfo
    {
        uint32_t Size;
        uint32_t Offset;
        uint32_t Duration;
        bool     bKeyFrame;
    };

    FILE*  File = nullptr;
    int32  Width = 0;
    int32  Height = 0;
    int32  FPS = 0;
    int32  TotalFrames = 0;
    uint32_t Timescale = 0;
    uint32_t FrameDuration = 0;

    std::vector<SampleInfo> Samples;

    std::vector<uint8_t> SPS;
    std::vector<uint8_t> PPS;
    bool bHeadersExtracted = false;

    int64 MdatStartPos = 0;
    int64 MdatDataSize = 0;

    void ExtractHeaders(const uint8* Data, size_t Size);
    std::vector<uint8_t> ConvertToAvc1(const uint8* AnnexBData, size_t Size);

    static void WriteU32(std::vector<uint8_t>& Buf, uint32_t V);
    static void WriteU16(std::vector<uint8_t>& Buf, uint16_t V);
    static void WriteU8 (std::vector<uint8_t>& Buf, uint8_t  V);
    static void WriteFourCC(std::vector<uint8_t>& Buf, const char* CC);
    static void WriteBytes(std::vector<uint8_t>& Buf, const uint8_t* Data, size_t Len);
    static void WriteZero(std::vector<uint8_t>& Buf, size_t Len);

    void WriteFtyp();
    void WriteMoov();
    void WriteAvcC(std::vector<uint8_t>& Out);
};
