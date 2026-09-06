#pragma once

// 設定視窗的「關於」分頁。
//
// 這頁扛三件事：
// ① **使用者回報問題時貼得出來的第一手資料** —— 編譯期與執行期的 Qt 版本
//    （不同就代表機器上放了另一版 Qt DLL，這是「在我這台好好的」類問題的第一個線索）、
//    設定檔與模型資料夾的實際路徑。
// ② **著作權與授權聲明** —— 本體是 MIT（© RookieStudio，見 repo 根目錄的 LICENSE），
//    但 Cubism Core 是 Live2D 的閉源專有元件，這頁底下的 notice 就是那份商標與授權聲明。
//    作者名與 "MIT License" 刻意寫死在 .cpp 而不進 i18n：翻譯了反而對不上 LICENSE 檔。
// ③ **開啟日誌資料夾** —— 使用者回報問題時最常需要的一個動作。這顆按鈕不只服務
//    當機報告：一般的 live2d_mate-*.log 與 crash-*.log 住在同一個資料夾，
//    請對方「把 logs 整個壓縮寄過來」時有個地方按。**開不起來一定要有退路**：
//    `QDesktopServices::openUrl` 在沒有預設瀏覽器或 shell 關聯壞掉時只回 false 而
//    畫面毫無反應，那就是「按了沒反應」的無聲失敗，所以失敗時改把路徑塞進剪貼簿
//    並在狀態列說明。
//
// 對應的表單是 about_page.ui：appName／tagline／versions／notice 四個 QLabel、
// openLogsButton 一顆按鈕，與一個空的 QFormLayout —— 四列
// （著作權／授權／設定檔／模型資料夾）由程式碼填。版本字串全部來自編譯期巨集
// （L2M_APP_VERSION、L2M_CUBISM_VERSION），沒有第二份寫死的數字。

#include <QWidget>

#include <memory>

#include "settings_context.h"

namespace Ui {
class AboutPage;
}

namespace l2m {

class AboutPage : public QWidget {
  Q_OBJECT

public:
  explicit AboutPage(const SettingsContext& context, QWidget* parent = nullptr);
  ~AboutPage() override;

  void retranslate();
  void refresh();

private:
  QString tr2(const char* key) const;

  SettingsContext ctx_;
  std::unique_ptr<Ui::AboutPage> ui_;
};

}  // namespace l2m
