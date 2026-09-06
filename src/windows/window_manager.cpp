#include "window_manager.h"

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>

#include <algorithm>
#include <cmath>

#include "../platform/window_effects.h"
#include "core/topmost_watchdog.h"

namespace l2m {

namespace {

// 位置 preset 與螢幕邊緣保留的距離
constexpr int kEdgeMargin = 24;

}  // namespace

QSize sizeForScale(double scale) { return QSize(static_cast<int>(std::lround(kBaseWidth * scale)), static_cast<int>(std::lround(kBaseHeight * scale))); }

WindowManager::WindowManager(QWindow& window, ConfigStore& config, QObject* parent) : QObject(parent), window_(window), config_(config) {
  const AppConfig& cfg = config_.get();
  setOpacity(cfg.model.opacity);
  applyBounds();

  // 置頂看門狗。掛在這裡而不是等某個事件：把桌寵擠出置頂層的來源不只一個
  //（鎖定螢幕、顯示模式變更、別的 app 進全螢幕、程式沒回應被換成 ghost window，
  // 還有「開一次小畫家」這種毫無道理的），一個一個去接會漏，
  // 而且有些根本沒有可以接的通知。
  topmostTimer_.setInterval(kTopmostCheckIntervalMs);
  connect(&topmostTimer_, &QTimer::timeout, this, &WindowManager::reassertTopmostIfNeeded);
  topmostTimer_.start();
}

void WindowManager::reassertTopmostIfNeeded() {
  const bool nativeTopmost = platform::isTopmost(&window_);
  const bool covered = platform::isCoveredByNormalWindow(&window_);
  if (!shouldReassertTopmost(config_.get().app.alwaysOnTop, nativeTopmost, window_.isVisible(), covered)) {
    return;
  }
  // 一次事件只會印一行：宣告回去之後兩個條件都不成立，下一輪不會再進來。
  // 兩種症狀分開印 —— 「旗標還在卻被壓住」那條是後來才發現的，
  // 現場回報時要看得出踩到的是哪一種（見 core/topmost_watchdog.h）
  qDebug() << "[window] 置頂掉了，重新宣告：" << (nativeTopmost ? "旗標還在但被壓在一般視窗底下" : "旗標被系統拔掉");
  platform::applyTopmost(&window_, true);
}

QRect WindowManager::workArea(const QPoint* nearPoint) const {
  const QPoint point = nearPoint ? *nearPoint : QCursor::pos();
  QScreen* screen = QGuiApplication::screenAt(point);
  if (!screen) screen = QGuiApplication::primaryScreen();
  return screen->availableGeometry();
}

void WindowManager::applyBounds() {
  const AppConfig& cfg = config_.get();
  const QSize size = sizeForScale(cfg.model.scale);
  const QPoint origin = resolveOrigin(size.width(), size.height());
  setBoundsSafely(QRect(origin, size));
}

// 依設定推導視窗左上角座標：明確座標優先，否則用 preset
QPoint WindowManager::resolveOrigin(int width, int height) const {
  const AppConfig& cfg = config_.get();
  if (cfg.model.x.has_value() && cfg.model.y.has_value()) {
    return clampPosition(static_cast<int>(*cfg.model.x), static_cast<int>(*cfg.model.y), width, height);
  }
  return originForPreset(cfg.model.preset, width, height);
}

QPoint WindowManager::originForPreset(const std::string& preset, int width, int height) const {
  const QRect area = workArea();
  const int left = area.x() + kEdgeMargin;
  const int right = area.x() + area.width() - width - kEdgeMargin;
  const int top = area.y() + kEdgeMargin;
  const int bottom = area.y() + area.height() - height - kEdgeMargin;

  if (preset == "bottom-left") return QPoint(left, bottom);
  if (preset == "top-right") return QPoint(right, top);
  if (preset == "top-left") return QPoint(left, top);
  if (preset == "center") {
    return QPoint(area.x() + (area.width() - width) / 2, area.y() + (area.height() - height) / 2);
  }
  return QPoint(right, bottom);  // bottom-right（預設）
}

// 把視窗夾在所在螢幕的工作區內，至少留一部分可見
QPoint WindowManager::clampPosition(int x, int y, int width, int height) const {
  const QPoint point(x, y);
  const QRect area = workArea(&point);
  const int cx = static_cast<int>(std::lround(std::min(std::max<double>(x, area.x() - width / 3.0), area.x() + area.width() - width * 2.0 / 3.0)));
  const int cy = static_cast<int>(std::lround(std::min(std::max<double>(y, area.y()), area.y() + area.height() - height / 3.0)));
  return QPoint(cx, cy);
}

void WindowManager::setBoundsSafely(const QRect& bounds) {
  window_.setGeometry(bounds);
  emit boundsChanged(bounds);
}

QPoint WindowManager::moveBy(int dx, int dy) {
  const QRect b = window_.geometry();
  const QPoint p = clampPosition(b.x() + dx, b.y() + dy, b.width(), b.height());
  setBoundsSafely(QRect(p, b.size()));
  config_.patch(QStringLiteral("{\"model\":{\"x\":%1,\"y\":%2}}").arg(p.x()).arg(p.y()).toStdString());
  return p - b.topLeft();
}

QPoint WindowManager::moveTo(int x, int y) {
  const QRect b = window_.geometry();
  const QPoint p = clampPosition(x, y, b.width(), b.height());
  setBoundsSafely(QRect(p, b.size()));
  config_.patch(QStringLiteral("{\"model\":{\"x\":%1,\"y\":%2}}").arg(p.x()).arg(p.y()).toStdString());
  return p;
}

QPoint WindowManager::moveToRatio(double rx, double ry) {
  const QRect b = window_.geometry();
  const QPoint topLeft = b.topLeft();
  const QRect area = workArea(&topLeft);
  const double cx = std::clamp(rx, 0.0, 1.0);
  const double cy = std::clamp(ry, 0.0, 1.0);
  const int x = static_cast<int>(std::lround(area.x() + (area.width() - b.width()) * cx));
  const int y = static_cast<int>(std::lround(area.y() + (area.height() - b.height()) * cy));
  return moveTo(x, y);
}

QPoint WindowManager::moveToPreset(const std::string& preset) {
  const QRect b = window_.geometry();
  const QPoint origin = originForPreset(preset, b.width(), b.height());
  setBoundsSafely(QRect(origin, b.size()));
  config_.patch(QStringLiteral("{\"model\":{\"preset\":\"%1\",\"x\":%2,\"y\":%3}}").arg(QString::fromStdString(preset)).arg(origin.x()).arg(origin.y()).toStdString());
  return origin;
}

void WindowManager::applyScale(double scale) {
  const QRect before = window_.geometry();
  const QSize size = sizeForScale(scale);
  // 錨定底部中央
  const int anchorX = before.x() + before.width() / 2;
  const int anchorBottom = before.y() + before.height();
  const QPoint p = clampPosition(anchorX - size.width() / 2, anchorBottom - size.height(), size.width(), size.height());
  setBoundsSafely(QRect(p, size));
  config_.patch(QStringLiteral("{\"model\":{\"scale\":%1,\"x\":%2,\"y\":%3}}").arg(scale).arg(p.x()).arg(p.y()).toStdString());
}

void WindowManager::setOpacity(double opacity) {
  window_.setOpacity(opacity);
  // Qt 對透明 GL 視窗自己掛的 layered style 會造成半透明區抖色，
  // 整窗不透明度為 1 時拔掉（<1 時 Qt 需要 layered 來做整窗透明度）
  if (opacity >= 0.999) platform::stripLayeredStyle(&window_);
}

void WindowManager::setAlwaysOnTop(bool enabled) {
  window_.setFlag(Qt::WindowStaysOnTopHint, enabled);
  // setFlag 不能單獨信：Qt 在 flag 跟快取一樣時直接 return，而系統把
  // WS_EX_TOPMOST 拔掉之後 Qt 的快取仍然是「置頂中」—— 那時候使用者從設定頁
  // 或 MCP 再按一次「永遠置頂」會完全沒有反應（實測確認）。原生這一刀一定要補。
  platform::applyTopmost(&window_, enabled);
  // 改 flag 可能讓 Qt 重套 layered style，再拔一次
  if (config_.get().model.opacity >= 0.999) platform::stripLayeredStyle(&window_);
  // 同理，重建原生視窗會讓 NSPanel 的 hidesOnDeactivate 回到預設值
  platform::keepVisibleWhenAppInactive(&window_);
}

void WindowManager::setVisible(bool visible) {
  window_.setVisible(visible);
  // 每次顯示都要套：桌寵長期處於非作用中，NSPanel 的預設會把它收起來再也回不來
  if (visible) platform::keepVisibleWhenAppInactive(&window_);
  if (visible && config_.get().model.opacity >= 0.999) platform::stripLayeredStyle(&window_);
  // 隱藏期間掉的置頂看門狗刻意不管（window_.isVisible() 為 false），
  // 所以重新現身的當下要自己補一次，不必等下一個 tick
  if (visible && config_.get().app.alwaysOnTop) platform::applyTopmost(&window_, true);
  emit visibleChanged(visible);
}

}  // namespace l2m
