// -----------------------------------------------------------------------------------------
// QSVEnc/NVEnc by rigaya
// -----------------------------------------------------------------------------------------

#include "rgy_tscut.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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
