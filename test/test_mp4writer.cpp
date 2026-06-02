// ============================================================================
// MP4Writer 独立单元测试
// ============================================================================

#include "core/MP4Writer.h"
#include <iostream>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

#ifdef _WIN32
#define DeleteTestFile(p) DeleteFileA(p)
#else
#define DeleteTestFile(p) remove(p)
#endif
#include <cassert>

#ifdef _WIN32
#include <windows.h>
#endif

// ---- 位写入器（用于构造 H.264 NALU 中的 ue(v) 编码） ----
class BitWriter
{
    std::vector<uint8_t> Buf;
    int BitPos = 0; // 下一个 bit 写入位置（0=最高位）

public:
    void WriteBit(int b)
    {
        int byteIdx = BitPos / 8;
        int bitIdx  = 7 - (BitPos % 8);
        if (byteIdx >= (int)Buf.size()) Buf.push_back(0);
        if (b) Buf[byteIdx] |= (1 << bitIdx);
        ++BitPos;
    }

    void WriteUE(uint32_t v)
    {
        uint32_t m = v + 1;
        int leadingZeros = 0;
        uint32_t tmp = m;
        while (tmp > 1) { tmp >>= 1; ++leadingZeros; }
        for (int i = 0; i < leadingZeros; ++i) WriteBit(0);
        WriteBit(1);
        for (int i = leadingZeros - 1; i >= 0; --i) WriteBit((m >> i) & 1);
    }

    void WriteBits(uint32_t val, int n)
    {
        for (int i = n - 1; i >= 0; --i) WriteBit((val >> i) & 1);
    }

    void PadToByte()
    {
        WriteBit(1); // rbsp_stop_one_bit
        while (BitPos % 8 != 0) WriteBit(0);
    }

    const std::vector<uint8_t>& Data() const { return Buf; }
};

// ---- 构造合法的 H.264 SPS（1920x1080, Baseline Profile, Level 4.0） ----
static std::vector<uint8_t> BuildSPS(int width, int height)
{
    BitWriter w;

    // NAL header: forbidden=0, nal_ref_idc=3, nal_unit_type=7
    w.WriteBits(0x67, 8);

    // profile_idc = 66 (Baseline)
    w.WriteBits(66, 8);

    // constraint_set0_flag=1, others=0, reserved_zero_2bits=0
    w.WriteBits(0x80, 8);

    // level_idc = 40 (4.0)
    w.WriteBits(40, 8);

    // seq_parameter_set_id = 0
    w.WriteUE(0);

    // log2_max_frame_num_minus4 = 0 (max_frame_num = 16)
    w.WriteUE(0);

    // pic_order_cnt_type = 0
    w.WriteUE(0);

    // log2_max_pic_order_cnt_lsb_minus4 = 0
    w.WriteUE(0);

    // num_ref_frames = 1
    w.WriteUE(1);

    // gaps_in_frame_num_value_allowed_flag
    w.WriteBit(0);

    // pic_width_in_mbs_minus1 = (width / 16) - 1
    w.WriteUE(static_cast<uint32_t>(width / 16 - 1));

    // pic_height_in_map_units_minus1 = (height / 16) - 1
    w.WriteUE(static_cast<uint32_t>(height / 16 - 1));

    // frame_mbs_only_flag
    w.WriteBit(1);

    // direct_8x8_inference_flag
    w.WriteBit(0);

    // frame_cropping_flag
    w.WriteBit(0);

    // vui_parameters_present_flag
    w.WriteBit(0);

    w.PadToByte();
    return w.Data();
}

// ---- 构造合法的 H.264 PPS ----
static std::vector<uint8_t> BuildPPS()
{
    BitWriter w;

    // NAL header: nal_unit_type=8, nal_ref_idc=3
    w.WriteBits(0x68, 8);

    // pic_parameter_set_id = 0
    w.WriteUE(0);

    // seq_parameter_set_id = 0
    w.WriteUE(0);

    // entropy_coding_mode_flag = 0 (CAVLC)
    w.WriteBit(0);

    // bottom_field_pic_order_in_frame_present_flag = 0
    w.WriteBit(0);

    // num_slice_groups_minus1 = 0
    w.WriteUE(0);

    // num_ref_idx_l0_default_active_minus1 = 0
    w.WriteUE(0);

    // num_ref_idx_l1_default_active_minus1 = 0
    w.WriteUE(0);

    // weighted_pred_flag = 0
    w.WriteBit(0);

    // weighted_bipred_idc = 0
    w.WriteBits(0, 2);

    // pic_init_qp_minus26 = 0
    w.WriteUE(0); // signed, but we use 0

    // pic_init_qs_minus26 = 0
    w.WriteUE(0); // signed

    // chroma_qp_index_offset = 0
    w.WriteUE(0); // signed

    // deblocking_filter_control_present_flag = 1
    w.WriteBit(1);

    // constrained_intra_pred_flag = 0
    w.WriteBit(0);

    // redundant_pic_cnt_present_flag = 0
    w.WriteBit(0);

    w.PadToByte();
    return w.Data();
}

