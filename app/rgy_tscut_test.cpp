// -----------------------------------------------------------------------------------------
// QSVEnc/NVEnc by rigaya
// -----------------------------------------------------------------------------------------

#include "rgy_tscut.h"
#include "rgy_tsstruct.h"
#include "rgy_tsutil.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

class TestFile {
public:
    TestFile(const std::string& name, const std::string& content) :
        m_path(std::filesystem::temp_directory_path() / ("rgy_tscut_" + name + ".txt")) {
        std::ofstream output(m_path);
        output << content;
    }

    ~TestFile() {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    tstring path() const {
        return m_path.native();
    }

private:
    std::filesystem::path m_path;
};

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        failures++;
    }
}

std::string manifest(const std::string& cuts = {}) {
    return "# tsreplace-cut-v2\n"
        "timebase=90000\n\n" + cuts;
}

void expectLoadAndResolve(TSRCutTimeline& timeline, const TestFile& file, int64_t refPTS = 0) {
    expect(timeline.load(file.path()) == RGY_ERR_NONE, "カットリストをロードできる");
    expect(timeline.resolve(refPTS) == RGY_ERR_NONE, "カットリストを解決できる");
}

void testEmpty() {
    TestFile file("empty", manifest());
    TSRCutTimeline timeline;
    expectLoadAndResolve(timeline, file);
    expect(timeline.enabled(), "cut 0 個でもロード成功なら有効になる");
    expect(timeline.totalRemoved() == 0, "cut なしの削除時間は 0");
    expect(!timeline.isCut(100), "cut なしでは isCut が false");
    expect(timeline.removedBefore(100) == 0, "cut なしでは removedBefore が 0");
}

void testSingleRange() {
    TestFile file("single", manifest("cut 100 130\n"));
    TSRCutTimeline timeline;
    expectLoadAndResolve(timeline, file);
    expect(timeline.enabled(), "cut ありでは有効になる");
    expect(!timeline.isCut(90) && timeline.removedBefore(90) == 0, "開始前");
    expect(timeline.isCut(100) && timeline.removedBefore(100) == 0, "開始境界");
    expect(timeline.isCut(110) && timeline.removedBefore(110) == 10, "区間内");
    expect(!timeline.isCut(130) && timeline.removedBefore(130) == 30, "終了境界");
    expect(!timeline.isCut(150) && timeline.removedBefore(150) == 30, "終了後");
}

void testMultipleRanges() {
    TestFile file("multiple", manifest("cut 100 130\ncut 200 250\n"));
    TSRCutTimeline timeline;
    expectLoadAndResolve(timeline, file);
    expect(timeline.removedBefore(220) == 50, "複数 cut の区間内累積");
    expect(timeline.removedBefore(300) == 80, "複数 cut の累積");
    expect(timeline.removedBefore(110) == 10, "逆順アクセスでも正しく検索できる");
}

void testNormalization() {
    {
        TestFile file("overlap", manifest("cut 100 150\ncut 120 180\n"));
        TSRCutTimeline timeline;
        expectLoadAndResolve(timeline, file);
        expect(timeline.rangeCount() == 1 && timeline.totalRemoved() == 80 && timeline.isCut(160),
            "overlap cut を 1 区間に統合する");
    }
    {
        TestFile file("adjacent", manifest("cut 100 130\ncut 130 160\n"));
        TSRCutTimeline timeline;
        expectLoadAndResolve(timeline, file);
        expect(timeline.rangeCount() == 1 && timeline.totalRemoved() == 60 && timeline.isCut(130),
            "隣接 cut を 1 区間に統合する");
    }
    {
        TestFile file("unsorted", manifest("cut 200 250\ncut 100 130\n"));
        TSRCutTimeline timeline;
        expectLoadAndResolve(timeline, file);
        expect(timeline.removedBefore(150) == 30 && timeline.removedBefore(300) == 80,
            "順不同 cut を並べ替える");
    }
}

void expectLoadError(const std::string& name, const std::string& content) {
    TestFile file(name, content);
    TSRCutTimeline timeline;
    expect(timeline.load(file.path()) != RGY_ERR_NONE, name.c_str());
    expect(!timeline.loadError().empty(), "ロード失敗時にエラー理由を保持する");
    expect(!timeline.enabled(), "ロード失敗後は無効状態を維持する");
}

