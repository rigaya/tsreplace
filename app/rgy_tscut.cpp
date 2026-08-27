// -----------------------------------------------------------------------------------------
// QSVEnc/NVEnc by rigaya
// -----------------------------------------------------------------------------------------

#include "rgy_tscut.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

TSRCutTimeline::TSRCutTimeline() :
    m_ranges(),
    m_removedBeforeRange(),
    m_loadError(),
    m_loaded(false),
    m_originPTS(0),
    m_totalRemoved(0),
    m_cachedRange(0) {
}

void TSRCutTimeline::clear() {
    m_ranges.clear();
    m_removedBeforeRange.clear();
    m_loadError.clear();
    m_loaded = false;
    m_originPTS = 0;
    m_totalRemoved = 0;
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
    bool hasOrigin = false;
    bool hasOriginPTS = false;
    int64_t originPTS = 0;
    std::vector<TSRCutRange> ranges;

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
            if (text != "# tsreplace-cut-v1") {
                m_loadError = _T("1行目が識別行 \"# tsreplace-cut-v1\" ではない");
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
            if (range.start < 0 || range.end < 0) {
                m_loadError = cutRangeError(lineNumber, _T("cut の start / end が負値"), range);
                return RGY_ERR_INVALID_PARAM;
            }
            if (range.start >= range.end) {
                m_loadError = cutRangeError(lineNumber, _T("cut の start >= end"), range);
                return RGY_ERR_INVALID_PARAM;
            }
            ranges.push_back(range);
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
        } else if (key == "origin") {
            if (hasOrigin) {
                m_loadError = lineError(lineNumber, _T("origin が重複している"));
                return RGY_ERR_INVALID_PARAM;
            }
            if (value != "first-frame") {
                m_loadError = lineError(lineNumber, _T("origin は \"first-frame\" でなければならない"));
                return RGY_ERR_INVALID_PARAM;
            }
            hasOrigin = true;
        } else if (key == "origin_pts") {
            if (hasOriginPTS) {
                m_loadError = lineError(lineNumber, _T("origin_pts が重複している"));
                return RGY_ERR_INVALID_PARAM;
            }
            if (!parseInt64(value, originPTS)) {
                m_loadError = lineError(lineNumber, _T("origin_pts が整数ではない"));
                return RGY_ERR_INVALID_PARAM;
            }
            if (originPTS < 0 || originPTS >= (int64_t{ 1 } << 33)) {
                m_loadError = lineError(lineNumber, _T("origin_pts が 33bit の範囲外"));
                return RGY_ERR_INVALID_PARAM;
            }
            hasOriginPTS = true;
        } else {
            m_loadError = lineError(lineNumber, _T("不明なキー \"") + toTString(key) + _T("\""));
            return RGY_ERR_INVALID_FORMAT;
        }
    }
    if (input.bad()) {
        m_loadError = _T("カットリストの読み込み中にエラーが発生した");
        return RGY_ERR_UNKNOWN;
    }
    if (!hasTimebase || !hasOrigin || !hasOriginPTS) {
        m_loadError = _T("timebase / origin / origin_pts のいずれかが欠落している");
        return RGY_ERR_INVALID_FORMAT;
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
    m_loaded = true;
    m_originPTS = originPTS;
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

int64_t TSRCutTimeline::originPTS() const {
    return m_originPTS;
}

size_t TSRCutTimeline::rangeCount() const {
    return m_ranges.size();
}

const std::vector<TSRCutRange>& TSRCutTimeline::ranges() const {
    return m_ranges;
}

int64_t TSRCutTimeline::totalRemoved() const {
    return m_totalRemoved;
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
    if (m_ranges.empty()) {
        return false;
    }
    const auto& range = m_ranges[findRange(t)];
    return range.start <= t && t < range.end;
}

int64_t TSRCutTimeline::removedBefore(int64_t t) const {
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
