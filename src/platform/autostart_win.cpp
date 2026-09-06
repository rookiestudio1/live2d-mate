#include "autostart.h"

#include <QCoreApplication>
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

}  // namespace l2m::platform
