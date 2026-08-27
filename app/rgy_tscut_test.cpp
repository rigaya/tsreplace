// -----------------------------------------------------------------------------------------
// QSVEnc/NVEnc by rigaya
// -----------------------------------------------------------------------------------------

#include "rgy_tscut.h"

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

std::string manifest(const std::string& cuts = {}, const std::string& originPTS = "1234567890") {
    return "# tsreplace-cut-v1\n"
        "timebase=90000\n"
        "origin=first-frame\n"
        "origin_pts=" + originPTS + "\n\n" + cuts;
}

void testEmpty() {
    TestFile file("empty", manifest());
    TSRCutTimeline timeline;
    expect(timeline.load(file.path()) == RGY_ERR_NONE, "cut なしをロードできる");
    expect(timeline.enabled(), "cut 0 個でもロード成功なら有効になる");
    expect(timeline.totalRemoved() == 0, "cut なしの削除時間は 0");
    expect(!timeline.isCut(100), "cut なしでは isCut が false");
    expect(timeline.removedBefore(100) == 0, "cut なしでは removedBefore が 0");
}

void testSingleRange() {
    TestFile file("single", manifest("cut 100 130\n"));
    TSRCutTimeline timeline;
    expect(timeline.load(file.path()) == RGY_ERR_NONE, "cut 1 個をロードできる");
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
    expect(timeline.load(file.path()) == RGY_ERR_NONE, "cut 複数をロードできる");
    expect(timeline.removedBefore(220) == 50, "複数 cut の区間内累積");
    expect(timeline.removedBefore(300) == 80, "複数 cut の累積");
    expect(timeline.removedBefore(110) == 10, "逆順アクセスでも正しく検索できる");
}

void testNormalization() {
    {
        TestFile file("overlap", manifest("cut 100 150\ncut 120 180\n"));
        TSRCutTimeline timeline;
        expect(timeline.load(file.path()) == RGY_ERR_NONE, "overlap cut をロードできる");
        expect(timeline.rangeCount() == 1 && timeline.totalRemoved() == 80 && timeline.isCut(160),
            "overlap cut を 1 区間に統合する");
    }
    {
        TestFile file("adjacent", manifest("cut 100 130\ncut 130 160\n"));
        TSRCutTimeline timeline;
        expect(timeline.load(file.path()) == RGY_ERR_NONE, "隣接 cut をロードできる");
        expect(timeline.rangeCount() == 1 && timeline.totalRemoved() == 60 && timeline.isCut(130),
            "隣接 cut を 1 区間に統合する");
    }
    {
        TestFile file("unsorted", manifest("cut 200 250\ncut 100 130\n"));
        TSRCutTimeline timeline;
        expect(timeline.load(file.path()) == RGY_ERR_NONE, "順不同 cut をロードできる");
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
    expectLoadError("start == end", manifest("cut 100 100\n"));
    expectLoadError("start > end", manifest("cut 130 100\n"));
    expectLoadError("負値", manifest("cut -1 100\n"));
    expectLoadError("timebase 不正",
        "# tsreplace-cut-v1\ntimebase=1000\norigin=first-frame\norigin_pts=1\n");
    expectLoadError("origin 不正",
        "# tsreplace-cut-v1\ntimebase=90000\norigin=unknown\norigin_pts=1\n");
    expectLoadError("timebase 欠落",
        "# tsreplace-cut-v1\norigin=first-frame\norigin_pts=1\n");
    expectLoadError("origin 欠落",
        "# tsreplace-cut-v1\ntimebase=90000\norigin_pts=1\n");
    expectLoadError("origin_pts 欠落",
        "# tsreplace-cut-v1\ntimebase=90000\norigin=first-frame\n");
    expectLoadError("識別行なし",
        "timebase=90000\norigin=first-frame\norigin_pts=1\n");
}

void testOriginPTS() {
    TestFile file("origin_wrap", manifest({}, "8589934000"));
    TSRCutTimeline timeline;
    expect(timeline.load(file.path()) == RGY_ERR_NONE, "33bit 上限付近の origin_pts をロードできる");
    expect(timeline.originPTS() == 8589934000LL, "33bit 上限付近の origin_pts を保持する");
}

} // namespace

int main() {
    testEmpty();
    testSingleRange();
    testMultipleRanges();
    testNormalization();
    testErrors();
    testOriginPTS();

    if (failures != 0) {
        std::cerr << failures << " 件のテストが失敗しました。" << std::endl;
        return 1;
    }
    std::cout << "TSRCutTimeline の全テストが成功しました。" << std::endl;
    return 0;
}
