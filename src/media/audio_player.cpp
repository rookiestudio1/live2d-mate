#include "audio_player.h"

#include <QDebug>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "miniaudio.h"

namespace l2m {

namespace {

constexpr ma_format kFormat = ma_format_f32;
constexpr ma_uint32 kChannels = kPlaybackChannels;
constexpr ma_uint32 kSampleRate = kPlaybackSampleRate;

// 包絡調參（B1 階段實測調出來的）：
//  kNoiseFloor 以下視為靜音，kLoudRms 以上視為滿開。
//  兩者之間做線性正規化再開 0.6 次方 —— 輕聲說話時嘴巴也看得出在動。
constexpr float kNoiseFloor = 0.01f;  // 約 -40 dBFS
constexpr float kLoudRms = 0.25f;     // 約 -12 dBFS
constexpr float kCurve = 0.6f;
// 張嘴要快、閉嘴要慢，不然子音之間的空檔會讓嘴巴一直抖
constexpr float kAttack = 0.5f;
constexpr float kRelease = 0.15f;

// GUI 執行緒多久補一次環形緩衝、並檢查「播完了沒」
constexpr int kPollIntervalMs = 50;

// 環形緩衝 2 秒（f32 × 2ch × 48k ＝ 384 KB/s，所以是 750 KB）。
// 這是「能容忍多長的網路停頓」的上限，不是目標 —— 第一塊音訊一進來就開始出聲，
// 所以放大它不會拖慢開口延遲，只會多吃記憶體。
constexpr ma_uint32 kRingFrames = kSampleRate * 2;

// pump() 佔住 GUI 執行緒超過這麼久就印一行拆解。一幀 16.7 ms，
// 超過就是使用者看得見的頓一下
constexpr qint64 kPumpWarnMs = 20;

}  // namespace

// ---------------------------------------------------------------------------
// ma_pcm_rb 的薄包裝。lock-free 的單生產者單消費者佇列：
// GUI 執行緒寫（pump），即時音訊執行緒讀（dataCallback）。
//
// acquire_read / acquire_write **只會給到緩衝區尾端為止的連續空間**，繞回起點時
// 一次只拿得到一半 —— 所以兩側都必須寫成 while 迴圈。寫成 if 的話，環第一次
// 繞回就會固定少寫一段，表現成週期性的斷音。
// ---------------------------------------------------------------------------
class PcmRingBuffer {
public:
  ~PcmRingBuffer() {
    if (ok_) ma_pcm_rb_uninit(&rb_);
  }

  bool init(ma_uint32 frames) {
    if (ma_pcm_rb_init(kFormat, kChannels, frames, nullptr, nullptr, &rb_) != MA_SUCCESS) {
      return false;
    }
    ok_ = true;
    return true;
  }

  ma_uint32 availableRead() { return ok_ ? ma_pcm_rb_available_read(&rb_) : 0; }
  ma_uint32 availableWrite() { return ok_ ? ma_pcm_rb_available_write(&rb_) : 0; }

  // 回傳實際取得的 frame 數（可能少於要求的，見類別註解）
  ma_uint32 acquireRead(ma_uint32 frames, const float** out) {
    if (!ok_ || frames == 0) return 0;
    void* buffer = nullptr;
    if (ma_pcm_rb_acquire_read(&rb_, &frames, &buffer) != MA_SUCCESS) return 0;
    *out = static_cast<const float*>(buffer);
    return frames;
  }
  void commitRead(ma_uint32 frames) {
    if (ok_ && frames > 0) ma_pcm_rb_commit_read(&rb_, frames);
  }

  ma_uint32 acquireWrite(ma_uint32 frames, float** out) {
    if (!ok_ || frames == 0) return 0;
    void* buffer = nullptr;
    if (ma_pcm_rb_acquire_write(&rb_, &frames, &buffer) != MA_SUCCESS) return 0;
    *out = static_cast<float*>(buffer);
    return frames;
  }
  void commitWrite(ma_uint32 frames) {
    if (ok_ && frames > 0) ma_pcm_rb_commit_write(&rb_, frames);
  }

private:
  ma_pcm_rb rb_{};
  bool ok_ = false;
};

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent) {
  pollTimer_.setInterval(kPollIntervalMs);
  connect(&pollTimer_, &QTimer::timeout, this, &AudioPlayer::poll);
}