void testErrors() {
    expectLoadError("負値", manifest("cut -2 100\n"));
    expectLoadError("-1 が中間の cut", manifest("cut 100 -1\ncut 200 300\n"));
    expectLoadError("-1 が中間の cut の start", manifest("cut 100 200\ncut -1 300\n"));
    expectLoadError("start と end が両方 -1", manifest("cut -1 -1\n"));
    expectLoadError("33bit 範囲外", manifest("cut 100 8589934592\n"));
    expectLoadError("timebase 不正",
        "# tsreplace-cut-v2\ntimebase=1000\n");
    expectLoadError("timebase 欠落",
        "# tsreplace-cut-v2\n");
    expectLoadError("origin 廃止",
        "# tsreplace-cut-v2\ntimebase=90000\norigin=first-frame\n");
    expectLoadError("origin_pts 廃止",
        "# tsreplace-cut-v2\ntimebase=90000\norigin_pts=1\n");
    expectLoadError("v1 廃止",
        "# tsreplace-cut-v1\ntimebase=90000\norigin=first-frame\norigin_pts=1\n");
    expectLoadError("識別行なし",
        "timebase=90000\n");

    for (const auto& test : { std::make_pair("start == end", manifest("cut 100 100\n")),
                              std::make_pair("start > end", manifest("cut 130 100\n")) }) {
        TestFile file(test.first, test.second);
        TSRCutTimeline timeline;
        expect(timeline.load(file.path()) == RGY_ERR_NONE, "start/end の順序は load 時には確定しない");
        expect(timeline.resolve(0) != RGY_ERR_NONE, test.first);
        expect(!timeline.loadError().empty(), "resolve 失敗時にエラー理由を保持する");
        expect(timeline.loadError().find(_T("絶対 PTS")) != tstring::npos
            && timeline.loadError().find(_T("相対値")) != tstring::npos,
            "resolve 失敗理由に絶対 PTS と相対値を含める");
    }
}

void testTrim() {
    {
        TestFile file("trim_none", manifest("cut 100 130\n"));
        TSRCutTimeline timeline;
        expectLoadAndResolve(timeline, file);
        expect(timeline.headTrimPTS() == TIMESTAMP_INVALID_VALUE
            && timeline.tailTrimPTS() == TIMESTAMP_INVALID_VALUE,
            "トリム指定なしでは TIMESTAMP_INVALID_VALUE を返す");
    }
    {
        TestFile file("trim_both", manifest("cut -1 50\ncut 100 130\ncut 400 -1\n"));
        TSRCutTimeline timeline;
        expectLoadAndResolve(timeline, file);
        expect(timeline.headTrimPTS() == 50, "先頭トリムを cut -1 <pts> から取得する");
        expect(timeline.tailTrimPTS() == 400, "末尾トリムを cut <pts> -1 から取得する");
        expect(timeline.rangeCount() == 1 && timeline.totalRemoved() == 30,
            "トリム行は中間カットとして扱わない");
        expect(timeline.removedBefore(500) == 30, "トリム行は timeline を詰めない");
    }
    {
        TestFile file("trim_head_only", manifest("cut -1 50\n"));
        TSRCutTimeline timeline;
        expectLoadAndResolve(timeline, file);
        expect(timeline.enabled() && timeline.rangeCount() == 0,
            "先頭トリムのみでも有効になり、中間カットは 0 個");
        expect(timeline.headTrimPTS() == 50 && timeline.tailTrimPTS() == TIMESTAMP_INVALID_VALUE,
            "先頭トリムのみを取得する");
    }
    {
        TestFile file("trim_tail_only", manifest("cut 400 -1\n"));
        TSRCutTimeline timeline;
        expectLoadAndResolve(timeline, file);
        expect(timeline.rangeCount() == 0 && timeline.tailTrimPTS() == 400,
            "末尾トリムのみを取得する");
    }
}

void testResolve() {
    TestFile file("resolve", manifest("cut 1100 1130\n"));
    TSRCutTimeline timeline;
    expectLoadAndResolve(timeline, file, 1000);
    expect(timeline.absoluteRanges().size() == 1
        && timeline.absoluteRanges()[0].start == 1100
        && timeline.absoluteRanges()[0].end == 1130,
        "ログ出力用の絶対 PTS 範囲を保持する");
    expect(timeline.rangeCount() == 1, "resolve 後に cut を保持する");
    expect(timeline.ranges()[0].start == 100 && timeline.ranges()[0].end == 130,
        "絶対 PTS を基準相対値へ解決する");
    expect(timeline.resolve(1000) != RGY_ERR_NONE, "resolve の2回目はエラーになる");
    expect(!timeline.loadError().empty(), "resolve の再実行エラー理由を保持する");
}

