// Live2D Viewer 的進入點。
//
// 用法：
//   live2d_viewer [模型路徑]
//   —— 路徑可以是資料夾裡的 *.model3.json、整個 *.zip，也可以是模型資料夾本身
//      （第一層有 *.model3.json 就開它）；省略的話開一個空視窗，
//      再從「開啟模型…」或拖放進來。
//
// 與桌寵的 src/app/main.cpp 相比刻意只留兩件必須排在 QApplication 之前的事
// （理由與那邊完全相同，改動前先讀那份註解）：
//
//   1. QSurfaceFormat 的 **CompatibilityProfile** —— Cubism 的 GL renderer 是
//      ES2 風格（client-side vertex array），core profile 完全禁止這種寫法。
//      這一項漏掉的症狀是「視窗開得起來、模型一片空白」。
//      alpha buffer 這裡不需要（Viewer 的畫布是不透明的），但留著沒有壞處，
//      而且與桌寵用同一份 format 比較不容易在某台機器上分岔。
//   2. setApplicationName —— **決定 %APPDATA% 底下的路徑**：viewerSettingsPath()
//      （viewer_window.cpp）就是拿 AppDataLocation 把最後一層換成桌寵那個目錄，
//      推出 %APPDATA%/live2d_mate/viewer.json（見 core/viewer_settings.h）。改掉
//      或把它挪到查詢之後，視窗幾何會靜靜地寫到別的目錄去 —— 沒有錯誤訊息，
//      只是每次開窗都回到預設大小。i18n 目錄另外走執行檔旁邊的 fallback，不受影響。
//
// 沒有 single-instance 鎖：同時開好幾個 Viewer 比對不同模型是正常用法。

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QIcon>
#include <QSurfaceFormat>

#include <filesystem>
#include <system_error>

#include "core/i18n.h"
#include "viewer_window.h"

namespace fs = std::filesystem;

namespace {

// i18n 訊息表目錄：開發環境用 repo 的 i18n/，部署用執行檔旁的 i18n/
fs::path resolveI18nDir() {
#ifdef L2M_DEV_I18N_DIR
  const fs::path dev = fs::u8path(L2M_DEV_I18N_DIR);
  std::error_code ec;
  if (fs::exists(dev / "en.json", ec)) return dev;
#endif
  return fs::u8path(QCoreApplication::applicationDirPath().toStdString()) / "i18n";
}

}  // namespace

int main(int argc, char* argv[]) {
  QSurfaceFormat fmt;
  fmt.setAlphaBufferSize(8);
  fmt.setRenderableType(QSurfaceFormat::OpenGL);
  fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
  fmt.setSwapInterval(1);  // vsync
  QSurfaceFormat::setDefaultFormat(fmt);

  QCoreApplication::setApplicationName(QStringLiteral("live2d_viewer"));

  QApplication app(argc, argv);

  // 視窗標題列與工作列按鈕的圖示，與桌寵是同一個 resources/app.ico ——
  // 兩支執行檔並排在工作列上時長得一樣是刻意的，它們本來就是同一套東西。
  // **執行檔在檔案總管裡的圖示是另一回事**，走 resources/app.rc 的 IDI_ICON1
  //（同 src/app/main.cpp 的說明）。
  //
  // 桌寵那邊讀不到 .ico 時會退回 icon.png，這裡沒有：那張 PNG 有 1.5 MB，
  // Viewer 只為了一個圖示不值得帶著它。但**還是要留一句 qWarning** ——
  // Windows 上有 app.rc 的 IDI_ICON1 接著，看起來一切正常；這個 target 卻也
  // 建 macOS bundle（qt_add_executable 的 MACOSX_BUNDLE），那邊沒有 .rc 這層，
  // 缺了 qico plugin 就是「沒有圖示，而且沒有任何線索說為什麼」。
  const QIcon appIcon(QStringLiteral(":/icons/app.ico"));
  if (appIcon.isNull()) qWarning() << "[viewer] app.ico 讀取失敗（缺少 qico plugin？），視窗沒有圖示";
  QApplication::setWindowIcon(appIcon);

  l2m::i18n::setMessagesDir(resolveI18nDir());

  ViewerWindow window;
  window.show();

  // 命令列給的路徑：看起來不像模型入口就當它不存在（不擋啟動）
  const QStringList args = QCoreApplication::arguments();
  if (args.size() >= 2 && ViewerWindow::looksLikeModelPath(args.at(1))) window.loadModel(args.at(1));

  return app.exec();
}