AudioPlayer::~AudioPlayer() { teardown(); }

void AudioPlayer::dataCallback(ma_device* device, void* output, const void* input, unsigned int frameCount) {
  (void)input;
  auto* self = static_cast<AudioPlayer*>(device->pUserData);
  if (!self) return;
  auto* samples = static_cast<float*>(output);

  // **順序很重要：先讀 EOS，再讀環形緩衝。**
  // 反過來的話，「看到環是空的」與「看到 EOS」之間 producer 剛好 commit 進來的
  // 那一塊會被當成不存在，句尾就少一截。producerEos_ 是在 pump() 最後一次
  // commit_write 之後才 release 寫的，這裡 acquire 讀，所以「EOS 為真」就保證
  // 前面所有的 commit 都已經對這條執行緒可見。
  const bool eos = self->producerEos_.load(std::memory_order_acquire);

  unsigned int done = 0;
  if (self->ring_) {
    while (done < frameCount) {
      const float* source = nullptr;
      const ma_uint32 got = self->ring_->acquireRead(frameCount - done, &source);
      if (got == 0) break;
      std::memcpy(samples + static_cast<size_t>(done) * kChannels, source, static_cast<size_t>(got) * kChannels * sizeof(float));
      self->ring_->commitRead(got);
      done += got;
    }
  }

  if (done < frameCount) {
    // 尾巴補零，不然裝置會重播上一塊緩衝區的內容
    std::fill_n(samples + static_cast<size_t>(done) * kChannels, static_cast<size_t>(frameCount - done) * kChannels, 0.0f);
    // EOS 之前的缺料是 underrun（合成或網路跟不上），**不是**播完了
    if (!eos) self->underruns_.fetch_add(1, std::memory_order_relaxed);
  }

  // RMS 對整個輸出緩衝取（含補的靜音）：缺料時嘴巴會以 kRelease 柔和收合，
  // 而不是硬歸零。包絡本身是 per-callback RMS + 一階 IIR，沒有 lookahead，
  // 天生就是增量式的 —— 串流化一行都不用改。
  const ma_uint64 sampleCount = static_cast<ma_uint64>(frameCount) * kChannels;
  double sum = 0;
  for (ma_uint64 i = 0; i < sampleCount; ++i) sum += static_cast<double>(samples[i]) * samples[i];
  const float rms = sampleCount > 0 ? static_cast<float>(std::sqrt(sum / static_cast<double>(sampleCount))) : 0.0f;

  float target = (rms - kNoiseFloor) / (kLoudRms - kNoiseFloor);
  target = std::clamp(target, 0.0f, 1.0f);
  if (target > 0) target = std::pow(target, kCurve);

  self->level_ += (target - self->level_) * (target > self->level_ ? kAttack : kRelease);
  self->mouthOpen_.store(self->level_, std::memory_order_relaxed);

  const uint64_t before = self->framesRendered_.load(std::memory_order_relaxed);
  self->framesRendered_.store(before + frameCount, std::memory_order_release);

  // 記下最後一個「真音訊」的 frame 序號。之後全是靜音，poll() 靠它算出
  // 「最後一個音真的離開喇叭」的時間點。
  if (eos && done < frameCount && self->lastAudioFrame_.load(std::memory_order_relaxed) == kNoMark) {
    self->lastAudioFrame_.store(before + done, std::memory_order_release);
  }
}

bool AudioPlayer::beginUtterance(double volume) { return beginUtterance(volume, Effects{}); }