void testResolveWrap() {
    TestFile file("resolve_wrap", manifest("cut 8589934500 1000\n"));
    TSRCutTimeline timeline;
    expectLoadAndResolve(timeline, file, 8589934000LL);
    expect(timeline.rangeCount() == 1, "33bit wrap を跨ぐ cut を保持する");
    expect(timeline.ranges()[0].start == 500 && timeline.ranges()[0].end == 1592,
        "33bit wrap を跨ぐ cut を基準相対値へ解決する");
    expect(timeline.totalRemoved() == 1092, "33bit wrap を跨ぐ cut 幅を計算する");
}

void testTextEncoding() {
    {
        TestFile file("utf8_bom", std::string("\xef\xbb\xbf") + manifest("cut 100 130\n"));
        TSRCutTimeline timeline;
        expectLoadAndResolve(timeline, file);
        expect(timeline.rangeCount() == 1 && timeline.isCut(100), "UTF-8 BOM 付きでも cut を保持する");
    }
    {
        const auto lfText = manifest("cut 100 130\n");
        std::string crlfText;
        crlfText.reserve(lfText.size() + 8);
        for (const auto c : lfText) {
            if (c == '\n') {
                crlfText.push_back('\r');
            }
            crlfText.push_back(c);
        }
        TestFile file("crlf", crlfText);
        TSRCutTimeline timeline;
        expectLoadAndResolve(timeline, file);
        expect(timeline.rangeCount() == 1 && timeline.isCut(100), "CRLF でも cut を保持する");
    }
}

std::array<uint8_t, 188> makeTSPacket(uint16_t pid, uint8_t adaptationFieldControl, uint8_t cc,
    uint8_t scrambling = 0) {
    std::array<uint8_t, 188> packet = {};
    packet[0] = 0x47;
    packet[1] = (uint8_t)((pid >> 8) & 0x1f);
    packet[2] = (uint8_t)(pid & 0xff);
    packet[3] = (uint8_t)((scrambling << 6) | (adaptationFieldControl << 4) | (cc & 0x0f));
    return packet;
}

uint8_t packetCC(const std::array<uint8_t, 188>& packet) {
    return packet[3] & 0x0f;
}

void setPacketClock(std::array<uint8_t, 188>& packet, size_t offset, int64_t base,
    uint8_t reservedAndExtHigh, uint8_t extLow) {
    packet[offset + 0] = (uint8_t)(base >> 25);
    packet[offset + 1] = (uint8_t)(base >> 17);
    packet[offset + 2] = (uint8_t)(base >> 9);
    packet[offset + 3] = (uint8_t)(base >> 1);
    packet[offset + 4] = (uint8_t)(((base & 0x01) << 7) | (reservedAndExtHigh & 0x7f));
    packet[offset + 5] = extLow;
}

std::array<uint8_t, 188> makeClockPacket(uint8_t adaptationFieldLength, uint8_t flags) {
    auto packet = makeTSPacket(0x0100, 0x03, 0);
    packet.fill(0xff);
    packet[0] = 0x47;
    packet[1] = 0x01;
    packet[2] = 0x00;
    packet[3] = 0x30;
    packet[4] = adaptationFieldLength;
    packet[5] = flags;
    return packet;
}

