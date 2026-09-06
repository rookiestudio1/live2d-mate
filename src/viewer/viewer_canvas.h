#pragma once

// Live2D Viewer 的 OpenGL 畫布。
//
// 與桌寵的 CharacterWindow（src/windows/character_window.h）相比，能共用的全部共用：
// 同一個 ModelController、同一支 update()（待機動作、呼吸、眨眼、物理都在裡面）、
// 同一套視線跟隨、同一份 ParameterOverlay。差別只有下面兩處。
//
// **一、這裡是 QOpenGLWidget 而不是 QOpenGLWindow。**
// CharacterWindow 用後者是因為 QOpenGLWidget 當「頂層透明視窗」時在 Windows 上
// 內容合成不到螢幕（M0 實測）。那個限制對「不透明的嵌入式子控制項」不成立，
// 而 Viewer 要把畫布塞進 QSplitter 的版面裡，QOpenGLWidget 才是對的工具
// （QOpenGLWindow 得再包一層 createWindowContainer 的原生子視窗）。
// 唯一要確認的是 FBO：QOpenGLWidget 畫的是自己的離屏 FBO 而不是 0 號。
// Cubism 5-r.5 的 CubismRenderer_OpenGLES2::DoDrawModel 開頭就
// glGetIntegerv(GL_FRAMEBUFFER_BINDING) 存下當前的 FBO、結束時還原，
// 所以剪裁遮罩那幾次切換不會把我們的 FBO 弄丟。
//
// **二、沒有命中遮罩、沒有淡入淡出、沒有視窗形狀。**
// 那三件事都是「貼在桌面上的角色」才需要的（穿透點擊、切模型的腳底對齊），
// Viewer 是一塊有邊界的畫布，一件都用不到 —— 順帶也省掉每幀的 PBO 回讀。
//
// **「模型大小隨視窗縮放」不必寫任何程式碼**：ModelController::draw() 每幀
// 依 viewport 的長寬比重算投影（見那支函式的註解），視窗一放大模型就跟著放大。
//
// 重繪節奏用 QTimer 而不是 frameSwapped：QOpenGLWidget 的 swap 由頂層視窗的
// backing store 負責，不保證跟著 vsync，接 frameSwapped 會空轉滿一顆核心。

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QPointF>
#include <QString>

#include <memory>
#include <string>

namespace l2m {
class ModelController;
class ParameterOverlay;
}  // namespace l2m

class ViewerCanvas : public QOpenGLWidget {
  Q_OBJECT

public:
  explicit ViewerCanvas(QWidget* parent = nullptr);
  ~ViewerCanvas() override;

  // 載入（或切換）模型。GL 還沒初始化時先記著，initializeGL 之後自動載入。
  void requestModelLoad(const QString& entryPath);

  l2m::ModelController* modelController() { return controller_.get(); }

  // 最近一次載入失敗的原因（英文，見 live2d/model_controller.h）。失敗的
  // ModelController 是 loadPendingModel() 的本地 unique_ptr，出了那個函式就沒了，
  // 所以在那裡抄一份給狀態列用。
  const std::string& lastLoadError() const { return lastLoadError_; }
  l2m::ParameterOverlay& overlay() { return *overlay_; }

signals:
  // 載入結果。GL 尚未就緒時會延後到 initializeGL 才發出。
  void modelLoaded(bool ok);

  // 在模型上點了一下（畫布內座標）。挑哪一段動作來播是 ViewerWindow 的事
  // —— 那邊才有 model_（動作清單）與「自動重播」。
  //
  // 這裡刻意不做遞延判定（桌寵那條要等 OS 雙擊間隔，見 core/gesture_detector.h）：
  // 檢視器的畫布沒有拖曳搬視窗、沒有連點反應，一放開就成立最直覺。
  void modelTapped(const QPointF& localPos);

protected:
  void initializeGL() override;
  void paintGL() override;

  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;

private:
  void loadPendingModel();
  // 視線焦點；座標為畫布內像素（映射成視窗邊緣＝滿偏，同 CharacterWindow）
  void setFocusFromLocal(const QPointF& localPos);

  std::unique_ptr<l2m::ModelController> controller_;
  std::unique_ptr<l2m::ParameterOverlay> overlay_;
  QString pendingModelEntry_;
  std::string lastLoadError_;
  bool glReady_ = false;
  int lastRenderTargetW_ = 0;
  int lastRenderTargetH_ = 0;
  // 按下的位置：放開時位移超過 kTapThresholdPx 就不算點擊
  // （在畫布上劃一下是想轉視線，不是想戳角色）
  QPointF pressPos_;
  bool pressing_ = false;
  QElapsedTimer frameClock_;
};
