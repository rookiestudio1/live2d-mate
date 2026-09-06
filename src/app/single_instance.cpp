#include "single_instance.h"

#include <QDebug>
#include <QDir>
#include <QLocalSocket>

namespace {

// 既有實例回一行的等待上限。換模型那條在既有實例裡是同步做完才回覆
//（describeModel 解析 model3.json ＋ 重掃 models 目錄），
// 但真正耗時的 GL 載入排在淡出之後，不在這段裡面。
constexpr int kReplyTimeoutMs = 5000;

}  // namespace

SingleInstance::SingleInstance(const QString& key, QObject* parent) : QObject(parent), key_(key) {}

bool SingleInstance::tryAcquire(const QString& setModelPath) {
  // 鎖檔放在系統暫存目錄；QLockFile 會自動處理持有者已死掉的殘留鎖
  const QString lockPath = QDir::temp().filePath(key_ + QStringLiteral(".lock"));
  lockFile_ = std::make_unique<QLockFile>(lockPath);
  lockFile_->setStaleLockTime(0);  // 持有程序不存在即視為過期

  const bool locked = lockFile_->tryLock(100);
  qInfo() << "SingleInstance: lock =" << lockPath << "locked =" << locked << "error =" << lockFile_->error();
  if (!locked) {
    // 已有實例：送出請求、讀回一行，然後退出
    const std::string request = setModelPath.isEmpty() ? l2m::encodeShowRequest() : l2m::encodeSetModelRequest(setModelPath.toStdString());

    QLocalSocket socket;
    socket.connectToServer(key_);
    if (!socket.waitForConnected(500)) {
      reply_ = l2m::InstanceReply{false, "live2d_mate is running but not answering"};
      return false;
    }

    socket.write(QByteArray::fromStdString(request));
    socket.waitForBytesWritten(500);

    QByteArray answer;
    while (!answer.contains('\n') && socket.waitForReadyRead(kReplyTimeoutMs)) answer.append(socket.readAll());

    if (!answer.isEmpty()) {
      reply_ = l2m::parseInstanceReply(answer.toStdString());
    } else if (setModelPath.isEmpty()) {
      // 一個字都沒回，但 show 本來就不會失敗（而且舊版根本不回話）
      reply_ = l2m::InstanceReply{true, {}};
    } else {
      // 換模型沒有回覆就**不能**當成成功：既有實例若是不認得 setmodel 的舊版，
      // 它只會把角色叫出來然後沉默，這裡回 true 的話 Viewer 會說「已設定為桌寵」
      // 而畫面上什麼都沒變 —— 靜默的假成功比一句錯誤糟得多。
      //
      // 訊息刻意**不斷言原因**：超時也可能是新版桌寵慢（它還沒載過任何模型時
      // fadeOutThen 走同步捷徑，回覆要等整個 GL 載入跑完才寫得出來），
      // 那種情況其實是成功的。講「太舊」會在那條路上說謊。
      reply_ = l2m::InstanceReply{false, "live2d_mate did not answer in time; if it is an older build, restart it and try again"};
    }

    socket.disconnectFromServer();
    return false;
  }

  // 本程序是第一個實例：開 local server 等待後續實例敲門。
  // 前一個實例異常結束時 named pipe 可能殘留，先移除再監聽。
  QLocalServer::removeServer(key_);
  server_.listen(key_);
  connect(&server_, &QLocalServer::newConnection, this, [this] {
    while (QLocalSocket* conn = server_.nextPendingConnection()) {
      // 一條連線一則請求。緩衝跟著連線走，讀到換行就處理；
      // 讀不到換行也要在斷線時再處理一次 —— 舊版的 "show" 不帶換行。
      auto buffer = std::make_shared<QByteArray>();
      connect(conn, &QLocalSocket::readyRead, conn, [this, conn, buffer] {
        buffer->append(conn->readAll());
        if (!buffer->contains('\n')) return;
        handleRequest(conn, *buffer);
        buffer->clear();
      });
      connect(conn, &QLocalSocket::disconnected, conn, [this, conn, buffer] {
        if (!buffer->isEmpty()) {
          handleRequest(conn, *buffer);
          buffer->clear();
        }
        conn->deleteLater();
      });
    }
  });
  return true;
}

void SingleInstance::handleRequest(QLocalSocket* conn, const QByteArray& raw) {
  const l2m::InstanceRequest request = l2m::parseInstanceRequest(raw.toStdString());

  l2m::CommandResult result = l2m::CommandResult::failure("Unknown request", "Valid requests are \"show\" and \"setmodel <path>\".");
  switch (request.kind) {
    case l2m::InstanceRequestKind::Show:
      emit secondInstanceLaunched();
      result = l2m::CommandResult::success();
      break;
    case l2m::InstanceRequestKind::SetModel:
      result =
        onSetModel ? onSetModel(QString::fromStdString(request.path)) : l2m::CommandResult::failure("This build cannot switch models", "Update live2d_mate to a build that supports --set-model.");
      break;
    case l2m::InstanceRequestKind::Unknown:
      qWarning() << "SingleInstance: 認不得的請求" << raw;
      break;
  }

  // 一則回覆只有一行，所以 hint 直接接在 error 後面 —— 本專案的 hint 是「該怎麼
  // 修正」那一半（見 core/command_result.h），在最後一哩路上丟掉的話，
  // 使用者只看得到「不行」而不知道下一步該做什麼。
  const std::string message = result.hint.empty() ? result.error : result.error + " " + result.hint;
  const std::string answer = l2m::encodeInstanceReply(result.ok, message);

  // 舊版的第二實例送完不帶換行的 "show" 就直接斷線，這裡是從 disconnected 那條
  // 進來的 —— socket 已經是 NotOpen，寫下去只會在日誌裡留一行 QIODevice 警告。
  if (conn->state() != QLocalSocket::ConnectedState) return;
  conn->write(QByteArray::fromStdString(answer));
  conn->flush();
}