void testPCRReadWrite() {
    constexpr int64_t PCR_BASE = 0x123456789LL;
    constexpr int64_t PCR_BASE_MAX = 0x1ffffffffLL;
    auto packet = makeClockPacket(7, 0x10);
    setPacketClock(packet, 6, PCR_BASE, 0x55, 0xa5);
    expect(tsPacketReadPCRBase(packet.data()) == PCR_BASE, "PCR_base を読み取る");
    const auto preservedLow7 = packet[10] & 0x7f;
    const auto preservedExtLow = packet[11];
    expect(tsPacketWritePCRBase(packet.data(), PCR_BASE_MAX), "PCR_base を書き込む");
    expect(tsPacketReadPCRBase(packet.data()) == PCR_BASE_MAX, "33bit 上限の PCR_base を往復する");
    expect((packet[10] & 0x7f) == preservedLow7 && packet[11] == preservedExtLow,
        "PCR 書き込み時に reserved bit と ext を保持する");

    constexpr int64_t OPCR_BASE = 0x102030405LL;
    constexpr int64_t NEW_PCR_BASE = 0x010203040LL;
    constexpr int64_t NEW_OPCR_BASE = 0x1abcdef01LL;
    auto pcrOpcrPacket = makeClockPacket(13, 0x18);
    setPacketClock(pcrOpcrPacket, 6, PCR_BASE, 0x3f, 0x12);
    setPacketClock(pcrOpcrPacket, 12, OPCR_BASE, 0x41, 0x34);
    const auto preservedOPCRLow7 = pcrOpcrPacket[16] & 0x7f;
    const auto preservedOPCRExtLow = pcrOpcrPacket[17];
    expect(tsPacketReadPCRBase(pcrOpcrPacket.data()) == PCR_BASE
        && tsPacketReadOPCRBase(pcrOpcrPacket.data()) == OPCR_BASE,
        "PCR / OPCR を独立して読み取る");
    expect(tsPacketWritePCRBase(pcrOpcrPacket.data(), NEW_PCR_BASE)
        && tsPacketReadOPCRBase(pcrOpcrPacket.data()) == OPCR_BASE,
        "PCR 書き込みで OPCR を変更しない");
    expect(tsPacketWriteOPCRBase(pcrOpcrPacket.data(), NEW_OPCR_BASE)
        && tsPacketReadPCRBase(pcrOpcrPacket.data()) == NEW_PCR_BASE
        && tsPacketReadOPCRBase(pcrOpcrPacket.data()) == NEW_OPCR_BASE,
        "OPCR 書き込みで PCR を変更しない");
    expect((pcrOpcrPacket[16] & 0x7f) == preservedOPCRLow7 && pcrOpcrPacket[17] == preservedOPCRExtLow,
        "OPCR 書き込み時に reserved bit と ext を保持する");

    auto shortOPCRPacket = makeClockPacket(12, 0x18);
    const auto shortOPCRPacketOriginal = shortOPCRPacket;
    expect(tsPacketReadOPCRBase(shortOPCRPacket.data()) < 0
        && !tsPacketWriteOPCRBase(shortOPCRPacket.data(), OPCR_BASE)
        && shortOPCRPacket == shortOPCRPacketOriginal,
        "OPCR までの adaptation field 長が足りないとき packet を変更しない");

    auto opcrOnlyPacket = makeClockPacket(7, 0x08);
    setPacketClock(opcrOnlyPacket, 6, OPCR_BASE, 0x7e, 0x56);
    expect(tsPacketReadPCRBase(opcrOnlyPacket.data()) < 0
        && tsPacketReadOPCRBase(opcrOnlyPacket.data()) == OPCR_BASE,
        "PCR なしの OPCR を読み取る");

    auto shortPacket = makeClockPacket(6, 0x10);
    const auto shortPacketOriginal = shortPacket;
    expect(tsPacketReadPCRBase(shortPacket.data()) < 0
        && !tsPacketWritePCRBase(shortPacket.data(), PCR_BASE)
        && shortPacket == shortPacketOriginal,
        "adaptation field が短いとき packet を変更しない");

    auto noAdaptationPacket = makeClockPacket(7, 0x10);
    noAdaptationPacket[3] = 0x10;
    expect(tsPacketReadPCRBase(noAdaptationPacket.data()) < 0
        && !tsPacketWritePCRBase(noAdaptationPacket.data(), PCR_BASE),
        "adaptation field がない packet を変更しない");

    auto noPCRFlagPacket = makeClockPacket(7, 0x00);
    expect(tsPacketReadPCRBase(noPCRFlagPacket.data()) < 0
        && !tsPacketWritePCRBase(noPCRFlagPacket.data(), PCR_BASE),
        "PCR_flag がない packet を変更しない");
}

void setTestPESTimestamp(std::array<uint8_t, 188>& packet, size_t offset, int64_t timestamp, uint8_t prefix) {
    const auto ts = (uint64_t)timestamp & ((uint64_t{ 1 } << 33) - 1);
    packet[offset + 0] = (uint8_t)((prefix << 4) | (((ts >> 30) & 0x07) << 1) | 0x01);
    packet[offset + 1] = (uint8_t)(ts >> 22);
    packet[offset + 2] = (uint8_t)((((ts >> 15) & 0x7f) << 1) | 0x01);
    packet[offset + 3] = (uint8_t)(ts >> 7);
    packet[offset + 4] = (uint8_t)(((ts & 0x7f) << 1) | 0x01);
}

int64_t readTestPESTimestamp(const uint8_t *field) {
    return ((int64_t)((field[0] & 0x0e) >> 1) << 30)
        | ((int64_t)field[1] << 22)
        | ((int64_t)(field[2] >> 1) << 15)
        | ((int64_t)field[3] << 7)
        | ((int64_t)field[4] >> 1);
}

