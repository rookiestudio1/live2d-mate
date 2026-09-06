#include "tts_engine_edge.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QPointer>
#include <QUuid>
#include <QWebSocket>

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/edge_tts_protocol.h"
#include "http_json.h"
#include "tts_stream_util.h"

namespace l2m {

namespace {

// 從連上到收到 turn.end 的整體上限。線上服務偶爾會卡住不回也不斷線。
constexpr int kSessionTimeoutMs = 60000;

// 只涵蓋「連上」這一段。取 6 秒是因為正常握手實測 216 ms，
// 而失敗的那條路是 Windows 的 21 秒 TCP 逾時 —— 中間空得很，怎麼抓都不會誤殺。
constexpr int kConnectTimeoutMs = 6000;

std::string randomRequestId() {
  // 服務端慣例是 16 bytes 的十六進位字串；UUID 去掉連字號剛好同長度
  return QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').toStdString();
}

}  // namespace

// 一次合成的整段狀態。
//
// 自己持有 QWebSocket 並在完成時 deleteLater()：合成是「開一條連線、
// 收完一段音訊就關」的一次性流程，讓 session 管自己的生命週期，
// 引擎本身就不必維護「現在有沒有連線中」這種狀態。
struct EdgeTtsEngine::Session : public QObject {
  QWebSocket* socket = nullptr;
  std::string speechConfig;
  std::string ssmlRequest;
  bool turnEnded = false;
  bool finished = false;
  // 有沒有送出過音訊。turn.end 時用它判斷「一個位元組都沒來」
  bool sentAudio = false;
  TtsStreamSink sink;

  // 一個 wss 訊框裡的音訊直接往外送。這就是 Edge 的串流：
  // 微軟是邊合成邊送訊框的，累積起來等 turn.end 純粹是浪費。
  void pushAudio(const char* data, size_t size) {
    if (finished || size == 0) return;
    if (!sentAudio) {
      sentAudio = true;
      if (sink.onOpen) sink.onOpen("audio/mpeg");
    }
    if (sink.onChunk) sink.onChunk(data, size);
  }

  void succeed() {
    if (finished) return;
    finished = true;
    if (sink.onDone) sink.onDone();
    close();
  }

  void fail(std::string error) {
    if (finished) return;
    finished = true;
    if (sink.onError) sink.onError(std::move(error));
    close();
  }

  // 被取消：關掉連線但**不回呼** —— 呼叫端已經表明不想再聽了
  void abandon() {
    if (finished) return;
    finished = true;
    close();
  }

private:
  void close() {
    if (socket) {
      socket->abort();
      socket->deleteLater();
      socket = nullptr;
    }
    deleteLater();
  }
};

EdgeTtsEngine::EdgeTtsEngine(HttpJson& http, QObject* parent) : QObject(parent), http_(http) {}

EdgeTtsEngine::~EdgeTtsEngine() = default;

void EdgeTtsEngine::listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) {
  if (voicesLoaded_) {
    done(voicesCache_, "");
    return;
  }

  pendingVoices_.push_back(std::move(done));
  if (pendingVoices_.size() > 1) return;  // 已有進行中的請求，排隊共用結果

  const QUrl base{QString::fromLatin1(edge::kVoicesUrl)};
  const QString host = base.host();
  prober_.resolve(host, 443, [this, base, host](const QString& endpoint) {
    QNetworkRequest request{base};
    if (endpoint != host) {
      // 以探測贏家的 IP 連線；憑證驗證與 SNI 仍用原主機名。
      // HTTP/2 的 :authority 取自 URL（會變成 IP），自訂的 Host 標頭蓋不掉，
      // 伺服器會回 400 —— 這個請求強制走 HTTP/1.1
      QUrl byAddress = base;
      byAddress.setHost(endpoint);
      request.setUrl(byAddress);
      request.setRawHeader("Host", host.toLatin1());
      request.setPeerVerifyName(host);
      request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    }

    const std::string engineId = id_;
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    http_.get(request, 15000, [this, host, engineId, elapsed](HttpJson::Reply reply) {
      std::string error;
      if (!reply.ok()) {
        // 傳輸層失敗可能是探測贏家已經漂移，下一次重新探測
        if (!reply.transportError.empty()) prober_.forget(host, 443);
        error = "Edge voice list failed - " + (reply.transportError.empty() ? "HTTP " + std::to_string(reply.status) : reply.transportError);
        qWarning() << "[tts] edge 語音清單 GET 失敗（" << elapsed->elapsed() << "ms）:" << QString::fromStdString(error);
      } else {
        qDebug() << "[tts] edge 語音清單 GET 完成（" << elapsed->elapsed() << "ms）";
        voicesCache_ = edge::parseVoiceList(reply.bodyText(), engineId);
        voicesLoaded_ = !voicesCache_.empty();
        if (!voicesLoaded_) error = "The Edge voice list is empty; the network may be unreachable";
      }

      auto waiters = std::move(pendingVoices_);
      pendingVoices_.clear();
      for (auto& waiter : waiters) {
        if (error.empty()) {
          waiter(voicesCache_, "");
        } else {
          waiter({}, error);
        }
      }
    });
  });
}

