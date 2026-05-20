#include "MP4Writer.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

MP4Writer::MP4Writer() = default;

MP4Writer::~MP4Writer()
{
    Destroy();
}

bool MP4Writer::Create(const char* OutputPath, int32 InWidth, int32 InHeight, int32 InFPS)
{
    Width = InWidth;
    Height = InHeight;
    FPS = InFPS;
    Timescale = static_cast<uint32_t>(FPS) * 1000;
    FrameDuration = 1000;
    TotalFrames = 0;
    bHeadersExtracted = false;
    SPS.clear();
    PPS.clear();
    Samples.clear();
    MdatStartPos = 0;
    MdatDataSize = 0;

    File = fopen(OutputPath, "wb");
    if (!File) return false;

    WriteFtyp();

    // 预留 mdat 头位置（8 字节），写占位
    MdatStartPos = static_cast<int64_t>(ftell(File));
    uint8_t placeholder[8] = {};
    fwrite(placeholder, 1, 8, File);

    return true;
}

bool MP4Writer::WriteFrame(const uint8_t* AnnexBData, size_t Size, int64_t /*PTS*/, bool bIsKeyFrame)
{
    if (!File) return false;

    if (!bHeadersExtracted && bIsKeyFrame)
        ExtractHeaders(AnnexBData, Size);

    std::vector<uint8_t> avc1 = ConvertToAvc1(AnnexBData, Size);
    if (avc1.empty()) return true;

    size_t written = fwrite(avc1.data(), 1, avc1.size(), File);
    if (written != avc1.size()) return false;

    SampleInfo sample;
    sample.Size      = static_cast<uint32_t>(avc1.size());
    sample.Offset    = static_cast<uint32_t>(MdatStartPos + 8 + MdatDataSize);
    sample.Duration   = FrameDuration;
    sample.bKeyFrame  = bIsKeyFrame;
    Samples.push_back(sample);

    MdatDataSize += static_cast<int64_t>(avc1.size());
    ++TotalFrames;

    return true;
}

bool MP4Writer::Finalize()
{
    if (!File) return false;

    // 回写 mdat 头（MP4 要求大端序）
    uint32_t mdatTotalSize = 8 + static_cast<uint32_t>(MdatDataSize);
    uint8_t  sizeBE[4] = {
        static_cast<uint8_t>((mdatTotalSize >> 24) & 0xFF),
        static_cast<uint8_t>((mdatTotalSize >> 16) & 0xFF),
        static_cast<uint8_t>((mdatTotalSize >> 8) & 0xFF),
        static_cast<uint8_t>(mdatTotalSize & 0xFF)
    };
    fseek(File, MdatStartPos, SEEK_SET);
    fwrite(sizeBE, 1, 4, File);
    fwrite("mdat", 1, 4, File);

    // 跳到文件末尾写 moov
    fseek(File, 0, SEEK_END);
    WriteMoov();

    fclose(File);
    File = nullptr;

    return true;
}

void MP4Writer::Destroy()
{
    if (File)
    {
        fclose(File);
        File = nullptr;
    }
    Samples.clear();
    SPS.clear();
    PPS.clear();
}

// ============================================================================
// SPS/PPS 提取（复用已有逻辑）
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

// 去除 NAL 末尾可能包含的下一起始码前缀字节
static size_t TrimTrailingStartCode(const uint8_t* Data, size_t NaluStart, size_t NaluEnd)
{
    size_t len = NaluEnd - NaluStart;
    if (len >= 4)
    {
        const uint8_t* p = Data + NaluEnd - 4;
        if (p[0] == 0x00 && p[1] == 0x00 && (p[2] == 0x01 || (p[2] == 0x00 && p[3] == 0x01)))
            len -= 4;
    }
    else if (len >= 3)
    {
        const uint8_t* p = Data + NaluEnd - 3;
        if (p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01)
            len -= 3;
    }
    return len;
}

void MP4Writer::ExtractHeaders(const uint8_t* Data, size_t Size)
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
            size_t len = TrimTrailingStartCode(Data, naluStart, naluEnd);
            if (type == 7) SPS.assign(Data + naluStart, Data + naluStart + len);
            if (type == 8) PPS.assign(Data + naluStart, Data + naluStart + len);
        }
        pos = naluStart + TrimTrailingStartCode(Data, naluStart, naluEnd);
        if (pos >= Size) break;
    }
}

// ============================================================================
// Annex B → avc1 转换（SPS/PPS→avcC，AUD→丢弃，SEI/Filler 保留）
// ============================================================================

std::vector<uint8_t> MP4Writer::ConvertToAvc1(const uint8_t* Data, size_t Size)
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
        size_t len = TrimTrailingStartCode(Data, naluStart, naluEnd);

        if (type == 7 || type == 8 || type == 9)  // SPS/PPS→avcC, AUD→丢弃
        {
            pos = naluStart + len;
            continue;
        }

        WriteU32(avc1, static_cast<uint32_t>(len));
        WriteBytes(avc1, Data + naluStart, len);
        pos = naluStart + len;
    }

    return avc1;
}

