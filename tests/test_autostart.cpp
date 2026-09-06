// 開機自動啟動的純邏輯（core/autostart_command.h）。
//
// 自啟是直接寫 Windows 的 HKCU\...\Run，可測的部分是「註冊表要寫哪一行」，
// 所以斷言的是命令列的形狀；shouldStartHidden 那組只看 argv 有沒有 --hidden。
#include <QtTest>

#include <string>
#include <vector>

#include "core/autostart_command.h"

using namespace l2m::platform;

class TestAutostart : public QObject {
  Q_OBJECT

private slots:
  // 註冊值是「"exe 路徑" --hidden」
  void commandLineCarriesHiddenFlag() {
    QCOMPARE(autostartCommandLine("C:\\Program Files\\Live2D Mate\\live2d_mate.exe"), std::string("\"C:\\Program Files\\Live2D Mate\\live2d_mate.exe\" --hidden"));
  }

  // 路徑一律加引號 —— 含空白的安裝路徑不加引號會被切成兩個參數
  void quotesPathWithSpaces() {
    const std::string line = autostartCommandLine("C:/Program Files/app.exe");
    QVERIFY(line.front() == '"');
    QVERIFY(line.find("\" --hidden") != std::string::npos);
  }

  // 命令列有旗標就隱藏（開機自啟走的就是這條）
  void hiddenWhenFlagPresent() { QVERIFY(shouldStartHidden({"live2d_mate.exe", "--hidden"})); }

  // 沒有旗標就正常顯示
  void visibleWithoutFlag() {
    QVERIFY(!shouldStartHidden({"live2d_mate.exe"}));
    QVERIFY(!shouldStartHidden({}));
    // 只是長得像也不算
    QVERIFY(!shouldStartHidden({"live2d_mate.exe", "--hidden-extra"}));
  }

  // 旗標出現在任何位置都算
  void flagAnywhereCounts() { QVERIFY(shouldStartHidden({"live2d_mate.exe", "--mcp-stdio", "--hidden"})); }
};

QTEST_APPLESS_MAIN(TestAutostart)
#include "test_autostart.moc"
