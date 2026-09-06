#include "tts_manager.h"

#include <QDebug>
#include <QElapsedTimer>

#include <memory>
#include <utility>

#include "string_util.h"

namespace l2m {

namespace {

int64_t monotonicNowMs() {
  static QElapsedTimer timer = [] {
    QElapsedTimer t;
    t.start();
    return t;
  }();
  return timer.elapsed();
}

std::string joinErrors(const std::vector<std::string>& errors) {
  std::string out;
  for (size_t i = 0; i < errors.size(); ++i) {
    if (i > 0) out += "; ";
    out += errors[i];
  }
  return out;
}

// stream() 的進行中狀態。handle 與 tryNext 鏈共用，所以 cancel() 既能中止
// 目前的引擎，也能讓還沒跑到的那幾家不再被試。
struct StreamState {
  std::unique_ptr<TtsRequestHandle> current;
  // 每換一次引擎就 +1。引擎在 synthesize() 內部就同步失敗時，tryNext 會遞迴
  // 進去把下一家的 handle 存好，這時候外層再存自己那個就會把它蓋掉 ——
  // 靠這個序號判斷「我這一輪已經被取代了」。
  uint64_t generation = 0;
  bool cancelled = false;
  // 第一塊音訊已經送進播放器 ＝ 承諾點之後，不再 fallback
  bool committed = false;
  bool finished = false;
  std::string mime;
};

class ManagerHandle : public TtsRequestHandle {
public:
  explicit ManagerHandle(std::shared_ptr<StreamState> state) : state_(std::move(state)) {}

