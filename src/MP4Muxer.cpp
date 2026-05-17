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
// 开始录制：打开文件 → ftyp → moov(mvex)
// ============================================================================

bool MP4Muxer::Start(const char* OutputPath, int32 InWidth, int32 InHeight)
{
    Width  = InWidth;
    Height = InHeight;
    TotalFrames = 0;
    FragmentSequence = 0;
    bHeadersExtracted = false;
    SPS.clear();
    PPS.clear();
    PendingFrames.clear();

    OutputFile = fopen(OutputPath, "wb");
    if (!OutputFile) return false;

    WriteFtyp();
    WriteMoov();
    fflush(OutputFile);
    return true;
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
// 刷写 fragment
// ============================================================================

void MP4Muxer::FlushFragment()
{
    if (PendingFrames.empty()) return;
    WriteMoofAndMdat();
    PendingFrames.clear();
    fflush(OutputFile);
}

// ============================================================================
// 完成
// ============================================================================

bool MP4Muxer::Finish()
{
    FlushFragment();
    if (OutputFile)
    {
        fclose(OutputFile);
        OutputFile = nullptr;
    }
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
        size_t naluEnd   = FindStartCode(Data, naluStart, Size);
        if (naluStart < naluEnd && naluStart < Size)
        {
            uint8_t type = Data[naluStart] & 0x1F;
            size_t len = naluEnd - naluStart;
            if (type == 7) SPS.assign(Data + naluStart, Data + naluStart + len);
            if (type == 8) PPS.assign(Data + naluStart, Data + naluStart + len);
        }
        pos = naluEnd;
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
        size_t naluEnd   = FindStartCode(Data, naluStart, Size);
        if (naluStart >= naluEnd || naluStart >= Size) break;

        uint8_t type = Data[naluStart] & 0x1F;
        size_t len = naluEnd - naluStart;

        // SPS/PPS/AUD/SEI 不放入 mdat
        if (type == 7 || type == 8 || type == 9 || type == 6) { pos = naluEnd; continue; }

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

// ---- fwrite 快捷 ----
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
// moov box（fMP4 版本：含 mvex，不含 stts/stsz/stsc/stco）
// ============================================================================

void MP4Muxer::WriteMoov()
{
    std::vector<uint8_t> trakBuf;
    {
        // --- tkhd ---
        WriteU32(trakBuf, 92);
        WriteFourCC(trakBuf, "tkhd");
        WriteU32(trakBuf, 0x00000007);
        WriteU32(trakBuf, 0); WriteU32(trakBuf, 0);
        WriteU32(trakBuf, TrackID); WriteU32(trakBuf, 0);
        WriteU32(trakBuf, 0); // duration = 0 for fMP4
        WriteZero(trakBuf, 8);
        WriteU16(trakBuf, 0); WriteU16(trakBuf, 0);
        WriteU16(trakBuf, 0x0100); WriteU16(trakBuf, 0);
        WriteU32(trakBuf, 0x00010000); WriteZero(trakBuf, 4);
        WriteZero(trakBuf, 4); WriteU32(trakBuf, 0x00010000);
        WriteZero(trakBuf, 12); WriteU32(trakBuf, 0x40000000);
        WriteU32(trakBuf, Width*0x10000); WriteU32(trakBuf, Height*0x10000);

        // --- mdia ---
        std::vector<uint8_t> mdiaBuf;
        WriteU32(mdiaBuf, 32); WriteFourCC(mdiaBuf, "mdhd");
        WriteU32(mdiaBuf, 0); WriteU32(mdiaBuf, 0); WriteU32(mdiaBuf, 0);
        WriteU32(mdiaBuf, Timescale); WriteU32(mdiaBuf, 0);
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

        // --- stbl (仅 stsd) ---
        std::vector<uint8_t> stblBuf;
        WriteAvcC(stblBuf); // 先构建 avcC，得到大小
        // 注意 WriteAvcC 写入了完整的 avcC box (header+data)，需要提取 payload 大小
        // 在这里我们直接在 stsd 中构建
        // 重新构建 stsd（由于 MP4Muxer 的设计问题，我们在此直接写）

        std::vector<uint8_t> stsdBuf; // 我们要构建 stsd 内的 avc1 entry
        // avc1 entry header (86 bytes)
        WriteZero(stsdBuf, 6);
        WriteU16(stsdBuf, 1);
        WriteU16(stsdBuf, 0); WriteU16(stsdBuf, 0);
        WriteZero(stsdBuf, 12);
        WriteU16(stsdBuf, Width);
        WriteU16(stsdBuf, Height);
        WriteU32(stsdBuf, 0x00480000);
        WriteU32(stsdBuf, 0x00480000);
        WriteU32(stsdBuf, 0);
        WriteU16(stsdBuf, 1);
        WriteZero(stsdBuf, 32);
        WriteU16(stsdBuf, 0x0018);
        WriteU16(stsdBuf, 0xFFFF);

        // avcC
        std::vector<uint8_t> avcCBuf;
        uint32_t avcCSize = 19 + (uint32_t)(SPS.size() + PPS.size());
        WriteU32(avcCBuf, avcCSize);
        WriteFourCC(avcCBuf, "avcC");
        WriteU8(avcCBuf, 1);
        WriteU8(avcCBuf, SPS.empty() ? 0x42 : SPS[1]);
        WriteU8(avcCBuf, SPS.empty() ? 0x00 : SPS[2]);
        WriteU8(avcCBuf, SPS.empty() ? 0x1F : SPS[3]);
        WriteU8(avcCBuf, 0xFF);
        WriteU8(avcCBuf, 0xE1);
        WriteU16(avcCBuf, (uint16_t)SPS.size());
        WriteBytes(avcCBuf, SPS.data(), SPS.size());
        WriteU8(avcCBuf, 0x01);
        WriteU16(avcCBuf, (uint16_t)PPS.size());
        WriteBytes(avcCBuf, PPS.data(), PPS.size());

        stsdBuf.insert(stsdBuf.end(), avcCBuf.begin(), avcCBuf.end());

        uint32_t avc1EntrySize = 86 + avcCSize;
        uint32_t stsdSize = 16 + avc1EntrySize;

        WriteU32(stblBuf, stsdSize);
        WriteFourCC(stblBuf, "stsd");
        WriteU32(stblBuf, 0);
        WriteU32(stblBuf, 1);
        WriteU32(stblBuf, avc1EntrySize);
        WriteFourCC(stblBuf, "avc1");
        WriteBytes(stblBuf, stsdBuf.data(), stsdBuf.size());

        WriteU32(minfBuf, 8 + (uint32_t)stblBuf.size());
        WriteFourCC(minfBuf, "stbl");
        minfBuf.insert(minfBuf.end(), stblBuf.begin(), stblBuf.end());

        WriteU32(mdiaBuf, 8 + (uint32_t)minfBuf.size());
        WriteFourCC(mdiaBuf, "minf");
        mdiaBuf.insert(mdiaBuf.end(), minfBuf.begin(), minfBuf.end());

        WriteU32(trakBuf, 8 + (uint32_t)mdiaBuf.size());
        WriteFourCC(trakBuf, "mdia");
        trakBuf.insert(trakBuf.end(), mdiaBuf.begin(), mdiaBuf.end());
    }

    // --- mvex ---
    std::vector<uint8_t> mvexBuf;
    WriteU32(mvexBuf, 16); WriteFourCC(mvexBuf, "mehd");
    WriteU32(mvexBuf, 0);
    WriteU32(mvexBuf, 0); // fragment_duration = 0 (unknown)

    WriteU32(mvexBuf, 32); WriteFourCC(mvexBuf, "trex");
    WriteU32(mvexBuf, 0);
    WriteU32(mvexBuf, TrackID);
    WriteU32(mvexBuf, 1);    // default_sample_description_index
    WriteU32(mvexBuf, FrameDuration); // default_sample_duration
    WriteU32(mvexBuf, 0);    // default_sample_size
    WriteU32(mvexBuf, 0);    // default_sample_flags

    WriteU32(trakBuf, 8 + (uint32_t)mvexBuf.size());
    WriteFourCC(trakBuf, "mvex");
    trakBuf.insert(trakBuf.end(), mvexBuf.begin(), mvexBuf.end());

    // --- mvhd ---
    std::vector<uint8_t> mvhdBuf;
    WriteU32(mvhdBuf, 108); WriteFourCC(mvhdBuf, "mvhd");
    WriteU32(mvhdBuf, 0); WriteU32(mvhdBuf, 0); WriteU32(mvhdBuf, 0);
    WriteU32(mvhdBuf, Timescale);
    WriteU32(mvhdBuf, 0); // duration = 0 for fMP4
    WriteU32(mvhdBuf, 0x00010000); WriteU16(mvhdBuf, 0x0100); WriteU16(mvhdBuf, 0);
    WriteZero(mvhdBuf, 8);
    WriteU32(mvhdBuf, 0x00010000); WriteZero(mvhdBuf, 4);
    WriteZero(mvhdBuf, 4); WriteU32(mvhdBuf, 0x00010000);
    WriteZero(mvhdBuf, 12); WriteU32(mvhdBuf, 0x40000000);
    WriteZero(mvhdBuf, 24); WriteU32(mvhdBuf, 2);

    // --- 组装 moov ---
    uint32_t moovSize = 8 + (uint32_t)mvhdBuf.size() + (uint32_t)trakBuf.size();
    std::vector<uint8_t> moovBuf;
    WriteU32(moovBuf, moovSize); WriteFourCC(moovBuf, "moov");
    moovBuf.insert(moovBuf.end(), mvhdBuf.begin(), mvhdBuf.end());
    moovBuf.insert(moovBuf.end(), trakBuf.begin(), trakBuf.end());

    FWrite(moovBuf, OutputFile);
}

// ============================================================================
// avcC（在 WriteMoov 中内联使用）
// ============================================================================

void MP4Muxer::WriteAvcC(std::vector<uint8_t>& Out)
{
    (void)Out; // 不使用，在 WriteMoov 中直接构建
}

// ============================================================================
// moof + mdat（写入一个 fragment）
// ============================================================================

void MP4Muxer::WriteMoofAndMdat()
{
    uint32_t N = (uint32_t)PendingFrames.size();
    if (N == 0) return;

    // 计算 mdat 数据大小
    uint32_t mdatDataSize = 0;
    for (auto& f : PendingFrames)
        mdatDataSize += (uint32_t)f.Avc1Data.size();

    // 计算 baseMediaDecodeTime（在 timescale 单位中）
    // 本 fragment 的第一帧的全局帧索引 = TotalFrames - N（AddFrame 已递增 TotalFrames）
    uint32_t firstFrameGlobal = TotalFrames - N;
    uint32_t baseDecodeTime = firstFrameGlobal * FrameDuration;

    // --- tfhd ---
    std::vector<uint8_t> tfhd;
    WriteU32(tfhd, 16); // size: 16 (without base_data_offset)
    WriteFourCC(tfhd, "tfhd");
    WriteU32(tfhd, 0);  // flags=0
    WriteU32(tfhd, TrackID);

    // --- tfdt (version 0) ---
    std::vector<uint8_t> tfdt;
    WriteU32(tfdt, 16);
    WriteFourCC(tfdt, "tfdt");
    WriteU32(tfdt, 0);
    WriteU32(tfdt, baseDecodeTime);

    // --- trun ---
    // flags: 0x000005 = data_offset_present | first_sample_flags_present
    // 不加 sample_duration/size 用 trex 默认值
    // 但实际上 duration 恒定，size 每帧不同需要显式列出。用 flags=0x000205
    // data_offset + first_sample_flags + sample_size
    std::vector<uint8_t> trun;
    uint32_t trunSize = 8 + 4 + 4 + 4 + 4 + N * 4; // header + data_offset + flags + count + N*sizes
    WriteU32(trun, trunSize);
    WriteFourCC(trun, "trun");
    WriteU32(trun, 0x000205); // version=0, flags: data_offset + first_flags + sample_size
    WriteU32(trun, N);        // sample_count

    // data_offset: moof 结束后到 mdat 数据的偏移
    // moof size = 8 + mfhd(16) + traf(8+tfhd+tfdt+trun)
    uint32_t trafSize = 8 + (uint32_t)tfhd.size() + (uint32_t)tfdt.size() + (uint32_t)trunSize;
    uint32_t mfhdSize = 16;
    uint32_t moofSize = 8 + mfhdSize + trafSize;
    WriteU32(trun, moofSize + 8); // mdat 数据从 moof 结尾后 8 字节开始（8 = mdat header）

    // first_sample_flags: keyframe 标记
    uint32_t firstFlags = 0;
    if (!PendingFrames.empty() && PendingFrames[0].bKeyFrame)
        firstFlags = 0x02000000; // sample_is_non_sync_sample = 0 → 关键帧（不设就是非关键帧）
    // 实际上 ISOBMFF 的 sample_flags: bit 16 是 sample_depends_on, 值 2 = depends on others
    // 对于 IDR: flags = 0x02000000 (sample_depends_on=2, is_leading=0, ...)
    // Wait, the correct flag for sync sample is 0x02000000 which means "does not depend on others"
    // For non-sync frames, it's 0x01010000
    if (firstFlags == 0)
        firstFlags = 0x01010000; // depends on others (non-keyframe)
    else
        firstFlags = 0x02000000;
    WriteU32(trun, firstFlags);

    // 每帧的 size
    for (auto& f : PendingFrames)
        WriteU32(trun, (uint32_t)f.Avc1Data.size());

    // --- mfhd ---
    std::vector<uint8_t> mfhd;
    WriteU32(mfhd, 16);
    WriteFourCC(mfhd, "mfhd");
    WriteU32(mfhd, 0);
    WriteU32(mfhd, FragmentSequence++);

    // --- 组装 traf ---
    std::vector<uint8_t> traf;
    WriteU32(traf, trafSize);
    WriteFourCC(traf, "traf");
    traf.insert(traf.end(), tfhd.begin(), tfhd.end());
    traf.insert(traf.end(), tfdt.begin(), tfdt.end());
    traf.insert(traf.end(), trun.begin(), trun.end());

    // --- 组装 moof ---
    std::vector<uint8_t> moof;
    WriteU32(moof, moofSize);
    WriteFourCC(moof, "moof");
    moof.insert(moof.end(), mfhd.begin(), mfhd.end());
    moof.insert(moof.end(), traf.begin(), traf.end());

    // --- mdat ---
    std::vector<uint8_t> mdat;
    WriteU32(mdat, 8 + mdatDataSize);
    WriteFourCC(mdat, "mdat");
    for (auto& f : PendingFrames)
        WriteBytes(mdat, f.Avc1Data.data(), f.Avc1Data.size());

    // 写入文件
    FWrite(moof, OutputFile);
    FWrite(mdat, OutputFile);
}
