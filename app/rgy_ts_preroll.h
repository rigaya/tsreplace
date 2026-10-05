#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

constexpr int TSR_STARTUP_PREROLL_DEFAULT_MS = 0;

// TvtPlayは約500msを先行送信し、PAT/PMTと先頭キーフレームが同時に届くと、
// TVTestの非同期初期化中にキーフレームが失われる。既定値は先行送信500msと、
// 実測750msの初期化時間に余裕を加えた1600msとする。
// 時計付きのPSI準備区間を追加し、映像・音声のPTS/DTSは変更しない。
inline std::vector<uint8_t> tsrMakeStartupPreroll(
    const std::vector<uint8_t>& input, uint16_t pmtPID, uint16_t pcrPID,
    int durationMs = TSR_STARTUP_PREROLL_DEFAULT_MS) {
    if (durationMs <= 0 || durationMs > 60000) return {};
    const int repetitions = (durationMs + 19) / 20;
    struct Table {
        std::vector<uint8_t> packets;
        size_t remaining = 0;
        bool complete = false;
        void append(const uint8_t *p, size_t payload) {
            if (complete || payload >= 188) return;
            if (p[1] & 0x40) {
                const size_t section = payload + 1 + p[payload];
                if (section + 3 > 188) return;
                remaining = 3 + ((p[section + 1] & 0x0f) << 8) + p[section + 2];
                packets.clear();
                payload = section;
            }
            if (remaining == 0) return;
            packets.insert(packets.end(), p, p + 188);
            remaining -= std::min(remaining, 188 - payload);
            complete = remaining == 0;
        }
    } pat, pmt;
    int64_t firstPCR = -1;
    std::array<uint8_t, 188> clock{};
    bool haveClockCounter = false;
    uint8_t clockCounter = 0;
    for (size_t i = 0; i + 188 <= input.size(); i += 188) {
        const auto *p = input.data() + i;
        if (p[0] != 0x47 || (p[1] & 0x80) || (p[3] & 0xc0)) continue;
        const auto pid = ((p[1] & 0x1f) << 8) | p[2];
        const auto afc = (p[3] >> 4) & 3;
        if (pid == pcrPID && !haveClockCounter) {
            clockCounter = (p[3] - ((afc & 1) ? 1 : 0)) & 0x0f;
            haveClockCounter = true;
        }
        if (pid == pcrPID && firstPCR < 0 && (afc & 2) && p[4] >= 7 && p[4] <= 183 && (p[5] & 0x10)) {
            firstPCR = (int64_t(p[6]) << 25) | (int64_t(p[7]) << 17)
                | (int64_t(p[8]) << 9) | (int64_t(p[9]) << 1) | (p[10] >> 7);
            clock.fill(0xff);
            clock[0] = 0x47;
            clock[1] = uint8_t(pcrPID >> 8);
            clock[2] = uint8_t(pcrPID);
            clock[3] = 0x20 | clockCounter;
            clock[4] = 183;
            clock[5] = 0x10;
            // 先頭PCRの拡張部を維持する。増分は90kHzの整数刻みとする。
            clock[10] = p[10] & 0x7f;
            clock[11] = p[11];
        }
        size_t payload = 4;
        if (afc & 2) payload += 1 + p[4];
        if (afc & 1) {
            if (pid == 0) pat.append(p, payload);
            else if (pid == pmtPID) pmt.append(p, payload);
        }
        if (pat.complete && pmt.complete && firstPCR >= 0) break;
    }
    if (!pat.complete || !pmt.complete || firstPCR < 0) return {};
    std::vector<uint8_t> result;
    const size_t tableBytes = pat.packets.size() + pmt.packets.size();
    result.reserve(repetitions * (tableBytes + 188) + 256 * 188);
    for (int repetition = 0; repetition < repetitions; repetition++) {
        for (const auto *table : {&pat, &pmt}) {
            const auto start = result.size();
            result.insert(result.end(), table->packets.begin(), table->packets.end());
            for (size_t i = 0; i < table->packets.size(); i += 188) {
                auto& cc = result[start + i + 3];
                // 繰り返し数によらず、元のPSIへCCを連続させる。
                const int packetCount = int(table->packets.size() / 188);
                cc = (cc & 0xf0) | ((cc + (repetition - repetitions) * packetCount) & 0x0f);
            }
        }
        const auto pcr = (firstPCR - (repetitions - repetition) * int64_t(1800)) & ((int64_t(1) << 33) - 1);
        clock[6] = uint8_t(pcr >> 25);
        clock[7] = uint8_t(pcr >> 17);
        clock[8] = uint8_t(pcr >> 9);
        clock[9] = uint8_t(pcr >> 1);
        clock[10] = (clock[10] & 0x7f) | uint8_t(pcr << 7);
        result.insert(result.end(), clock.begin(), clock.end());
        if (repetition == 3) {
            // TvtPlayはPCRの検出やシーク時に先頭のパケットを読み捨てる。
            // 4組目の後に送信単位を超えるNULLを置き、残ったPAT/PMTを
            // 映像とは別のバッファで早期送信させる。PCRだけの疎なTSで
            // 最大400msの送信待ちが起こることも防ぐ。
            std::array<uint8_t, 188> padding;
            padding.fill(0xff);
            padding[0] = 0x47; padding[1] = 0x1f; padding[2] = 0xff; padding[3] = 0x10;
            for (int i = 0; i < 256; i++) result.insert(result.end(), padding.begin(), padding.end());
        }
    }
    return result;
}
