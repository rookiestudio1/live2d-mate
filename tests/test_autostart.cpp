// 開機自動啟動的純邏輯（core/autostart_command.h）。
//
// 自啟是直接寫 Windows 的 HKCU\...\Run 與 Linux 的 .desktop Exec=，可測的部分是
//「那一行要寫什麼」，所以斷言的是命令列的形狀；shouldStartHidden 那組只看 argv
// 有沒有 --hidden —— 旗標仍然支援手動指定，只是**自啟不再替使用者加上它**
//（症狀與理由見 core/autostart_command.h 的檔頭），所以這裡特別釘住那一行不含旗標。
#include <QtTest>

#include <string>
#include <vector>

#include "core/autostart_command.h"

using namespace l2m::platform;

class TestAutostart : public QObject {
  Q_OBJECT

private slots:
  // 註冊值就是「"exe 路徑"」，後面什麼都不接
  void commandLineIsBareExePath() { QCOMPARE(autostartCommandLine("C:\\Program Files\\Live2D Mate\\live2d_mate.exe"), std::string("\"C:\\Program Files\\Live2D Mate\\live2d_mate.exe\"")); }

  // 自啟那一行不准帶 --hidden —— 帶了就是「開機之後只剩托盤圖示，模型與 splash 都不見」
  void commandLineCarriesNoHiddenFlag() { QVERIFY(autostartCommandLine("C:/app.exe").find(kHiddenFlag) == std::string::npos); }

  // 路徑一律加引號 —— 含空白的安裝路徑不加引號會被切成兩個參數
  void quotesPathWithSpaces() { QCOMPARE(autostartCommandLine("C:/Program Files/app.exe"), std::string("\"C:/Program Files/app.exe\"")); }

  // 命令列有旗標就隱藏（現在只有手動下才會走到這條）
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

  // 舊格式（就是這支 exe 加上旗標）認得出來，該就地改寫
  void migratesOwnHiddenLine() { QVERIFY(needsHiddenFlagStripped("\"C:\\Program Files\\Live2D Mate\\live2d_mate.exe\" --hidden", "C:\\Program Files\\Live2D Mate\\live2d_mate.exe")); }

  // 已經是新格式就不要再動它（每次啟動都寫一次註冊表是白費工）
  void leavesFreshLineAlone() { QVERIFY(!needsHiddenFlagStripped("\"C:/app.exe\"", "C:/app.exe")); }

  // 路徑對不上一律不碰 —— 那一筆是另一個安裝（便攜版／舊安裝目錄）寫的，
  // 改寫它等於把別人的自啟項劫持成自己的
  void leavesOtherInstallAlone() { QVERIFY(!needsHiddenFlagStripped("\"D:/portable/live2d_mate.exe\" --hidden", "C:/app.exe")); }

  // 使用者自己多加了參數的也不動
  void leavesCustomisedLineAlone() {
    QVERIFY(!needsHiddenFlagStripped("\"C:/app.exe\" --hidden --mcp-stdio", "C:/app.exe"));
    QVERIFY(!needsHiddenFlagStripped("C:/app.exe --hidden", "C:/app.exe"));  // 沒加引號的也不是我們寫的
  }
};

QTEST_APPLESS_MAIN(TestAutostart)
#include "test_autostart.moc"
