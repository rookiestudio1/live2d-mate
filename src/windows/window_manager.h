#pragma once

// 角色視窗的位置、大小、透明度管理。
//
// 所有位置與大小的變更都集中在 setBoundsSafely()，避免多處各自改 geometry
// 造成位置與設定不同步。

#include <QObject>
#include <QRect>
#include <QTimer>
#include <QWindow>

#include "core/config_store.h"

namespace l2m {

// scale = 1 時的視窗尺寸；模型會被縮放到填滿這個舞台
inline constexpr int kBaseWidth = 400;
inline constexpr int kBaseHeight = 600;

QSize sizeForScale(double scale);

class WindowManager : public QObject {
  Q_OBJECT

public:
  WindowManager(QWindow& window, ConfigStore& config, QObject* parent = nullptr);

  // 依目前設定重新計算視窗大小與位置並套用
  void applyBounds();

  // 拖曳：以位移量移動視窗，並把新位置寫回設定。
  // 回傳夾限後「實際套用」的位移 —— 貼螢幕邊時會小於（甚至異於）要求值，
  // 拖曳搖晃要吃這個值，視窗沒動就不該甩頭髮。
  QPoint moveBy(int dx, int dy);

  // 移到指定螢幕座標（視窗左上角）；回傳夾限後的實際位置
  QPoint moveTo(int x, int y);

  // 以工作區比例移動：0 = 貼左/上，1 = 貼右/下
  QPoint moveToRatio(double rx, double ry);

  QPoint moveToPreset(const std::string& preset);

  // 縮放時錨定「底部中央」，角色不會因為變大而跳走
  void applyScale(double scale);

  void setOpacity(double opacity);
  void setAlwaysOnTop(bool enabled);

  void setVisible(bool visible);
  bool isVisible() const { return window_.isVisible(); }

  QRect bounds() const { return window_.geometry(); }

signals:
  // 位置／大小變更（氣泡視窗之後靠它跟著走）
  void boundsChanged(const QRect& bounds);
  void visibleChanged(bool visible);

private:
  // 系統偶爾會把桌寵擠出置頂層（鎖定螢幕最常見），而 Qt 的 setFlag 打不回去。
  // 兩種症狀都要問：旗標被拔掉，以及旗標還在卻已經被排到一般視窗底下
  //（後者是開小畫家踩到的）。低頻檢查原生實況，掉了才宣告回來。
  // 實測數字與兩個症狀的細節見 core/topmost_watchdog.h。
  void reassertTopmostIfNeeded();

  QRect workArea(const QPoint* nearPoint = nullptr) const;
  QPoint resolveOrigin(int width, int height) const;
  QPoint originForPreset(const std::string& preset, int width, int height) const;
  QPoint clampPosition(int x, int y, int width, int height) const;
  void setBoundsSafely(const QRect& bounds);

  QWindow& window_;
  ConfigStore& config_;
  QTimer topmostTimer_;
};

}  // namespace l2m
