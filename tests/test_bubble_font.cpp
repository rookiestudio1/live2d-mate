// 氣泡字型的家族挑選規則。
//
// 字型家族得自己挑，而挑錯的代價是 1.4 秒的畫面凍結
// （見 core/bubble_font.h 的實測數字），所以規則必須被釘住。
#include <QtTest>

#include <string>
#include <vector>

#include "core/bubble_font.h"

using namespace l2m;

class TestBubbleFont : public QObject {
  Q_OBJECT

private slots:
  // 日韓要各自的家族，否則會被套成中文字形
  void japaneseAndKoreanHaveOwnFamilies() {
    QCOMPARE(bubbleFontCandidates("ja"), (std::vector<std::string>{"Yu Gothic UI", "Meiryo UI"}));
    QCOMPARE(bubbleFontCandidates("ja-JP"), (std::vector<std::string>{"Yu Gothic UI", "Meiryo UI"}));
    QCOMPARE(bubbleFontCandidates("ko"), (std::vector<std::string>{"Malgun Gothic"}));
  }

  // 簡繁分開：zh-CN 用雅黑，其餘 zh 用正黑
  void simplifiedAndTraditionalDiffer() {
    QCOMPARE(bubbleFontCandidates("zh-CN"), (std::vector<std::string>{"Microsoft YaHei UI", "Microsoft YaHei"}));
    QCOMPARE(bubbleFontCandidates("zh-TW"), (std::vector<std::string>{"Microsoft JhengHei UI", "Microsoft JhengHei"}));
    QCOMPARE(bubbleFontCandidates("zh-HK"), (std::vector<std::string>{"Microsoft JhengHei UI", "Microsoft JhengHei"}));
  }

  // 其餘語系（含空字串）一律 Segoe UI
  void othersFallBackToSegoe() {
    QCOMPARE(bubbleFontCandidates("en"), (std::vector<std::string>{"Segoe UI"}));
    QCOMPARE(bubbleFontCandidates(""), (std::vector<std::string>{"Segoe UI"}));
    QCOMPARE(bubbleFontCandidates("de-DE"), (std::vector<std::string>{"Segoe UI"}));
  }

  // 依序挑第一個「這台機器真的裝了」的家族
  void picksFirstInstalledCandidate() {
    const std::vector<std::string> installed{"Segoe UI", "Microsoft JhengHei UI", "Microsoft JhengHei"};
    QCOMPARE(pickInstalledFamily({"Microsoft JhengHei UI", "Microsoft JhengHei"}, installed), std::string("Microsoft JhengHei UI"));
    // 第一個沒裝就往下找，而不是整個放棄
    QCOMPARE(pickInstalledFamily({"Yu Gothic UI", "Microsoft JhengHei"}, installed), std::string("Microsoft JhengHei"));
  }

  // 家族名稱比對不分大小寫：QFontDatabase 回的大小寫不保證與我們寫死的一致
  void matchIsCaseInsensitive() { QCOMPARE(pickInstalledFamily({"microsoft jhenghei ui"}, {"Microsoft JhengHei UI"}), std::string("Microsoft JhengHei UI")); }

  // 挑不到就回空字串，讓呼叫端保留 Qt 的預設字型而不是硬塞一個不存在的家族
  // —— 硬塞的代價就是那 1.4 秒的 DirectWrite 後援解析
  void returnsEmptyWhenNothingInstalled() {
    QCOMPARE(pickInstalledFamily({"Yu Gothic UI"}, {"Segoe UI"}), std::string());
    QCOMPARE(pickInstalledFamily({}, {"Segoe UI"}), std::string());
    QCOMPARE(pickInstalledFamily({"Segoe UI"}, {}), std::string());
  }
};

QTEST_MAIN(TestBubbleFont)
#include "test_bubble_font.moc"
