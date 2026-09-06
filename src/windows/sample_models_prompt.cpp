#include "sample_models_prompt.h"

#include <QDebug>
#include <QDesktopServices>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QUrl>

#include "core/i18n.h"
#include "core/sample_models.h"

namespace l2m {

namespace {

QString tr2(const std::string& locale, const char* key, const i18n::TParams& params = {}) { return QString::fromStdString(i18n::translate(locale, key, params)); }

}  // namespace

bool promptForSampleModels(const std::string& uiLocale, const std::filesystem::path& modelsDir) {
  const QString folder = QString::fromStdString(modelsDir.u8string());

  QMessageBox box(QMessageBox::Question, tr2(uiLocale, "dialog.noModels.title"), tr2(uiLocale, "dialog.noModels.text"));
  box.setInformativeText(tr2(uiLocale, "dialog.noModels.detail", i18n::TParams().arg("folder", folder.toStdString())));

  // 自己加按鈕而不是用 QMessageBox::Yes/No：那兩顆的文字由 Qt 依系統語系決定，
  // 而本專案的語系是自己那五份 JSON 決定的 —— 使用者把介面切成日文，
  // 系統卻是英文的話，同一個對話框會一半日文一半英文。
  QPushButton* download = box.addButton(tr2(uiLocale, "dialog.noModels.download"), QMessageBox::AcceptRole);
  box.addButton(tr2(uiLocale, "dialog.noModels.later"), QMessageBox::RejectRole);
  box.setDefaultButton(download);

  // 角色視窗是 always-on-top，不跟著置頂的話這個框會開在它後面 ——
  // 而角色視窗又是穿透點擊的，畫面上就成了一個看得到、按不到也關不掉的桌寵。
  box.setWindowFlag(Qt::WindowStaysOnTopHint, true);
  box.exec();

  if (box.clickedButton() != download) {
    qInfo() << "[models] 使用者選擇稍後再找模型";
    return false;
  }

  const std::string url = sampleModelsUrl(uiLocale);
  qInfo() << "[models] 開啟官方免費模型頁面:" << QString::fromStdString(url);
  QDesktopServices::openUrl(QUrl(QString::fromStdString(url)));
  // models 目錄一起開：下載回來的 zip 要有地方丟，而那個路徑在 %APPDATA%
  // 底下，只把網頁打開等於把最後一哩留給使用者自己猜。
  QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
  return true;
}

}  // namespace l2m
