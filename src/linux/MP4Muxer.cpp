#include "MP4Muxer.h"
#include <cstring>
#include <algorithm>

// ============================================================================
// 构造 / 析构
// ============================================================================

MP4Muxer::MP4Muxer() = default;

MP4Muxer::~MP4Muxer()
{
    Finish();
}

// ============================================================================
// 开始录制：打开文件 → ftyp
// ============================================================================

bool MP4Muxer::Start(const char* OutputPath, int32_t InWidth, int32_t InHeight)
{
    Width  = InWidth;
    Height = InHeight;
    TotalFrames = 0;
    PendingFrames.clear();
    AllFrames.clear();
    // 如果已通过 SetHeaders() 预注入 SPS/PPS，则保留
    if (!bHeadersExtracted)
    {
        SPS.clear();
        PPS.clear();
    }

    OutputFile = fopen(OutputPath, "wb");
    if (!OutputFile) return false;

    WriteFtyp();
    fflush(OutputFile);
    return true;
}

// ============================================================================
// 预注入 SPS/PPS
// ============================================================================

void MP4Muxer::SetHeaders(const uint8_t* SpsData, size_t SpsLen,
                           const uint8_t* PpsData, size_t PpsLen)
{
    bHeadersExtracted = true;
    SPS.assign(SpsData, SpsData + SpsLen);
    PPS.assign(PpsData, PpsData + PpsLen);
}

// ============================================================================
// 添加帧
// ============================================================================

void MP4Muxer::AddFrame(const uint8_t* AnnexBData, size_t Size, bool bIsKeyFrame)
{
    if (!bHeadersExtracted && bIsKeyFrame)
        ExtractHeaders(AnnexBData, Size);

    ConvertAndCacheFrame(AnnexBData, Size, bIsKeyFrame);
    ++TotalFrames;
}

// ============================================================================
// 刷写缓冲区（仅 fflush，不写 box；WriteMdat 在 Finish 统一写入）
// ============================================================================

void MP4Muxer::FlushFragment()
{
    if (PendingFrames.empty()) return;
    // 将当前累积帧移入全局列表，不写磁盘
    AllFrames.insert(AllFrames.end(),
                     std::make_move_iterator(PendingFrames.begin()),
                     std::make_move_iterator(PendingFrames.end()));
    PendingFrames.clear();
    fflush(OutputFile);
}

// ============================================================================
// 完成：写入 mdat + moov（标准 MP4，moov 在末尾）
// ============================================================================

bool MP4Muxer::Finish()
{
    FlushFragment();  // 确保 PendingFrames 归入 AllFrames
    if (!OutputFile) return true;

    // 写 mdat
    WriteMdat();

    // 写 moov（含完整 stts/stsz/stsc/stco/stss 表）
    WriteMoov();
    fflush(OutputFile);
    fclose(OutputFile);
    OutputFile = nullptr;
    return true;
}

// ============================================================================
// SPS/PPS 提取
// ============================================================================

static size_t FindStartCode(const uint8_t* Data, size_t Offset, size_t Size)
{
    for (size_t i = Offset; i + 2 < Size; ++i)
    {
        if (Data[i] == 0x00 && Data[i + 1] == 0x00)
        {
            if (Data[i + 2] == 0x01) return i + 3;
            if (i + 3 < Size && Data[i + 2] == 0x00 && Data[i + 3] == 0x01) return i + 4;
        }
    }
    return Size;
}

