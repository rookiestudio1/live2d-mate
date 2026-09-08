// Linux 的開機自啟：XDG autostart。
//
// ~/.config/autostart/ 底下一個 .desktop 檔就是一個登入啟動項（freedesktop
// 規格，GNOME/KDE/XFCE 都吃）。開＝寫入這個檔、關＝刪掉它、查詢＝檔案存不存在，
// 語意與 Windows 的 HKCU\...\Run 一對一。
//
// Exec= 的引號規則（含空白的路徑加雙引號）與註冊表那行相容，
// 直接重用 core 的 autostartCommandLine —— 兩邊寫出來的東西也就自動一致
//（就是執行檔路徑本身，不帶旗標；為什麼不再帶 --hidden 見 core/autostart_command.h）。

#include "autostart.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace l2m::platform {

namespace {

QString autostartFilePath() { return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/autostart/live2d_mate.desktop"); }

}  // namespace

void setOpenAtLogin(bool enabled) {
  const QString path = autostartFilePath();
  if (!enabled) {
    QFile::remove(path);
    return;
  }

  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;

  const std::string exec = autostartCommandLine(QCoreApplication::applicationFilePath().toStdString());
  QByteArray content;
  content += "[Desktop Entry]\n";
  content += "Type=Application\n";
  content += "Name=Live2D Mate\n";
  content += "Exec=" + QByteArray::fromStdString(exec) + "\n";
  content += "X-GNOME-Autostart-enabled=true\n";
  file.write(content);
}

bool isOpenAtLogin() { return QFile::exists(autostartFilePath()); }

void refreshOpenAtLogin() {
  QFile file(autostartFilePath());
  if (!file.open(QIODevice::ReadOnly)) return;  // 沒開自啟就什麼都不做，絕不自己寫一份出來
  const QByteArray content = file.readAll();
  file.close();

  // 只看 Exec= 那一行：判別交給 core，這裡只負責把字串挖出來。
  // trimmed() 順手吃掉 CRLF —— 使用者的 .desktop 不見得是我們寫的那份。
  const std::string exe = QCoreApplication::applicationFilePath().toStdString();
  for (const QByteArray& line : content.split('\n')) {
    if (!line.startsWith("Exec=")) continue;
    if (!needsHiddenFlagStripped(line.mid(5).trimmed().toStdString(), exe)) return;
    setOpenAtLogin(true);  // 整份重寫，格式跟著現在的產生器走
    qInfo() << "[autostart] 舊的 .desktop 還帶著隱藏旗標，已就地改寫";
    return;
  }
}

}  // namespace l2m::platform
