#pragma once

// 播放音訊（可以邊收邊播），同時算出音量包絡供口型同步使用。
//
// Qt 沒有現成的「播放同時取得波形」元件（這個 Qt 安裝連 QtMultimedia
// 都沒有），所以用 miniaudio 自己解碼、自己算 RMS。
//
// 執行緒：ma_device 的資料回呼跑在 miniaudio 自己的即時執行緒上。
// 那裡面只做「從環形緩衝 memcpy → 算 RMS → 寫 atomic」，不碰 Qt 物件、
// 不配置記憶體、不上鎖、**也不解碼**。解碼全部在 GUI 執行緒的 pump() 裡。
// 「播完了」不由回呼通知，而是 GUI 執行緒每 50ms 輪詢 —— 在即時執行緒裡發
// Qt 訊號會把音訊執行緒拖進事件迴圈的排程，那是爆音的來源。
//
// 為什麼是「環形緩衝 + 顯式 EOS」而不是舊版的單一 ma_decoder：
// 舊版把「解碼器短讀」直接當成「播完了」。串流之下短讀是 **underrun**（網路或
// 合成跟不上），誤判成結束會收掉氣泡、提早回覆 speak(wait=true)、還會跳去講
// 下一句。所以這兩件事必須拆開 —— 見 dataCallback 與 poll() 的註解。

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "core/echo_filter.h"
#include "stream_decoders.h"

typedef struct ma_device ma_device;

namespace l2m {

// audio_player.cpp 內部型別：包住 ma_pcm_rb。
// 它在 miniaudio 裡是匿名 struct 的 typedef，沒辦法像 ma_device / ma_decoder
// 那樣前置宣告，所以這裡走 pimpl，把那顆四萬行的標頭擋在 .cpp 裡面。
class PcmRingBuffer;

class AudioPlayer : public QObject {
  Q_OBJECT

public:
  explicit AudioPlayer(QObject* parent = nullptr);
  ~AudioPlayer() override;

  // 這一句的特效。目前只有 MCP 的 think 工具會用到：聲音帶殘響、嘴巴不動。
  //
  // mouthStill 刻意由播放器負責而不是在 main.cpp 的 mouthOpenSource 那條
  // lambda 上分支 —— 那樣就得再拉一條「現在是不是在思考」的線穿過
  // SpeechController 與 AppController。這裡改一個回傳值就夠了，而且
  // **lipSyncActive 維持為真**：嘴巴不動的同時仍要擋住 AI 的 set_parameters
  //（ParameterOverlay::skipForLipSync），否則思考中 AI 反而能把嘴掰開。
  struct Effects {
    bool echo = false;
    bool mouthStill = false;
  };

  // ── 串流介面 ──────────────────────────────────────────────────────────
  // 一句話開始。這裡只做 ma_device_init（貴的那半，實測 15-30 ms），讓它跟
  // 網路延遲重疊；**ma_device_start 刻意延到第一塊 PCM 真的進環之後**才做。
  // 一啟動 WASAPI 就會連續回呼預填 3 個 period，那時候環還是空的 ——
  // 早開的代價是 30-50 ms 的靜音會真的播出去，等於白白加在開口延遲上
  //（實測：整段音訊早就在手上的情況下也會印出「缺料 5 次」）。
  // 沒有音效裝置時回 false，呼叫端要退回「只顯示氣泡」。
  // volume 0~2，>1 為軟體增益（見 .cpp 的 set_master_volume 註解）。
  //
  // 不寫 `const Effects& effects = {}` 的預設引數：預設引數屬於外層類別的
  // complete-class context，會在 AudioPlayer 本身還沒定義完成時就要用到巢狀的
  // Effects 的成員預設值 —— clang 直接報錯（MSVC 放行），所以拆成兩個多載。
  // 同一個坑在 core/behavior_pick.h、core/gesture_detector.h、core/mood.h、
  // core/presence_tracker.h 也踩過，那幾處是拆成兩個建構子。
  bool beginUtterance(double volume, const Effects& effects);
  bool beginUtterance(double volume);

  // 一塊編碼位元組。mime 只作診斷用，實際格式由解碼器自己嗅探。
  // 尚未 beginUtterance（或已經 stop）時回 false。
  bool pushEncoded(const std::string& mime, const char* data, size_t size);

  // 這一段收完了，但整句還沒完 —— 下一段的位元組會另外推進來（句段管線用）
  void endSegment();
  // 整句的位元組全部到齊了，播完就結束
  void endUtterance();

  // ── 整段播放的便利包裝（beginUtterance → pushEncoded → endUtterance）──
  // 裝置或解碼失敗回 false，呼叫端要退回「只顯示氣泡」。
  bool play(std::vector<char> bytes, const std::string& mime, double volume);

  void stop();

