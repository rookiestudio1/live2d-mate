#pragma once

// 微軟 Edge 線上語音。中文品質最好且免費，但需要網路。
// 沒有現成的函式庫可用，協定自己實作 —— 細節見 core/edge_tts_protocol.h。
//
// 連線流程：
//   1. GET 語音清單（純 HTTPS）決定要用哪個 voice
//   2. 開 wss（帶 Sec-MS-GEC 簽章、Edge 的 UA 與 Origin）
//   3. 連上後送 speech.config 文字訊框協商輸出格式
//   4. 送 ssml 文字訊框
//   5. 收二進位訊框，取 "Path:audio\r\n" 之後的位元組串接
//   6. 收到文字訊框 Path:turn.end 才算完成
//
// 第 5 步就是這個引擎的串流：微軟是**邊合成邊送訊框**的，把它們累積起來等
// turn.end 才交出去純粹是浪費 —— 逐訊框往播放器送，開口延遲從「整段合成完」
// 變成「第一個訊框到達」。
//
// **turn.end 之前 socket 就關掉 = 音訊被截斷**，一律當錯誤回報，
// 不可以假裝正常結束 —— 那會播出半句話還以為成功。
// 串流之後這個錯誤已經沒辦法「換下一個引擎重試整句」了（前半句已經唸出去），
// TtsManager 的承諾點會把它轉成「播完收到的部分並回報失敗」。
//
// 連線一律先過 EndpointProber（迷你 Happy Eyeballs）：這個服務的 DNS
// 會回多個位址，其中某個 IPv6 在部分網路是 TCP 黑洞，Qt 逐一嘗試會先
// 卡滿 21 秒才換下一個（實測 wss 連線卡到 42 秒）。探測出能連的位址後
// 以 IP 連線，憑證與 SNI 用 setPeerVerifyName 維持原主機名。

#include <QObject>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/tts_types.h"
#include "endpoint_prober.h"

class QWebSocket;

namespace l2m {

class HttpJson;

class EdgeTtsEngine : public QObject, public TtsEngine {
  Q_OBJECT

public:
  explicit EdgeTtsEngine(HttpJson& http, QObject* parent = nullptr);
  ~EdgeTtsEngine() override;

  const std::string& id() const override { return id_; }
  const std::string& name() const override { return name_; }

  // 微軟是邊合成邊送 wss 訊框的，所以這是真正的位元組串流 ——
  // 上層因此不必幫它套文字切句的管線
  bool streams() const override { return true; }

  void isAvailable(std::function<void(bool)> done) override;
  void listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) override;
  std::unique_ptr<TtsRequestHandle> synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) override;

private:
  // 一次合成的整段狀態。socket 的訊號都導到它，完成後自我了斷。
  struct Session;

  // 延遲填入的中止動作。synthesize() 必須馬上回傳 handle，但 session 可能要等
  // 語音清單回來才建得起來 —— 這個空殼先交給 handle，session 建好再把
  // 「abort 這條 wss」填進去。
  using AbortSlot = std::shared_ptr<std::function<void()>>;

  void startSession(const std::string& voice, const std::string& text, int ratePercent, const AbortSlot& abort, TtsStreamSink sink);
  // 真正開 wss。一律以主機名連線 —— QWebSocket 不吃 setPeerVerifyName()，
  // 連 IP 的話 SNI 會是 IP，拿到的是錯的虛擬主機（細節見 .cpp 裡 open() 前的說明）
  void openSession(const std::string& voice, const std::string& text, int ratePercent, const AbortSlot& abort, TtsStreamSink sink);

  HttpJson& http_;
  // 只服務語音清單那個 HTTPS GET：QNetworkAccessManager 認得 setPeerVerifyName，
  // 連到探測贏家的 IP 也拿得到正確的憑證與虛擬主機
  EndpointProber prober_;
  const std::string id_ = "edge";
  const std::string name_ = "Microsoft Edge Online Voices";

  std::vector<VoiceInfo> voicesCache_;
  bool voicesLoaded_ = false;
  // 語音清單請求進行中時，後到的呼叫排隊共用同一個 GET
  //（啟動時系統匣的引擎清單與語音清單會同時打進來）
  std::vector<std::function<void(std::vector<VoiceInfo>, std::string)>> pendingVoices_;
};

}  // namespace l2m
