// TtsManager：引擎選擇、可用性探測與 fallback、串流的承諾點與取消。
//
// 引擎介面是回呼式的，假引擎一律同步觸發回呼。
// 「四個引擎都在」指的是 edge / gptsovits / voicebox / sapi，預設引擎是 edge。
// 引擎陣列由 main.cpp 組裝（TtsManager 不自己 new 引擎），
// 所以「預設引擎註冊」那組驗的是註冊順序的常數清單。
#include <QtTest>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/tts_manager.h"

using namespace l2m;

namespace {

// 假引擎：記錄每次合成的參數，可設定成不可用或必定失敗
class FakeEngine : public TtsEngine {
public:
  struct Call {
    std::string text;
    SpeakOptions options;
  };

  FakeEngine(std::string id, bool available = true, bool fail = false) : id_(std::move(id)), name_("fake-" + id_), available_(available), fail_(fail) {}

  const std::string& id() const override { return id_; }
  const std::string& name() const override { return name_; }

  void isAvailable(std::function<void(bool)> done) override {
    ++availabilityChecks;
    if (deferAvailability) {
      pendingAvailability.push_back(std::move(done));
      return;
    }
    done(available_);
  }

  // deferAvailability 模式下手動觸發偵測完成，模擬線上服務的非同步回應
  void flushAvailability() {
    auto pending = std::move(pendingAvailability);
    pendingAvailability.clear();
    for (auto& done : pending) done(available_);
  }

  void listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) override { done(voices, ""); }

  // 一律同步回呼。fail_ 的引擎**不送任何 chunk**，所以永遠不會走到承諾點，
  // fallback 行為與串流化之前完全一致 —— 既有那批測試因此一行都不用改。
  std::unique_ptr<TtsRequestHandle> synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) override {
    calls.push_back({text, options});
    auto cancelled = std::make_shared<bool>(false);
    auto handle = std::make_unique<FunctionTtsHandle>(cancelled, [this] { ++cancels; });

    if (deferSynthesis) {
      pendingSynthesis.push_back(std::move(sink));
      return handle;
    }
    if (fail_) {
      if (sink.onError) sink.onError(id_ + " 壞了");
      return handle;
    }
    emit_(sink, text);
    return handle;
  }

  // deferSynthesis 模式下手動推進：先送一塊 chunk（跨過承諾點）再失敗
  void flushChunkThenFail(const std::string& text) {
    auto pending = std::move(pendingSynthesis);
    pendingSynthesis.clear();
    for (auto& sink : pending) {
      const std::string payload = id_ + ":" + text;
      if (sink.onOpen) sink.onOpen("audio/wav");
      if (sink.onChunk) sink.onChunk(payload.data(), payload.size());
      if (sink.onError) sink.onError(id_ + " 播到一半壞了");
    }
  }

  std::vector<Call> calls;
  std::vector<VoiceInfo> voices;
  int availabilityChecks = 0;
  int cancels = 0;
  bool deferAvailability = false;
  bool deferSynthesis = false;
  std::vector<std::function<void(bool)>> pendingAvailability;
  std::vector<TtsStreamSink> pendingSynthesis;

  void emit_(const TtsStreamSink& sink, const std::string& text) {
    const std::string payload = id_ + ":" + text;
    if (sink.onOpen) sink.onOpen("audio/wav");
    if (sink.onChunk) sink.onChunk(payload.data(), payload.size());
    if (sink.onDone) sink.onDone();
  }

private:
  std::string id_;
  std::string name_;
  bool available_;
  bool fail_;
};

struct Fixture {
  std::vector<FakeEngine*> raw;
  std::unique_ptr<TtsManager> manager;
};

Fixture makeManager(std::vector<std::unique_ptr<FakeEngine>> fakes) {
  Fixture fixture;
  std::vector<std::unique_ptr<TtsEngine>> engines;
  for (auto& fake : fakes) {
    fixture.raw.push_back(fake.get());
    engines.push_back(std::move(fake));
  }
  fixture.manager = std::make_unique<TtsManager>(std::move(engines));
  return fixture;
}

std::unique_ptr<FakeEngine> fake(const std::string& id, bool available = true, bool fail = false) { return std::make_unique<FakeEngine>(id, available, fail); }

}  // namespace

