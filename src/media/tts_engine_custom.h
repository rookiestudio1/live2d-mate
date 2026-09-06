#pragma once

// 自訂 HTTP 語音端點。
//
// 使用者在設定視窗（語音 → 自定義語音設定）自己填 URL、HTTP 方法、參數樣板與標頭，
// 參數裡的 ${TEXT} 會被換成要唸的句子。組裝規則全部是 core/tts_http.h 的純函式，
// 這個類別只負責「把它交給 QNetworkAccessManager」。
//
// 與其他三個引擎的三個刻意差異：
//
//  1. **isAvailable 永遠回 true**，而且不打網路。兩個理由疊在一起：
//     (a) 任意端點沒有 health path，拿合成端點當存活探測等於每次開語音分頁
//         就真的合成一次 —— 對計費 API 是直接扣錢。
//     (b) 「沒填 URL 就回 false」會變成雞生蛋蛋生雞：不可用的引擎在語音分頁
//         是**選不了**的（rebuildEngineCombo 會把它 disable），而填 URL 的欄位
//         又只在選到這個引擎時才出現 —— 使用者永遠沒有機會設定它。
//     設定不完整的回報改在真正要用的時候做：synthesize 直接回
//     buildCustomTtsRequest 的 error，設定頁則同步顯示同一句紅字。
//  2. **listVoices 回空清單且不算錯誤**。這裡沒有語音清單 API；語音是使用者
//     寫在參數樣板裡的，設定頁會把語音下拉停用並顯示說明。回 error 只會讓
//     語音分頁跳出一句沒人修得了的紅字。
//  3. **rate 與 voice 一律忽略**。樣板裡沒有對應的佔位符，硬塞進去只會讓
//     端點收到看不懂的欄位。

#include <QObject>

#include <functional>
#include <memory>
#include <string>

#include "core/config_schema.h"
#include "core/tts_types.h"
#include "tts_stream_util.h"

namespace l2m {

class HttpJson;

class CustomTtsEngine : public QObject, public TtsEngine {
  Q_OBJECT

public:
  CustomTtsEngine(HttpJson& http, std::function<CustomTtsConfig()> getConfig, QObject* parent = nullptr);

  const std::string& id() const override { return id_; }
  const std::string& name() const override { return name_; }

  void isAvailable(std::function<void(bool)> done) override;
  void listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) override;
  std::unique_ptr<TtsRequestHandle> synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) override;

private:
  HttpJson& http_;
  // 設定每次現取而不是建構時快照，使用者一改欄位下一句話就生效
  std::function<CustomTtsConfig()> getConfig_;
  const std::string id_ = "custom";
  const std::string name_ = "Custom HTTP endpoint";
};

}  // namespace l2m
