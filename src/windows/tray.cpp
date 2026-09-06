#include "tray.h"

#include <QApplication>
#include <QMessageBox>

#include <cmath>
#include <functional>
#include <map>

#include "../app/app_controller.h"
#include "core/config_patch.h"
#include "core/config_schema.h"
#include "core/i18n.h"

namespace l2m {

namespace {

const double kScaleChoices[] = {0.5, 0.75, 1, 1.25, 1.5, 2};
const double kOpacityChoices[] = {1, 0.9, 0.8, 0.7, 0.5};

const std::map<std::string, const char*>& presetKeys() {
  static const std::map<std::string, const char*> keys{
    {"bottom-right", "tray.position.bottomRight"}, {"bottom-left", "tray.position.bottomLeft"}, {"top-right", "tray.position.topRight"},
    {"top-left", "tray.position.topLeft"},         {"center", "tray.position.center"},
  };
  return keys;
}

}  // namespace

Tray::Tray(AppController& controller, Deps deps, QObject* parent) : QObject(parent), controller_(controller), deps_(std::move(deps)) {
#ifdef Q_OS_MAC
  QIcon icon(QStringLiteral(":/icons/trayTemplate.png"));
  icon.setIsMask(true);
#else
  QIcon icon(QStringLiteral(":/icons/tray.png"));
#endif
  icon_.setIcon(icon);

  rebuildTimer_.setSingleShot(true);
  rebuildTimer_.setInterval(150);
  connect(&rebuildTimer_, &QTimer::timeout, this, &Tray::rebuild);

  // 只剩大小／透明度／位置會被別處改動（滾輪縮放、拖曳、MCP 工具），
  // 這些全都走 stateChanged。模型清單已經不在選單上了，不必接 modelsChanged。
  connect(&controller_, &AppController::stateChanged, this, &Tray::scheduleRebuild);

  // Windows 慣例：左鍵單擊也開選單
  connect(&icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger && menu_) menu_->popup(QCursor::pos());
  });

  rebuild();
  updateToolTip();
  icon_.show();
}

QString Tray::tr2(const char* key) const { return QString::fromStdString(i18n::translate(controller_.uiLocale(), key)); }

void Tray::run(const CommandResult& result) {
  if (result.ok) return;
  QMessageBox box(QMessageBox::Warning, tr2("dialog.actionFailed.title"), QString::fromStdString(result.error));
  if (!result.hint.empty()) box.setInformativeText(QString::fromStdString(result.hint));
  box.exec();
}

void Tray::updateToolTip() {
  const bool enabled = deps_.mcpEnabled && deps_.mcpEnabled();
  if (!enabled) {
    icon_.setToolTip(tr2("tray.tooltip.mcpDisabled"));
    return;
  }
  const bool running = deps_.mcpRunning && deps_.mcpRunning();
  if (!running) {
    icon_.setToolTip(tr2("tray.tooltip.mcpStopped"));
    return;
  }
  const std::string url = deps_.mcpUrl ? deps_.mcpUrl() : std::string();
  icon_.setToolTip(QString::fromStdString(i18n::translate(controller_.uiLocale(), "tray.tooltip.mcpRunning", i18n::TParams().arg("url", url))));
}

void Tray::scheduleRebuild() {
  if (!rebuildTimer_.isActive()) rebuildTimer_.start();
}

void Tray::rebuild() {
  // 選單開在畫面上時不能整份汰換 —— menu_ 一換，舊選單當場解構，
  // 使用者眼前的選單就直接消失（閒置隨機動作的 stateChanged 最常踩到）。
  // 記下來，等它收起來再重建。
  if (menu_ && menu_->isVisible()) {
    rebuildPending_ = true;
    return;
  }
  rebuildPending_ = false;

  auto menu = std::make_unique<QMenu>();

  QAction* title = menu->addAction(QStringLiteral("Live2D Mate"));
  title->setEnabled(false);
  menu->addSeparator();

  buildScaleMenu(menu->addMenu(tr2("tray.scale")));
  buildOpacityMenu(menu->addMenu(tr2("tray.opacity")));
  buildPositionMenu(menu->addMenu(tr2("tray.position")));
  menu->addSeparator();
  menu->addAction(tr2("tray.settings"), [this] {
    if (deps_.openSettings) deps_.openSettings(SettingsTab::General);
  });
  menu->addSeparator();
  menu->addAction(tr2("tray.quit"), [this] {
    if (deps_.quit) deps_.quit();
  });

  // 開著的期間被要求重建的話，收起來後補做。不在 aboutToHide 裡就地
  // rebuild：那一刻選單還在自己的事件處理途中（hide → 才輪到 triggered），
  // 就地汰換等於解構正在發訊號的物件，交給節流計時器等事件跑完再換。
  connect(menu.get(), &QMenu::aboutToHide, this, [this] {
    if (rebuildPending_) scheduleRebuild();
  });

  icon_.setContextMenu(menu.get());
  menu_ = std::move(menu);  // 舊選單隨 unique_ptr 汰換釋放
}

void Tray::notify(const QString& title, const QString& body, std::function<void()> onClicked) {
  // 每次都先斷開上一個：messageClicked 是 icon_ 的訊號，不斷開的話
  // 第二次通知被點到時會把前一次的 callback 也叫一遍
  disconnect(&icon_, &QSystemTrayIcon::messageClicked, nullptr, nullptr);
  if (onClicked) {
    connect(&icon_, &QSystemTrayIcon::messageClicked, this, [handler = std::move(onClicked)] { handler(); });
  }
  icon_.showMessage(title, body, QSystemTrayIcon::Warning, 10000);
}

// ── 外觀與位置 ──────────────────────────────────────────

void Tray::buildScaleMenu(QMenu* menu) {
  const AppConfig& cfg = controller_.config().get();
  for (const double scale : kScaleChoices) {
    QAction* action = menu->addAction(QStringLiteral("%1%").arg(qRound(scale * 100)));
    action->setCheckable(true);
    action->setChecked(std::abs(cfg.model.scale - scale) < 0.001);
    connect(action, &QAction::triggered, this, [this, scale] { run(controller_.setScale(scale)); });
  }
}

void Tray::buildOpacityMenu(QMenu* menu) {
  const AppConfig& cfg = controller_.config().get();
  for (const double opacity : kOpacityChoices) {
    QAction* action = menu->addAction(QStringLiteral("%1%").arg(qRound(opacity * 100)));
    action->setCheckable(true);
    action->setChecked(std::abs(cfg.model.opacity - opacity) < 0.001);
    connect(action, &QAction::triggered, this, [this, opacity] { run(controller_.setOpacity(opacity)); });
  }
}

void Tray::buildPositionMenu(QMenu* menu) {
  const AppConfig& cfg = controller_.config().get();

  for (const auto& preset : positionPresets()) {
    const auto keyIt = presetKeys().find(preset);
    QAction* action = menu->addAction(tr2(keyIt->second));
    connect(action, &QAction::triggered, this, [this, preset] { run(controller_.moveToPreset(preset)); });
  }

  menu->addSeparator();
  QAction* lock = menu->addAction(tr2("tray.position.lock"));
  lock->setCheckable(true);
  lock->setChecked(cfg.interaction.lockPosition);
  connect(lock, &QAction::triggered, this, [this](bool value) {
    controller_.config().patch(boolPatch("interaction", "lockPosition", value));
    controller_.applyConfig();
  });
}

}  // namespace l2m
