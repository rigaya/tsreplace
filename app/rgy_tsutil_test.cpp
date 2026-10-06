#include "rgy_tsutil.h"
#include <algorithm>
#include <cstdio>

// 読み込み境界をまたぐパケットを残し、追記後も入力全体を復元できることを確認する。
static bool testAppend(const size_t firstSize, const size_t consumedSize, const size_t appendSize) {
    std::vector<uint8_t> input(firstSize + appendSize);
    for (size_t i = 0; i < input.size(); i++) {
        input[i] = static_cast<uint8_t>((i * 17 + i / 188) % 251);
    }
    RGYTSBuffer buffer;
    // 継続読み込みで拡張済みの容量を再現し、短い追記で詰め直しが発生しない状態を作る。
    buffer.addData(input.data(), firstSize);
    buffer.addData(input.data(), firstSize / 2);
    buffer.removeData(firstSize + firstSize / 2);
    const auto initialPos = buffer.pos();
    buffer.addData(input.data(), firstSize);
    buffer.removeData(consumedSize);
    buffer.addData(input.data() + firstSize, appendSize);
    const auto remaining = input.size() - consumedSize;
    if (buffer.size() != remaining || buffer.pos() != initialPos + static_cast<int64_t>(consumedSize)
        || !std::equal(input.begin() + consumedSize, input.end(), buffer.data())) {
        std::fprintf(stderr, "追記後のデータが入力と一致しません: %zu/%zu/%zu\n", firstSize, consumedSize, appendSize);
        return false;
    }
    buffer.removeData(remaining);
    buffer.addData(input.data(), 188);
    return buffer.size() == 188 && buffer.pos() == initialPos + static_cast<int64_t>(input.size())
        && std::equal(input.begin(), input.begin() + 188, buffer.data());
}

int main() {
    const size_t blockSize = 1024 * 1024;
    const size_t consumedSize = blockSize / 188 * 188;
    // 短い末尾の追記、バッファ内の詰め直し、容量拡張をそれぞれ確認する。
    return testAppend(blockSize, consumedSize, 159592448 % blockSize)
        && testAppend(blockSize, consumedSize, blockSize)
        && testAppend(blockSize, consumedSize, blockSize * 2) ? 0 : 1;
}