// ---- 构造一帧 Annex B 数据（包含 SPS+PPS+IDR 或 P 帧切片） ----
static std::vector<uint8_t> BuildAnnexBFrame(
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps,
    const std::vector<uint8_t>& sliceData,
    bool bIsIDR,
    bool bIncludeHeaders)
{
    std::vector<uint8_t> frame;
    const uint8_t sc[] = { 0x00, 0x00, 0x00, 0x01 };

    if (bIncludeHeaders)
    {
        frame.insert(frame.end(), sc, sc + 4);
        frame.insert(frame.end(), sps.begin(), sps.end());

        frame.insert(frame.end(), sc, sc + 4);
        frame.insert(frame.end(), pps.begin(), pps.end());
    }

    // Slice NAL
    frame.insert(frame.end(), sc, sc + 4);

    uint8_t naluType = bIsIDR ? 5 : 1;
    uint8_t naluHeader = (0 << 7) | (3 << 5) | naluType;
    frame.push_back(naluHeader);

    // 切片头：first_mb_in_slice=0, slice_type=7(IDR)/5(P), pic_parameter_set_id=0
    frame.push_back(0x88); // first_mb_in_slice ue(0), slice_type ue(7) for IDR
    if (!bIsIDR)
        frame.back() = 0x44; // slice_type ue(5)=2→"01101" + first_mb ue(0)="1" → some pattern

    // 切片数据：填充可识别模式
    frame.insert(frame.end(), sliceData.begin(), sliceData.end());

    return frame;
}

// ---- MP4 Box 解析器 ----
static bool FindBox(const char* name, FILE* f, int64_t* outSize, int64_t* outDataOff)
{
    fseek(f, 0, SEEK_SET);
    int64_t fsize = 0;
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char buf[4] = {};
    while (ftell(f) < fsize - 8)
    {
        uint8_t szBuf[4];
        if (fread(szBuf, 1, 4, f) != 4) return false;
        if (fread(buf, 1, 4, f) != 4) return false;

        uint32_t size = (uint32_t(szBuf[0]) << 24) | (uint32_t(szBuf[1]) << 16) |
                        (uint32_t(szBuf[2]) << 8)  | uint32_t(szBuf[3]);

        if (memcmp(buf, name, 4) == 0)
        {
            *outSize = size;
            *outDataOff = ftell(f);
            return true;
        }

        if (size < 8) break;
        if (size == 1) break; // 扩展大小，测试数据不会出现
        fseek(f, size - 8, SEEK_CUR);
    }
    return false;
}

// ---- 从 avcC 中读取 16bit BE ----
static uint16_t ReadU16BE(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }

// ============================================================================
// 测试用例
// ============================================================================

static int g_Failures = 0;
#define ASSERT(cond, msg) do { \
    if (!(cond)) { std::cerr << "  FAIL: " << msg << std::endl; ++g_Failures; return; } \
} while(0)

#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { std::cerr << "  FAIL: " << msg << " (" << (a) << " != " << (b) << ")" << std::endl; ++g_Failures; } \
} while(0)

static void TestBasicCreate()
{
    std::cout << "[TEST] 基本创建与销毁" << std::endl;

    MP4Writer w;
    // 不 Create 就 WriteFrame 应安全
    bool ok = w.WriteFrame(nullptr, 0, 0, false);
    ASSERT(!ok, "空 WriteFrame 应返回 false");
    // Destroy 可安全重复调用
    w.Destroy();
    w.Destroy();

    std::cout << "  PASS" << std::endl;
}