bool AudioPlayer::beginUtterance(double volume, const Effects& effects) {
  stop();

  ring_ = std::make_unique<PcmRingBuffer>();
  if (!ring_->init(kRingFrames)) {
    qWarning() << "[tts] 環形緩衝配置失敗，改為只顯示氣泡";
    ring_.reset();
    return false;
  }

  device_ = new ma_device();
  ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
  deviceConfig.playback.format = kFormat;
  deviceConfig.playback.channels = kChannels;
  deviceConfig.sampleRate = kSampleRate;
  deviceConfig.dataCallback = &AudioPlayer::dataCallback;
  deviceConfig.pUserData = this;

  if (ma_device_init(nullptr, &deviceConfig, device_) != MA_SUCCESS) {
    qWarning() << "[tts] 音訊裝置初始化失敗，改為只顯示氣泡";
    delete device_;
    device_ = nullptr;
    ring_.reset();
    return false;
  }

  // 音量在裝置層套用，所以 RMS 是「增益前」測的 ——
  // 把音量調大調小時嘴型都不會跟著變，這是刻意的。
  // >1（最大 2）是軟體增益：這份 vendored miniaudio 的 set_master_volume 只擋負值
  //（miniaudio.h:42913，文件寫 0..1 但實作沒擋上限），增益是在 dataCallback 返回後
  // 才乘的（ma_device__handle_data_callback），乘完由內建的 ma_clip_samples_f32
  // 硬限幅到 ±1（noClip 預設關、格式是 f32）—— 所以口型與限幅都不必自己處理。
  // 保險：未來升版若上游真的加了上限檢查，退回 1.0 保底（劣化成頂到原始音量，
  // 而不是整句失去音量控制）。屆時要減少破音可改走「callback 先算 RMS 再乘增益」。
  const float gain = static_cast<float>(std::clamp(volume, 0.0, 2.0));
  if (ma_device_set_master_volume(device_, gain) != MA_SUCCESS) {
    qWarning() << "[tts] 裝置增益" << gain << "被拒絕，退回 1.0";
    ma_device_set_master_volume(device_, std::min(gain, 1.0f));
  }

  // 驅動裡排隊、還沒真的出喇叭的 frame 數。internalPeriodSizeInFrames 是
  // **裝置內部取樣率**的單位，我們的 frame 計數是 48 kHz 的單位 ——
  // 裝置跑 44.1 kHz 時不換算會少算 8.8%，句尾就會被切掉一截。
  const auto& playback = device_->playback;
  tailFrames_ = playback.internalSampleRate > 0 ? static_cast<uint64_t>(static_cast<double>(playback.internalPeriodSizeInFrames) * playback.internalPeriods *
                                                                        (static_cast<double>(kSampleRate) / static_cast<double>(playback.internalSampleRate)))
                                                : 0;

  // 殘響要在第一塊 PCM 之前備好 —— pump() 是同步的，chunk 一到就解
  mouthStill_ = effects.mouthStill;
  echoPrimed_ = false;
  echoFlush_ = 0;
  if (effects.echo) {
    echo_.reset(kSampleRate, kChannels, EchoFilter::thinkingVoice());
  } else {
    echo_.clear();
  }

  torn_ = false;
  utteranceClosed_ = false;
  startedEmitted_ = false;
  decodeFailed_ = false;
  level_ = 0;
  mouthOpen_.store(0.0f, std::memory_order_relaxed);
  framesRendered_.store(0, std::memory_order_relaxed);
  lastAudioFrame_.store(kNoMark, std::memory_order_relaxed);
  underruns_.store(0, std::memory_order_relaxed);
  producerEos_.store(false, std::memory_order_release);
  deviceStarted_ = false;

  // 這裡**不** ma_device_start —— 等第一塊 PCM 進環之後才啟動（見標頭註解）
  utteranceClock_.start();
  speaking_ = true;
  pollTimer_.start();
  return true;
}

