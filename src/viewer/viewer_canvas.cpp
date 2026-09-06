// glew 必須在任何會引入 gl.h 的標頭（含 Qt 的 OpenGL 標頭）之前
#include <GL/glew.h>

#include "viewer_canvas.h"

#include <QDebug>
#include <QLineF>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <filesystem>

#include "core/interaction_logic.h"
#include "live2d/cubism_runtime.h"
#include "live2d/model_controller.h"
#include "live2d/parameter_overlay.h"

namespace {

// 重繪間隔。60 FPS 對桌寵等級的模型綽綽有餘，而且不必依賴 vsync
// （QOpenGLWidget 的 swap 走頂層 backing store，swapInterval 不一定生效）。
constexpr int kFrameIntervalMs = 16;

// 畫布底色。刻意是不透明的深灰而不是透明 —— 這裡不是桌寵，
// 有一塊看得出邊界的畫布才知道模型有沒有被裁到。
constexpr float kBackground[3] = {0.13f, 0.14f, 0.16f};

}  // namespace

ViewerCanvas::ViewerCanvas(QWidget* parent) : QOpenGLWidget(parent) {
  overlay_ = std::make_unique<l2m::ParameterOverlay>();
  // 視線要跟著游標，所以沒按著按鍵時也要收到 move 事件
  setMouseTracking(true);
  setMinimumSize(240, 320);

  auto* timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
  timer->start(kFrameIntervalMs);
}

ViewerCanvas::~ViewerCanvas() {
  // GL 資源需在 context 有效時釋放
  makeCurrent();
  overlay_->detach();
  controller_.reset();
  doneCurrent();
}

void ViewerCanvas::requestModelLoad(const QString& entryPath) {
  pendingModelEntry_ = entryPath;
  if (!glReady_) return;
  makeCurrent();
  loadPendingModel();
  doneCurrent();
  update();
}

void ViewerCanvas::loadPendingModel() {
  if (pendingModelEntry_.isEmpty()) return;
  const QString entry = pendingModelEntry_;
  pendingModelEntry_.clear();

  // 舊模型的覆寫層一定要先斷開：它記著上一隻模型的參數索引，
  // 沿用到新模型會寫到完全不相干的部位上。
  overlay_->detach();
  controller_.reset();
  lastRenderTargetW_ = 0;  // 新 renderer 需重新同步離屏尺寸
  lastRenderTargetH_ = 0;

  auto next = std::make_unique<l2m::ModelController>();
  const bool ok = next->load(std::filesystem::u8path(entry.toStdString()));
  if (ok) {
    controller_ = std::move(next);
    overlay_->attach(controller_.get());
    controller_->earlyParameterHook = [this](Csm::CubismModel* model) { overlay_->applyEarly(model); };
    controller_->lateParameterHook = [this](Csm::CubismModel* model) { overlay_->applyLate(model); };
    qInfo() << "[live2d] 模型載入完成:" << entry;
  } else {
    lastLoadError_ = next->loadError();
    qWarning() << "[live2d] 模型載入失敗:" << entry;
    // noquote：句子裡本來就有引號（格式名），再讓 QDebug 包一層會變成 \" 的跳脫地獄
    qWarning().noquote() << "[live2d]" << QString::fromStdString(lastLoadError_);
    qWarning().noquote() << "[live2d]" << QString::fromStdString(next->loadHint());
  }
  emit modelLoaded(ok);
}

void ViewerCanvas::initializeGL() {
  // Framework 的 GL renderer 用 glew 取函式指標
  glewExperimental = GL_TRUE;
  const GLenum glewResult = glewInit();
  if (glewResult != GLEW_OK) {
    qWarning() << "[live2d] glewInit 失敗:" << reinterpret_cast<const char*>(glewGetErrorString(glewResult));
  }
  glGetError();  // 清掉 glewInit 可能留下的無害錯誤

  l2m::CubismRuntime::initialize();

  glReady_ = true;
  loadPendingModel();
  frameClock_.start();
}

void ViewerCanvas::paintGL() {
  glClearColor(kBackground[0], kBackground[1], kBackground[2], 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  if (!controller_) return;

  // 半透明漸層在某些驅動上會被 dithering 弄出棋盤紋（GL 預設開啟）
  glDisable(GL_DITHER);

  // 上限 0.1 秒：視窗被拖著不放或剛從最小化回來時，一大格 dt 會讓動作瞬移
  const float dt = std::min(frameClock_.restart() / 1000.0f, 0.1f);
  controller_->update(dt);

  const qreal ratio = devicePixelRatio();
  const int vw = static_cast<int>(width() * ratio);
  const int vh = static_cast<int>(height() * ratio);
  glViewport(0, 0, vw, vh);
  // 離屏 buffer 跟畫布同尺寸，混合模式的離屏合成才不會有縮放紋
  if (vw != lastRenderTargetW_ || vh != lastRenderTargetH_) {
    controller_->setRenderTargetSize(vw, vh);
    lastRenderTargetW_ = vw;
    lastRenderTargetH_ = vh;
  }
  controller_->draw(vw, vh);
}

void ViewerCanvas::setFocusFromLocal(const QPointF& localPos) {
  if (!controller_) return;
  // 映射規則與 CharacterWindow::setFocusFromLocal 相同：畫布邊緣＝滿偏。
  // 刻意不走 screenToView（那條要除以投影縮放，寬畫布模型會提早被 clamp）。
  const float nx = static_cast<float>(localPos.x() / std::max(1, width())) * 2.0f - 1.0f;
  const float ny = -(static_cast<float>(localPos.y() / std::max(1, height())) * 2.0f - 1.0f);
  controller_->setFocus(std::clamp(nx, -1.0f, 1.0f), std::clamp(ny, -1.0f, 1.0f));
}

void ViewerCanvas::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    pressing_ = true;
    pressPos_ = event->position();
  }
  QOpenGLWidget::mousePressEvent(event);
}

void ViewerCanvas::mouseMoveEvent(QMouseEvent* event) { setFocusFromLocal(event->position()); }

void ViewerCanvas::mouseReleaseEvent(QMouseEvent* event) {
  const bool wasPressing = pressing_;
  pressing_ = false;
  // 位移超過門檻的那一下是「劃過去轉視線」，不是點擊（門檻與桌寵共用同一個常數）
  if (wasPressing && event->button() == Qt::LeftButton && QLineF(pressPos_, event->position()).length() <= l2m::kTapThresholdPx) {
    emit modelTapped(event->position());
  }
  QOpenGLWidget::mouseReleaseEvent(event);
}

void ViewerCanvas::leaveEvent(QEvent* event) {
  // 游標離開畫布就回正前方，免得視線永遠僵在最後一個邊緣位置
  setFocusFromLocal(QPointF(width() / 2.0, height() / 2.0));
  QOpenGLWidget::leaveEvent(event);
}
