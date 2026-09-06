#pragma once

// 非同步 HTTP 用戶端的薄包裝（帶整體逾時）。
//
// 用 QNetworkAccessManager 而不是 vendored 的 cpp-httplib：
// httplib 的 client 是同步阻塞的，在 GUI 執行緒呼叫會凍住畫面，
// 另外開執行緒又得處理跨執行緒回呼 —— QNAM 本來就是事件迴圈驅動的。
// （httplib 只用在 M6 的 MCP 伺服器那一側。）
//
// 逾時靠 QTimer::singleShot → abort()：QNetworkRequest 的
// TransferTimeout 只管「傳輸停滯」，管不到「整體超時」，本機推論一跑幾十秒
// 但一直有連線的情況會被它放行，不是我們要的語意。

#include <QNetworkRequest>
#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

namespace l2m {

class HttpJson : public QObject {
  Q_OBJECT

public:
  struct Reply {
    // HTTP 狀態碼；連線層失敗時為 0
    int status = 0;
    std::string contentType;
    std::vector<char> body;
    // 非空代表連不上、逾時、被中止之類的傳輸層錯誤
    std::string transportError;

    bool ok() const { return transportError.empty() && status >= 200 && status < 300; }
    std::string bodyText() const { return std::string(body.begin(), body.end()); }
  };

  using Handler = std::function<void(Reply)>;

  // 一次請求的控制代碼。cancel() 之後**保證不會再呼叫 Handler**。
  //
  // 為什麼需要它：stopSpeaking() 以前只停得了播放器，飛在半路的 HTTP body
  // 還是會一路收完再回呼 —— 邊收邊播之後那會打到已經拆掉的播放器。
  // 每個請求都回一個，用不到的呼叫端直接忽略回傳值即可。
  class Call {
  public:
    void cancel();
    bool cancelled() const { return cancelled_; }

  private:
    friend class HttpJson;
    QPointer<QNetworkReply> reply_;
    bool cancelled_ = false;
  };
  using CallPtr = std::shared_ptr<Call>;

  explicit HttpJson(QObject* parent = nullptr);
  ~HttpJson() override;

  CallPtr get(const std::string& url, int timeoutMs, Handler done);
  // 給需要自訂標頭或 peerVerifyName 的呼叫端（例如 Edge TTS 以 IP 連線時
  // 憑證與 SNI 仍要用原主機名）。重新導向政策會統一補上
  CallPtr get(QNetworkRequest request, int timeoutMs, Handler done);
  CallPtr postJson(const std::string& url, const std::string& body, int timeoutMs, Handler done);
  // 給需要自訂標頭或非 JSON content-type 的呼叫端（自訂語音端點的 form / JSON 主體）。
  // 標頭與 ContentTypeHeader 由呼叫端設好，重新導向政策這裡統一補上。
  CallPtr post(QNetworkRequest request, const std::string& body, int timeoutMs, Handler done);

  // 增量回傳（cloud LLM 的 SSE 串流用）：readyRead 每來一批位元組就呼叫一次
  // onChunk（data 只在回呼期間有效），結束時 done 收尾 —— 那時 Reply.body 是
  // **空的**（位元組都從 onChunk 走了），只看 status / transportError。
  // 例外：非 2xx 時位元組**不走 onChunk 而是累積進 Reply.body**，
  // 錯誤回應是一份 JSON 而不是串流，呼叫端要拿整包去組錯誤訊息。
  //
  // 逾時語意與其他方法不同：stallTimeoutMs 是「兩批資料之間」的停滯上限，
  // 每收到一批就重算 —— 本機推論慢慢吐 token 可以吐十分鐘，只要一直有東西來
  // 就不算逾時；整體逾時對串流沒有意義（上面 get/post 的整體逾時理由正好相反，
  // 見檔頭）。cancel() 之後 onChunk 與 done 都保證不再被呼叫。
  //
  // onChunk 是在 QNetworkReply 的訊號槽裡同步呼出去的，所以 reply 的回收會
  // 推遲到回呼完全退棧才發 —— 呼叫端在 onChunk / done 裡開巢狀事件迴圈
  //（QDialog::exec、QMessageBox）不會再讓 reply 死在自己的訊號中途。
  // 理由與那個 0xC0000005 的堆疊寫在 .cpp。
  CallPtr postStream(QNetworkRequest request, const std::string& body, int stallTimeoutMs, std::function<void(const char* data, size_t size)> onChunk, Handler done);

private:
  // 送出請求並接上逾時與完成處理。是成員函式而不是自由函式，這樣才碰得到
  // Call 的私有成員（巢狀類別的 friend 只認得外層類別）。
  static CallPtr attach(QNetworkReply* reply, int timeoutMs, Handler done);

  QNetworkAccessManager* manager_ = nullptr;
};

}  // namespace l2m