// ============================================================================
// Box 写入辅助
// ============================================================================

void MP4Writer::WriteU32(std::vector<uint8_t>& Buf, uint32_t V)
{
    Buf.push_back(static_cast<uint8_t>((V >> 24) & 0xFF));
    Buf.push_back(static_cast<uint8_t>((V >> 16) & 0xFF));
    Buf.push_back(static_cast<uint8_t>((V >> 8) & 0xFF));
    Buf.push_back(static_cast<uint8_t>(V & 0xFF));
}

void MP4Writer::WriteU16(std::vector<uint8_t>& Buf, uint16_t V)
{
    Buf.push_back(static_cast<uint8_t>((V >> 8) & 0xFF));
    Buf.push_back(static_cast<uint8_t>(V & 0xFF));
}

void MP4Writer::WriteU8(std::vector<uint8_t>& Buf, uint8_t V)
{
    Buf.push_back(V);
}

void MP4Writer::WriteFourCC(std::vector<uint8_t>& Buf, const char* CC)
{
    Buf.insert(Buf.end(), CC, CC + 4);
}

void MP4Writer::WriteBytes(std::vector<uint8_t>& Buf, const uint8_t* Data, size_t Len)
{
    Buf.insert(Buf.end(), Data, Data + Len);
}

void MP4Writer::WriteZero(std::vector<uint8_t>& Buf, size_t Len)
{
    Buf.resize(Buf.size() + Len, 0);
}

static void FWrite(const std::vector<uint8_t>& Buf, FILE* f)
{
    fwrite(Buf.data(), 1, Buf.size(), f);
}

// ============================================================================
// ftyp box
// ============================================================================

void MP4Writer::WriteFtyp()
{
    std::vector<uint8_t> buf;
    WriteU32(buf, 24);
    WriteFourCC(buf, "ftyp");
    WriteFourCC(buf, "isom");
    WriteU32(buf, 0x00000200);
    WriteFourCC(buf, "isom");
    WriteFourCC(buf, "avc1");
    FWrite(buf, File);
}

// ============================================================================
// moov box（含完整 stts/stsz/stsc/stco/stss/stsd）
// ============================================================================

