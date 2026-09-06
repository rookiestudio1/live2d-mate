#pragma once

// 單一實例鎖：第二次啟動不會再開一個角色，只會叫醒既有的實例。
// 用 QLockFile 判定是否已有實例在跑；若有，透過 QLocalSocket 送一則請求給
// 既有實例，然後自己退出。
//
// 請求有兩種（線上格式與剖析全在 core/external_model.h，那邊有測試釘住）：
//
//   show              把角色叫出來，也就是原本「再按一次捷徑」的行為。
//   setmodel <path>   換成這個模型。命令列的 --set-model 走這條，
//                     Live2D Viewer 的「設定為桌寵」按鈕也是。
//
// 既有實例一律回一行（ok 或 error <訊息>），送出的那一端用 reply() 讀。
// 為什麼要有回覆：Viewer 想在狀態列說出到底成不成功，而「送出去了」跟
// 「桌寵真的換好了」是兩件事 —— 路徑可能根本不是 Cubism 4 模型。
//
// 收端要讀到換行、或讀到對方斷線為止：舊版送的是不帶換行的 "show"，
// 那條路要繼續認得（升級中途兩個版本並存是真的會發生的）。

#include <QLocalServer>
#include <QLockFile>
#include <QObject>

#include <functional>
#include <memory>

#include "core/command_result.h"
#include "core/external_model.h"

class QLocalSocket;

class SingleInstance : public QObject {
  Q_OBJECT

public:
  explicit SingleInstance(const QString& key, QObject* parent = nullptr);

  // 嘗試取得鎖。回傳 false 表示已有實例在跑（請求已送出，結果見 reply()）。
  // setModelPath 非空時送的是 setmodel，否則是 show。
  bool tryAcquire(const QString& setModelPath = QString());

  // 送出請求之後既有實例回的那一行。只有 tryAcquire() 回 false 時有意義。
  const l2m::InstanceReply& reply() const { return reply_; }

  // main() 注入：收到 setmodel 要做什麼，回傳值原樣回給送出的那一端。
  // 用 std::function 而不是 signal，是因為這裡需要回傳值，而且
  // SingleInstance 不該認識 AppController（同 windows/tray.h 的理由）。
  std::function<l2m::CommandResult(const QString& path)> onSetModel;

signals:
  // 第二個實例啟動時，既有實例會收到此訊號
  void secondInstanceLaunched();

private:
  // 收到完整一則請求：分派、然後回一行給對方
  void handleRequest(QLocalSocket* conn, const QByteArray& raw);

  QString key_;
  std::unique_ptr<QLockFile> lockFile_;
  QLocalServer server_;
  l2m::InstanceReply reply_;
};
