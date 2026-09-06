// 句段管線：把整段引擎包成串流引擎（core/tts_segment_pipeline.h）。
//
// 重點不是「有沒有切句」（那是 test_text_segments 的事），而是時序：
// 第一句一回來就要送出去、第二句要在那之前就先發出請求、順序不能亂、
// 取消之後不能再有回呼。
#include <QtTest>

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "core/tts_segment_pipeline.h"

using namespace l2m;

namespace {

// 內層假引擎：每次合成都掛起來，由測試決定什麼時候、以什麼順序完成。
class ManualEngine : public TtsEngine {
public:
  struct Pending {
    std::string text;
    TtsStreamSink sink;
    bool settled = false;
  };

  const std::string& id() const override { return id_; }
  const std::string& name() const override { return name_; }

  void isAvailable(std::function<void(bool)> done) override { done(true); }
  void listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) override { done({}, ""); }

  std::unique_ptr<TtsRequestHandle> synthesize(const std::string& text, const SpeakOptions&, TtsStreamSink sink) override {
    pending.push_back({text, std::move(sink), false});
    auto cancelled = std::make_shared<bool>(false);
    return std::make_unique<FunctionTtsHandle>(cancelled, [this] { ++cancels; });
  }

  // 完成第 index 個尚未結算的請求。越界就當作沒事 —— 測試常常想表達
  // 「就算再多來一個回覆也不該有反應」，不該因此爆掉。
  void finish(size_t index) {
    if (index >= pending.size()) return;
    Pending& job = pending[index];
    if (job.settled) return;
    job.settled = true;
    const std::string payload = "[" + job.text + "]";
    if (job.sink.onOpen) job.sink.onOpen("audio/wav");
    if (job.sink.onChunk) job.sink.onChunk(payload.data(), payload.size());
    if (job.sink.onDone) job.sink.onDone();
  }

  void failAt(size_t index, const std::string& error) {
    if (index >= pending.size()) return;
    Pending& job = pending[index];
    if (job.settled) return;
    job.settled = true;
    if (job.sink.onError) job.sink.onError(error);
  }

  std::deque<Pending> pending;
  int cancels = 0;

private:
  std::string id_ = "manual";
  std::string name_ = "Manual";
};

// 記錄 sink 收到的事件序列
struct Recorder {
  std::vector<std::string> events;
  std::string audio;
  std::string error;
  bool done = false;

  TtsStreamSink sink() {
    TtsStreamSink out;
    out.onOpen = [this](std::string mime) { events.push_back("open:" + mime); };
    out.onChunk = [this](const char* data, size_t size) {
      audio.append(data, size);
      events.push_back("chunk");
    };
    out.onSegmentEnd = [this] { events.push_back("segmentEnd"); };
    out.onDone = [this] {
      done = true;
      events.push_back("done");
    };
    out.onError = [this](std::string message) {
      error = std::move(message);
      events.push_back("error");
    };
    return out;
  }
};

const char* kThreeSentences = "第一句話講得夠長了喔。第二句話也一樣長喔。第三句話同樣夠長囉。";

}  // namespace

class TestTtsSegments : public QObject {
  Q_OBJECT

private slots:
  // 只有一句就不套管線，直接透傳內層（多一層只會多一次搬運）
  void singleSentenceBypassesThePipeline() {
    auto inner = std::make_unique<ManualEngine>();
    ManualEngine* raw = inner.get();
    SegmentedTtsEngine engine(std::move(inner));

    Recorder recorder;
    engine.synthesize("哼！", {}, recorder.sink());

    QCOMPARE(raw->pending.size(), size_t(1));
    QCOMPARE(raw->pending[0].text, std::string("哼！"));
    raw->finish(0);
    QCOMPARE(recorder.audio, std::string("[哼！]"));
    QVERIFY(recorder.done);
  }

  // id 與 name 透傳：包不包管線對 TtsManager 與設定頁來說是實作細節
  void forwardsIdentityToTheInnerEngine() {
    auto inner = std::make_unique<ManualEngine>();
    SegmentedTtsEngine engine(std::move(inner));
    QCOMPARE(engine.id(), std::string("manual"));
    QCOMPARE(engine.name(), std::string("Manual"));
    QVERIFY(engine.streams());
  }

  // 預設不預取：一開始只有一個請求在飛。實測本機推論引擎上兩個請求會互相
  // 搶資源，先開口的時間反而變成兩倍（理由寫在 tts_segment_pipeline.h）。
  void doesNotPrefetchByDefault() {
    auto inner = std::make_unique<ManualEngine>();
    ManualEngine* raw = inner.get();
    SegmentedTtsEngine engine(std::move(inner));

    Recorder recorder;
    engine.synthesize(kThreeSentences, {}, recorder.sink());

    QCOMPARE(raw->pending.size(), size_t(1 + kSegmentPrefetch));
    QCOMPARE(raw->pending[0].text, std::string("第一句話講得夠長了喔。"));
    QVERIFY(recorder.events.empty());
  }

  // 管線的全部意義：第二句的請求在第一句**交給播放器的那一刻**就送出去，
  // 而不是等它播完 —— 合成與播放因此完全重疊。
  void requestsTheNextSegmentAsSoonAsThisOneIsHandedOver() {
    auto inner = std::make_unique<ManualEngine>();
    ManualEngine* raw = inner.get();
    SegmentedTtsEngine engine(std::move(inner));

    Recorder recorder;
    engine.synthesize(kThreeSentences, {}, recorder.sink());
    const size_t before = raw->pending.size();

    raw->finish(0);

    // 第一句剛送進播放器（還在播），第二句的請求已經在路上
    QCOMPARE(recorder.events.back(), std::string("segmentEnd"));
    QCOMPARE(raw->pending.size(), before + 1);
    QCOMPARE(raw->pending.back().text, std::string("第二句話也一樣長喔。"));
  }