void MP4Writer::WriteMoov()
{
    uint32_t N = static_cast<uint32_t>(Samples.size());
    uint32_t TrackID = 1;

    // ---- Write moov with actual sizes ----
    // Build all leaf boxes first, then compute container sizes from actual data

    // ---- stsd (avc1 + avcC) ----
    std::vector<uint8_t> avcCBuf;
    WriteAvcC(avcCBuf);

    std::vector<uint8_t> stsd;
    uint32_t avc1EntrySize = 86 + static_cast<uint32_t>(avcCBuf.size());
    uint32_t stsdSize = 16 + avc1EntrySize;
    WriteU32(stsd, stsdSize);
    WriteFourCC(stsd, "stsd");
    WriteU32(stsd, 0);
    WriteU32(stsd, 1);
    WriteU32(stsd, avc1EntrySize);
    WriteFourCC(stsd, "avc1");
    WriteZero(stsd, 6);
    WriteU16(stsd, 1);
    WriteU16(stsd, 0);
    WriteU16(stsd, 0);
    WriteU32(stsd, 0);
    WriteU32(stsd, 0); WriteU32(stsd, 0);
    WriteU16(stsd, Width);
    WriteU16(stsd, Height);
    WriteU32(stsd, 0x00480000);
    WriteU32(stsd, 0x00480000);
    WriteU32(stsd, 0);
    WriteU16(stsd, 1);
    WriteZero(stsd, 32);
    WriteU16(stsd, 0x0018);
    WriteU16(stsd, 0xFFFF);
    WriteBytes(stsd, avcCBuf.data(), avcCBuf.size());

    // ---- stts ----
    std::vector<uint8_t> stts;
    WriteU32(stts, 24); WriteFourCC(stts, "stts");
    WriteU32(stts, 0);
    WriteU32(stts, 1);
    WriteU32(stts, N);
    WriteU32(stts, FrameDuration);

    // ---- stsz ----
    std::vector<uint8_t> stsz;
    WriteU32(stsz, 20 + N * 4); WriteFourCC(stsz, "stsz");
    WriteU32(stsz, 0);
    WriteU32(stsz, 0);
    WriteU32(stsz, N);
    for (auto& s : Samples) WriteU32(stsz, s.Size);

    // ---- stsc ----
    std::vector<uint8_t> stsc;
    WriteU32(stsc, 16 + 12); WriteFourCC(stsc, "stsc");
    WriteU32(stsc, 0);
    WriteU32(stsc, 1);  // entry_count
    WriteU32(stsc, 1);  // first_chunk
    WriteU32(stsc, 1);  // samples_per_chunk (1 sample per chunk)
    WriteU32(stsc, 1);  // sample_description_index

    // ---- stco ----
    std::vector<uint8_t> stco;
    WriteU32(stco, 16 + N * 4); WriteFourCC(stco, "stco");
    WriteU32(stco, 0);
    WriteU32(stco, N);
    for (auto& s : Samples) WriteU32(stco, s.Offset);

    // ---- stss ----
    std::vector<uint8_t> stss;
    uint32_t keyframeCount = 0;
    for (auto& s : Samples) if (s.bKeyFrame) ++keyframeCount;
    WriteU32(stss, 16 + keyframeCount * 4); WriteFourCC(stss, "stss");
    WriteU32(stss, 0);
    WriteU32(stss, keyframeCount);
    for (uint32_t i = 0; i < N; ++i)
        if (Samples[i].bKeyFrame) WriteU32(stss, i + 1);

    // ---- stbl (size computed from actual children) ----
    std::vector<uint8_t> stblChildren;
    stblChildren.insert(stblChildren.end(), stsd.begin(), stsd.end());
    stblChildren.insert(stblChildren.end(), stts.begin(), stts.end());
    stblChildren.insert(stblChildren.end(), stsz.begin(), stsz.end());
    stblChildren.insert(stblChildren.end(), stsc.begin(), stsc.end());
    stblChildren.insert(stblChildren.end(), stco.begin(), stco.end());
    stblChildren.insert(stblChildren.end(), stss.begin(), stss.end());
    uint32_t stblSize = 8 + static_cast<uint32_t>(stblChildren.size());

    std::vector<uint8_t> stbl;
    WriteU32(stbl, stblSize);
    WriteFourCC(stbl, "stbl");
    stbl.insert(stbl.end(), stblChildren.begin(), stblChildren.end());

    // ---- vmhd ----
    std::vector<uint8_t> vmhd;
    WriteU32(vmhd, 20); WriteFourCC(vmhd, "vmhd");
    WriteU32(vmhd, 1);
    WriteU16(vmhd, 0); WriteZero(vmhd, 6);

    // ---- dref (inside dinf) ----
    std::vector<uint8_t> dref;
    WriteU32(dref, 28); WriteFourCC(dref, "dref");
    WriteU32(dref, 0);
    WriteU32(dref, 1);
    WriteU32(dref, 12); WriteFourCC(dref, "url ");
    WriteU32(dref, 0x00000001);

    // ---- dinf (contains dref) ----
    std::vector<uint8_t> dinf;
    WriteU32(dinf, 8 + static_cast<uint32_t>(dref.size())); WriteFourCC(dinf, "dinf");
    dinf.insert(dinf.end(), dref.begin(), dref.end());

    // ---- minf (size from actual children) ----
    std::vector<uint8_t> minfChildren;
    minfChildren.insert(minfChildren.end(), vmhd.begin(), vmhd.end());
    minfChildren.insert(minfChildren.end(), dinf.begin(), dinf.end());
    minfChildren.insert(minfChildren.end(), stbl.begin(), stbl.end());
    uint32_t minfSize = 8 + static_cast<uint32_t>(minfChildren.size());

    std::vector<uint8_t> minf;
    WriteU32(minf, minfSize);
    WriteFourCC(minf, "minf");
    minf.insert(minf.end(), minfChildren.begin(), minfChildren.end());

    // ---- mdhd ----
    std::vector<uint8_t> mdhd;
    WriteU32(mdhd, 32); WriteFourCC(mdhd, "mdhd");
    WriteU32(mdhd, 0); WriteU32(mdhd, 0); WriteU32(mdhd, 0);
    WriteU32(mdhd, Timescale);
    WriteU32(mdhd, N * FrameDuration);
    WriteU16(mdhd, 0x55C4);
    WriteU16(mdhd, 0);

    // ---- hdlr ----
    std::vector<uint8_t> hdlr;
    WriteU32(hdlr, 45); WriteFourCC(hdlr, "hdlr");
    WriteU32(hdlr, 0); WriteU32(hdlr, 0);
    WriteFourCC(hdlr, "vide"); WriteZero(hdlr, 12);
    WriteBytes(hdlr, reinterpret_cast<const uint8_t*>("VideoHandler"), 12);
    WriteU8(hdlr, 0);

    // ---- mdia (size from actual children) ----
    std::vector<uint8_t> mdiaChildren;
    mdiaChildren.insert(mdiaChildren.end(), mdhd.begin(), mdhd.end());
    mdiaChildren.insert(mdiaChildren.end(), hdlr.begin(), hdlr.end());
    mdiaChildren.insert(mdiaChildren.end(), minf.begin(), minf.end());
    uint32_t mdiaSize = 8 + static_cast<uint32_t>(mdiaChildren.size());

    std::vector<uint8_t> mdia;
    WriteU32(mdia, mdiaSize);
    WriteFourCC(mdia, "mdia");
    mdia.insert(mdia.end(), mdiaChildren.begin(), mdiaChildren.end());

    // ---- tkhd ----
    std::vector<uint8_t> tkhd;
    WriteU32(tkhd, 92); WriteFourCC(tkhd, "tkhd");
    WriteU32(tkhd, 7);
    WriteU32(tkhd, 0); WriteU32(tkhd, 0);
    WriteU32(tkhd, TrackID); WriteU32(tkhd, 0);
    WriteU32(tkhd, N * FrameDuration);
    WriteZero(tkhd, 8);
    WriteU16(tkhd, 0); WriteU16(tkhd, 0);
    WriteU16(tkhd, 0x0100); WriteU16(tkhd, 0);
    WriteU32(tkhd, 0x00010000); WriteZero(tkhd, 4);
    WriteZero(tkhd, 4); WriteU32(tkhd, 0x00010000);
    WriteZero(tkhd, 16); WriteU32(tkhd, 0x40000000);
    WriteU32(tkhd, Width  * 0x10000);
    WriteU32(tkhd, Height * 0x10000);

    // ---- trak (size from actual children) ----
    std::vector<uint8_t> trakChildren;
    trakChildren.insert(trakChildren.end(), tkhd.begin(), tkhd.end());
    trakChildren.insert(trakChildren.end(), mdia.begin(), mdia.end());
    uint32_t trakSize = 8 + static_cast<uint32_t>(trakChildren.size());

    std::vector<uint8_t> trak;
    WriteU32(trak, trakSize);
    WriteFourCC(trak, "trak");
    trak.insert(trak.end(), trakChildren.begin(), trakChildren.end());

    // ---- mvhd ----
    std::vector<uint8_t> mvhd;
    WriteU32(mvhd, 108); WriteFourCC(mvhd, "mvhd");
    WriteU32(mvhd, 0); WriteU32(mvhd, 0); WriteU32(mvhd, 0);
    WriteU32(mvhd, Timescale);
    WriteU32(mvhd, N * FrameDuration);
    WriteU32(mvhd, 0x00010000);
    WriteU16(mvhd, 0x0100); WriteU16(mvhd, 0);
    WriteZero(mvhd, 8);
    WriteU32(mvhd, 0x00010000); WriteZero(mvhd, 4);
    WriteZero(mvhd, 4); WriteU32(mvhd, 0x00010000);
    WriteZero(mvhd, 12); WriteU32(mvhd, 0x40000000);
    WriteZero(mvhd, 28);  // matrix[8] + pre_defined[0..5] = 4 + 6*4 = 28
    WriteU32(mvhd, 2);

    // ---- moov (size from actual children, fixing container truncation) ----
    std::vector<uint8_t> moovChildren;
    moovChildren.insert(moovChildren.end(), mvhd.begin(), mvhd.end());
    moovChildren.insert(moovChildren.end(), trak.begin(), trak.end());
    uint32_t moovSize = 8 + static_cast<uint32_t>(moovChildren.size());

    std::vector<uint8_t> moov;
    WriteU32(moov, moovSize);
    WriteFourCC(moov, "moov");
    moov.insert(moov.end(), moovChildren.begin(), moovChildren.end());

    FWrite(moov, File);
}