static void TestWriteAndFinalize()
{
    std::cout << "[TEST] 写入帧并 Finalize" << std::endl;

    const int32 W = 640, H = 480, FPS = 30;
    const int32 N = 10; // 共 10 帧

    auto sps = BuildSPS(W, H);
    auto pps = BuildPPS();

    // 切片数据（每帧不同，用于验证）
    std::vector<std::vector<uint8_t>> frames;

    for (int32 i = 0; i < N; ++i)
    {
        bool isIDR = (i == 0);
        bool includeHeaders = (i == 0);
        std::vector<uint8_t> slicePayload(64, static_cast<uint8_t>(0xAA + i));

        auto frame = BuildAnnexBFrame(sps, pps, slicePayload, isIDR, includeHeaders);
        frames.push_back(frame);
    }

    DeleteTestFile("test_mp4writer_output.mp4");

    {
        MP4Writer w;
        ASSERT(w.Create("test_mp4writer_output.mp4", W, H, FPS),
               "Create 失败");

        for (int32 i = 0; i < N; ++i)
        {
            bool isIDR = (i == 0);
            ASSERT(w.WriteFrame(frames[i].data(), frames[i].size(), i, isIDR),
                   (std::string("WriteFrame #") + std::to_string(i) + " 失败").c_str());
        }

        ASSERT(w.Finalize(), "Finalize 失败");
    }

    // ---- 验证输出文件结构 ----
    FILE* f = fopen("test_mp4writer_output.mp4", "rb");
    ASSERT(f != nullptr, "无法打开输出文件");

    // 验证 ftyp
    int64_t ftypSize = 0, ftypData = 0;
    ASSERT(FindBox("ftyp", f, &ftypSize, &ftypData), "找不到 ftyp box");
    std::cout << "  ftyp: size=" << ftypSize << " dataOffset=" << ftypData << std::endl;

    // 验证 mdat
    int64_t mdatSize = 0, mdatData = 0;
    ASSERT(FindBox("mdat", f, &mdatSize, &mdatData), "找不到 mdat box");
    std::cout << "  mdat: size=" << mdatSize << " dataOffset=" << mdatData << std::endl;

    // 验证 moov
    int64_t moovSize = 0, moovData = 0;
    ASSERT(FindBox("moov", f, &moovSize, &moovData), "找不到 moov box");
    std::cout << "  moov: size=" << moovSize << " dataOffset=" << moovData << std::endl;

    // 读取 moov 内容
    std::vector<uint8_t> moovBuf(static_cast<size_t>(moovSize));
    fseek(f, moovData, SEEK_SET);
    fread(moovBuf.data(), 1, moovBuf.size(), f);

    // 搜索 avcC box（在 stsd→avc1→avcC 链中）
    const char* avcC_str = "avcC";
    bool foundAvcC = false;
    for (size_t i = 0; i + 8 <= moovBuf.size(); ++i)
    {
        if (memcmp(&moovBuf[i], avcC_str, 4) == 0)
        {
            uint32_t avcCSize = (uint32_t(moovBuf[i-4]) << 24) | (uint32_t(moovBuf[i-3]) << 16) |
                                (uint32_t(moovBuf[i-2]) << 8)  | uint32_t(moovBuf[i-1]);
            std::cout << "  avcC: size=" << avcCSize << " at offset " << i << " in moov" << std::endl;

            // avcC 结构验证
            const uint8_t* avcC = &moovBuf[i];
            uint8_t cfgVer = avcC[4];
            uint8_t profile = avcC[5];
            uint8_t compat  = avcC[6];
            uint8_t level   = avcC[7];
            uint8_t naluLen = avcC[8] & 0x03;
            std::cout << "    configVersion=" << (int)cfgVer
                      << " profile=" << (int)profile
                      << " compat=" << (int)compat
                      << " level=" << (int)level/10.0
                      << " naluLengthSize=" << (int)(naluLen + 1) << std::endl;

            // 验证 SPS
            uint8_t numSPS = avcC[9] & 0x1F;
            std::cout << "    numSPS=" << (int)numSPS;
            if (numSPS >= 1)
            {
                uint16_t SpsLen = ReadU16BE(&avcC[10]);
                std::cout << " spsLen=" << SpsLen << " expected=" << sps.size();
                ASSERT(SpsLen == sps.size(), "SPS 长度不匹配");
            }
            std::cout << std::endl;

            // 验证 PPS
            uint8_t numPPS = avcC[12 + (numSPS >= 1 ? ReadU16BE(&avcC[10]) : 0)];
            if (numSPS >= 1)
            {
                size_t ppsNumOff = 12 + ReadU16BE(&avcC[10]);
                uint8_t numPPS2 = avcC[ppsNumOff];
                uint16_t ppsLen = ReadU16BE(&avcC[ppsNumOff + 1]);
                std::cout << "    numPPS=" << (int)numPPS2 << " ppsLen=" << ppsLen << std::endl;
                ASSERT(ppsLen == pps.size(), "PPS 长度不匹配");
            }

            foundAvcC = true;
            break;
        }
    }
    ASSERT(foundAvcC, "moov 中找不到 avcC box");

    // 验证 stss (sync sample box) — 应标出第 1 帧为关键帧
    const char* stss_str = "stss";
    bool foundStss = false;
    for (size_t i = 0; i + 12 <= moovBuf.size(); ++i)
    {
        if (memcmp(&moovBuf[i], stss_str, 4) == 0)
        {
            uint32_t entryCount = (uint32_t(moovBuf[i+8]) << 24) | (uint32_t(moovBuf[i+9]) << 16) |
                                  (uint32_t(moovBuf[i+10]) << 8) | moovBuf[i+11];
            uint32_t firstSync = (uint32_t(moovBuf[i+12]) << 24) | (uint32_t(moovBuf[i+13]) << 16) |
                                  (uint32_t(moovBuf[i+14]) << 8) | moovBuf[i+15];
            std::cout << "  stss: entryCount=" << entryCount << " firstSync=" << firstSync << std::endl;
            ASSERT(entryCount == 1, "应只有 1 个关键帧（IDR）");
            ASSERT(firstSync == 1, "第一个同步帧应为 sample #1");
            foundStss = true;
            break;
        }
    }
    ASSERT(foundStss, "moov 中找不到 stss box");

    // 验证 stsz (sample size box) — sample count 应为 N
    const char* stsz_str = "stsz";
    bool foundStsz = false;
    for (size_t i = 0; i + 16 <= moovBuf.size(); ++i)
    {
        if (memcmp(&moovBuf[i], stsz_str, 4) == 0)
        {
            uint32_t sampleCount = (uint32_t(moovBuf[i+12]) << 24) | (uint32_t(moovBuf[i+13]) << 16) |
                                    (uint32_t(moovBuf[i+14]) << 8) | moovBuf[i+15];
            std::cout << "  stsz: sampleCount=" << sampleCount << std::endl;
            ASSERT_EQ(sampleCount, N, "stsz 中的 sample 数量不正确");
            foundStsz = true;
            break;
        }
    }
    ASSERT(foundStsz, "moov 中找不到 stsz box");

    fclose(f);
    std::cout << "  PASS" << std::endl;
}

