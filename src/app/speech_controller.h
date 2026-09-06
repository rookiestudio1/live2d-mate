#pragma once

// 說話：合成 → 播放 → 氣泡 → 口型，整條流程與它的佇列。
//
// 佇列的用途是「避免多個 AI 呼叫同時發聲互相蓋台」，所以這裡是等到
// **播放真的結束**才跑下一句 —— 只串到「指令送出」為止的話，
// wait:false 的連續呼叫會互相打斷。
//
// 合成失敗不讓角色整個啞掉：仍然顯示氣泡，只是沒有聲音，
// 並撐滿 tts.bubbleMinDuration 讓字讀得完。
//
// 串流化之後有三個時機被拆開了（以前全擠在「合成完成」那一刻）：
//   氣泡       → **開始出聲**才顯示（見下）。
//   wait=false → **開始出聲**（AudioPlayer::started）才回覆，語意更準確。
//   wait=true  → 維持「播放真的結束」（AudioPlayer::finished）。
//
// 氣泡的顯示時機有四個入口，先到的那個生效（bubbleShown_ 保證只顯示一次）：
//   ① AudioPlayer::started     —— 正常路徑。第一個 PCM frame 進環，
//                                  那一刻才是使用者眼裡角色開口的瞬間。
//   ② beginUtterance() 回 false —— 沒有音效裝置，同步就知道不會有聲音。
//   ③ 合成結束但一個音都沒出    —— 失敗，收到錯誤的當下就顯示。
//   ④ tts.bubbleMaxDelay 到期  —— 保險。合成是非同步的，本機推論慢的時候
//                                  （GPT-SoVITS 約 1.7 秒固定開銷 ＋ 0.155 秒/字）
//                                  會有一段字與聲音都沒有的空窗，服務卡死時甚至
//                                  要等到 timeoutMs 才知道失敗。逾時就先把字放出來。
// 原本是「開始合成就顯示」，於是本機 TTS 上字會比聲音早兩秒出現。
// 代價是氣泡的可見時長少掉開口延遲那一段 —— 那是字幕化的自然結果。
//
// **「真的開口了」那一刻在等的不只有氣泡**：perform 押後的視覺步驟（動作、
// 表情）也接在同一個閘門上（Request::onSpeechStart，規則在 core/perform_sync.h）。
// 使用者眼裡「角色開口」只有一個瞬間，字、聲音與動作都該落在那一拍。
// 一次性旗標刻意分成兩個（speechStarted_ vs bubbleShown_）—— 氣泡自己還有
// tts.showBubble 這個開關，共用的話「關掉氣泡」會連押後的動作一起吃掉，
// 症狀是動作整個不見。

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/command_result.h"
#include "core/config_store.h"
#include "core/tts_manager.h"
#include "core/tts_types.h"

namespace l2m {

class AudioPlayer;
class BubbleWindow;

class SpeechController : public QObject {
  Q_OBJECT

public:
  struct Request {
    std::string text;
    std::optional<std::string> voice;
    std::optional<std::string> engine;
    std::optional<double> rate;
    // true 時等到播放結束才回呼；false 時播放一開始就回呼
    bool wait = false;
    // 思考（MCP 的 think 工具）：氣泡換成思考泡泡、聲音帶殘響、嘴巴不動。
    // 三件事都只是把旗標往下傳 —— 外型交給 BubbleWindow，另外兩件交給
    // AudioPlayer::Effects，這裡不做任何判斷。
    bool thinking = false;
    // 「真的開口了」的一次性通知（見上面的閘門說明）。perform 押後的視覺步驟
    // 靠它放行；沒人設就是空的，整條路等於不存在。
    // **在 showText 之前呼叫** —— 第一次建氣泡視窗要付 DirectWrite 的字型後援
    // （實測 1398 ms），排在後面的話動作會晚那麼多才起播。
    std::function<void()> onSpeechStart;
  };

  SpeechController(ConfigStore& config, TtsManager& tts, AudioPlayer& player, BubbleWindow& bubble, QObject* parent = nullptr);
  ~SpeechController() override;

  void speak(const Request& request, std::function<void(CommandResult)> done);