std::array<uint8_t, 188> makePESPacket(uint8_t streamId, uint8_t ptsDtsFlags, size_t startOffset = 4) {
    std::array<uint8_t, 188> packet;
    packet.fill(0xff);
    packet[0] = 0x47;
    packet[1] = 0x41;
    packet[2] = 0x12;
    packet[3] = 0x10;
    if (startOffset + PES_HEADER_SIZE <= packet.size()) {
        packet[startOffset + 0] = 0x00;
        packet[startOffset + 1] = 0x00;
        packet[startOffset + 2] = 0x01;
        packet[startOffset + 3] = streamId;
        packet[startOffset + 4] = 0x00;
        packet[startOffset + 5] = 0x00;
        packet[startOffset + 6] = 0x80;
        packet[startOffset + 7] = ptsDtsFlags;
        packet[startOffset + 8] = (ptsDtsFlags & 0x40) ? 10 : ((ptsDtsFlags & 0x80) ? 5 : 0);
    }
    return packet;
}

bool pesMarkersSet(const uint8_t *field) {
    return (field[0] & 0x01) && (field[2] & 0x01) && (field[4] & 0x01);
}

void testPESRewrite() {
    constexpr int64_t OLD_PTS = 0x012345678LL;
    constexpr int64_t OLD_DTS = 0x001234567LL;
    constexpr int64_t NEW_PTS = 0x1ffffffffLL;
    constexpr int64_t NEW_DTS = 0x1abcdef01LL;

    auto ptsOnly = makePESPacket(0xc0, 0x80);
    setTestPESTimestamp(ptsOnly, 13, OLD_PTS, 0x02);
    expect(tsPacketRewritePESTimestamps(ptsOnly.data(), ptsOnly.size(), NEW_PTS, TIMESTAMP_INVALID_VALUE),
        "PTS のみの PES timestamp を書き換える");
    expect(readTestPESTimestamp(ptsOnly.data() + 13) == NEW_PTS,
        "33bit 上限の PTS を往復する");
    expect((ptsOnly[13] >> 4) == 0x02 && pesMarkersSet(ptsOnly.data() + 13),
        "PTS の prefix を保持し marker bit を立てる");

    auto ptsDts = makePESPacket(0xe0, 0xc0);
    setTestPESTimestamp(ptsDts, 13, OLD_PTS, 0x03);
    setTestPESTimestamp(ptsDts, 18, OLD_DTS, 0x01);
    expect(tsPacketRewritePESTimestamps(ptsDts.data(), ptsDts.size(), NEW_PTS, NEW_DTS),
        "PTS / DTS の両方を書き換える");
    expect(readTestPESTimestamp(ptsDts.data() + 13) == NEW_PTS
        && readTestPESTimestamp(ptsDts.data() + 18) == NEW_DTS,
        "書き換え後の PTS / DTS を読み戻せる");
    expect((ptsDts[13] >> 4) == 0x03 && (ptsDts[18] >> 4) == 0x01
        && pesMarkersSet(ptsDts.data() + 13) && pesMarkersSet(ptsDts.data() + 18),
        "PTS / DTS それぞれの prefix と marker bit を保持する");

    auto preserveDts = makePESPacket(0xe0, 0xc0);
    setTestPESTimestamp(preserveDts, 13, OLD_PTS, 0x03);
    setTestPESTimestamp(preserveDts, 18, OLD_DTS, 0x01);
    std::array<uint8_t, 5> originalDts = {};
    std::copy_n(preserveDts.data() + 18, originalDts.size(), originalDts.data());
    expect(tsPacketRewritePESTimestamps(preserveDts.data(), preserveDts.size(), NEW_PTS, TIMESTAMP_INVALID_VALUE)
        && std::equal(originalDts.begin(), originalDts.end(), preserveDts.begin() + 18),
        "DTS に無効値を渡したとき DTS フィールドを変更しない");

    auto noPtsFlag = makePESPacket(0xc0, 0x00);
    const auto noPtsFlagOriginal = noPtsFlag;
    expect(!tsPacketRewritePESTimestamps(noPtsFlag.data(), noPtsFlag.size(), NEW_PTS, NEW_DTS)
        && noPtsFlag == noPtsFlagOriginal,
        "PTS flag がない PES を変更しない");

    auto privateStream2 = makePESPacket(0xbf, 0xc0);
    setTestPESTimestamp(privateStream2, 13, OLD_PTS, 0x03);
    setTestPESTimestamp(privateStream2, 18, OLD_DTS, 0x01);
    const auto privateStream2Original = privateStream2;
    expect(!tsPacketRewritePESTimestamps(privateStream2.data(), privateStream2.size(), NEW_PTS, NEW_DTS)
        && privateStream2 == privateStream2Original,
        "private_stream_2 を optional PES header として扱わない");

    auto truncated = makePESPacket(0xc0, 0x80, 176);
    truncated[185] = 0x21;
    truncated[186] = 0x00;
    truncated[187] = 0x01;
    const auto truncatedOriginal = truncated;
    expect(!tsPacketRewritePESTimestamps(truncated.data(), truncated.size(), NEW_PTS, TIMESTAMP_INVALID_VALUE)
        && truncated == truncatedOriginal,
        "PTS が packet 末尾に収まらない PES を変更しない");
}