  void cancel() override {
    if (!state_ || state_->cancelled) return;
    state_->cancelled = true;
    if (state_->current) state_->current->cancel();
  }

private:
  std::shared_ptr<StreamState> state_;
};

}  // namespace

TtsManager::TtsManager(std::vector<std::unique_ptr<TtsEngine>> engines, std::function<int64_t()> nowMs) : engines_(std::move(engines)), nowMs_(nowMs ? std::move(nowMs) : monotonicNowMs) {}

TtsManager::~TtsManager() = default;

std::string TtsManager::defaultEngineId() const { return engines_.empty() ? std::string("edge") : engines_.front()->id(); }

TtsEngine* TtsManager::find(const std::optional<std::string>& id) const {
  if (!id.has_value() || id->empty()) return nullptr;
  for (const auto& engine : engines_) {
    if (engine->id() == *id) return engine.get();
  }
  return nullptr;
}

void TtsManager::checkAvailable(TtsEngine* engine, std::function<void(bool)> done) {
  const std::string id = engine->id();

  const auto cached = availability_.find(id);
  if (cached != availability_.end()) {
    // 「不可用」只信任 kUnavailableRetryMs，過期就重探（見標頭的說明）
    if (cached->second.ok || nowMs_() - cached->second.atMs < kUnavailableRetryMs) {
      done(cached->second.ok);
      return;
    }
    availability_.erase(cached);
  }

  probing_[id].push_back(std::move(done));
  if (probing_[id].size() > 1) return;  // 已有進行中的偵測，排隊共用結果

  engine->isAvailable([this, id](bool ok) {
    availability_[id] = {ok, nowMs_()};
    // 先把佇列搬出來再逐一回呼：回呼裡可能立刻再查同一個引擎
    auto waiters = std::move(probing_[id]);
    probing_.erase(id);
    for (auto& waiter : waiters) waiter(ok);
  });
}

void TtsManager::listEngines(std::function<void(std::vector<TtsEngineInfo>)> done) {
  // 逐一偵測（不是併發）：本機服務探測只有 2 秒逾時，序列化比較好推理，
  // 而且結果會進快取，第二次之後是瞬間的。
  auto results = std::make_shared<std::vector<TtsEngineInfo>>();
  auto step = std::make_shared<std::function<void(size_t)>>();

  *step = [this, results, step, done](size_t index) {
    if (index >= engines_.size()) {
      done(*results);
      return;
    }
    TtsEngine* engine = engines_[index].get();
    checkAvailable(engine, [engine, results, step, index](bool ok) {
      results->push_back({engine->id(), engine->name(), ok});
      (*step)(index + 1);
    });
  };
  (*step)(0);
}

void TtsManager::listVoices(const std::optional<std::string>& engineId, std::function<void(std::vector<VoiceInfo>)> done) {
  auto targets = std::make_shared<std::vector<TtsEngine*>>();
  if (TtsEngine* only = find(engineId)) {
    targets->push_back(only);
  } else {
    for (const auto& engine : engines_) targets->push_back(engine.get());
  }

  auto voices = std::make_shared<std::vector<VoiceInfo>>();
  auto step = std::make_shared<std::function<void(size_t)>>();

  *step = [this, targets, voices, step, done](size_t index) {
    if (index >= targets->size()) {
      done(*voices);
      return;
    }
    TtsEngine* engine = (*targets)[index];
    checkAvailable(engine, [engine, voices, step, index](bool ok) {
      if (!ok) {
        (*step)(index + 1);
        return;
      }
      engine->listVoices([engine, voices, step, index](std::vector<VoiceInfo> list, std::string error) {
        if (!error.empty()) {
          qWarning() << "[tts]" << QString::fromStdString(engine->id()) << "取得語音清單失敗：" << QString::fromStdString(error);
        }
        voices->insert(voices->end(), list.begin(), list.end());
        (*step)(index + 1);
      });
    });
  };
  (*step)(0);
}

std::unique_ptr<TtsRequestHandle> TtsManager::stream(const std::string& text, const SynthesizeRequest& request, StreamSink sink) {
  auto state = std::make_shared<StreamState>();
  auto handle = std::make_unique<ManagerHandle>(state);

  const std::string trimmed = strutil::trim(text);
  if (trimmed.empty()) {
    state->finished = true;
    if (sink.onError) sink.onError("Text to speak must not be empty", false);
    return handle;
  }

  TtsEngine* primary = find(request.engine);

  // 指定的引擎排第一，其餘維持註冊順序
  auto ordered = std::make_shared<std::vector<TtsEngine*>>();
  if (primary) ordered->push_back(primary);
  for (const auto& engine : engines_) {
    if (engine.get() != primary) ordered->push_back(engine.get());
  }

  auto errors = std::make_shared<std::vector<std::string>>();
  auto tryNext = std::make_shared<std::function<void(size_t)>>();

  *tryNext = [this, ordered, errors, tryNext, sink, trimmed, request, primary, state](size_t index) {
    if (state->cancelled || state->finished) return;

    if (index >= ordered->size()) {
      state->finished = true;
      if (sink.onError) sink.onError("All TTS engines failed - " + joinErrors(*errors), false);
      return;
    }

    TtsEngine* engine = (*ordered)[index];
    const bool isPrimary = engine == primary;

    checkAvailable(engine, [this, engine, isPrimary, errors, tryNext, sink, trimmed, request, index, state](bool ok) {
      if (state->cancelled || state->finished) return;
      if (!ok) {
        errors->push_back(engine->id() + ": unavailable");
        (*tryNext)(index + 1);
        return;
      }

      SpeakOptions options;
      // fallback 時不沿用原本的 voice —— 那是別的引擎的識別字串
      if (isPrimary) options.voice = request.voice;
      options.rate = request.rate;
      const std::optional<std::string> usedVoice = options.voice;

      TtsStreamSink engineSink;
      engineSink.onOpen = [state](std::string mime) { state->mime = std::move(mime); };

      engineSink.onChunk = [state, sink, engine](const char* data, size_t size) {
        if (state->cancelled || state->finished) return;
        if (!state->committed) {
          // **承諾點**：第一塊音訊要交給呼叫端了。onOpen 回 true 代表它真的會
          // 出聲，從這裡開始就不能再換引擎（會把已經唸出去的前半句重唸一次）。
          state->committed = sink.onOpen ? sink.onOpen(state->mime) : true;
          qDebug().nospace() << "[tts] 已承諾引擎 " << engine->id().c_str() << "（" << (state->committed ? "會出聲" : "沒有音訊裝置") << "），之後再失敗不會換引擎重講";
        }
        if (sink.onChunk) sink.onChunk(data, size);
      };

      engineSink.onSegmentEnd = [state, sink] {
        if (state->cancelled || state->finished) return;
        if (sink.onSegmentEnd) sink.onSegmentEnd();
      };

      engineSink.onDone = [state, sink, engine, usedVoice] {
        if (state->cancelled || state->finished) return;
        state->finished = true;
        SynthesisInfo info;
        info.engine = engine->id();
        info.voice = usedVoice;
        info.mime = state->mime;
        if (sink.onDone) sink.onDone(std::move(info));
      };

      engineSink.onError = [this, state, sink, engine, errors, tryNext, index](std::string error) {
        if (state->cancelled || state->finished) return;
        if (state->committed) {
          // 承諾點之後：已經有聲音出去了，換引擎只會讓前半句重播一次
          qWarning() << "[tts] 承諾之後失敗，整段截斷（不 fallback）：" << QString::fromStdString(error);
          state->finished = true;
          if (sink.onError) {
            sink.onError(error.empty() ? engine->id() + ": unknown error" : error, true);
          }
          return;
        }
        // 一次失敗就把可用性標記清掉，下次重新偵測
        qWarning() << "[tts]" << QString::fromStdString(engine->id()) << "失敗且尚未出聲，改試下一家引擎：" << QString::fromStdString(error);
        forgetEngine(engine->id());
        errors->push_back(engine->id() + ": " + (error.empty() ? "unknown error" : error));
        (*tryNext)(index + 1);
      };

      const uint64_t generation = ++state->generation;
      auto engineHandle = engine->synthesize(trimmed, options, std::move(engineSink));
      // generation 變了代表這個引擎在 synthesize() 內部就同步結束、而且鏈已經
      // 往下走了 —— 它的 handle 直接丟掉（同步結束代表沒有進行中的工作可取消）
      if (state->generation != generation) return;
      state->current = std::move(engineHandle);
      if (state->cancelled && state->current) state->current->cancel();
    });
  };

  (*tryNext)(0);
  return handle;
}

void TtsManager::synthesize(const std::string& text, const SynthesizeRequest& request, std::function<void(std::optional<SynthesizeOutcome>, std::string error)> done) {
  auto buffer = std::make_shared<std::vector<char>>();

  StreamSink sink;
  // 緩衝模式永遠不承諾：位元組只是進了記憶體，一個音都還沒出去，
  // 所以 fallback 語意與串流化之前一模一樣
  sink.onOpen = [](const std::string&) { return false; };
  sink.onChunk = [buffer](const char* data, size_t size) { buffer->insert(buffer->end(), data, data + size); };
  sink.onDone = [buffer, done](SynthesisInfo info) {
    SynthesizeOutcome outcome;
    outcome.audio = std::move(*buffer);
    outcome.mime = std::move(info.mime);
    outcome.engine = std::move(info.engine);
    outcome.voice = std::move(info.voice);
    done(std::move(outcome), "");
  };
  sink.onError = [done](std::string error, bool) { done(std::nullopt, std::move(error)); };

  // 舊介面沒有取消能力，handle 直接丟掉
  stream(text, request, std::move(sink));
}

}  // namespace l2m
