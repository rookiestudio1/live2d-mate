// Linux 的開機自啟：XDG autostart。
//
// ~/.config/autostart/ 底下一個 .desktop 檔就是一個登入啟動項（freedesktop
// 規格，GNOME/KDE/XFCE 都吃）。開＝寫入這個檔、關＝刪掉它、查詢＝檔案存不存在，
// 語意與 Windows 的 HKCU\...\Run 一對一。
//
// Exec= 的引號規則（含空白的路徑加雙引號）與註冊表那行相容，
// 直接重用 core 的 autostartCommandLine —— 旗標語意也就自動一致（--hidden）。

#include "autostart.h"

#include <QCoreApplication>
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

}  // namespace l2m::platform