void testPESCutSelection() {
    constexpr int AUD0_PID = 0x0112;
    constexpr int AUD1_PID = 0x0113;
    constexpr int CAPTION_PID = 0x0116;
    constexpr int SUPERIMPOSE_PID = 0x0117;
    expect(tsrIsPESCutTargetPID(AUD0_PID, AUD0_PID, AUD1_PID, CAPTION_PID, SUPERIMPOSE_PID),
        "aud0 を PES カット対象にする");
    expect(tsrIsPESCutTargetPID(AUD1_PID, AUD0_PID, AUD1_PID, CAPTION_PID, SUPERIMPOSE_PID),
        "aud1 を PES カット対象にする");
    expect(tsrIsPESCutTargetPID(CAPTION_PID, AUD0_PID, AUD1_PID, CAPTION_PID, SUPERIMPOSE_PID),
        "caption を PES カット対象にする");
    expect(tsrIsPESCutTargetPID(SUPERIMPOSE_PID, AUD0_PID, AUD1_PID, CAPTION_PID, SUPERIMPOSE_PID),
        "superimpose を PES カット対象にする");
    expect(!tsrIsPESCutTargetPID(0x0000, 0, AUD1_PID, CAPTION_PID, SUPERIMPOSE_PID),
        "未設定 PID 0 と PAT を PES カット対象にしない");
    expect(!tsrIsPESCutTargetPID(0x1fff, AUD0_PID, AUD1_PID, CAPTION_PID, 0x1fff),
        "null PID を PES カット対象にしない");
    expect(!tsrIsPESCutTargetPID(0x0100, AUD0_PID, AUD1_PID, CAPTION_PID, SUPERIMPOSE_PID),
        "対象サービスの PES PID 以外をカット対象にしない");

    constexpr int64_t PES_PTS = 123456;
    constexpr int64_t SOURCE_CLOCK = 654321;
    expect(tsrPESCutReferenceTimestamp(PES_PTS, SOURCE_CLOCK) == PES_PTS,
        "PES に PTS があるときは PTS を判定時刻に使う");
    expect(tsrPESCutReferenceTimestamp(TIMESTAMP_INVALID_VALUE, SOURCE_CLOCK) == SOURCE_CLOCK,
        "PES に PTS がないときは直近 source clock を判定時刻に使う");
    expect(tsrPESCutReferenceTimestamp(TIMESTAMP_INVALID_VALUE, TIMESTAMP_INVALID_VALUE) == TIMESTAMP_INVALID_VALUE,
        "PTS と source clock がともに未確定なら判定時刻も未確定にする");
}

std::vector<uint8_t> makeADTSFrame(size_t frameLength, uint8_t fill) {
    std::vector<uint8_t> frame(frameLength, fill);
    frame[0] = 0xff;
    frame[1] = 0xf1;
    frame[2] = 0x4c; // AAC LC / 48kHz / stereo
    frame[3] = (uint8_t)(0x80 | ((frameLength >> 11) & 0x03));
    frame[4] = (uint8_t)(frameLength >> 3);
    frame[5] = (uint8_t)(((frameLength & 0x07) << 5) | 0x1f);
    frame[6] = 0xfc;
    return frame;
}

