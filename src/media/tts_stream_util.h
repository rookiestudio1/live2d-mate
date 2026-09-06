#pragma once

// 四個網路 TTS 引擎共用的兩件小事：把回覆餵進串流 sink，以及提供可取消的 handle。
//
// 抽出來是因為這兩段在每個引擎裡都一模一樣，而它們又剛好是最容易寫錯的地方：
// 漏掉 cancelled 的檢查，stopSpeaking() 之後遲到的回覆就會打到已經拆掉的播放器。

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/tts_types.h"
#include "http_json.h"

namespace l2m {

// 一連串 HTTP 請求的取消器。GPT-SoVITS 合成前還要先切兩次權重，所以「一次合成」
// 不見得只有一個請求 —— 每一步把自己的 Call 存進 current，後續步驟開頭先看
// cancelled。cancel() 之後保證不會再有任何 sink 回呼。
struct HttpCallChain {
  HttpJson::CallPtr current;
  bool cancelled = false;

  void cancel() {
    if (cancelled) return;
    cancelled = true;
    if (current) current->cancel();
  }
};
using HttpCallChainPtr = std::shared_ptr<HttpCallChain>;

class HttpChainHandle : public TtsRequestHandle {
public:
  explicit HttpChainHandle(HttpCallChainPtr chain) : chain_(std::move(chain)) {}
  void cancel() override {
    if (chain_) chain_->cancel();
  }

private:
  HttpCallChainPtr chain_;
};

inline std::unique_ptr<TtsRequestHandle> chainHandle(HttpCallChainPtr chain) { return std::make_unique<HttpChainHandle>(std::move(chain)); }

// 把「一次收完整個 body」的回覆餵進 sink：整句就是一段。
// 位元組串流接上來之後，這條路只剩下真的沒辦法分塊的來源會走。
inline void emitWholeBody(const TtsStreamSink& sink, const std::vector<char>& body, const std::string& mime) {
  if (sink.onOpen) sink.onOpen(mime);
  if (sink.onChunk && !body.empty()) sink.onChunk(body.data(), body.size());
  if (sink.onDone) sink.onDone();
}

}  // namespace l2m