class TestTts : public QObject {
  Q_OBJECT

private slots:
  // 使用指定的引擎與語音
  void usesRequestedEngineAndVoice() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("a"));
    fakes.push_back(fake("b"));
    Fixture fixture = makeManager(std::move(fakes));

    TtsManager::SynthesizeRequest request;
    request.engine = "b";
    request.voice = "v1";
    request.rate = 1.5;

    std::optional<SynthesizeOutcome> outcome;
    fixture.manager->synthesize("哈囉", request, [&outcome](std::optional<SynthesizeOutcome> result, std::string) { outcome = std::move(result); });

    QVERIFY(outcome.has_value());
    QCOMPARE(outcome->engine, std::string("b"));
    QCOMPARE(outcome->voice.value_or(""), std::string("v1"));
    QCOMPARE(fixture.raw[1]->calls.size(), size_t(1));
    QCOMPARE(fixture.raw[1]->calls[0].text, std::string("哈囉"));
    QCOMPARE(fixture.raw[1]->calls[0].options.voice.value_or(""), std::string("v1"));
    QCOMPARE(fixture.raw[1]->calls[0].options.rate.value_or(0), 1.5);
    QCOMPARE(fixture.raw[0]->calls.size(), size_t(0));
  }

  // 指定的引擎失敗時 fallback 到下一個可用引擎
  void fallsBackToNextEngine() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("broken", true, true));
    fakes.push_back(fake("backup"));
    Fixture fixture = makeManager(std::move(fakes));

    TtsManager::SynthesizeRequest request;
    request.engine = "broken";
    request.voice = "v1";

    std::optional<SynthesizeOutcome> outcome;
    fixture.manager->synthesize("測試", request, [&outcome](std::optional<SynthesizeOutcome> result, std::string) { outcome = std::move(result); });

    QVERIFY(outcome.has_value());
    QCOMPARE(outcome->engine, std::string("backup"));
    // fallback 不沿用原引擎的 voice —— 那是另一個引擎的識別字串
    QVERIFY(!outcome->voice.has_value());
    QVERIFY(!fixture.raw[1]->calls[0].options.voice.has_value());
  }

  // 略過不可用的引擎
  void skipsUnavailableEngines() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("offline", false));
    fakes.push_back(fake("usable"));
    Fixture fixture = makeManager(std::move(fakes));

    std::optional<SynthesizeOutcome> outcome;
    fixture.manager->synthesize("嗨", {}, [&outcome](std::optional<SynthesizeOutcome> result, std::string) { outcome = std::move(result); });

    QVERIFY(outcome.has_value());
    QCOMPARE(outcome->engine, std::string("usable"));
    QCOMPARE(fixture.raw[0]->calls.size(), size_t(0));
  }

  // 全部引擎都失敗時，錯誤裡帶著各引擎的原因
  void reportsEveryEngineFailure() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("a", true, true));
    fakes.push_back(fake("b", true, true));
    Fixture fixture = makeManager(std::move(fakes));

    std::string error;
    fixture.manager->synthesize("壞掉", {}, [&error](std::optional<SynthesizeOutcome> result, std::string err) {
      QVERIFY(!result.has_value());
      error = std::move(err);
    });

    QVERIFY(error.find("a: a 壞了") != std::string::npos);
    QVERIFY(error.find("b: b 壞了") != std::string::npos);
  }

  // 空字串直接拒絕
  void rejectsBlankText() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("a"));
    Fixture fixture = makeManager(std::move(fakes));

    std::string error;
    fixture.manager->synthesize("   ", {}, [&error](std::optional<SynthesizeOutcome> result, std::string err) {
      QVERIFY(!result.has_value());
      error = std::move(err);
    });
    QVERIFY(error.find("must not be empty") != std::string::npos);
  }

  // listEngines 帶出可用狀態，而且只偵測一次
  void listEnginesCachesAvailability() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("a"));
    Fixture fixture = makeManager(std::move(fakes));

    std::vector<TtsEngineInfo> infos;
    fixture.manager->listEngines([](std::vector<TtsEngineInfo>) {});
    fixture.manager->listEngines([&infos](std::vector<TtsEngineInfo> list) { infos = list; });

    QCOMPARE(fixture.raw[0]->availabilityChecks, 1);
    QCOMPARE(infos.size(), size_t(1));
    QCOMPARE(infos[0].id, std::string("a"));
    QCOMPARE(infos[0].name, std::string("fake-a"));
    QVERIFY(infos[0].available);
  }

  // resetCache 之後會重新偵測可用性
  void resetCacheReprobes() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("a"));
    Fixture fixture = makeManager(std::move(fakes));

    fixture.manager->listEngines([](std::vector<TtsEngineInfo>) {});
    fixture.manager->resetCache();
    fixture.manager->listEngines([](std::vector<TtsEngineInfo>) {});

    QCOMPARE(fixture.raw[0]->availabilityChecks, 2);
  }

  // 合成失敗會把該引擎的可用性標記清掉，下一次重新偵測
  void failureForgetsAvailability() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("broken", true, true));
    fakes.push_back(fake("backup"));
    Fixture fixture = makeManager(std::move(fakes));

    fixture.manager->synthesize("測試", {}, [](std::optional<SynthesizeOutcome>, std::string) {});
    const int afterFirst = fixture.raw[0]->availabilityChecks;
    fixture.manager->synthesize("再一次", {}, [](std::optional<SynthesizeOutcome>, std::string) {});

    QVERIFY(fixture.raw[0]->availabilityChecks > afterFirst);
  }

  // 「不可用」的快取有 TTL，過期會重探 ——
  // 啟動時網路還沒好探成不可用，不該永久卡在 fallback 引擎
  void unavailableCacheExpires() {
    int64_t now = 0;
    std::vector<std::unique_ptr<TtsEngine>> engines;
    auto offline = fake("edge", false);
    FakeEngine* rawOffline = offline.get();
    engines.push_back(std::move(offline));
    engines.push_back(fake("sapi"));
    TtsManager manager(std::move(engines), [&now] { return now; });

    const auto ignore = [](std::optional<SynthesizeOutcome>, std::string) {};
    manager.synthesize("一", {}, ignore);
    QCOMPARE(rawOffline->availabilityChecks, 1);

    now += 1000;
    manager.synthesize("二", {}, ignore);
    QCOMPARE(rawOffline->availabilityChecks, 1);  // TTL 內信任「不可用」快取

    now += kUnavailableRetryMs;
    manager.synthesize("三", {}, ignore);
    QCOMPARE(rawOffline->availabilityChecks, 2);  // 過期重探
  }

  // 偵測進行中時，後到的查詢共用同一次偵測 ——
  // 啟動時引擎清單與語音清單同時打進來，不該對線上服務發兩個一樣的請求
  void concurrentChecksShareOneProbe() {
    std::vector<std::unique_ptr<TtsEngine>> engines;
    auto slow = fake("edge");
    slow->deferAvailability = true;
    FakeEngine* rawSlow = slow.get();
    engines.push_back(std::move(slow));
    TtsManager manager(std::move(engines));

    int callbacks = 0;
    manager.listEngines([&callbacks](std::vector<TtsEngineInfo>) { ++callbacks; });
    manager.listEngines([&callbacks](std::vector<TtsEngineInfo>) { ++callbacks; });
    QCOMPARE(rawSlow->availabilityChecks, 1);
    QCOMPARE(callbacks, 0);

    rawSlow->flushAvailability();
    QCOMPARE(callbacks, 2);  // 兩個呼叫端都拿到同一次偵測的結果
  }

  // 預設引擎是陣列的第一個；main.cpp 註冊的順序即 fallback 順序
  void defaultEngineIsFirst() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("edge"));
    fakes.push_back(fake("gptsovits"));
    fakes.push_back(fake("voicebox"));
    fakes.push_back(fake("sapi"));
    Fixture fixture = makeManager(std::move(fakes));

    QCOMPARE(fixture.manager->defaultEngineId(), std::string("edge"));

    std::vector<std::string> ids;
    fixture.manager->listEngines([&ids](std::vector<TtsEngineInfo> list) {
      for (const auto& info : list) ids.push_back(info.id);
    });
    QCOMPARE(ids, (std::vector<std::string>{"edge", "gptsovits", "voicebox", "sapi"}));
  }

  // ── 串流：承諾點與取消 ──

  // 已經送出第一塊音訊之後才失敗，就不能再換引擎（否則前半句會重播一次）
  void committedFailureDoesNotFallBack() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("a"));
    fakes.push_back(fake("b"));
    Fixture fixture = makeManager(std::move(fakes));
    fixture.raw[0]->deferSynthesis = true;

    std::string error;
    bool committed = false;
    int chunks = 0;
    TtsManager::StreamSink sink;
    sink.onOpen = [](const std::string&) { return true; };  // 呼叫端真的會出聲
    sink.onChunk = [&chunks](const char*, size_t) { ++chunks; };
    sink.onError = [&error, &committed](std::string message, bool wasCommitted) {
      error = std::move(message);
      committed = wasCommitted;
    };
    fixture.manager->stream("嗨", {}, sink);
    fixture.raw[0]->flushChunkThenFail("嗨");

    QCOMPARE(chunks, 1);
    QVERIFY(committed);
    QCOMPARE(error, std::string("a 播到一半壞了"));
    // 第二個引擎連碰都沒碰
    QCOMPARE(fixture.raw[1]->calls.size(), size_t(0));
  }

  // 還沒出聲就失敗（例如整段緩衝模式）仍然照舊往下試
  void uncommittedFailureStillFallsBack() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("a"));
    // 第二家也讓它壞，錯誤才會一路傳回來，看得到 committed 的最終值
    fakes.push_back(fake("b", true, true));
    Fixture fixture = makeManager(std::move(fakes));
    fixture.raw[0]->deferSynthesis = true;

    std::string error;
    bool errorFired = false;
    bool committed = true;
    TtsManager::StreamSink sink;
    // 緩衝模式：位元組只是進了記憶體，一個音都還沒出去
    sink.onOpen = [](const std::string&) { return false; };
    sink.onChunk = [](const char*, size_t) {};
    sink.onError = [&error, &committed, &errorFired](std::string message, bool wasCommitted) {
      errorFired = true;
      error = std::move(message);
      committed = wasCommitted;
    };
    fixture.manager->stream("嗨", {}, sink);
    fixture.raw[0]->flushChunkThenFail("嗨");

    // 送過 chunk 但呼叫端沒承諾 → 照樣換下一家
    QCOMPARE(fixture.raw[1]->calls.size(), size_t(1));
    QVERIFY(errorFired);
    QVERIFY(!committed);
    QVERIFY(error.find("All TTS engines failed") != std::string::npos);
  }

  // 取消：中止目前的引擎，而且不再往下試
  void cancelStopsFallbackChain() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("a"));
    fakes.push_back(fake("b"));
    Fixture fixture = makeManager(std::move(fakes));
    fixture.raw[0]->deferSynthesis = true;

    int chunks = 0;
    std::string error;
    TtsManager::StreamSink sink;
    sink.onOpen = [](const std::string&) { return true; };
    sink.onChunk = [&chunks](const char*, size_t) { ++chunks; };
    sink.onError = [&error](std::string message, bool) { error = std::move(message); };

    auto handle = fixture.manager->stream("嗨", {}, sink);
    handle->cancel();
    // 取消之後遲到的回呼一概不算數
    fixture.raw[0]->flushChunkThenFail("嗨");

    QCOMPARE(fixture.raw[0]->cancels, 1);
    QCOMPARE(chunks, 0);
    QVERIFY(error.empty());
    QCOMPARE(fixture.raw[1]->calls.size(), size_t(0));
  }

  // 整段緩衝的舊介面：fallback 語意與串流化之前一模一樣
  void bufferedSynthesizeStillConcatenatesChunks() {
    std::vector<std::unique_ptr<FakeEngine>> fakes;
    fakes.push_back(fake("a"));
    Fixture fixture = makeManager(std::move(fakes));

    std::optional<SynthesizeOutcome> outcome;
    fixture.manager->synthesize("嗨", {}, [&outcome](std::optional<SynthesizeOutcome> result, std::string) { outcome = std::move(result); });

    QVERIFY(outcome.has_value());
    QCOMPARE(std::string(outcome->audio.begin(), outcome->audio.end()), std::string("a:嗨"));
    QCOMPARE(outcome->mime, std::string("audio/wav"));
    QCOMPARE(outcome->engine, std::string("a"));
  }
};

QTEST_APPLESS_MAIN(TestTts)
#include "test_tts.moc"
