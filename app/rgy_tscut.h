// -----------------------------------------------------------------------------------------
// QSVEnc/NVEnc by rigaya
// -----------------------------------------------------------------------------------------

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rgy_err.h"
#include "rgy_tchar.h"

struct TSRCutRange {
    int64_t start;
    int64_t end;
};

class TSRCutTimeline {
public:
    TSRCutTimeline();

    RGY_ERR load(const tstring& filename);
    const tstring& loadError() const;
    bool enabled() const;
    int64_t originPTS() const;
    size_t rangeCount() const;
    const std::vector<TSRCutRange>& ranges() const;
    int64_t totalRemoved() const;

    bool isCut(int64_t t) const;
    int64_t removedBefore(int64_t t) const;

private:
    // 単一スレッドからの呼び出しを前提とした線形探索キャッシュ。複数スレッドから呼ぶ場合は要保護。
    size_t findRange(int64_t t) const;
    void clear();

    std::vector<TSRCutRange> m_ranges;
    std::vector<int64_t> m_removedBeforeRange;
    tstring m_loadError;
    bool m_loaded;
    int64_t m_originPTS;
    int64_t m_totalRemoved;
    mutable size_t m_cachedRange;
};
