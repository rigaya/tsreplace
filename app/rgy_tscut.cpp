// -----------------------------------------------------------------------------------------
// QSVEnc/NVEnc by rigaya
// -----------------------------------------------------------------------------------------

#include "rgy_tscut.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "rgy_bitstream_aac.h"
#include "rgy_tsstruct.h"
#include "rgy_tsutil.h"

namespace {

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool parseInt64(const std::string& value, int64_t& result) {
    const auto text = trim(value);
    if (text.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}

tstring toTString(const std::string& value) {
    return tstring(value.begin(), value.end());
}

tstring lineError(size_t lineNumber, const tstring& reason) {
    std::basic_ostringstream<TCHAR> stream;
    stream << lineNumber << _T("行目: ") << reason;
    return stream.str();
}

tstring cutRangeError(size_t lineNumber, const TCHAR *reason, const TSRCutRange& range) {
    std::basic_ostringstream<TCHAR> stream;
    stream << reason << _T(" (") << range.start << _T(", ") << range.end << _T(")");
    return lineError(lineNumber, stream.str());
}

tstring resolvedCutRangeError(size_t lineNumber, const TCHAR *reason,
    const TSRCutRange& absolute, const TSRCutRange& relative) {
    std::basic_ostringstream<TCHAR> stream;
    stream << reason
        << _T(" (絶対 PTS: ") << absolute.start << _T(", ") << absolute.end
        << _T(" / 相対値: ") << relative.start << _T(", ") << relative.end << _T(")");
    return lineError(lineNumber, stream.str());
}

int64_t diffTimestamp33AMinusB(int64_t a, int64_t b) {
    constexpr int64_t WRAP = int64_t{ 1 } << 33;
    constexpr int64_t WRAP_THRESHOLD = (int64_t{ 1 } << 32) - 1;
    auto diff = a - b;
    if (diff > WRAP_THRESHOLD) {
        diff -= WRAP;
    } else if (diff < -WRAP_THRESHOLD) {
        diff += WRAP;
    }
    return diff;
}

} // namespace

namespace {

int tsPacketClockOffset(const uint8_t *pkt188, bool opcr) {
    if (pkt188 == nullptr) {
        return -1;
    }
    const auto adaptationFieldControl = (pkt188[3] >> 4) & 0x03;
    if ((adaptationFieldControl & 0x02) == 0) {
        return -1;
    }
    const auto adaptationFieldLength = pkt188[4];
    if (adaptationFieldLength < 1 || adaptationFieldLength > 183) {
        return -1;
    }
    const auto flags = pkt188[5];
    const auto targetFlag = opcr ? 0x08 : 0x10;
    if ((flags & targetFlag) == 0) {
        return -1;
    }
    const auto offset = 6 + ((opcr && (flags & 0x10)) ? 6 : 0);
    const auto requiredLength = offset + 1;
    return (adaptationFieldLength >= requiredLength) ? offset : -1;
}

int64_t tsPacketReadClockBase(const uint8_t *pkt188, bool opcr) {
    const auto offset = tsPacketClockOffset(pkt188, opcr);
    if (offset < 0) {
        return -1;
    }
    const auto *clock = pkt188 + offset;
    return ((int64_t)clock[0] << 25)
        | ((int64_t)clock[1] << 17)
        | ((int64_t)clock[2] << 9)
        | ((int64_t)clock[3] << 1)
        | ((clock[4] >> 7) & 0x01);
}

bool tsPacketWriteClockBase(uint8_t *pkt188, int64_t clockBase, bool opcr) {
    const auto offset = tsPacketClockOffset(pkt188, opcr);
    if (offset < 0) {
        return false;
    }
    const auto base = (uint64_t)clockBase & ((uint64_t{ 1 } << 33) - 1);
    auto *clock = pkt188 + offset;
    clock[0] = (uint8_t)(base >> 25);
    clock[1] = (uint8_t)(base >> 17);
    clock[2] = (uint8_t)(base >> 9);
    clock[3] = (uint8_t)(base >> 1);
    clock[4] = (uint8_t)((clock[4] & 0x7f) | ((base & 0x01) << 7));
    return true;
}

} // namespace

int64_t tsPacketReadPCRBase(const uint8_t *pkt188) {
    return tsPacketReadClockBase(pkt188, false);
}

bool tsPacketWritePCRBase(uint8_t *pkt188, int64_t pcrBase) {
    return tsPacketWriteClockBase(pkt188, pcrBase, false);
}

int64_t tsPacketReadOPCRBase(const uint8_t *pkt188) {
    return tsPacketReadClockBase(pkt188, true);
}

bool tsPacketWriteOPCRBase(uint8_t *pkt188, int64_t opcrBase) {
    return tsPacketWriteClockBase(pkt188, opcrBase, true);
}

namespace {

void tsPacketWritePESTimestamp(uint8_t *field, int64_t timestamp) {
    const auto ts = (uint64_t)timestamp & ((uint64_t{ 1 } << 33) - 1);
    field[0] = (uint8_t)((field[0] & 0xf0) | (((ts >> 30) & 0x07) << 1) | 0x01);
    field[1] = (uint8_t)(ts >> 22);
    field[2] = (uint8_t)((((ts >> 15) & 0x7f) << 1) | 0x01);
    field[3] = (uint8_t)(ts >> 7);
    field[4] = (uint8_t)(((ts & 0x7f) << 1) | 0x01);
}

} // namespace

bool tsPacketRewritePESTimestamps(uint8_t *pkt188, size_t size, int64_t pts, int64_t dts) {
    if (pkt188 == nullptr) {
        return false;
    }

    static const uint8_t PES_START_CODE[3] = { 0x00, 0x00, 0x01 };
    uint8_t *pesHeader = nullptr;
    for (size_t i = 4; i + sizeof(PES_START_CODE) <= size; i++) {
        if (memcmp(pkt188 + i, PES_START_CODE, sizeof(PES_START_CODE)) == 0) {
            pesHeader = pkt188 + i;
            break;
        }
    }
    if (pesHeader == nullptr) {
        return false;
    }

    uint8_t *const packetEnd = pkt188 + size;
    if (packetEnd - pesHeader < PES_START_SIZE) {
        return false;
    }
    const auto streamId = pesHeader[3];
    if (packetEnd - pesHeader < PES_HEADER_SIZE
        || !rgyPESStreamHasOptionalHeader(streamId)
        || (pesHeader[6] & 0xc0) != 0x80) {
        return false;
    }

    const auto ptsFlag = (pesHeader[7] & 0x80) != 0;
    const auto dtsFlag = (pesHeader[7] & 0x40) != 0;
    auto *field = pesHeader + PES_HEADER_SIZE;
    uint8_t *ptsField = nullptr;
    uint8_t *dtsField = nullptr;
    if (ptsFlag) {
        if (packetEnd - field < 5) {
            return false;
        }
        ptsField = field;
        field += 5;
    }
    if (dtsFlag) {
        if (packetEnd - field < 5) {
            return false;
        }
        dtsField = field;
    }

    const auto rewritePTS = ptsField != nullptr && pts != TIMESTAMP_INVALID_VALUE;
    const auto rewriteDTS = dtsField != nullptr && dts != TIMESTAMP_INVALID_VALUE;
    if (!rewritePTS && !rewriteDTS) {
        return false;
    }
    if (rewritePTS) {
        tsPacketWritePESTimestamp(ptsField, pts);
    }
    if (rewriteDTS) {
        tsPacketWritePESTimestamp(dtsField, dts);
    }
    return true;
}

bool tsrIsPESCutTargetPID(uint16_t packetPid, int aud0Pid, int aud1Pid, int captionPid, int superimposePid) {
    const auto matches = [packetPid](int targetPid) {
        // PID 0 は未設定値や PAT、0x1fff は null packet なので PES カット対象にしない。
        return targetPid > 0 && targetPid < 0x1fff && packetPid == targetPid;
    };
    return matches(aud0Pid) || matches(aud1Pid) || matches(captionPid) || matches(superimposePid);
}

int64_t tsrPESCutReferenceTimestamp(int64_t pts, int64_t sourceClock) {
    // PTS を持たない PES は、その時点で直近に確定した source clock で判定する。
    return pts != TIMESTAMP_INVALID_VALUE ? pts : sourceClock;
}

void TSRADTSChainState::reset() {
    frameRemaining = 0;
    headerPrefix.clear();
}

namespace {

bool parseADTSFrameLength(const uint8_t *data, size_t size, size_t& frameLength) {
    RGYAACHeader header = {};
    if (data == nullptr || size < RGYAACHeader::HEADER_BYTE_SIZE
        || header.parse(data, size) != 0
        || header.aac_frame_length < RGYAACHeader::HEADER_BYTE_SIZE) {
        return false;
    }
    frameLength = header.aac_frame_length;
    return true;
}

} // namespace

TSRADTSWalkResult tsrWalkADTSPayload(const uint8_t *payload, size_t size, TSRADTSChainState& state) {
    TSRADTSWalkResult result = { true, 0 };
    if (payload == nullptr && size > 0) {
        result.valid = false;
        state.reset();
        return result;
    }
    if (size == 0) {
        return result;
    }

    size_t pos = 0;
    if (state.frameRemaining > 0) {
        const auto consume = std::min(state.frameRemaining, size);
        state.frameRemaining -= consume;
        pos += consume;
        if (state.frameRemaining == 0) {
            result.lastCompleteOffset = pos;
        }
    }

    if (state.frameRemaining == 0 && !state.headerPrefix.empty()) {
        if (state.headerPrefix.size() >= RGYAACHeader::HEADER_BYTE_SIZE) {
            result.valid = false;
            state.reset();
            return result;
        }
        const auto needed = RGYAACHeader::HEADER_BYTE_SIZE - state.headerPrefix.size();
        const auto consume = std::min(needed, size - pos);
        if (consume > 0) {
            state.headerPrefix.insert(state.headerPrefix.end(), payload + pos, payload + pos + consume);
        }
        pos += consume;
        if (state.headerPrefix.size() < RGYAACHeader::HEADER_BYTE_SIZE) {
            return result;
        }
        size_t frameLength = 0;
        if (!parseADTSFrameLength(state.headerPrefix.data(), state.headerPrefix.size(), frameLength)) {
            result.valid = false;
            state.reset();
            return result;
        }
        const auto consumedHeader = state.headerPrefix.size();
        state.headerPrefix.clear();
        state.frameRemaining = frameLength - consumedHeader;
        const auto consumeFrame = std::min(state.frameRemaining, size - pos);
        state.frameRemaining -= consumeFrame;
        pos += consumeFrame;
        if (state.frameRemaining == 0) {
            result.lastCompleteOffset = pos;
        }
    }

    while (pos < size) {
        const auto available = size - pos;
        if (available < RGYAACHeader::HEADER_BYTE_SIZE) {
            state.headerPrefix.assign(payload + pos, payload + size);
            break;
        }
        size_t frameLength = 0;
        if (!parseADTSFrameLength(payload + pos, available, frameLength)) {
            result.valid = false;
            state.reset();
            return result;
        }
        const auto consume = std::min(frameLength, available);
        pos += consume;
        state.frameRemaining = frameLength - consume;
        if (state.frameRemaining == 0) {
            result.lastCompleteOffset = pos;
        }
    }
    return result;
}

bool tsrFindADTSSync(const uint8_t *payload, size_t size, size_t& offset) {
    offset = 0;
    if (payload == nullptr) {
        return false;
    }
    for (size_t pos = 0; pos + RGYAACHeader::HEADER_BYTE_SIZE <= size; pos++) {
        size_t frameLength = 0;
        if (parseADTSFrameLength(payload + pos, size - pos, frameLength)) {
            const auto remaining = size - pos;
            if (frameLength <= remaining
                && remaining - frameLength >= RGYAACHeader::HEADER_BYTE_SIZE) {
                size_t nextFrameLength = 0;
                if (!parseADTSFrameLength(payload + pos + frameLength,
                    remaining - frameLength, nextFrameLength)) {
                    continue;
                }
            }
            offset = pos;
            return true;
        }
    }
    return false;
}

bool tsrPacketizePES(uint16_t pid, const std::vector<uint8_t>& pesHeader,
    const std::vector<uint8_t>& esPayload, std::vector<std::vector<uint8_t>>& packets) {
    packets.clear();
    if (pid >= 0x1fff || pesHeader.size() < 6
        || pesHeader[0] != 0x00 || pesHeader[1] != 0x00 || pesHeader[2] != 0x01) {
        return false;
    }
    const auto pesPacketLength = pesHeader.size() + esPayload.size() - 6;
    if (pesPacketLength > 0xffff) {
        return false;
    }

    std::vector<uint8_t> pes = pesHeader;
    pes[4] = (uint8_t)(pesPacketLength >> 8);
    pes[5] = (uint8_t)(pesPacketLength & 0xff);
    pes.insert(pes.end(), esPayload.begin(), esPayload.end());

    for (size_t pos = 0; pos < pes.size();) {
        const auto len = std::min<size_t>(184, pes.size() - pos);
        std::vector<uint8_t> packet;
        packet.reserve(188);
        packet.push_back(0x47);
        packet.push_back((uint8_t)(((pos == 0) ? 0x40 : 0x00) | ((pid >> 8) & 0x1f)));
        packet.push_back((uint8_t)(pid & 0xff));
        packet.push_back((uint8_t)((len < 184) ? 0x30 : 0x10));
        if (len < 184) {
            packet.push_back((uint8_t)(183 - len));
            if (len < 183) {
                packet.push_back(0x00);
                packet.insert(packet.end(), 182 - len, 0xff);
            }
        }
        packet.insert(packet.end(), pes.begin() + pos, pes.begin() + pos + len);
        if (packet.size() != 188) {
            packets.clear();
            return false;
        }
        packets.push_back(std::move(packet));
        pos += len;
    }
    return !packets.empty();
}

TSRCutTimeline::TSRCutTimeline() :
    m_ranges(),
    m_removedBeforeRange(),
    m_absoluteRanges(),
    m_absoluteRangeLines(),
    m_loadError(),
    m_loaded(false),
    m_resolved(false),
    m_totalRemoved(0),
    m_headTrimPTS(TIMESTAMP_INVALID_VALUE),
    m_tailTrimPTS(TIMESTAMP_INVALID_VALUE),
    m_cachedRange(0) {
}

void TSRCutTimeline::clear() {
    m_ranges.clear();
    m_removedBeforeRange.clear();
    m_absoluteRanges.clear();
    m_absoluteRangeLines.clear();
    m_loadError.clear();
    m_loaded = false;
    m_resolved = false;
    m_totalRemoved = 0;
    m_headTrimPTS = TIMESTAMP_INVALID_VALUE;
    m_tailTrimPTS = TIMESTAMP_INVALID_VALUE;
    m_cachedRange = 0;
}

RGY_ERR TSRCutTimeline::load(const tstring& filename) {
    clear();

    std::ifstream input{ std::filesystem::path(filename) };
    if (!input) {
        m_loadError = _T("カットリストを開けない: \"") + filename + _T("\"");
        return RGY_ERR_FILE_OPEN;
    }

    bool hasTimebase = false;
    std::vector<TSRCutRange> ranges;
    std::vector<size_t> rangeLines;

    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line)) {
        lineNumber++;
        if (lineNumber == 1 && line.size() >= 3
            && (uint8_t)line[0] == 0xef && (uint8_t)line[1] == 0xbb && (uint8_t)line[2] == 0xbf) {
            // Windows などで生成された UTF-8 テキストの BOM は先頭行でのみ読み飛ばす。
            line.erase(0, 3);
        }
        const auto text = trim(line);

        if (lineNumber == 1) {
            if (text == "# tsreplace-cut-v1") {
                m_loadError = _T("v1 形式は廃止されたため、\"# tsreplace-cut-v2\" 形式を使用する");
                return RGY_ERR_INVALID_FORMAT;
            }
            if (text != "# tsreplace-cut-v2") {
                m_loadError = _T("1行目が識別行 \"# tsreplace-cut-v2\" ではない");
                return RGY_ERR_INVALID_FORMAT;
            }
            continue;
        }
        if (text.empty() || text[0] == '#') {
            continue;
        }

        if (text.rfind("cut", 0) == 0 && text.size() > 3 && (text[3] == ' ' || text[3] == '\t')) {
            std::istringstream stream(text.substr(3));
            std::string startText;
            std::string endText;
            std::string extra;
            if (!(stream >> startText >> endText) || (stream >> extra)) {
                m_loadError = lineError(lineNumber, _T("cut は \"cut <start> <end>\" の形式で指定する"));
                return RGY_ERR_INVALID_FORMAT;
            }
            TSRCutRange range = {};
            if (!parseInt64(startText, range.start) || !parseInt64(endText, range.end)) {
                m_loadError = lineError(lineNumber, _T("cut の start / end が整数ではない"));
                return RGY_ERR_INVALID_FORMAT;
            }
            // -1 は先頭/末尾トリムの sentinel。位置が妥当かは全行を読み終えてから判定する。
            const auto outOfRange = [](const int64_t v) {
                return (v < 0 && v != TSR_CUT_TRIM_MARK) || v >= (int64_t{ 1 } << 33);
            };
            if (outOfRange(range.start) || outOfRange(range.end)) {
                m_loadError = cutRangeError(lineNumber, _T("cut の start / end が 33bit の範囲外"), range);
                return RGY_ERR_INVALID_PARAM;
            }
            ranges.push_back(range);
            rangeLines.push_back(lineNumber);
            continue;
        }

        const auto separator = text.find('=');
        if (separator == std::string::npos) {
            m_loadError = lineError(lineNumber, _T("不正な行形式"));
            return RGY_ERR_INVALID_FORMAT;
        }
        const auto key = trim(text.substr(0, separator));
        const auto value = trim(text.substr(separator + 1));
        if (key == "timebase") {
            int64_t timebase = 0;
            if (hasTimebase) {
                m_loadError = lineError(lineNumber, _T("timebase が重複している"));
                return RGY_ERR_INVALID_PARAM;
            }
            if (!parseInt64(value, timebase)) {
                m_loadError = lineError(lineNumber, _T("timebase が整数ではない"));
                return RGY_ERR_INVALID_PARAM;
            }
            if (timebase != 90000) {
                m_loadError = lineError(lineNumber, _T("timebase は 90000 でなければならない"));
                return RGY_ERR_INVALID_PARAM;
            }
            hasTimebase = true;
        } else if (key == "origin" || key == "origin_pts") {
            m_loadError = lineError(lineNumber, _T("v1 形式は廃止されたため、origin / origin_pts は指定できない"));
            return RGY_ERR_INVALID_FORMAT;
        } else {
            m_loadError = lineError(lineNumber, _T("不明なキー \"") + toTString(key) + _T("\""));
            return RGY_ERR_INVALID_FORMAT;
        }
    }
    if (input.bad()) {
        m_loadError = _T("カットリストの読み込み中にエラーが発生した");
        return RGY_ERR_UNKNOWN;
    }
    if (!hasTimebase) {
        m_loadError = _T("timebase が欠落している");
        return RGY_ERR_INVALID_FORMAT;
    }

    // 先頭/末尾トリムを cut 範囲から分離する。
    // これらは中間カットと違い timeline を詰めないので、m_absoluteRanges には残さない。
    int64_t headTrimPTS = TIMESTAMP_INVALID_VALUE;
    int64_t tailTrimPTS = TIMESTAMP_INVALID_VALUE;
    if (!ranges.empty() && ranges.front().start == TSR_CUT_TRIM_MARK) {
        if (ranges.front().end == TSR_CUT_TRIM_MARK) {
            m_loadError = lineError(rangeLines.front(), _T("cut の start と end を同時に -1 にはできない"));
            return RGY_ERR_INVALID_PARAM;
        }
        headTrimPTS = ranges.front().end;
        ranges.erase(ranges.begin());
        rangeLines.erase(rangeLines.begin());
    }
    if (!ranges.empty() && ranges.back().end == TSR_CUT_TRIM_MARK) {
        tailTrimPTS = ranges.back().start;
        ranges.pop_back();
        rangeLines.pop_back();
    }
    for (size_t i = 0; i < ranges.size(); i++) {
        if (ranges[i].start == TSR_CUT_TRIM_MARK || ranges[i].end == TSR_CUT_TRIM_MARK) {
            m_loadError = lineError(rangeLines[i],
                _T("-1 は先頭トリム(最初の cut の start)と末尾トリム(最後の cut の end)にのみ指定できる"));
            return RGY_ERR_INVALID_PARAM;
        }
    }

    m_headTrimPTS = headTrimPTS;
    m_tailTrimPTS = tailTrimPTS;
    m_absoluteRanges = std::move(ranges);
    m_absoluteRangeLines = std::move(rangeLines);
    m_loaded = true;
    return RGY_ERR_NONE;
}