  // 嘀咕：只出氣泡、不經過 TTS（autonomy.speech == "bubble" 的自主台詞）。
  // 走同一條佇列 —— 與真的說話共用「一次只有一句」的互斥，氣泡不會互相蓋台；
  // 時長依字數推估閱讀速度，至少撐滿 tts.bubbleMinDuration。
  // 講完（時間到）才回呼 done，語意等同 speak 的 wait=true。
  void mutter(const std::string& text, std::function<void(CommandResult)> done);

  CommandResult stopSpeaking();

  // 閒置判斷用：說話中（含合成中）就是忙
  bool speaking() const { return running_; }

  void listVoices(const std::optional<std::string>& engineId, std::function<void(std::vector<VoiceInfo>)> done);
  void listEngines(std::function<void(std::vector<TtsEngineInfo>)> done);

  // get_state 的 tts 區塊
  std::string stateJson() const;
  // list_voices 工具的 JSON（引擎為 nullopt 代表全部可用引擎）
  void voicesJson(const std::optional<std::string>& engineId, std::function<void(std::string)> done);

signals:
  // 一句話真的講完（或被中止）；閒置計時靠它重新倒數
  void finished();

private:
  struct Job {
    Request request;
    std::function<void(CommandResult)> done;
    // wait=false 時 done 已經先回呼過了，播完不要再叫一次
    bool answered = false;
    // mutter：只出氣泡，跳過合成與播放
    bool bubbleOnly = false;
  };

  void runNext();
  // 合成結束（成功或失敗）：決定要等播放器播完，還是直接用氣泡撐時間
  void finishSynthesis(const std::string& engine, const std::string& error, bool committed);
  // 一句結束：收氣泡、回呼、跑下一句
  void completeCurrent();
  void answer(CommandResult result);
  // 「開始出聲」的統一閘門：先放行押後的視覺步驟，再顯示氣泡。
  // 四個入口（見標頭）一律走這裡，不要直接呼叫 showBubbleOnce ——
  // 少接一個入口的症狀是「某些情況下動作永遠不演」，而那幾個入口正是
  // 沒有音效裝置、合成失敗這類本來就難重現的路徑。
  void markSpeechStart(const char* reason);
  // 把這一句的氣泡放出來（四個入口共用，重複呼叫無效）。
  // reason 是入口名稱，只進 log —— 光看毫秒數分不出「跟著聲音」與「保險到期」，
  // 兩者在慢引擎上只差幾百毫秒，而那正是這個功能對錯的分界。
  void showBubbleOnce(const char* reason);

  ConfigStore& config_;
  TtsManager& tts_;
  AudioPlayer& player_;
  BubbleWindow& bubble_;

  std::deque<Job> queue_;
  std::unique_ptr<Job> current_;
  bool running_ = false;

  // 進行中的合成。stopSpeaking() 要先取消它再拆播放器，否則飛在半路的
  // wss 訊框與 HTTP body 會回呼到已經拆掉的播放器上。
  std::unique_ptr<TtsRequestHandle> ttsHandle_;
  // 這一句的播放器有沒有開起來（沒有音效裝置時是 false，退回只顯示氣泡）
  bool playerOpen_ = false;
  // 這一句真的出過聲了嗎。沒有的話播放器不必等，直接用氣泡撐 bubbleMinDuration
  bool playerStarted_ = false;
  // 承諾點之後才失敗的錯誤：已經唸出去半句，wait=true 要如實回報
  std::string committedError_;
  // 目前串流的音訊格式（來自引擎的 onOpen），推給播放器只作診斷
  std::string streamMime_;

  // 這一句的氣泡已經放出來了嗎（四個入口共用，先到先生效）
  bool bubbleShown_ = false;
  // 這一句的 onSpeechStart 已經發過了嗎。與 bubbleShown_ 分開的理由見標頭
  bool speechStarted_ = false;

  // 沒有音訊時，氣泡至少顯示這麼久
  QTimer bubbleHold_;
  // 等第一個聲音等到不耐煩就先顯示氣泡（tts.bubbleMaxDelay）
  QTimer bubbleDelay_;
  // 這一句從開始合成算起的時間。用來把「氣泡顯示」印成跟 AudioPlayer 的
  // 「開始出聲（N ms）」同一個基準，兩個數字對得起來才代表時機是對的。
  QElapsedTimer speechClock_;
  // 最近一次實際用到的引擎與語音（get_state 用）
  std::string lastEngine_;
};

}  // namespace l2m