void MP4Muxer::ExtractHeaders(const uint8_t* Data, size_t Size)
{
    bHeadersExtracted = true;
    size_t pos = 0;
    while (pos < Size)
    {
        size_t scEnd = FindStartCode(Data, pos, Size);
        if (scEnd == pos) { pos = FindStartCode(Data, pos + 1, Size); continue; }
        size_t naluStart = scEnd;
        size_t naluAfter  = FindStartCode(Data, naluStart, Size);
        // 回退 start code 长度，得到实际 NAL 数据末尾
        size_t naluEnd = naluAfter;
        if (naluEnd >= 4 && Data[naluEnd-4]==0x00 && Data[naluEnd-3]==0x00
            && Data[naluEnd-2]==0x00 && Data[naluEnd-1]==0x01)
            naluEnd -= 4;
        else if (naluEnd >= 3 && Data[naluEnd-3]==0x00 && Data[naluEnd-2]==0x00
                 && Data[naluEnd-1]==0x01)
            naluEnd -= 3;

        if (naluStart < naluEnd && naluStart < Size)
        {
            uint8_t type = Data[naluStart] & 0x1F;
            size_t len = naluEnd - naluStart;
            if (type == 7) SPS.assign(Data + naluStart, Data + naluStart + len);
            if (type == 8) PPS.assign(Data + naluStart, Data + naluStart + len);
        }
        pos = naluEnd;  // 回退到下一个 start code 开头
    }
}

// ============================================================================
// Annex B → avc1 转换并缓存
// ============================================================================

void MP4Muxer::ConvertAndCacheFrame(const uint8_t* Data, size_t Size, bool bIsKeyFrame)
{
    std::vector<uint8_t> avc1;
    size_t pos = 0;

    while (pos < Size)
    {
        size_t scEnd = FindStartCode(Data, pos, Size);
        if (scEnd == pos) { pos = FindStartCode(Data, pos + 1, Size); continue; }
        size_t naluStart = scEnd;
        size_t naluAfter  = FindStartCode(Data, naluStart, Size);
        // 回退 start code
        size_t naluEnd = naluAfter;
        if (naluEnd >= 4 && Data[naluEnd-4]==0x00 && Data[naluEnd-3]==0x00
            && Data[naluEnd-2]==0x00 && Data[naluEnd-1]==0x01)
            naluEnd -= 4;
        else if (naluEnd >= 3 && Data[naluEnd-3]==0x00 && Data[naluEnd-2]==0x00
                 && Data[naluEnd-1]==0x01)
            naluEnd -= 3;

        if (naluStart >= naluEnd || naluStart >= Size) break;

        uint8_t type = Data[naluStart] & 0x1F;
        size_t len = naluEnd - naluStart;

        // AUD/SEI 不放入 mdat
        if (type == 9 || type == 6) { pos = naluEnd; continue; }

        WriteU32(avc1, (uint32_t)len);
        WriteBytes(avc1, Data + naluStart, len);
        pos = naluEnd;
    }

    if (avc1.empty()) return;

    FrameData frame;
    frame.Avc1Data  = std::move(avc1);
    frame.bKeyFrame = bIsKeyFrame;
    PendingFrames.push_back(std::move(frame));
}

// ============================================================================
// Box 写入辅助
// ============================================================================

void MP4Muxer::WriteU32(std::vector<uint8_t>& Buf, uint32_t V) {
    Buf.push_back((V>>24)&0xFF); Buf.push_back((V>>16)&0xFF);
    Buf.push_back((V>>8)&0xFF);  Buf.push_back(V&0xFF);
}
void MP4Muxer::WriteU16(std::vector<uint8_t>& Buf, uint16_t V) {
    Buf.push_back((V>>8)&0xFF);  Buf.push_back(V&0xFF);
}
void MP4Muxer::WriteU8(std::vector<uint8_t>& Buf, uint8_t V) { Buf.push_back(V); }
void MP4Muxer::WriteFourCC(std::vector<uint8_t>& Buf, const char* CC) {
    Buf.insert(Buf.end(), CC, CC+4);
}
void MP4Muxer::WriteBytes(std::vector<uint8_t>& Buf, const uint8_t* Data, size_t Len) {
    Buf.insert(Buf.end(), Data, Data+Len);
}
void MP4Muxer::WriteZero(std::vector<uint8_t>& Buf, size_t Len) {
    Buf.resize(Buf.size()+Len, 0);
}

static void FWrite(const std::vector<uint8_t>& Buf, FILE* f) {
    fwrite(Buf.data(), 1, Buf.size(), f);
}

// ============================================================================
// ftyp box
// ============================================================================