bool AudioPlayer::pushEncoded(const std::string& mime, const char* data, size_t size) {
  if (torn_ || !speaking_ || utteranceClosed_) return false;

  if (segments_.empty() || segments_.back().closed) {
    AudioSegment segment;
    segment.mime = mime;
    segments_.push_back(std::move(segment));
  }
  if (data && size > 0) {
    AudioSegment& segment = segments_.back();
    segment.encoded.insert(segment.encoded.end(), data, data + size);
  }

  // chunk 一到就解，不等 timer —— 那 50 ms 會直接加在開口延遲上
  pump();
  return true;
}

void AudioPlayer::endSegment() {
  if (torn_) return;
  if (segments_.empty()) {
    // 上游宣告「這一段的位元組完了」，但根本沒有段 —— 代表那一句一個位元組
    // 都沒推進來（合成成功卻回了空 body）。整個呼叫變成 no-op，那一句無聲消失
    qWarning() << "[tts] endSegment 沒有對應的音訊段，這一句沒有任何位元組";
    return;
  }
  segments_.back().closed = true;
  pump();
}

void AudioPlayer::endUtterance() {
  if (torn_) return;
  if (!segments_.empty()) segments_.back().closed = true;
  utteranceClosed_ = true;
  pump();
}

bool AudioPlayer::play(std::vector<char> bytes, const std::string& mime, double volume) {
  stop();
  if (bytes.empty()) return false;
  if (!beginUtterance(volume)) return false;

  pushEncoded(mime, bytes.data(), bytes.size());
  endUtterance();

  // 整段推完的情況下，endUtterance() 裡的 pump() 就已經建過解碼器了，
  // 所以「這段音訊解不解得了」在這裡就知道，回傳值的語意與舊版一致。
  if (decodeFailed_) {
    stop();
    return false;
  }
  return true;
}

