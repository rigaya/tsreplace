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
#include "rgy_tsutil.h"

struct TSRCutRange {
    int64_t start;
    int64_t end;
};

// カットリスト中で先頭/末尾トリムを表す sentinel。
// 33bit PTS の値域外なので、通常の cut 範囲と取り違えることはない。
//   cut -1 <pts>  先頭トリム: <pts> から出力を開始する (置換映像の先頭フレームの元TS上のPTS)
//   cut <pts> -1  末尾トリム: <pts> で出力を終了する
static const int64_t TSR_CUT_TRIM_MARK = -1;

int64_t tsPacketReadPCRBase(const uint8_t *pkt188);
bool tsPacketWritePCRBase(uint8_t *pkt188, int64_t pcrBase);
int64_t tsPacketReadOPCRBase(const uint8_t *pkt188);
bool tsPacketWriteOPCRBase(uint8_t *pkt188, int64_t opcrBase);
bool tsPacketRewritePESTimestamps(uint8_t *pkt188, size_t size, int64_t pts, int64_t dts);
// 対象サービス内で PES 単位のカットを行う PID かを判定する。
bool tsrIsPESCutTargetPID(uint16_t packetPid, int aud0Pid, int aud1Pid, int captionPid, int superimposePid);
// PES に PTS がなければ、直近の source clock をカット判定に使う。
int64_t tsrPESCutReferenceTimestamp(int64_t pts, int64_t sourceClock);

struct TSRADTSChainState {
    size_t frameRemaining = 0;
    std::vector<uint8_t> headerPrefix;

    void reset();
};

struct TSRADTSWalkResult {
    bool valid;
    size_t lastCompleteOffset;
};

// PES をまたぐ ADTS フレーム列を追跡し、この payload 内で最後に完結した位置を返す。
TSRADTSWalkResult tsrWalkADTSPayload(const uint8_t *payload, size_t size, TSRADTSChainState& state);
// 孤児フレーム断片を飛ばすため、最初の有効な ADTS syncword を探す。
bool tsrFindADTSSync(const uint8_t *payload, size_t size, size_t& offset);
// PES header と ES payload を 188 byte TS packet へ再パケット化する。
bool tsrPacketizePES(uint16_t pid, const std::vector<uint8_t>& pesHeader,
    const std::vector<uint8_t>& esPayload, std::vector<std::vector<uint8_t>>& packets);

class TSRCutTimeline {
public:
    TSRCutTimeline();

    RGY_ERR load(const tstring& filename); // 絶対PTSのcutをロードする
    RGY_ERR resolve(int64_t refPTS);       // 入力TSの基準PTSから相対化して内部テーブルを構築する
    const tstring& loadError() const;
    bool enabled() const;
    size_t rangeCount() const;
    const std::vector<TSRCutRange>& absoluteRanges() const;
    const std::vector<TSRCutRange>& ranges() const;
    int64_t totalRemoved() const;
    // 先頭/末尾トリム位置 (元TSの絶対PTS)。未指定なら TIMESTAMP_INVALID_VALUE。
    // 中間カットと違い timeline は詰めず、出力の開始点/終了点を決めるだけなので
    // ranges() には含めない。
    int64_t headTrimPTS() const;
    int64_t tailTrimPTS() const;

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
    int64_t m_headTrimPTS;
    int64_t m_tailTrimPTS;
    mutable size_t m_cachedRange;
};

class TSRContinuityRewriter {
public:
    void process(uint8_t *pkt188);
    void reset();

private:
    std::unordered_map<uint16_t, uint8_t> m_cc;
};
