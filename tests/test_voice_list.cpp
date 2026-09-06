// 語音清單的顯示規則（core/voice_list.h）：
// 使用者切語系後看到的第一批語音是誰。
#include <QtTest>

#include <string>
#include <vector>

#include "core/voice_list.h"

using namespace l2m;

namespace {

VoiceInfo makeVoice(const std::string& id, const std::string& locale) {
  VoiceInfo info;
  info.id = id;
  info.name = id;
  info.locale = locale;
  info.engine = "edge";
  return info;
}

std::vector<std::string> localesOf(const std::vector<VoiceInfo>& voices) {
  std::vector<std::string> out;
  out.reserve(voices.size());
  for (const auto& v : voices) out.push_back(v.locale);
  return out;
}

}  // namespace

class TestVoiceList : public QObject {
  Q_OBJECT

private slots:
  // UI 語系對應的語音地區
  void uiLocaleMapsToVoiceLocale() {
    QCOMPARE(preferredVoiceLocale("ja"), std::string("ja-JP"));
    QCOMPARE(preferredVoiceLocale("ko"), std::string("ko-KR"));
    QCOMPARE(preferredVoiceLocale("en"), std::string("en-US"));
    QCOMPARE(preferredVoiceLocale("zh-TW"), std::string("zh-TW"));
    QCOMPARE(preferredVoiceLocale("zh-CN"), std::string("zh-CN"));
    // 未支援的語系沒有偏好，排序就退回固定順序
    QCOMPARE(preferredVoiceLocale("de"), std::string(""));
  }

  // 使用者切到韓文就該先看到韓文語音，不必自己往下捲
  void preferredLocaleComesFirst() {
    const std::vector<VoiceInfo> input{makeVoice("en1", "en-US"), makeVoice("ja1", "ja-JP"), makeVoice("ko1", "ko-KR"), makeVoice("tw1", "zh-TW")};
    QCOMPARE(localesOf(orderVoicesForDisplay(input, "ko")).front(), std::string("ko-KR"));
    QCOMPARE(localesOf(orderVoicesForDisplay(input, "ja")).front(), std::string("ja-JP"));
    QCOMPARE(localesOf(orderVoicesForDisplay(input, "zh-TW")).front(), std::string("zh-TW"));
  }

  // 偏好之外照 voiceDisplayLocales() 的固定順序
  void restFollowsFixedOrder() {
    const std::vector<VoiceInfo> input{makeVoice("en1", "en-US"), makeVoice("cn1", "zh-CN"), makeVoice("ja1", "ja-JP"), makeVoice("tw1", "zh-TW")};
    const std::vector<std::string> expected{"ja-JP", "zh-TW", "zh-CN", "en-US"};
    QCOMPARE(localesOf(orderVoicesForDisplay(input, "ja")), expected);
  }

  // 同一個地區內維持引擎回報的原順序（stable）
  void sameLocaleKeepsSourceOrder() {
    const std::vector<VoiceInfo> input{makeVoice("a", "ja-JP"), makeVoice("b", "ja-JP"), makeVoice("c", "ja-JP")};
    const auto shown = orderVoicesForDisplay(input, "ja");
    QCOMPARE(shown.size(), size_t(3));
    QCOMPARE(shown[0].id, std::string("a"));
    QCOMPARE(shown[1].id, std::string("b"));
    QCOMPARE(shown[2].id, std::string("c"));
  }

  // 不在顯示清單上的地區被濾掉
  void unlistedLocalesFilteredOut() {
    const std::vector<VoiceInfo> input{makeVoice("de1", "de-DE"), makeVoice("ja1", "ja-JP"), makeVoice("fr1", "fr-FR")};
    const std::vector<std::string> expected{"ja-JP"};
    QCOMPARE(localesOf(orderVoicesForDisplay(input, "ja")), expected);
  }

  // 一個都對不上（GPT-SoVITS 的自訂 preset）就照原樣顯示前幾個，
  // 總比讓使用者面對一片空白好
  void fallsBackWhenNothingMatches() {
    std::vector<VoiceInfo> input;
    for (int i = 0; i < 100; ++i) {
      input.push_back(makeVoice("p" + std::to_string(i), "custom"));
    }
    const auto shown = orderVoicesForDisplay(input, "zh-TW");
    QCOMPARE(static_cast<int>(shown.size()), kVoiceFallbackLimit);
    QCOMPARE(shown.front().id, std::string("p0"));
  }

  // 空清單就是空清單，不會憑空生出東西
  void emptyStaysEmpty() { QVERIFY(orderVoicesForDisplay({}, "zh-TW").empty()); }

  // 內建引擎的顯示名走 i18n，未知引擎回 nullptr 讓呼叫端退回引擎自報的 name
  void engineLabelKeyLookup() {
    QCOMPARE(std::string(engineLabelKey("edge")), std::string("tts.engine.edge"));
    QCOMPARE(std::string(engineLabelKey("voicebox")), std::string("tts.engine.voicebox"));
    QCOMPARE(std::string(engineLabelKey("custom")), std::string("tts.engine.custom"));
    QVERIFY(engineLabelKey("something-else") == nullptr);
  }
};

QTEST_APPLESS_MAIN(TestVoiceList)
#include "test_voice_list.moc"