void EdgeTtsEngine::isAvailable(std::function<void(bool)> done) {
  listVoices([done](std::vector<VoiceInfo> voices, std::string error) { done(error.empty() && !voices.empty()); });
}

void EdgeTtsEngine::startSession(const std::string& voice, const std::string& text, int ratePercent, const AbortSlot& abort, TtsStreamSink sink) {
  // 這裡不先跑端點探測：wss 用不到贏家的位址（原因見 openSession 裡 open() 前的說明），
  // 探一輪只是白等。語音清單那半邊仍然照探。
  openSession(voice, text, ratePercent, abort, std::move(sink));
}

void EdgeTtsEngine::openSession(const std::string& voice, const std::string& text, int ratePercent, const AbortSlot& abort, TtsStreamSink sink) {
  auto* session = new Session();
  // 掛在引擎下面：引擎收掉時還沒講完的 session 要跟著收，不能變成孤兒
  session->setParent(this);
  session->sink = std::move(sink);

  // handle 的 cancel() 從這裡起才真的中止得了連線
  if (abort) {
    *abort = [guard = QPointer<Session>(session)] {
      if (guard) guard->abandon();
    };
  }

  std::string locale = edge::localeFromVoiceName(voice);
  if (locale.empty()) locale = "en-US";

  session->speechConfig = edge::buildSpeechConfig(edge::kOutputFormat);
  session->ssmlRequest = edge::buildSsmlRequest(randomRequestId(), edge::buildSsml(voice, locale, text, ratePercent));

  const int64_t now = QDateTime::currentSecsSinceEpoch();
  const std::string url = edge::buildSynthUrl(edge::secMsGec(now, edge::kTrustedClientToken), QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').toStdString());

  session->socket = new QWebSocket();
  QWebSocket* socket = session->socket;

  // 連線耗時是這裡的歷史雷區（Happy Eyeballs 之前實測卡到 42 秒），留個記錄好排查
  auto connectElapsed = std::make_shared<QElapsedTimer>();
  connectElapsed->start();

  connect(socket, &QWebSocket::connected, session, [session, connectElapsed] {
    qDebug() << "[tts] edge wss 連線完成（" << connectElapsed->elapsed() << "ms）";
    // 連上後第一件事是協商輸出格式，之後才送 SSML
    session->socket->sendTextMessage(QString::fromStdString(session->speechConfig));
    session->socket->sendTextMessage(QString::fromStdString(session->ssmlRequest));
  });

  connect(socket, &QWebSocket::textMessageReceived, session, [session](const QString& message) {
    const std::string path = edge::framePath(message.toStdString());
    if (path == "turn.end") {
      session->turnEnded = true;
      if (!session->sentAudio) {
        session->fail("Edge TTS returned empty audio");
        return;
      }
      session->succeed();
    }
    // turn.start / response / audio.metadata 都不必處理
  });

  connect(socket, &QWebSocket::binaryMessageReceived, session, [session](const QByteArray& frame) {
    const size_t offset = edge::findAudioPayload(frame.constData(), static_cast<size_t>(frame.size()));
    if (offset == std::string::npos) return;
    session->pushAudio(frame.constData() + offset, static_cast<size_t>(frame.size()) - offset);
  });

  connect(socket, &QWebSocket::disconnected, session, [session] {
    if (session->turnEnded) return;
    // turn.end 之前就斷線 = 音訊被截斷，不能假裝成功
    session->fail(
      "Edge TTS closed the connection before the synthesis completed "
      "(the audio would be truncated)");
  });

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  connect(socket, &QWebSocket::errorOccurred, session, [session, socket](QAbstractSocket::SocketError) {
    const std::string detail = socket->errorString().toStdString();
    // 握手被拒幾乎都是簽章版本過期，講明白才有辦法排查
    qWarning() << "[tts] Edge 連線失敗（Sec-MS-GEC-Version 可能已過期）:" << socket->errorString();
    session->fail("Edge TTS connection failed - " + detail);
  });
#else
  connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), session, [session, socket](QAbstractSocket::SocketError) {
    const std::string detail = socket->errorString().toStdString();
    // 握手被拒幾乎都是簽章版本過期，講明白才有辦法排查
    qWarning() << "[tts] Edge 連線失敗（Sec-MS-GEC-Version 可能已過期）:" << socket->errorString();
    session->fail("Edge TTS connection failed - " + detail);
  });