void testADTSBoundaryHelpers() {
    const auto frame20 = makeADTSFrame(20, 0x11);
    const auto frame15 = makeADTSFrame(15, 0x22);

    TSRADTSChainState chain;
    const auto first = tsrWalkADTSPayload(frame20.data(), 12, chain);
    expect(first.valid && first.lastCompleteOffset == 0 && chain.frameRemaining == 8,
        "PES をまたぐ ADTS frame の残りを保持する");
    std::vector<uint8_t> secondPayload(frame20.begin() + 12, frame20.end());
    secondPayload.insert(secondPayload.end(), frame15.begin(), frame15.end());
    const auto second = tsrWalkADTSPayload(secondPayload.data(), secondPayload.size(), chain);
    expect(second.valid && second.lastCompleteOffset == secondPayload.size()
        && chain.frameRemaining == 0 && chain.headerPrefix.empty(),
        "次 PES で継続 frame と後続 frame を完結できる");

    chain.reset();
    const auto prefixFrame = makeADTSFrame(11, 0x33);
    const auto followingFrame = makeADTSFrame(13, 0x44);
    std::vector<uint8_t> headerSplitFirst = prefixFrame;
    headerSplitFirst.insert(headerSplitFirst.end(), followingFrame.begin(), followingFrame.begin() + 3);
    const auto headerFirst = tsrWalkADTSPayload(headerSplitFirst.data(), headerSplitFirst.size(), chain);
    expect(headerFirst.valid && headerFirst.lastCompleteOffset == prefixFrame.size()
        && chain.headerPrefix.size() == 3,
        "PES 末尾に分割された ADTS header を保持する");
    const auto headerSecond = tsrWalkADTSPayload(followingFrame.data() + 3, followingFrame.size() - 3, chain);
    expect(headerSecond.valid && headerSecond.lastCompleteOffset == followingFrame.size() - 3
        && chain.headerPrefix.empty() && chain.frameRemaining == 0,
        "次 PES で分割 ADTS header を復元して frame を完結する");

    chain.reset();
    std::vector<uint8_t> truncatePayload = frame15;
    truncatePayload.insert(truncatePayload.end(), frame20.begin(), frame20.begin() + 5);
    const auto truncate = tsrWalkADTSPayload(truncatePayload.data(), truncatePayload.size(), chain);
    expect(truncate.valid && truncate.lastCompleteOffset == frame15.size(),
        "末尾の不完全 frame より前の truncate 位置を返す");

    std::vector<uint8_t> orphan = { 0x12, 0x34, 0x56, 0x78 };
    orphan.insert(orphan.end(), frame15.begin(), frame15.end());
    size_t syncOffset = 0;
    expect(tsrFindADTSSync(orphan.data(), orphan.size(), syncOffset) && syncOffset == 4,
        "孤児断片の後ろにある最初の ADTS syncword を見つける");

    auto falseCandidate = makeADTSFrame(7, 0x55);
    falseCandidate[5] = 0xe0;
    falseCandidate[6] = 0x00;
    std::vector<uint8_t> falseThenValid = falseCandidate;
    falseThenValid.push_back(0x00);
    falseThenValid.insert(falseThenValid.end(), frame15.begin(), frame15.end());
    falseThenValid.insert(falseThenValid.end(), frame20.begin(), frame20.end());
    expect(tsrFindADTSSync(falseThenValid.data(), falseThenValid.size(), syncOffset)
        && syncOffset == falseCandidate.size() + 1,
        "次 frame が成立しない偽 syncword を読み飛ばす");
    const std::array<uint8_t, 8> noSync = { 0, 1, 2, 3, 4, 5, 6, 7 };
    expect(!tsrFindADTSSync(noSync.data(), noSync.size(), syncOffset),
        "有効な ADTS syncword がない payload を検出する");
}

std::vector<uint8_t> collectTSPayload(const std::vector<std::vector<uint8_t>>& packets) {
    std::vector<uint8_t> payload;
    for (const auto& packet : packets) {
        size_t offset = 4;
        if (((packet[3] >> 4) & 0x02) != 0) {
            offset += 1 + packet[4];
        }
        payload.insert(payload.end(), packet.begin() + offset, packet.end());
    }
    return payload;
}

void testPESPacketize() {
    std::vector<uint8_t> pesHeader = {
        0x00, 0x00, 0x01, 0xc0, 0x00, 0x00, 0x80, 0x80, 0x05,
        0x21, 0x00, 0x01, 0x00, 0x01
    };
    std::vector<uint8_t> esPayload(200);
    for (size_t i = 0; i < esPayload.size(); i++) {
        esPayload[i] = (uint8_t)i;
    }
    std::vector<std::vector<uint8_t>> packets;
    expect(tsrPacketizePES(0x0112, pesHeader, esPayload, packets),
        "変更した音声 PES を再パケット化できる");
    expect(packets.size() == 2
        && packets[0].size() == 188 && packets[1].size() == 188,
        "再パケット化した TS packet は 188 byte になる");
    expect((packets[0][1] & 0x40) != 0 && (packets[1][1] & 0x40) == 0,
        "先頭 TS packet だけに PUSI を立てる");
    expect((((packets[0][1] & 0x1f) << 8) | packets[0][2]) == 0x0112
        && (((packets[1][1] & 0x1f) << 8) | packets[1][2]) == 0x0112,
        "再パケット化で音声 PID を保持する");

    const auto pes = collectTSPayload(packets);
    const auto expectedPacketLength = pesHeader.size() + esPayload.size() - 6;
    expect(pes.size() == pesHeader.size() + esPayload.size()
        && (((size_t)pes[4] << 8) | pes[5]) == expectedPacketLength,
        "PES_packet_length を変更後の長さへ書き換える");
    expect(std::equal(pesHeader.begin(), pesHeader.begin() + 4, pes.begin())
        && std::equal(pesHeader.begin() + 6, pesHeader.end(), pes.begin() + 6)
        && std::equal(esPayload.begin(), esPayload.end(), pes.begin() + pesHeader.size()),
        "PES_packet_length 以外の header と ES payload を保存する");
    expect(((packets.back()[3] >> 4) & 0x03) == 0x03,
        "最後の端数 packet を adaptation field stuffing で埋める");
}