  // 有設預取時就真的多發請求
  void prefetchIssuesExtraRequestsUpFront() {
    auto inner = std::make_unique<ManualEngine>();
    ManualEngine* raw = inner.get();
    SegmentedTtsEngine engine(std::move(inner), {}, 1);

    Recorder recorder;
    engine.synthesize(kThreeSentences, {}, recorder.sink());

    QCOMPARE(raw->pending.size(), size_t(2));
    QCOMPARE(raw->pending[1].text, std::string("第二句話也一樣長喔。"));
  }

  // 第一句一回來就送出去，不等後面
  void emitsTheFirstSegmentImmediately() {
    auto inner = std::make_unique<ManualEngine>();
    ManualEngine* raw = inner.get();
    SegmentedTtsEngine engine(std::move(inner));

    Recorder recorder;
    engine.synthesize(kThreeSentences, {}, recorder.sink());
    raw->finish(0);

    QCOMPARE(recorder.audio, std::string("[第一句話講得夠長了喔。]"));
    QCOMPARE(recorder.events, (std::vector<std::string>{"open:audio/wav", "chunk", "segmentEnd"}));
    QVERIFY(!recorder.done);
  }

  // 後面的句子先合成完也不能插隊
  void keepsSegmentOrderEvenWhenLaterOnesFinishFirst() {
    auto inner = std::make_unique<ManualEngine>();
    ManualEngine* raw = inner.get();
    // 預設不預取，同時只有一句在飛，亂序根本發生不了 —— 要驗這條就得開預取
    SegmentedTtsEngine engine(std::move(inner), {}, 1);

    Recorder recorder;
    engine.synthesize(kThreeSentences, {}, recorder.sink());

    raw->finish(1);  // 第二句先好
    QVERIFY(recorder.audio.empty());

    raw->finish(0);
    // 第一句一到，兩段一起依序放出來
    QCOMPARE(recorder.audio, std::string("[第一句話講得夠長了喔。][第二句話也一樣長喔。]"));

    raw->finish(2);
    QVERIFY(recorder.done);
    QCOMPARE(recorder.audio, std::string("[第一句話講得夠長了喔。][第二句話也一樣長喔。]"
                                         "[第三句話同樣夠長囉。]"));
  }

  // 每一段結束都要通知一次，播放器才換得了解碼器
  void reportsOneSegmentEndPerSentence() {
    auto inner = std::make_unique<ManualEngine>();
    ManualEngine* raw = inner.get();
    SegmentedTtsEngine engine(std::move(inner));

    Recorder recorder;
    engine.synthesize(kThreeSentences, {}, recorder.sink());
    raw->finish(0);
    raw->finish(1);
    raw->finish(2);

    int segmentEnds = 0;
    for (const auto& event : recorder.events) {
      if (event == "segmentEnd") ++segmentEnds;
    }
    QCOMPARE(segmentEnds, 3);
    QCOMPARE(recorder.events.back(), std::string("done"));
  }

  // 第一句就失敗 → 一個位元組都沒送出去，上層還可以換引擎重試整段
  void failureOnTheFirstSegmentEmitsNothing() {
    auto inner = std::make_unique<ManualEngine>();
    ManualEngine* raw = inner.get();
    SegmentedTtsEngine engine(std::move(inner));

    Recorder recorder;
    engine.synthesize(kThreeSentences, {}, recorder.sink());
    raw->failAt(0, "壞了");

    QVERIFY(recorder.audio.empty());
    QCOMPARE(recorder.error, std::string("壞了"));
    QVERIFY(!recorder.done);
  }

  // 中途失敗 → 前面的段已經送出去了，只能報錯，不能重來
  void failureMidwayKeepsWhatWasAlreadyEmitted() {
    auto inner = std::make_unique<ManualEngine>();
    ManualEngine* raw = inner.get();
    SegmentedTtsEngine engine(std::move(inner));

    Recorder recorder;
    engine.synthesize(kThreeSentences, {}, recorder.sink());
    raw->finish(0);
    raw->failAt(1, "第二句壞了");

    QCOMPARE(recorder.audio, std::string("[第一句話講得夠長了喔。]"));
    QCOMPARE(recorder.error, std::string("第二句壞了"));
    QVERIFY(!recorder.done);
  }

  // 取消：中止所有在飛的請求，而且之後遲到的回覆一概不算數
  void cancelStopsEverything() {
    auto inner = std::make_unique<ManualEngine>();
    ManualEngine* raw = inner.get();
    SegmentedTtsEngine engine(std::move(inner));

    Recorder recorder;
    auto handle = engine.synthesize(kThreeSentences, {}, recorder.sink());
    handle->cancel();

    QCOMPARE(raw->cancels, static_cast<int>(1 + kSegmentPrefetch));
    QCOMPARE(raw->pending.size(), size_t(1 + kSegmentPrefetch));
    raw->finish(0);
    raw->finish(1);

    QVERIFY(recorder.events.empty());
    QVERIFY(!recorder.done);
    QVERIFY(recorder.error.empty());
  }
};

QTEST_APPLESS_MAIN(TestTtsSegments)
#include "test_tts_segments.moc"