void AudioPlayer::pump() {
  if (torn_ || !ring_) return;

  // **這支函式整段跑在 GUI 執行緒上**（chunk 一到就同步解，見 pushEncoded 的註解），
  // 所以它佔用多久，模型動畫就凍多久。實測「音訊位元組到手 → 開始出聲」偶爾會
  // 拉到 500~1000 ms，但單獨量 ma_device_start 只有 0.3 ms、解碼也只有幾 ms ——
  // 光看「開始出聲（N ms）」那一行分不出錢花在哪裡，所以這裡按段計時，
  // 超過一幀（kPumpWarnMs）才印。平常一句話只會 pump 個位數毫秒，不會洗版。
  QElapsedTimer pumpClock;
  pumpClock.start();
  qint64 makeNs = 0;
  qint64 decodeNs = 0;
  qint64 startNs = 0;
  uint64_t framesProduced = 0;

  bool produced = false;
  for (;;) {
    if (segments_.empty()) break;
    AudioSegment& segment = segments_.front();

    if (!decoder_) {
      const qint64 mark = pumpClock.nsecsElapsed();
      decoder_ = makeDecoder(segment);
      makeNs += pumpClock.nsecsElapsed() - mark;
      // nullptr ＝ 位元組還不夠認出格式（段還沒收完），等下一塊再問
      if (!decoder_) break;
    }

    float* destination = nullptr;
    const ma_uint32 room = ring_->acquireWrite(ring_->availableWrite(), &destination);
    if (room == 0) break;  // 環滿了，等音訊執行緒消耗

    const qint64 mark = pumpClock.nsecsElapsed();
    const uint32_t wrote = decoder_->decode(segment, destination, room);
    decodeNs += pumpClock.nsecsElapsed() - mark;
    if (wrote > 0) {
      // 殘響就地套在剛解出來的那一段上（GUI 執行緒，環還沒 commit）。
      // 位置刻意在 commit 之前：音訊執行緒讀到的永遠是已經混好的樣本，
      // 即時執行緒那邊一行都不必改。
      if (echo_.active()) {
        echo_.process(destination, wrote);
        // 尾巴的長度在**第一次真的有音訊進濾波器**時就決定好，之後只扣不加。
        // 放到 EOS 那邊才算的話，「扣到 0」與「還沒開始算」就成了同一個狀態，
        // 每一輪 pump 都會重新裝填，producerEos_ 永遠等不到（實測：氣泡不收、
        // 佇列裡的下一句永遠輪不到）。
        if (!echoPrimed_) {
          echoPrimed_ = true;
          echoFlush_ = echo_.tailFrames();
        }
      }
      ring_->commitWrite(wrote);
      framesProduced += wrote;
      produced = true;
      continue;
    }

    // 解不出更多：這一段還沒收完就等下一塊位元組，收完了就換下一段
    if (!segment.closed) break;
    if (!decoder_->error().empty()) {
      decodeFailed_ = true;
      // **這一段就這樣被丟掉，前後句照常銜接** —— 串流路徑（SpeechController 走的
      // 那條）沒有任何人讀 decodeFailed_，所以使用者聽到的是「中間漏了一句」而
      // 上游完全不知情。error() 馬上就要隨 decoder_ 一起銷毀，是唯一知道發生什麼
      // 事的地方，一定要在這裡印出來（連 mime 與位元組數，才分得出是「TTS 回了
      // 非音訊」還是「音訊本身壞了」）
      qWarning().nospace() << "[tts] 丟棄無法解碼的音訊段（" << segment.encoded.size() << " bytes，" << segment.mime.c_str() << "，佇列尚有 " << (segments_.size() - 1) << " 段）："
                           << QString::fromStdString(decoder_->error());
    }
    decoder_.reset();
    segments_.pop_front();
  }

  if (segments_.empty() && utteranceClosed_ && !decoder_) {
    // 乾聲收完了，但**殘響還在延遲線裡**。這裡直接宣告 EOS 的話，poll() 會在
    // 環一空就標記 lastAudioFrame_，尾巴被硬切成「像被拔插頭」。所以先推
    // tailFrames() 個 frame 的靜音進濾波器，把回音沖出來當成真音訊播掉。
    // 環滿時沖不完，剩下的留給下一輪 pump（poll 每 50 ms 會叫）。
    if (echoFlush_ > 0) {
      while (echoFlush_ > 0) {
        float* destination = nullptr;
        const ma_uint32 want = static_cast<ma_uint32>(std::min<size_t>(echoFlush_, ring_->availableWrite()));
        const ma_uint32 room = want > 0 ? ring_->acquireWrite(want, &destination) : 0;
        if (room == 0) break;
        std::fill_n(destination, static_cast<size_t>(room) * kChannels, 0.0f);
        echo_.process(destination, room);
        ring_->commitWrite(room);
        echoFlush_ -= room;
        framesProduced += room;
        produced = true;
      }
    }

    // release 寫：保證前面所有的 commit_write 對音訊執行緒可見（見 dataCallback）
    if (echoFlush_ == 0) producerEos_.store(true, std::memory_order_release);
  }

  // 環裡有東西了才啟動裝置：一啟動 WASAPI 就會連續回呼預填 period，
  // 那幾次要是撈到空的，播出去的就是實實在在的前置靜音。
  if (produced && !deviceStarted_) {
    const qint64 mark = pumpClock.nsecsElapsed();
    const ma_result started = ma_device_start(device_);
    startNs = pumpClock.nsecsElapsed() - mark;
    if (started != MA_SUCCESS) {
      qWarning() << "[tts] 音訊裝置啟動失敗";
      decodeFailed_ = true;
      teardown();
      return;
    }
    deviceStarted_ = true;
  }

  // 訊號留到最後才發：接收端可能在 slot 裡直接呼叫 stop()，那會把上面用到的
  // 環形緩衝與段佇列整個拆掉。
  if (deviceStarted_ && !startedEmitted_) {
    startedEmitted_ = true;
    const auto& playback = device_->playback;
    qDebug().nospace() << "[tts] 開始出聲（" << utteranceClock_.elapsed() << " ms，裝置 " << playback.internalSampleRate << " Hz，尾端 " << tailFrames_ << " frame）";
    emit started();
  }

  // 刻意放在 emit 之後：接收端（SpeechController 的氣泡）也是在這個呼叫堆疊裡跑的，
  // 使用者眼中的凍結是整段的長度。此時 ring_/segments_ 可能已經被 slot 裡的 stop()
  // 拆掉了，所以只讀堆疊上的計數，不碰任何成員。
  const qint64 totalNs = pumpClock.nsecsElapsed();
  if (totalNs > kPumpWarnMs * 1000000LL) {
    qWarning().nospace() << "[tts] pump 佔住 GUI 執行緒 " << totalNs / 1000000.0 << " ms（建解碼器 " << makeNs / 1000000.0 << "、解碼 " << decodeNs / 1000000.0 << "、裝置啟動 " << startNs / 1000000.0
                         << "、其餘 " << (totalNs - makeNs - decodeNs - startNs) / 1000000.0 << "，產出 " << framesProduced << " frame）";
  }
}

