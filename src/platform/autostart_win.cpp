#include "autostart.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QSettings>

namespace l2m::platform {

namespace {

constexpr const char* kRunKey = R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)";
constexpr const char* kValueName = "Live2D Mate";

}  // namespace

void setOpenAtLogin(bool enabled) {
  QSettings run(QString::fromUtf8(kRunKey), QSettings::NativeFormat);
  if (enabled) {
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    run.setValue(QString::fromUtf8(kValueName), QString::fromStdString(autostartCommandLine(exe.toStdString())));
  } else {
    run.remove(QString::fromUtf8(kValueName));
  }
}

bool isOpenAtLogin() {
  QSettings run(QString::fromUtf8(kRunKey), QSettings::NativeFormat);
  return run.contains(QString::fromUtf8(kValueName));
}

void refreshOpenAtLogin() {
  QSettings run(QString::fromUtf8(kRunKey), QSettings::NativeFormat);
  const QString stored = run.value(QString::fromUtf8(kValueName)).toString();
  if (stored.isEmpty()) return;  // 沒開自啟就什麼都不做，絕不自己開起來

  const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
  if (!needsHiddenFlagStripped(stored.toStdString(), exe.toStdString())) return;

  run.setValue(QString::fromUtf8(kValueName), QString::fromStdString(autostartCommandLine(exe.toStdString())));
  qInfo() << "[autostart] 舊的自啟項還帶著隱藏旗標，已就地改寫";
}

}  // namespace l2m::platform
