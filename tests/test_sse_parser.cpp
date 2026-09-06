// SSE 增量解析器：事件跨 chunk 邊界、CRLF/LF 混用、多行 data、註解行、finish 補派發。
#include <QtTest>

#include <string>
#include <vector>

#include "core/sse_parser.h"

using namespace l2m;

namespace {

// 把整段文字餵進去（一口氣）
std::vector<SseEvent> feedAll(SseParser& parser, const std::string& text) { return parser.feed(text.data(), text.size()); }

}  // namespace

class TestSseParser : public QObject {
  Q_OBJECT

private slots:
  void parsesSingleEvent() {
    SseParser parser;
    const auto events = feedAll(parser, "data: hello\n\n");
    QCOMPARE(events.size(), size_t(1));
    QCOMPARE(events[0].event, std::string());
    QCOMPARE(events[0].data, std::string("hello"));
  }

  void parsesNamedEvent() {
    SseParser parser;
    const auto events = feedAll(parser, "event: message_start\ndata: {\"type\":\"message_start\"}\n\n");
    QCOMPARE(events.size(), size_t(1));
    QCOMPARE(events[0].event, std::string("message_start"));
    QCOMPARE(events[0].data, std::string("{\"type\":\"message_start\"}"));
  }

  // 事件被切在任意位置（field 名稱中間也一樣）都要拼得回來
  void survivesArbitraryChunkBoundaries() {
    const std::string text = "event: e1\ndata: first\n\ndata: second\n\n";
    // 每一種切法都跑一遍
    for (size_t cut = 1; cut < text.size(); ++cut) {
      SseParser parser;
      std::vector<SseEvent> events = parser.feed(text.data(), cut);
      const auto more = parser.feed(text.data() + cut, text.size() - cut);
      events.insert(events.end(), more.begin(), more.end());
      QCOMPARE(events.size(), size_t(2));
      QCOMPARE(events[0].event, std::string("e1"));
      QCOMPARE(events[0].data, std::string("first"));
      QCOMPARE(events[1].data, std::string("second"));
    }
  }

  void handlesCrlfAndBareCr() {
    SseParser parser;
    const auto events = feedAll(parser, "data: a\r\n\r\ndata: b\r\r");
    QCOMPARE(events.size(), size_t(2));
    QCOMPARE(events[0].data, std::string("a"));
    QCOMPARE(events[1].data, std::string("b"));
  }

  // CRLF 被切在兩個 chunk 之間：LF 不能被當成第二個換行
  void crlfSplitAcrossChunks() {
    SseParser parser;
    auto events = feedAll(parser, "data: a\r");
    const auto more = feedAll(parser, "\n\n");
    events.insert(events.end(), more.begin(), more.end());
    QCOMPARE(events.size(), size_t(1));
    QCOMPARE(events[0].data, std::string("a"));
  }

  // 多行 data 以 \n 串接
  void joinsMultiLineData() {
    SseParser parser;
    const auto events = feedAll(parser, "data: line1\ndata: line2\n\n");
    QCOMPARE(events.size(), size_t(1));
    QCOMPARE(events[0].data, std::string("line1\nline2"));
  }

  // 註解行（keep-alive）整行忽略；值開頭只去一個空格
  void ignoresCommentsAndStripsOneSpace() {
    SseParser parser;
    const auto events = feedAll(parser, ": keep-alive\ndata:  two spaces\n\n");
    QCOMPARE(events.size(), size_t(1));
    QCOMPARE(events[0].data, std::string(" two spaces"));
  }

  // 伺服器送完最後一個事件就關連線（沒有結尾空行）：finish 要補派發
  void finishFlushesTrailingEvent() {
    SseParser parser;
    QCOMPARE(feedAll(parser, "data: [DONE]").size(), size_t(0));
    const auto events = parser.finish();
    QCOMPARE(events.size(), size_t(1));
    QCOMPARE(events[0].data, std::string("[DONE]"));
  }

  void finishWithNothingPendingIsEmpty() {
    SseParser parser;
    feedAll(parser, "data: a\n\n");
    QCOMPARE(parser.finish().size(), size_t(0));
  }

  // event 名稱在事件派發後要歸零，不能黏到下一個事件
  void eventNameResetsBetweenEvents() {
    SseParser parser;
    const auto events = feedAll(parser, "event: ping\ndata: {}\n\ndata: x\n\n");
    QCOMPARE(events.size(), size_t(2));
    QCOMPARE(events[0].event, std::string("ping"));
    QCOMPARE(events[1].event, std::string());
  }
};

QTEST_GUILESS_MAIN(TestSseParser)
#include "test_sse_parser.moc"