void AudioPlayer::poll() {
  if (!speaking_) return;
  pump();
  if (!speaking_) return;  // pump() 發出的 started() 可能已經把我們停掉了

  if (!deviceStarted_) {
    // 一個音都沒解出來就走到 EOS（空句子、或整段都解不了）：
    // 裝置從來沒啟動過，framesRendered_ 永遠是 0，得在這裡收掉。
    if (producerEos_.load(std::memory_order_acquire)) {
      qWarning() << "[tts] 整句都沒有解出音訊，裝置從未啟動（空句子，或整段都解不了）";
      teardown();
      emit finished();
    }
    return;
  }

  const uint64_t mark = lastAudioFrame_.load(std::memory_order_acquire);
  if (mark == kNoMark) return;  // 還在說話，或正在 underrun 空轉
  // mark 之後還要等驅動把排隊的那幾個 period 吐完，最後一個音才真的出喇叭
  if (framesRendered_.load(std::memory_order_acquire) < mark + tailFrames_) return;

  const uint32_t missed = underruns_.load(std::memory_order_relaxed);
  if (missed > 0) qWarning() << "[tts] 播放期間缺料" << missed << "次（合成或網路跟不上）";

  // 印出判定用的三個實值：句尾有沒有被切掉一個 device period 就看這裡
  qDebug().nospace() << "[tts] 播放結束（最後音訊 frame " << mark << "，已送出 " << framesRendered_.load(std::memory_order_relaxed) << "，尾端 " << tailFrames_ << "）";

  teardown();
  emit finished();
}

bool AudioPlayer::hasAudio() const { return ring_ && ring_->availableRead() > 0; }

void AudioPlayer::stop() {
  if (!speaking_ && !device_ && !ring_) return;
  teardown();
}

void AudioPlayer::teardown() {
  pollTimer_.stop();
  torn_ = true;
  speaking_ = false;
  mouthOpen_.store(0.0f, std::memory_order_relaxed);
  level_ = 0;

  if (device_) {
    // uninit 內部會停掉裝置並等資料回呼結束，之後才可以動環形緩衝
    ma_device_uninit(device_);
    delete device_;
    device_ = nullptr;
  }
  // 順序不能反：上面 join 完音訊執行緒，這裡才動得了它讀的東西
  ring_.reset();
  decoder_.reset();
  segments_.clear();

  // 殘響一定要跟著清：同一顆播放器連著播兩句時，上一句的延遲線殘留會在
  // 下一句的句首漏出一段別人的尾音
  echo_.clear();
  echoFlush_ = 0;
  echoPrimed_ = false;
  mouthStill_ = false;

  deviceStarted_ = false;
  utteranceClosed_ = false;
  startedEmitted_ = false;
  decodeFailed_ = false;
  tailFrames_ = 0;
  framesRendered_.store(0, std::memory_order_relaxed);
  lastAudioFrame_.store(kNoMark, std::memory_order_relaxed);
  underruns_.store(0, std::memory_order_relaxed);
  producerEos_.store(false, std::memory_order_release);
}

}  // namespace l2m
