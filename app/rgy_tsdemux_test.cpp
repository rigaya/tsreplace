#include "rgy_tsdemux.h"
#include <algorithm>
#include <cstdio>

static std::vector<uint8_t> makePackets(int unitSize, int count) {
    std::vector<uint8_t> data(unitSize * count, 0xff);
    for (int i = 0; i < count; i++) {
        auto *packet = data.data() + unitSize * i;
        packet[0] = 0x47;
        packet[1] = 0x1f;
        packet[2] = 0xff;
        packet[3] = 0x10 | (i & 15);
        packet[4] = static_cast<uint8_t>(i);
    }
    return data;
}

// 入力の分割位置にかかわらず、パケットの内容と消費位置が一致することを確認する。
static bool testSplit(std::vector<uint8_t> input, const std::vector<uint8_t>& expected,
    int unitSize, size_t chunkSize) {
    RGYTSPacketSplitter splitter;
    splitter.init(nullptr);
    std::vector<uint8_t> output;
    size_t packetCount = 0;
    for (size_t pos = 0; pos < input.size();) {
        // 192/204バイトの初回判定には、複数の同期バイトが見える入力を渡す。
        const auto readSize = std::min(input.size() - pos,
            pos == 0 && unitSize != 188 ? std::max(chunkSize, size_t(unitSize * 2 + 7)) : chunkSize);
        auto [err, packets] = splitter.split(input.data() + pos, readSize);
        pos += readSize;
        if (err != RGY_ERR_NONE) {
            std::fprintf(stderr, "パケット分割に失敗しました: %d/%zu\n", unitSize, chunkSize);
            return false;
        }
        for (const auto& packet : packets) {
            if (packet->packet.size() != static_cast<size_t>(unitSize)
                || packet->header.Sync != 0x47 || packet->header.pos < unitSize
                || packet->header.pos > static_cast<int64_t>(input.size())
                || !std::equal(packet->packet.begin(), packet->packet.end(),
                    input.begin() + packet->header.pos - unitSize)) {
                std::fprintf(stderr, "パケットサイズまたは位置が一致しません: %d/%zu\n", unitSize, chunkSize);
                return false;
            }
            output.insert(output.end(), packet->packet.begin(), packet->packet.end());
            packetCount++;
        }
    }
    if (output != expected || packetCount != expected.size() / unitSize
        || splitter.pos() != static_cast<int64_t>(input.size())) {
        std::fprintf(stderr, "分割結果が一致しません: %d/%zu、出力%zu、期待%zu\n",
            unitSize, chunkSize, output.size(), expected.size());
        return false;
    }
    return true;
}

int main() {
    for (const int unitSize : {188, 192, 204}) {
        const auto packets = makePackets(unitSize, 80);
        for (const size_t chunkSize : {size_t(1), size_t(187), size_t(1024), packets.size()}) {
            if (!testSplit(packets, packets, unitSize, chunkSize)) return 1;
        }
        // 先頭のごみと、読み込み境界で途切れた再同期先を確認する。
        auto prefixed = packets;
        prefixed.insert(prefixed.begin(), 7, 0);
        for (const size_t chunkSize : {size_t(unitSize), prefixed.size()}) {
            if (!testSplit(prefixed, packets, unitSize, chunkSize)) return 1;
        }
        // 同期確立後の短いごみ、およびパケット幅より長いごみから復帰する。
        for (const int gap : {3, unitSize * 3 + 5}) {
            auto corrupted = packets;
            corrupted.insert(corrupted.begin() + unitSize * 40, gap, 0);
            if (!testSplit(corrupted, packets, unitSize, unitSize * 40)) return 1;
        }
        // 同期バイトが壊れたパケットだけを飛ばし、前後の正常なパケットを保持する。
        auto brokenSync = packets;
        brokenSync[unitSize * 40] = 0;
        auto withoutBroken = packets;
        withoutBroken.erase(withoutBroken.begin() + unitSize * 40, withoutBroken.begin() + unitSize * 41);
        if (!testSplit(brokenSync, withoutBroken, unitSize, unitSize * 40)) return 1;
    }
    return 0;
}