RGY_ERR TSRCutTimeline::resolve(int64_t refPTS) {
    if (!m_loaded) {
        m_loadError = _T("カットリストがロードされていない");
        return RGY_ERR_INVALID_CALL;
    }
    if (m_resolved) {
        m_loadError = _T("cut 範囲は既に解決済みのため、resolve() を再実行できない");
        return RGY_ERR_INVALID_CALL;
    }
    m_ranges.clear();
    m_removedBeforeRange.clear();
    m_loadError.clear();
    m_resolved = false;
    m_totalRemoved = 0;
    m_cachedRange = 0;

    if (refPTS < 0 || refPTS >= (int64_t{ 1 } << 33)) {
        m_loadError = _T("cut 範囲の解決基準 PTS が 33bit の範囲外");
        return RGY_ERR_INVALID_PARAM;
    }

    std::vector<TSRCutRange> ranges;
    ranges.reserve(m_absoluteRanges.size());
    for (size_t i = 0; i < m_absoluteRanges.size(); i++) {
        const auto& absolute = m_absoluteRanges[i];
        const TSRCutRange relative = {
            diffTimestamp33AMinusB(absolute.start, refPTS),
            diffTimestamp33AMinusB(absolute.end, refPTS)
        };
        if (relative.start >= relative.end) {
            m_loadError = resolvedCutRangeError(m_absoluteRangeLines[i],
                _T("基準 PTS から解決した cut の start >= end"), absolute, relative);
            return RGY_ERR_INVALID_PARAM;
        }
        ranges.push_back(relative);
    }

    std::sort(ranges.begin(), ranges.end(), [](const TSRCutRange& lhs, const TSRCutRange& rhs) {
        return lhs.start < rhs.start || (lhs.start == rhs.start && lhs.end < rhs.end);
    });

    std::vector<TSRCutRange> merged;
    merged.reserve(ranges.size());
    for (const auto& range : ranges) {
        if (!merged.empty() && range.start <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, range.end);
        } else {
            merged.push_back(range);
        }
    }

    std::vector<int64_t> removedBeforeRange;
    removedBeforeRange.reserve(merged.size());
    int64_t totalRemoved = 0;
    for (const auto& range : merged) {
        removedBeforeRange.push_back(totalRemoved);
        totalRemoved += range.end - range.start;
    }

    m_ranges = std::move(merged);
    m_removedBeforeRange = std::move(removedBeforeRange);
    m_resolved = true;
    m_totalRemoved = totalRemoved;
    m_cachedRange = 0;
    return RGY_ERR_NONE;
}

