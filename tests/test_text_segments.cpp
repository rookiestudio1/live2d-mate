// 切句規則（core/text_segments.h）。
//
// 每一條斷言都對著一個具體的坑：3.14 被切成兩半、URL 斷在中間、
// 「好。」自己開一次合成請求、引號落到下一段開頭。
#include <QtTest>

#include <string>
#include <vector>

#include "core/string_util.h"
#include "core/text_segments.h"

using namespace l2m;

namespace {

// 不變式：所有段接起來、去掉空白之後要與原文相同
std::string squeeze(const std::string& text) {
  std::string out;
  for (const char ch : text) {
    if (static_cast<unsigned char>(ch) > ' ') out += ch;
  }
  return out;
}

std::string joined(const std::vector<std::string>& segments) {
  std::string out;
  for (const auto& piece : segments) out += piece;
  return squeeze(out);
}

}  // namespace

class TestTextSegments : public QObject {
  Q_OBJECT

private slots:
  // 中文句末標點各切一刀
  void splitsAtChineseTerminators() {
    const auto segments = splitIntoSpeechSegments("第一句話講得夠長了喔。第二句話也一樣長喔！第三句話同樣夠長囉？");
    QCOMPARE(segments.size(), size_t(3));
    QCOMPARE(segments[0], std::string("第一句話講得夠長了喔。"));
    QCOMPARE(segments[1], std::string("第二句話也一樣長喔！"));
    QCOMPARE(segments[2], std::string("第三句話同樣夠長囉？"));
  }

  // 連續標點與收尾引號跟著前一段走
  void keepsClosingPunctuationWithItsSentence() {
    const auto segments = splitIntoSpeechSegments("「你這傢伙到底在說什麼啊？！」本小姐完全聽不懂你的意思呢。");
    QCOMPARE(segments.size(), size_t(2));
    QCOMPARE(segments[0], std::string("「你這傢伙到底在說什麼啊？！」"));
    QCOMPARE(segments[1], std::string("本小姐完全聽不懂你的意思呢。"));
  }

  // 小數點與版本號不是句末
  void doesNotSplitInsideNumbers() {
    const auto segments = splitIntoSpeechSegments("圓周率大約是 3.14159 而版本是 v1.2 沒錯吧");
    QCOMPARE(segments.size(), size_t(1));
    QCOMPARE(joined(segments), squeeze("圓周率大約是 3.14159 而版本是 v1.2 沒錯吧"));
  }

  // 英文縮寫後面的句點不算句末
  void doesNotSplitAfterAbbreviations() {
    const auto segments = splitIntoSpeechSegments("Ask Dr. Smith about it. Then tell me.");
    QCOMPARE(segments.size(), size_t(2));
    QCOMPARE(segments[0], std::string("Ask Dr. Smith about it."));
    QCOMPARE(segments[1], std::string("Then tell me."));
  }

  // 英文句點後面接空白才算句末
  void splitsEnglishSentences() {
    const auto segments = splitIntoSpeechSegments("This is the first sentence. And here is the second one.");
    QCOMPARE(segments.size(), size_t(2));
    QCOMPARE(segments[0], std::string("This is the first sentence."));
  }

  // URL 不能斷在中間
  void neverSplitsInsideUrls() {
    const std::string text = "去看看 https://example.com/a,b,c/d?x=1&y=2 這個網址，然後再回來找本小姐吧。";
    const auto segments = splitIntoSpeechSegments(text, {20, 8});
    for (const auto& piece : segments) {
      const size_t at = piece.find("https://");
      if (at == std::string::npos) continue;
      // 有協定就要有完整的路徑，不能只剩前半截
      QVERIFY2(piece.find("y=2") != std::string::npos, piece.c_str());
    }
    QCOMPARE(joined(segments), squeeze(text));
  }

  // 太短的段併回前一段，不要為了「好。」單發一次合成
  void mergesShortSegmentsIntoThePrevious() {
    const auto segments = splitIntoSpeechSegments("本小姐才不是為了你才幫忙的呢。好。哼！");
    QCOMPARE(segments.size(), size_t(1));
    QCOMPARE(segments[0], std::string("本小姐才不是為了你才幫忙的呢。好。哼！"));
  }

  // 開頭的短句沒有前一段可併，要往**後**併進下一句。
  // 「哼！」單獨成段的話，角色會開口講兩個字然後靜音好幾秒等下一段合成完 ——
  // 那比晚一點開口還難聽。
  void mergesALeadingShortSegmentForward() {
    const auto segments = splitIntoSpeechSegments("好。本小姐這就去幫你處理那件麻煩事。");
    QCOMPARE(segments.size(), size_t(1));
    QCOMPARE(segments[0], std::string("好。本小姐這就去幫你處理那件麻煩事。"));
  }

  // 整段都很短時不能憑空消失
  void keepsAShortWholeText() {
    const auto segments = splitIntoSpeechSegments("好。");
    QCOMPARE(segments.size(), size_t(1));
    QCOMPARE(segments[0], std::string("好。"));
  }

  // 超過上限的長句在逗號處軟切
  void splitsLongSentencesAtCommas() {
    const std::string text =
      "本小姐今天心情不錯，所以勉為其難地告訴你一件事，"
      "那就是這段話真的非常非常長，長到必須被切開才行。";
    const auto segments = splitIntoSpeechSegments(text, {20, 8});
    QVERIFY(segments.size() >= 3);
    for (const auto& piece : segments) {
      QVERIFY2(strutil::utf8Length(piece) <= 20, piece.c_str());
    }
    QCOMPARE(joined(segments), squeeze(text));
  }

  // 完全沒有標點時硬切在上限
  void hardSplitsWhenThereIsNoBreakPoint() {
    const std::string text = "啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊";
    const auto segments = splitIntoSpeechSegments(text, {10, 3});
    QCOMPARE(segments.size(), size_t(3));
    for (const auto& piece : segments) QVERIFY(strutil::utf8Length(piece) <= 10);
    QCOMPARE(joined(segments), squeeze(text));
  }

  // 絕不切在多位元組序列中間：每一段都要是合法的 UTF-8
  void neverSplitsInsideAUtf8Sequence() {
    const std::string text = "本小姐的心情是這樣的🎀真的很好🎀不要懷疑🎀哼哼哼哼哼哼哼哼";
    const auto segments = splitIntoSpeechSegments(text, {6, 2});
    for (const auto& piece : segments) {
      // 續位元組不可能出現在段首
      QVERIFY2(!piece.empty() && (static_cast<unsigned char>(piece[0]) & 0xC0) != 0x80, piece.c_str());
    }
    QCOMPARE(joined(segments), squeeze(text));
  }

  // 空字串與純空白回空清單（呼叫端會退回原本的整段路徑）
  void emptyTextYieldsNoSegments() {
    QVERIFY(splitIntoSpeechSegments("").empty());
    QVERIFY(splitIntoSpeechSegments("   \n\t ").empty());
  }

  // 短句不切
  void shortTextStaysWhole() {
    const auto segments = splitIntoSpeechSegments("哼！");
    QCOMPARE(segments.size(), size_t(1));
    QCOMPARE(segments[0], std::string("哼！"));
  }
};

QTEST_APPLESS_MAIN(TestTextSegments)
#include "test_text_segments.moc"
