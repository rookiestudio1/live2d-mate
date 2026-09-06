#include "splash_process.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

#include "../windows/splash_window.h"

namespace l2m {

namespace {

// 主行程一直沒連上來（例如它在連上之前就崩了、或根本沒 spawn 成功）時的上限。
// 正常情況主行程在建完物件後幾百毫秒內就會連上。
constexpr int kConnectWaitMs = 10000;
// 換模型的小卡片給短一點：那是使用者主動操作，真出了差錯也不該在畫面上賴太久。
constexpr int kCompactConnectWaitMs = 1500;

// 連上之後的總上限。主行程若卡死不放，splash 也不該一直掛在畫面上。
// 給得寬鬆是因為冷啟動的 shader 編譯本來就要好幾秒。
constexpr int kMaxLifetimeMs = 60000;
constexpr int kCompactMaxLifetimeMs = 20000;

}  // namespace

QString splashServerName() { return QStringLiteral("live2d_mate-splash-%1").arg(QCoreApplication::applicationPid()); }

int runSplashProcess(int argc, char* argv[]) {
  QApplication app(argc, argv);
  // 這個行程只有 splash 一個視窗，但淡出時會先 hide 才結束，
  // 不關掉這個預設行為的話 hide 當下就會直接退出，淡出看不到
  app.setQuitOnLastWindowClosed(false);

  const QString serverName = argc >= 3 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("live2d_mate-splash");

  // --compact <x> <y> <w> <h>：換模型時的小卡片，貼在主行程傳來的角色視窗上。
  // 手動解析而不用 QCommandLineParser：參數固定就這一種形式，
  // 而且 parser 會把無法辨識的旗標當錯誤，跟 main() 那邊的短路風格也不一致。
  SplashWindow::Mode mode = SplashWindow::Mode::Startup;
  QRect anchor;
  if (argc >= 8 && QString::fromLocal8Bit(argv[3]) == QLatin1String("--compact")) {
    mode = SplashWindow::Mode::Compact;
    anchor = QRect(QString::fromLocal8Bit(argv[4]).toInt(), QString::fromLocal8Bit(argv[5]).toInt(), QString::fromLocal8Bit(argv[6]).toInt(), QString::fromLocal8Bit(argv[7]).toInt());
  }
  const bool compact = mode == SplashWindow::Mode::Compact;

  SplashWindow splash(mode);
  splash.setAnchor(anchor);

  // 先 listen 再顯示：主行程可能已經在重試連線了，早一刻 listen 就早一刻接上
  auto* server = new QLocalServer(&app);
  QLocalServer::removeServer(serverName);  // 清掉上次沒收乾淨的殘留
  if (!server->listen(serverName)) {
    qWarning() << "[splash] local server 起不來:" << server->errorString();
    return 1;
  }

  bool connected = false;
  QObject::connect(server, &QLocalServer::newConnection, &app, [&] {
    QLocalSocket* conn = server->nextPendingConnection();
    if (!conn) return;
    connected = true;

    if (compact) {
      // 換模型：主行程沒辦法在載入「之前」跟我們握手 —— 它得先等我們
      // listen（實測要 400 ms 以上），而那段等待是直接加在切換時間上的。
      // 所以它改成載入完成後才連上來一次，連線本身就是收尾訊號。
      // 崩潰保護交給下面 kCompactConnectWaitMs 那道逾時。
      conn->deleteLater();
      splash.finish();
      return;
    }

    // 啟動：主行程一路掛著這條連線，斷線才是訊號 ——
    // 它載入完成會主動斷，崩潰則由 OS 幫我們斷，孤兒清理是免費的。
    // 那邊等得起：主行程要建一整組物件，我們早就 listen 好了。
    // deleteLater 必須留在這個 lambda 裡：socket 得活到斷線那一刻，
    // 提前排刪除會讓解構時的 close() 自己發出 disconnected，
    // splash 在主行程剛連上（模型還沒載）時就被收掉（5a309b8 的回歸）。
    QObject::connect(conn, &QLocalSocket::disconnected, &splash, [&splash, conn] {
      conn->deleteLater();
      splash.finish();
    });
  });

  // 淡出真的跑完才結束行程
  QObject::connect(&splash, &SplashWindow::closed, &app, &QCoreApplication::quit);

  splash.begin();

  // 主行程沒在期限內連上來 —— 它多半在連線前就死了，別留一張孤兒 splash
  QTimer::singleShot(compact ? kCompactConnectWaitMs : kConnectWaitMs, &app, [&] {
    if (!connected) {
      qWarning() << "[splash] 主行程逾時未連上，自行收尾";
      splash.finish();
    }
  });
  QTimer::singleShot(compact ? kCompactMaxLifetimeMs : kMaxLifetimeMs, &app, [&splash] { splash.finish(); });

  return app.exec();
}

}  // namespace l2m