const tstring& TSRCutTimeline::loadError() const {
    return m_loadError;
}

bool TSRCutTimeline::enabled() const {
    return m_loaded;
}

size_t TSRCutTimeline::rangeCount() const {
    assert(m_resolved);
    return m_ranges.size();
}

const std::vector<TSRCutRange>& TSRCutTimeline::absoluteRanges() const {
    assert(m_loaded);
    return m_absoluteRanges;
}

const std::vector<TSRCutRange>& TSRCutTimeline::ranges() const {
    assert(m_resolved);
    return m_ranges;
}

int64_t TSRCutTimeline::totalRemoved() const {
    assert(m_resolved);
    return m_totalRemoved;
}

int64_t TSRCutTimeline::headTrimPTS() const {
    assert(m_loaded);
    return m_headTrimPTS;
}

int64_t TSRCutTimeline::tailTrimPTS() const {
    assert(m_loaded);
    return m_tailTrimPTS;
}

size_t TSRCutTimeline::findRange(int64_t t) const {
    if (m_ranges.empty()) {
        return 0;
    }

    auto index = std::min(m_cachedRange, m_ranges.size() - 1);
    while (index > 0 && t < m_ranges[index].start) {
        index--;
    }
    while (index + 1 < m_ranges.size() && t >= m_ranges[index + 1].start) {
        index++;
    }
    m_cachedRange = index;
    return index;
}