static void TestEmptyFrames()
{
    std::cout << "[TEST] 纯头部帧（SPS+PPS，无切片）" << std::endl;

    const int32 W = 320, H = 240, FPS = 15;
    auto sps = BuildSPS(W, H);
    auto pps = BuildPPS();

    // 构造仅含 SPS+PPS 无切片 NAL 的帧
    std::vector<uint8_t> headerOnlyFrame;
    const uint8_t sc[] = { 0x00, 0x00, 0x00, 0x01 };
    headerOnlyFrame.insert(headerOnlyFrame.end(), sc, sc + 4);
    headerOnlyFrame.insert(headerOnlyFrame.end(), sps.begin(), sps.end());
    headerOnlyFrame.insert(headerOnlyFrame.end(), sc, sc + 4);
    headerOnlyFrame.insert(headerOnlyFrame.end(), pps.begin(), pps.end());

    DeleteTestFile("test_mp4writer_empty.mp4");
    {
        FILE* check = fopen("test_mp4writer_empty.mp4", "rb");
        if (check) { fclose(check); DeleteTestFile("test_mp4writer_empty.mp4"); }
    }
    {
        MP4Writer w;
        ASSERT(w.Create("test_mp4writer_empty.mp4", W, H, FPS), "Create 失败");
        ASSERT(w.WriteFrame(headerOnlyFrame.data(), headerOnlyFrame.size(), 0, true), "WriteFrame 失败");
        ASSERT(w.Finalize(), "Finalize 失败");
    }

    FILE* f = fopen("test_mp4writer_empty.mp4", "rb");
    ASSERT(f != nullptr, "无法打开输出文件");

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    std::cout << "  FileSize=" << fsize << std::endl;

    int64_t mdatSize = 0, mdatData = 0;
    ASSERT(FindBox("mdat", f, &mdatSize, &mdatData), "找不到 mdat box");
    ASSERT(mdatSize == 8, "纯头部帧的 mdat 应为 8 字节");
    std::cout << "  mdatSize=" << mdatSize << " (expected 8)" << std::endl;

    fclose(f);
    std::cout << "  PASS" << std::endl;
}

// ============================================================================
int main()
{
    std::cout << "=== MP4Writer 单元测试 ===" << std::endl;
    std::cout << std::endl;

    TestBasicCreate();
    TestWriteAndFinalize();
    TestEmptyFrames();

    std::cout << std::endl;
    if (g_Failures == 0)
        std::cout << "=== 全部通过 ===" << std::endl;
    else
        std::cout << "=== " << g_Failures << " 个测试失败 ===" << std::endl;

    return g_Failures > 0 ? 1 : 0;
}