// ============================================================================
// avcC (AVC Decoder Configuration Record)
// ============================================================================

void MP4Writer::WriteAvcC(std::vector<uint8_t>& Out)
{
    uint32_t avcCSize = 19 + static_cast<uint32_t>(SPS.size() + PPS.size());
    WriteU32(Out, avcCSize);
    WriteFourCC(Out, "avcC");
    WriteU8(Out, 1); // configurationVersion
    WriteU8(Out, SPS.empty() ? 0x42 : SPS[1]); // AVCProfileIndication
    WriteU8(Out, SPS.empty() ? 0x00 : SPS[2]); // profile_compatibility
    WriteU8(Out, SPS.empty() ? 0x1F : SPS[3]); // AVCLevelIndication
    WriteU8(Out, 0xFF); // lengthSizeMinusOne (4字节 NAL 长度)
    WriteU8(Out, 0xE1); // numOfSequenceParameterSets (1)
    WriteU16(Out, static_cast<uint16_t>(SPS.size()));
    WriteBytes(Out, SPS.data(), SPS.size());
    WriteU8(Out, 0x01); // numOfPictureParameterSets (1)
    WriteU16(Out, static_cast<uint16_t>(PPS.size()));
    WriteBytes(Out, PPS.data(), PPS.size());
}
