#include "about_page.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFont>
#include <QFormLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>

#include "../../app/app_controller.h"
#include "core/i18n.h"
#include "ui_about_page.h"

namespace l2m {

namespace {

#ifndef L2M_CUBISM_VERSION
#define L2M_CUBISM_VERSION "unknown"
#endif

// 著作權與授權：跟 repo 根目錄的 LICENSE、README.md 是同一份事實，改這裡要一起改那兩份。
// 刻意不走 i18n —— 工作室名稱與授權名稱在任何語系都不翻譯，翻了反而對不上 LICENSE 檔。
constexpr char kCopyrightHolder[] = "© 2026 RookieStudio";
constexpr char kLicenseName[] = "MIT License";

// 右欄一律可選取：路徑很長，使用者要能直接選起來複製貼進問題回報
QLabel* valueLabel(QWidget* parent, const QString& text) {
  auto* label = new QLabel(text, parent);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  label->setWordWrap(true);
  return label;
}

}  // namespace

AboutPage::AboutPage(const SettingsContext& context, QWidget* parent) : QWidget(parent), ctx_(context), ui_(std::make_unique<Ui::AboutPage>()) {
  ui_->setupUi(this);

  QFont nameFont = ui_->appName->font();
  nameFont.setPointSize(nameFont.pointSize() + 6);
  nameFont.setBold(true);
  ui_->appName->setFont(nameFont);

  // 第三方聲明是法務性質的小字，壓小一級並用停用色，才不會跟版本號搶視線
  QFont noticeFont = ui_->notice->font();
  noticeFont.setPointSize(qMax(1, noticeFont.pointSize() - 1));
  ui_->notice->setFont(noticeFont);
  QPalette noticePalette = ui_->notice->palette();
  noticePalette.setColor(QPalette::WindowText, QApplication::palette().color(QPalette::Disabled, QPalette::WindowText));
  ui_->notice->setPalette(noticePalette);

  connect(ui_->openLogsButton, &QPushButton::clicked, this, [this] {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/logs");
    // openUrl 失敗時畫面一點反應都沒有（沒有預設瀏覽器、或 shell 關聯被改壞），
    // 退成「複製路徑 + 狀態列說明」才不會變成無聲失敗。
    // **複製出去的那一份要過 toNativeSeparators**：Qt 內部一律用 '/'，
    // 而使用者拿這串字要貼進檔案總管的位址列或 shell —— 那裡認的是反斜線
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) return;
    QApplication::clipboard()->setText(QDir::toNativeSeparators(dir));
    ctx_.setStatus(tr2("settings.about.openLogsCopied"));
  });
}

AboutPage::~AboutPage() = default;

QString AboutPage::tr2(const char* key) const { return QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), key)); }

void AboutPage::retranslate() {
  ui_->tagline->setText(tr2("settings.about.tagline"));
  ui_->notice->setText(tr2("settings.about.notice"));
  ui_->openLogsButton->setText(tr2("settings.about.openLogs"));

  const std::string locale = ctx_.controller->uiLocale();
  QStringList lines;
  lines << QString::fromStdString(i18n::translate(locale, "settings.about.version", i18n::TParams().arg("version", QApplication::applicationVersion().toStdString())));
  // 編譯期與執行期的 Qt 版本分開列：不同就代表機器上放了另一版 Qt DLL
  lines << QString::fromStdString(i18n::translate(locale, "settings.about.qtVersion", i18n::TParams().arg("build", QT_VERSION_STR).arg("runtime", qVersion())));
  lines << QString::fromStdString(i18n::translate(locale, "settings.about.cubismVersion", i18n::TParams().arg("version", L2M_CUBISM_VERSION)));
  ui_->versions->setText(lines.join(QStringLiteral("\n")));

  refresh();
}

void AboutPage::refresh() {
  // 整份重建：語系換了左欄標籤要跟著換，而 QFormLayout 沒有「只換標籤」的 API
  while (ui_->infoForm->rowCount() > 0) ui_->infoForm->removeRow(0);

  // 著作權與授權排在路徑之前：這兩列是不會變的事實，路徑才是每台機器不同的診斷資料
  ui_->infoForm->addRow(new QLabel(tr2("settings.about.copyright"), this), valueLabel(this, QString::fromUtf8(kCopyrightHolder)));
  ui_->infoForm->addRow(new QLabel(tr2("settings.about.license"), this), valueLabel(this, QString::fromUtf8(kLicenseName)));
  ui_->infoForm->addRow(new QLabel(tr2("settings.about.configPath"), this), valueLabel(this, QString::fromStdString(ctx_.controller->config().path().u8string())));
  ui_->infoForm->addRow(new QLabel(tr2("settings.about.modelsPath"), this), valueLabel(this, QString::fromStdString(ctx_.controller->modelsDir().u8string())));
}

}  // namespace l2m
