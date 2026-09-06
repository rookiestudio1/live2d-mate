// 子系統前綴的切割規則。
//
// 這個規則寬一格就會誤傷正常訊息，而誤判的後果是「本文被砍掉一截又沒人發現」——
// 不會當掉、不會有錯誤，只是日誌從此少字。所以誤判的方向要一併釘住，
// 不是只測 15 個真前綴切得對。
#include <QtTest>

#include "core/log_category.h"

using namespace l2m;

class TestLogCategory : public QObject {
  Q_OBJECT

private slots:
  // 全專案實際用到的 15 個前綴
  void splitsEveryRealPrefix() {
    const char* prefixes[] = {"live2d", "tts", "llm", "idle", "perf", "annotations", "models", "splash", "config", "cubism", "mcp", "persona", "autostart", "app", "i18n"};
    for (const char* prefix : prefixes) {
      const std::string message = std::string("[") + prefix + "] 中文訊息";
      const LogCategory split = splitLogCategory(message);
      QCOMPARE(split.name, std::string(prefix));
      QCOMPARE(split.body, std::string("中文訊息"));
    }
  }

  // 沒有前綴的兩處實際訊息，本文要一字不動
  void keepsUnprefixedMessagesIntact() {
    const LogCategory a = splitLogCategory("SingleInstance: lock = C:/x locked = true");
    QVERIFY(a.name.empty());
    QCOMPARE(a.body, std::string("SingleInstance: lock = C:/x locked = true"));

    const LogCategory b = splitLogCategory("已輸出 frame dump: C:/x.png");
    QVERIFY(b.name.empty());
    QCOMPARE(b.body, std::string("已輸出 frame dump: C:/x.png"));
  }

  // SSE 的 [DONE] 哨兵是大寫，不能被當成分類
  void uppercaseBracketIsNotAPrefix() {
    const LogCategory split = splitLogCategory("[DONE] 串流結束");
    QVERIFY(split.name.empty());
    QCOMPARE(split.body, std::string("[DONE] 串流結束"));
  }

  // 只吃掉一個空格 —— [perf] 的表格靠後面那幾個空格對齊欄位
  void keepsAlignmentSpacesInBody() {
    const LogCategory split = splitLogCategory("[perf]   update     n=  12 avg=  1.23 ms");
    QCOMPARE(split.name, std::string("perf"));
    QCOMPARE(split.body, std::string("  update     n=  12 avg=  1.23 ms"));
  }

  // 前綴後面沒有空格就不算前綴，免得把 "[abc]def" 這種字串切壞
  void requiresSpaceAfterBracket() {
    const LogCategory split = splitLogCategory("[tts]沒有空格");
    QVERIFY(split.name.empty());
    QCOMPARE(split.body, std::string("[tts]沒有空格"));
  }

  // 整則訊息就只有一個前綴時，本文是空的
  void prefixOnlyMessageHasEmptyBody() {
    const LogCategory split = splitLogCategory("[tts]");
    QCOMPARE(split.name, std::string("tts"));
    QVERIFY(split.body.empty());
  }

  // 真前綴裡就有帶數字的（live2d、i18n），字元規則不能只收字母
  void allowsDigitsInName() {
    QCOMPARE(splitLogCategory("[live2d] 開不了模型").name, std::string("live2d"));
    QCOMPARE(splitLogCategory("[i18n] 缺鍵").name, std::string("i18n"));
  }

  // 帶連字號的分類名要收（Qt 自己的 category 長這樣）
  void allowsHyphenInName() {
    const LogCategory split = splitLogCategory("[qt-qpa] 平台外掛的話");
    QCOMPARE(split.name, std::string("qt-qpa"));
    QCOMPARE(split.body, std::string("平台外掛的話"));
  }

  // 各種不成立的形狀，本文都要原封不動
  void rejectsMalformedBrackets() {
    const char* untouched[] = {
      "[沒有收尾的括號 訊息", "[]  空的括號", "[has space] 中間有空格", "[404] 純數字不是分類名", "[averyveryverylongprefix] 超過長度上限", "訊息在前面 [tts] 前綴在後面", "",
    };
    for (const char* message : untouched) {
      const LogCategory split = splitLogCategory(message);
      QVERIFY2(split.name.empty(), message);
      QCOMPARE(split.body, std::string(message));
    }
  }

  // 中文全形括號長得像但不是 '['，不能誤中
  void fullWidthBracketIsNotAPrefix() {
    const LogCategory split = splitLogCategory("【tts】全形括號");
    QVERIFY(split.name.empty());
    QCOMPARE(split.body, std::string("【tts】全形括號"));
  }
};

QTEST_APPLESS_MAIN(TestLogCategory)
#include "test_log_category.moc"
