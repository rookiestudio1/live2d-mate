#include "http_json.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace l2m {

namespace {

QNetworkRequest makeRequest(const std::string& url) {
  QNetworkRequest request{QUrl(QString::fromStdString(url))};
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  return request;
}

}  // namespace

// 送出請求並接上逾時與完成處理。timedOut 由 lambda 之間共享，
// 逾時中止與伺服器主動關閉的錯誤訊息要分得開。
HttpJson::CallPtr HttpJson::attach(QNetworkReply* reply, int timeoutMs, HttpJson::Handler done) {
  auto timedOut = std::make_shared<bool>(false);
  auto call = std::make_shared<HttpJson::Call>();
  call->reply_ = reply;

  if (timeoutMs > 0) {
    QPointer<QNetworkReply> guard(reply);
    QTimer::singleShot(timeoutMs, reply, [guard, timedOut] {
      if (!guard) return;
      *timedOut = true;
      guard->abort();
    });
  }

  QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done, timedOut, call] {
    // 被取消的請求：abort() 也會走到這裡，但呼叫端已經表明不想再聽了
    if (call->cancelled()) {
      reply->deleteLater();
      return;
    }

    HttpJson::Reply out;
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower().toStdString();

    // 連線被拒（本機服務沒開）時 reply 根本沒開過，直接 readAll 會噴
    // "QIODevice::read: device not open" —— 探測失敗是正常情況，不該有雜訊
    const QByteArray data = reply->isOpen() ? reply->readAll() : QByteArray();
    out.body.assign(data.begin(), data.end());

    if (reply->error() != QNetworkReply::NoError) {
      // 有狀態碼就代表伺服器有回話（例如 400），那不是傳輸層錯誤 ——
      // GPT-SoVITS 的存活探測正是靠「400 也算活著」判斷的
      if (*timedOut) {
        out.transportError = "request timed out";
      } else if (out.status == 0) {
        out.transportError = reply->errorString().toStdString();
      }
    }

    done(std::move(out));
    reply->deleteLater();
  });

  return call;
}

void HttpJson::Call::cancel() {
  if (cancelled_) return;
  cancelled_ = true;
  // abort() 會同步發 finished，但上面的 lambda 看到 cancelled_ 就不回呼了
  if (reply_) reply_->abort();
}

HttpJson::HttpJson(QObject* parent) : QObject(parent), manager_(new QNetworkAccessManager(this)) {}

HttpJson::~HttpJson() = default;

HttpJson::CallPtr HttpJson::get(const std::string& url, int timeoutMs, Handler done) { return attach(manager_->get(makeRequest(url)), timeoutMs, std::move(done)); }

HttpJson::CallPtr HttpJson::get(QNetworkRequest request, int timeoutMs, Handler done) {
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  return attach(manager_->get(request), timeoutMs, std::move(done));
}

HttpJson::CallPtr HttpJson::postJson(const std::string& url, const std::string& body, int timeoutMs, Handler done) {
  QNetworkRequest request = makeRequest(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  return post(std::move(request), body, timeoutMs, std::move(done));
}

HttpJson::CallPtr HttpJson::post(QNetworkRequest request, const std::string& body, int timeoutMs, Handler done) {
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  return attach(manager_->post(request, QByteArray(body.data(), static_cast<qsizetype>(body.size()))), timeoutMs, std::move(done));
}

HttpJson::CallPtr HttpJson::postStream(QNetworkRequest request, const std::string& body, int stallTimeoutMs, std::function<void(const char*, size_t)> onChunk, Handler done) {
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  QNetworkReply* reply = manager_->post(request, QByteArray(body.data(), static_cast<qsizetype>(body.size())));

  auto timedOut = std::make_shared<bool>(false);
  auto call = std::make_shared<Call>();
  call->reply_ = reply;
  // 非 2xx 的回應不是串流：位元組累積在這裡，done 時整包交出去組錯誤訊息
  auto errorBody = std::make_shared<std::vector<char>>();

  // **呼叫端的回呼還在堆疊上時絕對不能刪 reply**。串流的 onChunk 是從
  // QNetworkReply::readyRead 的訊號槽裡同步呼出去的，只要呼叫端在那裡開了
  // 巢狀事件迴圈（QDialog::exec / QMessageBox），緊接著送達的 finished 就會在
  // 那個巢狀迴圈裡跑，此處的 deleteLater() 也會被它**就地執行掉**
  //（QDeferredDeleteEvent 記的 loopLevel+scopeLevel 比巢狀迴圈自己高一級）——
  // QNetworkReply 於是死在自己的訊號中途，堆疊捲回 Qt6Network 的
  // replyDownloadData() 之後那句 `emit q->downloadProgress(...)` 就是
  // use-after-free（實測 0xC0000005，角色分頁的 AI 擴寫預覽框踩過）。
  // depth ＝ 回呼在堆疊上的層數；刪除一律推遲到層數歸零才發。
  auto depth = std::make_shared<int>(0);
  auto deleteWanted = std::make_shared<bool>(false);
  auto retire = [reply, depth, deleteWanted] {
    if (*depth > 0) {
      *deleteWanted = true;
      return;
    }
    reply->deleteLater();
  };

  // 停滯計時器：掛在 reply 底下，finished → deleteLater 時一起走
  QTimer* stall = nullptr;
  if (stallTimeoutMs > 0) {
    stall = new QTimer(reply);
    stall->setSingleShot(true);
    stall->setInterval(stallTimeoutMs);
    QPointer<QNetworkReply> guard(reply);
    QObject::connect(stall, &QTimer::timeout, reply, [guard, timedOut] {
      if (!guard) return;
      *timedOut = true;
      guard->abort();
    });
    stall->start();
  }

  QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, call, onChunk, errorBody, stall, depth, deleteWanted] {
    if (call->cancelled()) return;
    const QByteArray data = reply->readAll();
    if (data.isEmpty()) return;
    if (stall) stall->start();  // 有東西來就重算停滯
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 200 && status < 300) {
      if (onChunk) {
        ++*depth;
        onChunk(data.constData(), static_cast<size_t>(data.size()));
        // 回呼期間送達的 finished 只會立旗標，真正的刪除補在這裡
        if (--*depth == 0 && *deleteWanted) {
          *deleteWanted = false;
          reply->deleteLater();
        }
      }
    } else {
      errorBody->insert(errorBody->end(), data.begin(), data.end());
    }
  });

  QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done, timedOut, call, errorBody, onChunk, depth, retire] {
    if (call->cancelled()) {
      retire();
      return;
    }

    // 殘尾的 onChunk 也在這個處理器裡，一併算進層數
    ++*depth;
    Reply out;
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower().toStdString();
    out.body = std::move(*errorBody);
    // readyRead 沒消化完的殘尾（Qt 保證 finished 前資料都發過 readyRead，
    // 但保個險：2xx 的殘尾照樣走 onChunk，錯誤回應併進 body）
    if (reply->isOpen()) {
      const QByteArray rest = reply->readAll();
      if (!rest.isEmpty()) {
        if (out.status >= 200 && out.status < 300) {
          if (onChunk) onChunk(rest.constData(), static_cast<size_t>(rest.size()));
        } else {
          out.body.insert(out.body.end(), rest.begin(), rest.end());
        }
      }
    }

    if (reply->error() != QNetworkReply::NoError) {
      if (*timedOut) {
        out.transportError = "request stalled (no data received in time)";
      } else if (out.status == 0) {
        out.transportError = reply->errorString().toStdString();
      }
    }

    done(std::move(out));
    --*depth;
    retire();
  });

  return call;
}

}  // namespace l2m
