// 「一隻模型都沒有」時的引導：該不該問、問了要開哪個網址。
// 兩個判斷錯掉都是使用者第一眼就看到的東西 —— 網址對錯一格就是 404
// （官網沒有 zh-CHT，日文又是無前綴的根路徑），閘門錯一格則是
// 「每次啟動都彈對話框」或「開機自動啟動時當著登入畫面彈 modal」。
#include <QtTest>

#include "core/sample_models.h"

using namespace l2m;

class TestSampleModels : public QObject {
  Q_OBJECT

private slots:
  // 官網實際存在的三個語系版本，逐一釘住。日文是**沒有前綴的根路徑**，
  // 簡體中文的路徑名是 zh-CHS 而不是 zh-CN —— 兩個都不能用通則推出來
  void mapsTheLocalesTheSiteActuallyHas() {
    QCOMPARE(sampleModelsUrl("ja"), std::string("https://www.live2d.com/learn/sample/"));
    QCOMPARE(sampleModelsUrl("ko"), std::string("https://www.live2d.com/ko/learn/sample/"));
    QCOMPARE(sampleModelsUrl("zh-CN"), std::string("https://www.live2d.com/zh-CHS/learn/sample/"));
  }

  // 繁體中文沒有對應版本（實測 /zh-CHT/ 回 404），要退回英文而不是猜一個路徑出來
  void traditionalChineseFallsBackToEnglish() { QCOMPARE(sampleModelsUrl("zh-TW"), std::string("https://www.live2d.com/en/learn/sample/")); }

  // 英文與任何沒列到的字串都走同一條退路。給空字串也不能爆
  void unknownLocalesFallBackToEnglish() {
    const std::string english = "https://www.live2d.com/en/learn/sample/";
    QCOMPARE(sampleModelsUrl("en"), english);
    QCOMPARE(sampleModelsUrl(""), english);
    QCOMPARE(sampleModelsUrl("de"), english);
    QCOMPARE(sampleModelsUrl("zh"), english);
    // 大小寫與底線形式都不是本專案的語系寫法，照樣要落到英文而不是空字串
    QCOMPARE(sampleModelsUrl("JA"), english);
    QCOMPARE(sampleModelsUrl("zh_CN"), english);
  }

  // 每一條都要是 https 的官網網址 —— 這裡是要丟給瀏覽器開的東西
  void everyUrlIsHttpsOnTheOfficialSite() {
    for (const char* locale : {"en", "ja", "ko", "zh-CN", "zh-TW", "nope"}) {
      const std::string url = sampleModelsUrl(locale);
      QVERIFY2(url.rfind("https://www.live2d.com/", 0) == 0, url.c_str());
      QVERIFY2(url.back() == '/', url.c_str());
    }
  }

  // 正常啟動、沒有模型、還沒問過 —— 唯一該問的組合
  void asksOnlyOnAFreshEmptyInstall() { QVERIFY(shouldOfferSampleModels(false, false, false)); }

  // 有模型就不問，這才是絕大多數的啟動
  void neverAsksWhenAModelIsInstalled() {
    QVERIFY(!shouldOfferSampleModels(true, false, false));
    QVERIFY(!shouldOfferSampleModels(true, true, false));
  }

  // 問過就不再問。只記「問過」不記答案：選了「稍後再說」也不該每次啟動再彈一次
  void neverAsksTwice() { QVERIFY(!shouldOfferSampleModels(false, true, false)); }

  // --hidden 是開機自動啟動走的路，使用者要的是安靜地縮在系統匣。
  // 這種情況不問（呼叫端也因此不會記旗標，留到下次正常啟動再問）
  void neverAsksWhenStartedHidden() {
    QVERIFY(!shouldOfferSampleModels(false, false, true));
    QVERIFY(!shouldOfferSampleModels(false, true, true));
  }
};

QTEST_GUILESS_MAIN(TestSampleModels)
#include "test_sample_models.moc"