  // 「這隻角色現在正在說話」：beginUtterance() 到 finished()/stop() 之間**恆為 true**，
  // 即使中間 underrun 完全沒有聲音。口型同步與 lipSyncActive 要接的是這個 ——
  // 缺料時若讓它變 false，ParameterOverlay::skipForLipSync 會停止跳過口型參數，
  // AI 的 set_parameters 會當場把嘴巴搶回去。
  bool speaking() const { return speaking_; }
  // 舊名，語意同 speaking()
  bool playing() const { return speaking_; }

  // 此刻環形緩衝裡真的有音訊。只給診斷與測試用，不要拿去控嘴巴（理由同上）
  bool hasAudio() const;

  // 0..1 的嘴巴開合，給 ModelController::mouthOpenProvider 每幀讀。
  // Effects::mouthStill 的這一句永遠回 0 —— 口型是 AddParameterValue，
  // 加 0 等於不加，嘴巴就停在動作／閒置給的預設值（閉著）。
  float mouthOpen() const { return mouthStill_ ? 0.0f : mouthOpen_.load(std::memory_order_relaxed); }

  // 這一句播放期間缺料的次數（診斷用）
  uint32_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

signals:
  // 第一個 PCM frame 進了環形緩衝 ＝「開始出聲」
  void started();
  // 最後一個音真的離開喇叭了（stop() 造成的結束不發）
  void finished();

private:
  static void dataCallback(ma_device* device, void* output, const void* input, unsigned int frameCount);
  void poll();
  // 解碼：把手上的編碼位元組盡量填進環形緩衝。GUI 執行緒專用。
  void pump();
  void teardown();

  // lastAudioFrame_ 的「還沒標記」哨兵
  static constexpr uint64_t kNoMark = ~0ull;

  // ── 音訊執行緒與 GUI 執行緒共用的狀態（一律 atomic）──
  std::atomic<float> mouthOpen_{0.0f};
  // 已經交給裝置的 frame 總數。寫:音訊 讀:GUI
  std::atomic<uint64_t> framesRendered_{0};
  // 最後一個「真音訊」的 frame 序號，之後全是靜音。kNoMark 代表還沒到那一刻
  std::atomic<uint64_t> lastAudioFrame_{kNoMark};
  std::atomic<uint32_t> underruns_{0};
  // producer 宣告「位元組全部解完了」。寫:GUI 讀:音訊
  std::atomic<bool> producerEos_{false};
  // 包絡的平滑值只有音訊執行緒會碰，不必是 atomic
  float level_ = 0.0f;

  ma_device* device_ = nullptr;
  std::unique_ptr<PcmRingBuffer> ring_;

  // 待解碼的段。所有段共用同一個環形緩衝與同一個裝置，所以上一段的最後一個
  // frame 與下一段的第一個 frame 在環裡是相鄰的 —— 音訊執行緒根本不知道有
  // 「段」這回事，銜接天然無縫。
  std::deque<AudioSegment> segments_;
  std::unique_ptr<StreamDecoder> decoder_;

  // 殘響。解碼之後、寫進環形緩衝之前套用（見 core/echo_filter.h）
  EchoFilter echo_;
  // 還沒沖出去的殘響尾巴（frame）。環滿的時候要分好幾次 pump 才沖得完，
  // 所以是個扣到 0 的計數。**只在第一次真的有音訊進濾波器時裝填一次**：
  // 到 EOS 才算的話「扣到 0」與「還沒開始算」是同一個狀態，每輪 pump 都會
  // 重新裝填而永遠宣告不了 EOS（症狀是氣泡不收、佇列的下一句永遠輪不到）。
  size_t echoFlush_ = 0;
  // 尾巴已經裝填過了。沒有音訊進過濾波器的話延遲線是空的，不必沖也不該沖
  bool echoPrimed_ = false;
  // Effects::mouthStill：這一句嘴巴不動
  bool mouthStill_ = false;

  bool speaking_ = false;
  // ma_device_start() 已經叫過了（見 beginUtterance 的註解）
  bool deviceStarted_ = false;
  bool utteranceClosed_ = false;
  bool startedEmitted_ = false;
  bool decodeFailed_ = false;
  // teardown() 之後為 true：擋掉已經排進事件佇列、stop() 之後才投遞到的遲到回呼
  bool torn_ = true;
  // 驅動裡排隊、還沒真的出喇叭的 frame 數（以 48 kHz 為單位）
  uint64_t tailFrames_ = 0;

  QTimer pollTimer_;
  // beginUtterance() 到第一塊 PCM 進環的時間 ＝ 使用者感受到的「開口延遲」。
  // 這是整個串流化改造要壓下去的那個數字，所以每一句都印出來。
  QElapsedTimer utteranceClock_;
};

}  // namespace l2m
