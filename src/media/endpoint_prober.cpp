#include "endpoint_prober.h"

#include <QDebug>
#include <QHostInfo>
#include <QTcpSocket>
#include <QTimer>

#include <memory>
#include <utility>

#include "core/happy_eyeballs.h"

namespace l2m {

namespace {

QString cacheKey(const QString& host, quint16 port) { return host + QLatin1Char(':') + QString::number(port); }

}  // namespace

EndpointProber::EndpointProber(QObject* parent) : QObject(parent) {}

EndpointProber::~EndpointProber() = default;

void EndpointProber::resolve(const QString& host, quint16 port, std::function<void(QString)> done) {
  const QString key = cacheKey(host, port);

  const auto cached = winners_.find(key);
  if (cached != winners_.end()) {
    if (cached->second.age.elapsed() < net::kWinnerTtlMs) {
      done(cached->second.address);
      return;
    }
    winners_.erase(cached);
  }

  auto& waiters = pending_[key];
  waiters.push_back(std::move(done));
  if (waiters.size() > 1) return;  // 已有進行中的探測，共用結果

  QHostInfo::lookupHost(host, this, [this, key, host, port](const QHostInfo& info) {
    const QList<QHostAddress> resolved = info.addresses();
    if (info.error() != QHostInfo::NoError || resolved.size() < 2) {
      // 解析失敗交給後續連線自己報錯；單一位址沒有排序的意義
      conclude(key, host, false);
      return;
    }
    probe(key, host, port, std::vector<QHostAddress>(resolved.begin(), resolved.end()));
  });
}

void EndpointProber::forget(const QString& host, quint16 port) { winners_.erase(cacheKey(host, port)); }

void EndpointProber::probe(const QString& key, const QString& host, quint16 port, std::vector<QHostAddress> ordered) {
  ordered = net::orderForProbe(ordered);

  // ctx 統一持有這次探測的 socket 與計時器：結束時 deleteLater 一次收乾淨
  auto* ctx = new QObject(this);
  auto finished = std::make_shared<bool>(false);
  auto remaining = std::make_shared<int>(static_cast<int>(ordered.size()));

  const auto settle = [this, ctx, key, finished](const QString& result, bool won) {
    *finished = true;
    ctx->deleteLater();
    conclude(key, result, won);
  };

  for (size_t i = 0; i < ordered.size(); ++i) {
    const QHostAddress address = ordered[i];
    QTimer::singleShot(static_cast<int>(i) * net::kAttemptDelayMs, ctx, [ctx, finished, remaining, settle, host, address, port] {
      if (*finished) return;
      auto* socket = new QTcpSocket(ctx);
      connect(socket, &QTcpSocket::connected, ctx, [socket, finished, settle, address] {
        if (*finished) return;
        socket->abort();  // 只是探路，真正的連線由呼叫端自己開
        settle(address.toString(), true);
      });
      connect(socket, &QTcpSocket::errorOccurred, ctx, [socket, finished, remaining, settle, host](QAbstractSocket::SocketError) {
        socket->disconnect();  // 同一個 socket 不重複計數
        if (*finished) return;
        if (--(*remaining) == 0) settle(host, false);  // 全滅：退回原主機名
      });
      socket->connectToHost(address, port);
    });
  }

  QTimer::singleShot(net::kProbeTimeoutMs, ctx, [finished, settle, host] {
    if (*finished) return;
    settle(host, false);  // 逾時：退回原主機名
  });
}

void EndpointProber::conclude(const QString& key, const QString& result, bool won) {
  if (won) {
    Winner winner;
    winner.address = result;
    winner.age.start();
    winners_[key] = std::move(winner);
    qInfo() << "[tts] 端點探測贏家:" << key << "→" << result;
  }

  auto waiters = std::move(pending_[key]);
  pending_.erase(key);
  for (auto& done : waiters) done(result);
}

}  // namespace l2m
