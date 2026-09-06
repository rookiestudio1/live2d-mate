#pragma once

// 端點探測：迷你 Happy Eyeballs（RFC 8305），繞開 Qt 逐一嘗試位址的行為。
//
// 為什麼需要（實測數字見 core/happy_eyeballs.h 的說明）：
// speech.platform.bing.com 的 DNS 會回多個位址，其中某個 IPv6 在部分網路是
// TCP 黑洞，Qt 的逐一嘗試會先卡滿 Windows 的 21 秒連線逾時才換下一個，
// 「測試語音」因此要 20 秒以上才出聲。
//
// 做法：QHostInfo 解析後，把位址交錯排序（core/happy_eyeballs.h），
// 每隔 kAttemptDelayMs 對下一個位址發起 TCP 連線，最先連上的當贏家，
// 其餘全部中止。呼叫端拿贏家的位址字面值去連線，
// 憑證驗證與 SNI 用 QNetworkRequest::setPeerVerifyName 維持原主機名
//（Qt 的 OpenSSL 與 Schannel 後端都以 peerVerifyName 優先於連線主機名）。
//
// **只有 QNetworkAccessManager 能這樣用。** QWebSocket 完全不看
// QNetworkRequest::peerVerifyName，QSslConfiguration 也沒有等價的 SNI 設定，
// 所以拿贏家的 IP 去開 wss，SNI 會是那個 IP、伺服器回預設憑證、握手必定失敗
//（實測：Edge TTS 因此每次都無聲 fallback 到 SAPI）。
// wss 那條路只能乖乖用主機名，見 media/tts_engine_edge.cpp。
//
// 贏家有 TTL（anycast 路由會漂移），連線失敗時呼叫 forget() 立即重探。
// 探測失敗或逾時一律退回原主機名 —— 行為頂多跟沒有這一層一樣，不會更糟。

#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include <functional>
#include <map>
#include <vector>

namespace l2m {

class EndpointProber : public QObject {
  Q_OBJECT

public:
  explicit EndpointProber(QObject* parent = nullptr);
  ~EndpointProber() override;

  // 回呼參數是「應該拿去連線的 host」：
  // 探測成功 → 最快連上的位址字面值（IPv6 不帶中括號）；
  // DNS 失敗、位址不到兩個、或整體逾時 → 原樣回傳 host（退回 Qt 原本行為）。
  // 同一個 host:port 的併發呼叫共用同一次探測。回呼一律在 GUI 執行緒。
  void resolve(const QString& host, quint16 port, std::function<void(QString)> done);

  // 用贏家連線失敗時呼叫：清掉快取，下一次 resolve 重新探測
  void forget(const QString& host, quint16 port);

private:
  struct Winner {
    QString address;
    QElapsedTimer age;
  };

  void probe(const QString& key, const QString& host, quint16 port, std::vector<QHostAddress> ordered);
  // 探測結束（不論成敗）：視情況記下贏家，並喚醒所有等待中的回呼
  void conclude(const QString& key, const QString& result, bool won);

  std::map<QString, Winner> winners_;
  std::map<QString, std::vector<std::function<void(QString)>>> pending_;
};

}  // namespace l2m
