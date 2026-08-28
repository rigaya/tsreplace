// -----------------------------------------------------------------------------------------
// QSVEnc/NVEnc by rigaya
// -----------------------------------------------------------------------------------------

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "rgy_err.h"
#include "rgy_tchar.h"

struct TSRCutRange {
    int64_t start;
    int64_t end;
};

int64_t tsPacketReadPCRBase(const uint8_t *pkt188);
bool tsPacketWritePCRBase(uint8_t *pkt188, int64_t pcrBase);
int64_t tsPacketReadOPCRBase(const uint8_t *pkt188);
bool tsPacketWriteOPCRBase(uint8_t *pkt188, int64_t opcrBase);
bool tsPacketRewritePESTimestamps(uint8_t *pkt188, size_t size, int64_t pts, int64_t dts);
// 対象サービス内で PES 単位のカットを行う PID かを判定する。
bool tsrIsPESCutTargetPID(uint16_t packetPid, int aud0Pid, int aud1Pid, int captionPid, int superimposePid);
// PES に PTS がなければ、直近の source clock をカット判定に使う。
int64_t tsrPESCutReferenceTimestamp(int64_t pts, int64_t sourceClock);

class TSRCutTimeline {
public:
    TSRCutTimeline();

    RGY_ERR load(const tstring& filename); // 絶対PTSのcutをロードする
    RGY_ERR resolve(int64_t refPTS);       // 入力TSの基準PTSから相対化して内部テーブルを構築する
    const tstring& loadError() const;
    bool enabled() const;
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
    std::vector<TSRCutRange> m_absoluteRanges;
    std::vector<size_t> m_absoluteRangeLines;
    tstring m_loadError;
    bool m_loaded;
    bool m_resolved;
    int64_t m_totalRemoved;
    mutable size_t m_cachedRange;
};

class TSRContinuityRewriter {
public:
    void process(uint8_t *pkt188);
    void reset();

private:
    std::unordered_map<uint16_t, uint8_t> m_cc;
};
