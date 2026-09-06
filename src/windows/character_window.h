#pragma once

#include <QElapsedTimer>
#include <QOpenGLWindow>
#include <QRegion>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "core/fade_timing.h"

namespace l2m {
class AlphaHitMask;
class ModelController;
}  // namespace l2m

// 角色視窗：透明、無邊框、置頂、不進工作列的 OpenGL 畫布。
//
// 採用 QOpenGLWindow 而非 QOpenGLWidget：後者作為「頂層透明視窗」時
// 在 Windows 上內容無法合成到螢幕（FBO 有內容但螢幕看不到，M0 實測確認）。
//
// 只負責渲染與滑鼠事件的第一手處理；位置／設定的決策全部回呼給 AppController。
class CharacterWindow : public QOpenGLWindow {
  Q_OBJECT

public:
  CharacterWindow();
  ~CharacterWindow() override;

  // 載入（或切換）模型。GL 尚未初始化時先記著，初始化完成後自動載入。
  void requestModelLoad(const QString& entryPath);

  l2m::ModelController* modelController() { return controller_.get(); }

  // 最近一次載入失敗的原因與建議（英文，見 live2d/model_controller.h）。
  // 失敗的 ModelController 在 loadPendingModel() 結束時就被丟掉了，所以字串
  // 在那裡抄一份 —— 設定視窗要等 modelSwitchFinished 才有機會顯示，那時已經
  // 沒有物件可問。成功的載入不清空：只有失敗路徑會去讀它。
  const std::string& lastLoadError() const { return lastLoadError_; }
  const std::string& lastLoadHint() const { return lastLoadHint_; }

  // 開始淡入（visible=true）或淡出（false），時長為 core/fade_timing.h 的
  // kFadeDurationMs。中途反轉會從當下的透明度接回去，不會跳。
  //
  // 這裡只管畫面。「淡完之後要做什麼」（真的 hide、開始載入模型、結束程式）
  // 由呼叫端自己用同一個常數排 QTimer —— 視窗隱藏或模型還沒載好時根本沒有
  // frame 送出，改成等一個「淡完了」的訊號會永遠等不到，callback 整個掉。
  void beginFade(bool visible);

  // 視窗內像素座標是否踩在角色不透明區上（alpha 命中遮罩）
  bool isOpaqueAt(const QPointF& localPos) const;

  // 模型視覺底部的 normalized Y（0=視窗頂，1=視窗底），切換模型的腳底對齊用。
  // 遮罩未就緒或全透明回 nullopt。
  std::optional<double> modelBottomNormalized() const;

  // 模型視覺頂端的 normalized Y（0=視窗頂，1=視窗底），氣泡錨點用。
  // 遮罩未就緒或全透明回 nullopt。
  std::optional<double> modelTopNormalized() const;

  // 設定視線焦點（全域游標輪詢呼叫；座標為視窗內像素，可在視窗外）
  void setFocusFromLocal(const QPointF& localPos);

  // ── AppController 注入的互動回呼與開關 ──
  std::function<void(int dx, int dy)> onDragBy;
  std::function<void(int direction)> onWheelZoom;
  std::function<void()> onActivity;
  // 指標按下／放開（視窗內座標）。手勢偵測（單擊、連點、長按）吃這個 ——
  // 這裡只轉發純資料，判定全在 core/gesture_detector.h。
  // 點擊動作的播放也走這條：單擊要等 OS 雙擊間隔遞延判定，
  // 所以放開時不能直接播（AppController::onGesture 收 Tap 才播）。
  std::function<void(bool pressed, QPointF localPos)> onPointerEvent;
  std::function<bool()> dragEnabled;
  std::function<bool()> wheelZoomEnabled;
  // 點擊穿透（形狀視窗）開關；開啟時視窗形狀跟著 alpha 遮罩走
  std::function<bool()> clickThroughEnabled;

  // 游標是否在視窗範圍內（全域游標輪詢回報）。
  // 形狀只在游標靠近時才套用：遠離時整窗不裁切，畫面永遠完整；
  // 靠近時形狀決定點擊路由，並逐幀更新遮罩讓形狀跟緊動畫。
  void setRegionGate(bool cursorInside) { regionGate_ = cursorInside; }

signals:
  void modelLoaded(bool ok);
  // 命中遮罩收到新的一份回讀（version 變了）。腳底對齊等的是這個而不是
  // firstFrameRendered —— 底部量測要的是「遮罩落地」，比第一幀還晚 1~2 幀。
  void maskUpdated();
  // 載入後的第一幀真的送上螢幕了（swap 完成）。
  // 啟動畫面等的是這個而不是 modelLoaded —— 後者只代表 GL 資源準備好，
  // 此時畫面上還是空的，splash 一走就會露出一瞬間的空白。
  void firstFrameRendered();

protected:
  void initializeGL() override;
  void paintGL() override;

  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

private:
  void loadPendingModel();

  std::unique_ptr<l2m::ModelController> controller_;
  std::unique_ptr<l2m::AlphaHitMask> hitMask_;
  QString pendingModelEntry_;
  std::string lastLoadError_;
  std::string lastLoadHint_;
  bool glReady_ = false;
  // 第一幀的兩段式追蹤：載入成功後 awaiting=true，paintGL 畫完模型改成
  // pendingSwap=true，等 frameSwapped 才發訊號（那時畫面才真的在螢幕上）
  bool awaitingFirstFrame_ = false;
  bool firstFramePendingSwap_ = false;
  int lastRenderTargetW_ = 0;
  int lastRenderTargetH_ = 0;
  int lastRegionVersion_ = -1;
  int lastNotifiedMaskVersion_ = -1;  // maskUpdated 的去重（每幀整數比較）
  QRegion lastRegion_;                // 形狀沒變就不必再呼叫 SetWindowRgn（會讓 DWM 重算可見區）
  bool regionApplied_ = false;
  bool regionGate_ = false;
  QElapsedTimer frameClock_;
  QElapsedTimer runClock_;

  // 淡入淡出：曲線是 core/fade_timing.h 的純邏輯，時間取自 runClock_
  l2m::Fade fade_;
  // beginFade() 到下一次 paintGL 之間可能隔了幾十毫秒（視窗剛 show、或模型
  // 剛同步載入完那一下）。時鐘要在真的開始畫的那一幀才起算，
  // 否則動畫一開場就被吃掉一段。nullopt = 沒有待起算的淡化。
  std::optional<bool> pendingFade_;

  // 拖曳追蹤；位移小於門檻時視為點擊
  QPoint lastScreenPos_;
  QPoint pressPos_;
  double dragMoved_ = 0;
  bool dragging_ = false;
};