bool TSRCutTimeline::isCut(int64_t t) const {
    assert(m_resolved);
    if (m_ranges.empty()) {
        return false;
    }
    const auto& range = m_ranges[findRange(t)];
    return range.start <= t && t < range.end;
}

int64_t TSRCutTimeline::removedBefore(int64_t t) const {
    assert(m_resolved);
    if (m_ranges.empty() || t <= m_ranges.front().start) {
        return 0;
    }

    const auto index = findRange(t);
    const auto& range = m_ranges[index];
    if (t < range.start) {
        return m_removedBeforeRange[index];
    }
    return m_removedBeforeRange[index] + std::min(t, range.end) - range.start;
}

void TSRContinuityRewriter::process(uint8_t *pkt188) {
    if (pkt188 == nullptr) {
        return;
    }

    const auto pid = (uint16_t)(((pkt188[1] & 0x1f) << 8) | pkt188[2]);
    if (pid == 0x1fff) {
        return;
    }

    const auto adaptationFieldControl = (pkt188[3] >> 4) & 0x03;
    if (adaptationFieldControl == 0x00) {
        return;
    }

    const auto originalCC = pkt188[3] & 0x0f;
    const auto [entry, inserted] = m_cc.emplace(pid, originalCC);
    if (inserted) {
        return;
    }

    if (adaptationFieldControl & 0x01) {
        entry->second = (entry->second + 1) & 0x0f;
    }
    pkt188[3] = (pkt188[3] & 0xf0) | entry->second;
}

void TSRContinuityRewriter::reset() {
    m_cc.clear();
}