void MP4Muxer::WriteFtyp()
{
    std::vector<uint8_t> buf;
    WriteU32(buf, 24);
    WriteFourCC(buf, "ftyp");
    WriteFourCC(buf, "isom");
    WriteU32(buf, 0x00000200);
    WriteFourCC(buf, "isom");
    WriteFourCC(buf, "avc1");
    FWrite(buf, OutputFile);
}

// ============================================================================
// mdat：一次性写入所有帧
// ============================================================================

void MP4Muxer::WriteMdat()
{
    // 计算 mdat 数据总大小
    uint32_t dataSize = 0;
    for (auto& f : AllFrames)
        dataSize += (uint32_t)f.Avc1Data.size();

    std::vector<uint8_t> buf;
    WriteU32(buf, 8 + dataSize);
    WriteFourCC(buf, "mdat");
    FWrite(buf, OutputFile);

    // 逐帧写入
    for (auto& f : AllFrames)
        FWrite(f.Avc1Data, OutputFile);
}

// ============================================================================
// moov box（标准 MP4：含 stts/stsz/stsc/stco/stss）
// ============================================================================

void MP4Muxer::WriteMoov()
{
    uint32_t N = (uint32_t)AllFrames.size();

    // 统计关键帧索引
    std::vector<uint32_t> syncSamples;
    for (uint32_t i = 0; i < N; ++i)
        if (AllFrames[i].bKeyFrame) syncSamples.push_back(i + 1);

    // ≤- tkhd ---
    std::vector<uint8_t> trakBuf;
    WriteU32(trakBuf, 92); WriteFourCC(trakBuf, "tkhd");
    WriteU32(trakBuf, 0x00000007);
    WriteU32(trakBuf, 0); WriteU32(trakBuf, 0);
    WriteU32(trakBuf, TrackID); WriteU32(trakBuf, 0);
    WriteU32(trakBuf, N * FrameDuration);
    WriteZero(trakBuf, 8);
    WriteU16(trakBuf, 0); WriteU16(trakBuf, 0);
    WriteU16(trakBuf, 0x0100); WriteU16(trakBuf, 0);
    // matrix: 9 × 4 = 36 bytes (identity)
    WriteU32(trakBuf, 0x00010000); WriteZero(trakBuf, 4);
    WriteZero(trakBuf, 4); WriteZero(trakBuf, 4);
    WriteU32(trakBuf, 0x00010000); WriteZero(trakBuf, 4);
    WriteZero(trakBuf, 4); WriteZero(trakBuf, 4);
    WriteU32(trakBuf, 0x40000000);
    WriteU32(trakBuf, Width*0x10000); WriteU32(trakBuf, Height*0x10000);

    // --- mdia ---
    std::vector<uint8_t> mdiaBuf;
    WriteU32(mdiaBuf, 32); WriteFourCC(mdiaBuf, "mdhd");
    WriteU32(mdiaBuf, 0); WriteU32(mdiaBuf, 0); WriteU32(mdiaBuf, 0);
    WriteU32(mdiaBuf, Timescale);
    WriteU32(mdiaBuf, N * FrameDuration);
    WriteU16(mdiaBuf, 0x55C4); WriteU16(mdiaBuf, 0);

    WriteU32(mdiaBuf, 45); WriteFourCC(mdiaBuf, "hdlr");
    WriteU32(mdiaBuf, 0); WriteU32(mdiaBuf, 0);
    WriteFourCC(mdiaBuf, "vide"); WriteZero(mdiaBuf, 12);
    WriteBytes(mdiaBuf, (const uint8_t*)"VideoHandler", 12);
    WriteU8(mdiaBuf, 0);

    // --- minf ---
    std::vector<uint8_t> minfBuf;
    WriteU32(minfBuf, 20); WriteFourCC(minfBuf, "vmhd");
    WriteU32(minfBuf, 0x00000001);
    WriteU16(minfBuf, 0); WriteZero(minfBuf, 6);

    std::vector<uint8_t> dinfBuf;
    WriteU32(dinfBuf, 28); WriteFourCC(dinfBuf, "dref");
    WriteU32(dinfBuf, 0); WriteU32(dinfBuf, 1);
    WriteU32(dinfBuf, 12); WriteFourCC(dinfBuf, "url ");
    WriteU32(dinfBuf, 0x00000001);
    minfBuf.insert(minfBuf.end(), dinfBuf.begin(), dinfBuf.end());

    // --- stbl ---
    std::vector<uint8_t> stblBuf;

    // stsd
    std::vector<uint8_t> stsdEntry;
    WriteZero(stsdEntry, 6); WriteU16(stsdEntry, 1);
    WriteU16(stsdEntry, 0); WriteU16(stsdEntry, 0);
    WriteZero(stsdEntry, 12);
    WriteU16(stsdEntry, Width); WriteU16(stsdEntry, Height);
    WriteU32(stsdEntry, 0x00480000); WriteU32(stsdEntry, 0x00480000);
    WriteU32(stsdEntry, 0); WriteU16(stsdEntry, 1);
    WriteZero(stsdEntry, 32);
    WriteU16(stsdEntry, 0x0018); WriteU16(stsdEntry, 0xFFFF);

    // avcC
    std::vector<uint8_t> avcCBuf;
    uint32_t avcCSize = 19 + (uint32_t)(SPS.size() + PPS.size());
    WriteU32(avcCBuf, avcCSize); WriteFourCC(avcCBuf, "avcC");
    WriteU8(avcCBuf, 1);
    WriteU8(avcCBuf, SPS.empty() ? 0x42 : SPS[1]);
    WriteU8(avcCBuf, SPS.empty() ? 0x00 : SPS[2]);
    WriteU8(avcCBuf, SPS.empty() ? 0x1F : SPS[3]);
    WriteU8(avcCBuf, 0xFF);
    WriteU8(avcCBuf, 0xE0 | (SPS.empty() ? 0 : 1));
    WriteU16(avcCBuf, (uint16_t)SPS.size());
    WriteBytes(avcCBuf, SPS.data(), SPS.size());
    WriteU8(avcCBuf, PPS.empty() ? 0 : 1);
    WriteU16(avcCBuf, (uint16_t)PPS.size());
    WriteBytes(avcCBuf, PPS.data(), PPS.size());
    stsdEntry.insert(stsdEntry.end(), avcCBuf.begin(), avcCBuf.end());

    uint32_t avc1EntrySize = 86 + avcCSize;
    {
        std::vector<uint8_t> stsdBox;
        uint32_t stsdSize = 16 + avc1EntrySize;
        WriteU32(stsdBox, stsdSize); WriteFourCC(stsdBox, "stsd");
        WriteU32(stsdBox, 0); WriteU32(stsdBox, 1);
        WriteU32(stsdBox, avc1EntrySize); WriteFourCC(stsdBox, "avc1");
        WriteBytes(stsdBox, stsdEntry.data(), stsdEntry.size());
        stblBuf.insert(stblBuf.end(), stsdBox.begin(), stsdBox.end());
    }

    // stts: 所有帧 duration = FrameDuration
    {
        std::vector<uint8_t> box;
        WriteU32(box, 16 + 8); WriteFourCC(box, "stts");
        WriteU32(box, 0); WriteU32(box, 1);  // 1 entry, all same duration
        WriteU32(box, N); WriteU32(box, FrameDuration);
        stblBuf.insert(stblBuf.end(), box.begin(), box.end());
    }

    // stsz: 每帧大小
    {
        std::vector<uint8_t> box;
        WriteU32(box, 20 + N * 4); WriteFourCC(box, "stsz");
        WriteU32(box, 0); WriteU32(box, 0);  // sample_size=0 (variable)
        WriteU32(box, N);
        for (auto& f : AllFrames)
            WriteU32(box, (uint32_t)f.Avc1Data.size());
        stblBuf.insert(stblBuf.end(), box.begin(), box.end());
    }

    // stsc: 全部帧在一个 chunk 中
    {
        std::vector<uint8_t> box;
        WriteU32(box, 16 + 12); WriteFourCC(box, "stsc");
        WriteU32(box, 0); WriteU32(box, 1);  // 1 entry
        WriteU32(box, 1);     // first_chunk
        WriteU32(box, N);     // samples_per_chunk
        WriteU32(box, 1);     // sample_description_index
        stblBuf.insert(stblBuf.end(), box.begin(), box.end());
    }

    // stco: chunk offset (mdat 数据起始位置)
    // file layout: ftyp(24) + mdat_header(8) = 32
    {
        uint32_t mdatDataOffset = 24 + 8;
        std::vector<uint8_t> box;
        WriteU32(box, 16 + 4); WriteFourCC(box, "stco");
        WriteU32(box, 0); WriteU32(box, 1);  // 1 entry
        WriteU32(box, mdatDataOffset);
        stblBuf.insert(stblBuf.end(), box.begin(), box.end());
    }

    // stss: 关键帧索引表
    if (!syncSamples.empty())
    {
        std::vector<uint8_t> box;
        WriteU32(box, 16 + (uint32_t)syncSamples.size() * 4);
        WriteFourCC(box, "stss");
        WriteU32(box, 0);
        WriteU32(box, (uint32_t)syncSamples.size());
        for (auto idx : syncSamples)
            WriteU32(box, idx);
        stblBuf.insert(stblBuf.end(), box.begin(), box.end());
    }

    WriteU32(minfBuf, 8 + (uint32_t)stblBuf.size());
    WriteFourCC(minfBuf, "stbl");
    minfBuf.insert(minfBuf.end(), stblBuf.begin(), stblBuf.end());

    WriteU32(mdiaBuf, 8 + (uint32_t)minfBuf.size());
    WriteFourCC(mdiaBuf, "minf");
    mdiaBuf.insert(mdiaBuf.end(), minfBuf.begin(), minfBuf.end());

    WriteU32(trakBuf, 8 + (uint32_t)mdiaBuf.size());
    WriteFourCC(trakBuf, "mdia");
    trakBuf.insert(trakBuf.end(), mdiaBuf.begin(), mdiaBuf.end());

    // --- 包装 trak ---
    uint32_t trakSize = 8 + (uint32_t)trakBuf.size();
    std::vector<uint8_t> trakBox;
    WriteU32(trakBox, trakSize); WriteFourCC(trakBox, "trak");
    trakBox.insert(trakBox.end(), trakBuf.begin(), trakBuf.end());

    // --- mvhd ---
    std::vector<uint8_t> mvhdBuf;
    WriteU32(mvhdBuf, 108); WriteFourCC(mvhdBuf, "mvhd");
    WriteU32(mvhdBuf, 0); WriteU32(mvhdBuf, 0); WriteU32(mvhdBuf, 0);
    WriteU32(mvhdBuf, Timescale);
    WriteU32(mvhdBuf, N * FrameDuration);
    WriteU32(mvhdBuf, 0x00010000); WriteU16(mvhdBuf, 0x0100); WriteU16(mvhdBuf, 0);
    WriteZero(mvhdBuf, 8);
    WriteU32(mvhdBuf, 0x00010000); WriteZero(mvhdBuf, 4);
    WriteZero(mvhdBuf, 4); WriteZero(mvhdBuf, 4);
    WriteU32(mvhdBuf, 0x00010000); WriteZero(mvhdBuf, 4);
    WriteZero(mvhdBuf, 4); WriteZero(mvhdBuf, 4);
    WriteU32(mvhdBuf, 0x40000000);
    WriteZero(mvhdBuf, 24); WriteU32(mvhdBuf, 2);

    // --- 组装 moov ---
    uint32_t moovSize = 8 + (uint32_t)mvhdBuf.size() + (uint32_t)trakBox.size();
    std::vector<uint8_t> moovBuf;
    WriteU32(moovBuf, moovSize); WriteFourCC(moovBuf, "moov");
    moovBuf.insert(moovBuf.end(), mvhdBuf.begin(), mvhdBuf.end());
    moovBuf.insert(moovBuf.end(), trakBox.begin(), trakBox.end());

    FWrite(moovBuf, OutputFile);
}