#endif
  // 連線階段自己設一道短逾時。這條連線走主機名（見底下 open() 前的說明），
  // 因此又暴露在「DNS 回了一個 TCP 黑洞位址」那個老問題底下 ——
  // Qt 逐一嘗試會吃滿 Windows 的 21 秒。與其讓使用者等滿，不如早點放棄，
  // 交給 TtsManager 的 fallback 鏈（edge → gptsovits → voicebox → sapi）。
  // 已經連上就不管，之後的耗時歸整段的 kSessionTimeoutMs 管。
  QTimer::singleShot(kConnectTimeoutMs, session, [session, socket] {
    if (socket->state() == QAbstractSocket::ConnectedState) return;
    qWarning() << "[tts] Edge wss 連線逾時（" << kConnectTimeoutMs << "ms），改用下一個引擎";
    session->fail("Edge TTS connection timed out");
  });
  QTimer::singleShot(kSessionTimeoutMs, session, [session] { session->fail("Edge TTS timed out"); });

  // **這裡刻意不套用端點探測的贏家 IP。**
  // 語音清單那半邊（QNetworkAccessManager）可以：QNetworkRequest::setPeerVerifyName()
  // 會同時決定 SNI 與憑證驗證的名字，所以連到 IP 也拿得到正確的虛擬主機。
  // 但 QWebSocket 完全不看這個屬性，QSslConfiguration 也沒有等價的 SNI 設定，
  // 於是 ClientHello 的 SNI 會是那個 IP，Azure Front Door 回的是預設憑證
  // （實測 CN/SAN = *.azureedge.net），握手固定以 HostNameMismatch 收場。
  // 結果是 Edge 每次都失敗、無聲地 fallback 到 SAPI —— get_state 會看到
  // engine=edge 但 lastUsedEngine=sapi，使用者以為在用 Edge，其實不是。
  //
  // 那不是「驗證名字對不上」而已，是根本連到錯的服務，所以 ignoreSslErrors()
  // 這類補救一律不成立。唯一能讓 SNI 正確的方式就是把主機名交給 QWebSocket。
  // 代價是失去 Happy Eyeballs 的保護，用上面那道連線逾時把最壞情況壓住。
  QNetworkRequest request;
  request.setUrl(QUrl{QString::fromStdString(url)});
  request.setRawHeader("User-Agent", edge::kUserAgent);
  request.setRawHeader("Origin", edge::kOrigin);
  socket->open(request);
}

std::unique_ptr<TtsRequestHandle> EdgeTtsEngine::synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) {
  // SSML 的 rate 用相對百分比：1 → +0%，1.5 → +50%
  const int ratePercent = static_cast<int>(std::lround((options.rate.value_or(1) - 1) * 100));

  auto cancelled = std::make_shared<bool>(false);
  auto abort = std::make_shared<std::function<void()>>();
  auto handle = std::make_unique<FunctionTtsHandle>(cancelled, [abort] {
    if (*abort) (*abort)();
  });

  if (options.voice.has_value() && !options.voice->empty()) {
    startSession(*options.voice, text, ratePercent, abort, std::move(sink));
    return handle;
  }

  listVoices([this, text, ratePercent, sink, cancelled, abort](std::vector<VoiceInfo> voices, std::string error) {
    // 語音清單那個 GET 自己不吃取消（它也給設定頁用），靠這道護欄擋住後續動作
    if (*cancelled) return;
    if (!error.empty()) {
      if (sink.onError) sink.onError(std::move(error));
      return;
    }
    if (voices.empty()) {
      if (sink.onError) {
        sink.onError("The Edge voice list is empty; the network may be unreachable");
      }
      return;
    }

    // 依偏好地區挑第一個；都沒有就用清單第一個（已排序）
    std::string chosen = voices.front().id;
    for (const auto& locale : edge::preferredLocales()) {
      const auto it = std::find_if(voices.begin(), voices.end(), [&locale](const VoiceInfo& v) { return v.locale == locale; });
      if (it != voices.end()) {
        chosen = it->id;
        break;
      }
    }
    startSession(chosen, text, ratePercent, abort, sink);
  });
  return handle;
}

}  // namespace l2m