void testContinuityRewriter() {
    TSRContinuityRewriter rewriter;

    auto first = makeTSPacket(0x0100, 0x01, 7);
    auto second = makeTSPacket(0x0100, 0x01, 2);
    auto third = makeTSPacket(0x0100, 0x01, 15);
    rewriter.process(first.data());
    rewriter.process(second.data());
    rewriter.process(third.data());
    expect(packetCC(first) == 7, "初出 PID は元の CC を維持する");
    expect(packetCC(second) == 8 && packetCC(third) == 9, "同一 PID の payload で CC が 1 ずつ進む");

    rewriter.reset();
    auto wrapFirst = makeTSPacket(0x0100, 0x01, 15);
    auto wrapSecond = makeTSPacket(0x0100, 0x01, 8);
    rewriter.process(wrapFirst.data());
    rewriter.process(wrapSecond.data());
    expect(packetCC(wrapSecond) == 0, "CC を 15 から 0 へ wrap する");

    rewriter.reset();
    auto payload = makeTSPacket(0x0101, 0x01, 4);
    auto adaptationOnly = makeTSPacket(0x0101, 0x02, 12);
    auto payloadAfterAdaptation = makeTSPacket(0x0101, 0x03, 0);
    rewriter.process(payload.data());
    rewriter.process(adaptationOnly.data());
    rewriter.process(payloadAfterAdaptation.data());
    expect(packetCC(adaptationOnly) == 4, "adaptation field only で CC が進まない");
    expect(packetCC(payloadAfterAdaptation) == 5, "afc 0x03 は payload ありとして CC が進む");

    rewriter.reset();
    auto pidAFirst = makeTSPacket(0x0102, 0x01, 3);
    auto pidBFirst = makeTSPacket(0x0103, 0x01, 9);
    auto pidASecond = makeTSPacket(0x0102, 0x01, 0);
    auto pidBSecond = makeTSPacket(0x0103, 0x01, 0);
    rewriter.process(pidAFirst.data());
    rewriter.process(pidBFirst.data());
    rewriter.process(pidASecond.data());
    rewriter.process(pidBSecond.data());
    expect(packetCC(pidASecond) == 4 && packetCC(pidBSecond) == 10, "複数 PID の CC を独立に管理する");

    rewriter.reset();
    auto upperFirst = makeTSPacket(0x0104, 0x03, 5, 2);
    auto upperSecond = makeTSPacket(0x0104, 0x03, 0, 2);
    const auto upperBits = upperSecond[3] & 0xf0;
    rewriter.process(upperFirst.data());
    rewriter.process(upperSecond.data());
    expect((upperSecond[3] & 0xf0) == upperBits && packetCC(upperSecond) == 6,
        "CC 書き換え時に pkt[3] の上位 4bit を保持する");

    auto nullPacket = makeTSPacket(0x1fff, 0x01, 13);
    rewriter.process(nullPacket.data());
    expect(packetCC(nullPacket) == 13, "null packet は書き換えない");

    auto reserved = makeTSPacket(0x0105, 0x00, 6);
    auto validAfterReserved = makeTSPacket(0x0105, 0x01, 11);
    rewriter.process(reserved.data());
    rewriter.process(validAfterReserved.data());
    expect(packetCC(reserved) == 6 && packetCC(validAfterReserved) == 11,
        "afc 0x00 は状態を変更しない");
}

} // namespace

int main() {
    testEmpty();
    testSingleRange();
    testMultipleRanges();
    testNormalization();
    testErrors();
    testTrim();
    testResolve();
    testResolveWrap();
    testTextEncoding();
    testPCRReadWrite();
    testPESRewrite();
    testPESCutSelection();
    testADTSBoundaryHelpers();
    testPESPacketize();
    testContinuityRewriter();

    if (failures != 0) {
        std::cerr << failures << " 件のテストが失敗しました。" << std::endl;
        return 1;
    }
    std::cout << "TSRCutTimeline の全テストが成功しました。" << std::endl;
    return 0;
}
