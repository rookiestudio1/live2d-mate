#include "speech_controller.h"

#include <QDebug>

#include <algorithm>
#include <utility>

#include "../media/audio_player.h"
#include "../windows/bubble_window.h"
#include "core/bubble_shape.h"
#include "core/echo_filter.h"
#include "core/json_doc.h"
#include "core/string_util.h"

namespace l2m {

namespace {

// mutter 的氣泡時長：每個字元的閱讀時間與整句上限。
// 中日文約 5~6 字/秒，180 ms/字略慢於此 —— 氣泡寧可多留一拍也不要字還沒讀完就收
constexpr double kMutterMsPerChar = 180;
constexpr double kMutterMaxMs = 10000;

}  // namespace

SpeechController::SpeechController(ConfigStore& config, TtsManager& tts, AudioPlayer& player, BubbleWindow& bubble, QObject* parent)
  : QObject(parent), config_(config), tts_(tts), player_(player), bubble_(bubble) {
  bubbleHold_.setSingleShot(true);
  connect(&bubbleHold_, &QTimer::timeout, this, [this] { completeCurrent(); });

  // 保險：等第一個聲音等太久就先把字放出來（入口④，見標頭）
  bubbleDelay_.setSingleShot(true);
  connect(&bubbleDelay_, &QTimer::timeout, this, [this] { showBubbleOnce("保險到期"); });

  connect(&player_, &AudioPlayer::finished, this, [this] { completeCurrent(); });

  // 「開始出聲」＝ 第一塊 PCM 真的進了播放器的環形緩衝。
  // wait=false 的呼叫端等的就是這一刻，不是「整段合成完」；
  // 氣泡也是（入口①）—— 字與聲音一起出現，才對得上使用者眼裡的「開口」。
  connect(&player_, &AudioPlayer::started, this, [this] {
    playerStarted_ = true;
    showBubbleOnce("跟著聲音");
    if (current_ && !current_->request.wait) answer(CommandResult::success());
  });
}

SpeechController::~SpeechController() = default;

void SpeechController::speak(const Request& request, std::function<void(CommandResult)> done) {
  const std::string text = strutil::trim(request.text);
  if (text.empty()) {
    if (done) done(CommandResult::failure("Text to speak must not be empty"));
    return;
  }

  Job job;
  job.request = request;
  job.request.text = text;
  job.done = std::move(done);
  queue_.push_back(std::move(job));

  if (!running_) runNext();
}

void SpeechController::mutter(const std::string& rawText, std::function<void(CommandResult)> done) {
  const std::string text = strutil::trim(rawText);
  if (text.empty()) {
    if (done) done(CommandResult::failure("Text to speak must not be empty"));
    return;
  }

  Job job;
  job.request.text = text;
  job.request.wait = true;
  job.bubbleOnly = true;
  job.done = std::move(done);
  queue_.push_back(std::move(job));

  if (!running_) runNext();
}

void SpeechController::runNext() {
  if (queue_.empty()) {
    running_ = false;
    current_.reset();
    emit finished();
    return;
  }

  running_ = true;
  current_ = std::make_unique<Job>(std::move(queue_.front()));
  queue_.pop_front();

  // 段落的起點。氣泡拿的是這一份完整文字，TTS 拿的也是同一份 ——
  // 兩者的分歧一定發生在下游的切句與逐句合成（見 core/tts_segment_pipeline.cpp）
  qDebug().nospace() << "[tts] 開始說話（" << current_->request.text.size() << " bytes 文字，wait=" << current_->request.wait << "，佇列尚有 " << queue_.size() << " 句）";

  playerOpen_ = false;
  playerStarted_ = false;
  bubbleShown_ = false;
  committedError_.clear();
  streamMime_.clear();
  speechClock_.restart();

  const AppConfig& cfg = config_.get();

  // mutter：只出氣泡，不合成不播放。時長依字數推估閱讀速度，
  // 至少撐滿 bubbleMinDuration。氣泡整個關著時無事可做，直接完成。
  if (current_->bubbleOnly) {
    if (!cfg.tts.showBubble) {
      completeCurrent();
      return;
    }
    showBubbleOnce("mutter");
    const size_t chars = strutil::utf8Length(current_->request.text);
    const double duration = std::clamp(static_cast<double>(chars) * kMutterMsPerChar, cfg.tts.bubbleMinDuration, kMutterMaxMs);
    bubbleHold_.start(static_cast<int>(duration));
    return;
  }

  // 播放器先開起來：ma_device_init 是貴的那半，讓它跟合成的網路往返重疊。
  // 開不起來（沒有音效裝置）就退回「只顯示氣泡」，合成照跑但不推給它。
  // 思考的音量壓到正常說話的 kThinkingVolumeScale（core/echo_filter.h）
  const double volume = current_->request.thinking ? cfg.tts.volume * kThinkingVolumeScale : cfg.tts.volume;
  playerOpen_ = player_.beginUtterance(volume, {current_->request.thinking, current_->request.thinking});
  if (!playerOpen_) qWarning() << "[tts] 音訊裝置無法使用，改為只顯示氣泡";

  // 氣泡等到「第一個聲音真的出來」才顯示（入口①）。這裡只安排兩種例外：
  // 根本不會有聲音的話立刻顯示（入口②），否則掛上保險期限（入口④）。
  // bubbleMaxDelay 為 0 代表使用者要回到「開始合成就顯示」的舊行為。
  if (!playerOpen_) {
    showBubbleOnce("沒有音訊裝置");
  } else if (cfg.tts.bubbleMaxDelay <= 0) {
    showBubbleOnce("不等聲音");
  } else {
    bubbleDelay_.start(static_cast<int>(cfg.tts.bubbleMaxDelay));
  }

  TtsManager::SynthesizeRequest request;
  request.engine = current_->request.engine.has_value() ? current_->request.engine : std::optional<std::string>(cfg.tts.engine);
  request.voice = current_->request.voice.has_value() ? current_->request.voice : cfg.tts.voice;
  request.rate = current_->request.rate.value_or(cfg.tts.rate);

  TtsManager::StreamSink sink;
  // 承諾點：播放器開著就代表這一塊真的會出聲，TtsManager 從這裡起停止 fallback
  sink.onOpen = [this](const std::string& mime) {
    streamMime_ = mime;
    return playerOpen_;
  };
  sink.onChunk = [this](const char* data, size_t size) {
    if (!playerOpen_) return;
    // pushEncoded 的回傳值以前沒人看：整句已經收尾（utteranceClosed_）之後
    // 才到的位元組會被靜默丟棄，那一句就這樣少一截
    if (!player_.pushEncoded(streamMime_, data, size)) {
      qWarning().nospace() << "[tts] 播放器拒收 " << size << " bytes（整句已收尾），"
                           << "這段音訊被丟棄";
    }
  };
  sink.onSegmentEnd = [this] {
    if (playerOpen_) player_.endSegment();
  };
  sink.onDone = [this](SynthesisInfo info) { finishSynthesis(info.engine, "", false); };
  sink.onError = [this](std::string error, bool committed) { finishSynthesis("", error, committed); };

  const std::string text = current_->request.text;
  ttsHandle_ = tts_.stream(text, request, std::move(sink));
}

void SpeechController::finishSynthesis(const std::string& engine, const std::string& error, bool committed) {
  if (!current_) return;
  if (!engine.empty()) lastEngine_ = engine;

  if (!error.empty()) {
    if (committed) {
      // 承諾點之後才壞：已經唸出去的部分播完，但要如實回報
      committedError_ = error;
      qWarning() << "[tts] 合成中斷（已經出聲，播完收到的部分）：" << QString::fromStdString(error);
    } else {
      // 語音壞掉不該讓角色整個啞掉：仍然顯示氣泡，只是沒有聲音
      qWarning() << "[tts] 合成失敗，改為只顯示氣泡：" << QString::fromStdString(error);
    }
  }

  ttsHandle_.reset();

  // **順序不能反**：先宣告位元組到齊，再看有沒有出過聲。
  // 整段解碼的來源（沒有增量解碼路徑的格式）要等段關閉才解得出第一個 frame，
  // 先問 playerStarted_ 的話永遠是 false，整句都會被誤判成「合成失敗」。
  if (playerOpen_) player_.endUtterance();

  if (!playerStarted_) {
    // 一個音都沒出來：不必等播放器，直接用最短顯示時間撐住讓字讀得完。
    // 氣泡可能還沒放出來（保險期限還沒到就先收到錯誤），這裡是入口③。
    player_.stop();
    playerOpen_ = false;
    showBubbleOnce("合成失敗");
    bubbleHold_.start(static_cast<int>(config_.get().tts.bubbleMinDuration));
    // wait=false 的回覆平常掛在 started() 上，沒出聲就得在這裡補
    if (!current_->request.wait) answer(CommandResult::success());
    return;
  }
  // 播完環裡剩下的就會發 finished() → completeCurrent()
}

void SpeechController::answer(CommandResult result) {
  if (!current_ || current_->answered) return;
  current_->answered = true;
  if (current_->done) current_->done(std::move(result));
}

void SpeechController::showBubbleOnce(const char* reason) {
  if (bubbleShown_ || !current_) return;
  if (!config_.get().tts.showBubble) return;
  bubbleShown_ = true;
  bubbleDelay_.stop();
  qDebug().nospace() << "[tts] 氣泡顯示（" << reason << "，" << speechClock_.elapsed() << " ms）";
  // 第一次 showText 要建原生視窗並量字，DirectWrite 的字型後援在這裡付過一次
  // 1398 ms 的代價（見 windows/bubble_window.cpp 的字型註解）。那一下就落在
  // 這個呼叫裡、GUI 執行緒上，而上面那行 log 是在它**之前**印的 ——
  // 也就是說沒有這個計時，氣泡的成本一毫秒都不會出現在時間軸上。
  QElapsedTimer showClock;
  showClock.start();
  bubble_.showText(QString::fromStdString(current_->request.text), current_->request.thinking ? BubbleStyle::Thought : BubbleStyle::Speech);
  const qint64 showMs = showClock.elapsed();
  if (showMs > 20) qWarning().nospace() << "[tts] 氣泡顯示佔住 GUI 執行緒 " << showMs << " ms";
}

void SpeechController::completeCurrent() {
  bubbleHold_.stop();
  bubbleDelay_.stop();
  if (!current_) {
    running_ = false;
    return;
  }

  qDebug().nospace() << "[tts] 說完（總計 " << speechClock_.elapsed() << " ms，佇列尚有 " << queue_.size() << " 句）";
  bubble_.hideBubble();
  answer(committedError_.empty() ? CommandResult::success()
                                 : CommandResult::failure(committedError_,
                                                          "The voice was cut off mid-sentence. Try speaking again, "
                                                          "or switch engines in Settings > Voice."));
  committedError_.clear();
  current_.reset();
  runNext();
}

CommandResult SpeechController::stopSpeaking() {
  queue_.clear();

  // 順序很重要：
  // ① 先切斷還在飛的合成。不取消的話，wss 訊框與 HTTP body 會一路收完，
  //    然後回呼到下面已經拆掉的播放器上。
  if (ttsHandle_) {
    ttsHandle_->cancel();
    ttsHandle_.reset();
  }
  // ② 再拆播放器（ma_device_uninit 會阻塞等音訊執行緒收工）
  player_.stop();
  playerOpen_ = false;
  playerStarted_ = false;
  bubbleShown_ = false;
  committedError_.clear();

  bubbleHold_.stop();
  bubbleDelay_.stop();
  bubble_.hideBubble();

  if (current_) {
    answer(CommandResult::success());
    current_.reset();
  }
  running_ = false;
  emit finished();
  return CommandResult::success();
}

void SpeechController::listVoices(const std::optional<std::string>& engineId, std::function<void(std::vector<VoiceInfo>)> done) { tts_.listVoices(engineId, std::move(done)); }

void SpeechController::listEngines(std::function<void(std::vector<TtsEngineInfo>)> done) { tts_.listEngines(std::move(done)); }

void SpeechController::voicesJson(const std::optional<std::string>& engineId, std::function<void(std::string)> done) {
  const AppConfig& cfg = config_.get();
  const std::string currentEngine = cfg.tts.engine;
  const std::optional<std::string> currentVoice = cfg.tts.voice;

  tts_.listVoices(engineId, [done, currentEngine, currentVoice](std::vector<VoiceInfo> voices) {
    jsonu::MutDoc doc;
    yyjson_mut_doc* d = doc.get();
    yyjson_mut_val* root = yyjson_mut_obj(d);
    doc.setRoot(root);

    yyjson_mut_obj_add_strcpy(d, root, "currentEngine", currentEngine.c_str());
    if (currentVoice.has_value()) {
      yyjson_mut_obj_add_strcpy(d, root, "currentVoice", currentVoice->c_str());
    } else {
      yyjson_mut_obj_add_null(d, root, "currentVoice");
    }
    yyjson_mut_obj_add_int(d, root, "count", static_cast<int64_t>(voices.size()));

    yyjson_mut_val* arr = yyjson_mut_arr(d);
    for (const auto& voice : voices) {
      yyjson_mut_val* item = yyjson_mut_obj(d);
      yyjson_mut_obj_add_strcpy(d, item, "id", voice.id.c_str());
      yyjson_mut_obj_add_strcpy(d, item, "name", voice.name.c_str());
      yyjson_mut_obj_add_strcpy(d, item, "locale", voice.locale.c_str());
      yyjson_mut_obj_add_strcpy(d, item, "engine", voice.engine.c_str());
      yyjson_mut_arr_add_val(arr, item);
    }
    yyjson_mut_obj_add_val(d, root, "voices", arr);

    done(doc.write(true));
  });
}

std::string SpeechController::stateJson() const {
  const AppConfig& cfg = config_.get();

  jsonu::MutDoc doc;
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* root = yyjson_mut_obj(d);
  doc.setRoot(root);

  yyjson_mut_obj_add_strcpy(d, root, "engine", cfg.tts.engine.c_str());
  if (cfg.tts.voice.has_value()) {
    yyjson_mut_obj_add_strcpy(d, root, "voice", cfg.tts.voice->c_str());
  } else {
    yyjson_mut_obj_add_null(d, root, "voice");
  }
  if (!lastEngine_.empty()) {
    yyjson_mut_obj_add_strcpy(d, root, "lastUsedEngine", lastEngine_.c_str());
  }
  yyjson_mut_obj_add_bool(d, root, "speaking", running_);
  yyjson_mut_obj_add_real(d, root, "rate", cfg.tts.rate);
  yyjson_mut_obj_add_real(d, root, "volume", cfg.tts.volume);
  yyjson_mut_obj_add_bool(d, root, "showBubble", cfg.tts.showBubble);

  return doc.write(false);
}

}  // namespace l2m
