// glew 必須在任何會引入 gl.h 的標頭（含 Qt 的 OpenGL 標頭）之前
#include <GL/glew.h>

#include "character_window.h"

#include <QDebug>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <filesystem>

#include "../platform/window_effects.h"
#include "core/frame_profiler.h"
#include "core/interaction_logic.h"
#include "live2d/cubism_runtime.h"
#include "live2d/hit_mask.h"
#include "live2d/model_controller.h"
#include "window_manager.h"

CharacterWindow::CharacterWindow() {
  // 無邊框 + 不進工作列（Tool）+ 置頂
  setFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
  // 透明的關鍵：surface format 要有 alpha channel
  QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
  fmt.setAlphaBufferSize(8);
  setFormat(fmt);
  setTitle(QStringLiteral("Live2D Mate"));
  resize(l2m::kBaseWidth, l2m::kBaseHeight);

  // 以 vsync 節奏連續重繪（模型有呼吸與待機動作，需要每幀更新）
  connect(this, &QOpenGLWindow::frameSwapped, this, [this] {
    if (firstFramePendingSwap_) {
      firstFramePendingSwap_ = false;
      emit firstFrameRendered();
    }
    update();
  });
}

CharacterWindow::~CharacterWindow() {
  // GL 資源需在 context 有效時釋放
  makeCurrent();
  hitMask_.reset();
  controller_.reset();
  doneCurrent();
}

void CharacterWindow::requestModelLoad(const QString& entryPath) {
  pendingModelEntry_ = entryPath;
  if (glReady_) {
    makeCurrent();
    loadPendingModel();
    doneCurrent();
    update();
  }
}

void CharacterWindow::beginFade(bool visible) {
  pendingFade_ = visible;
  update();
}

void CharacterWindow::loadPendingModel() {
  if (pendingModelEntry_.isEmpty()) return;
  const QString entry = pendingModelEntry_;
  pendingModelEntry_.clear();

  controller_.reset();
  lastRenderTargetW_ = 0;  // 新 renderer 需重新同步離屏尺寸
  lastRenderTargetH_ = 0;
  // 舊模型可能還有一筆回讀在途：作廢它，之後的 version 遞增才必為新模型
  if (hitMask_) hitMask_->invalidateForModelSwitch();
  auto next = std::make_unique<l2m::ModelController>();
  const bool ok = next->load(std::filesystem::u8path(entry.toStdString()));
  if (ok) {
    controller_ = std::move(next);
    awaitingFirstFrame_ = true;
    // 載入完成的這一刻就把 alpha 壓到 0，載入後的第一幀才是全透明的
    //（等下一幀才設的話，第一幀早就以全不透明畫上螢幕，淡入等於白做）。
    // 啟動與切換模型的淡入共用這一條路徑，main.cpp 不必為啟動另外處理。
    fade_.set(0.0);
    pendingFade_ = true;
    qInfo() << "[live2d] 模型載入完成:" << entry;
  } else {
    // next 出了這個 if 就沒了，原因要在這裡抄走（見 character_window.h 的說明）
    lastLoadError_ = next->loadError();
    lastLoadHint_ = next->loadHint();
    qWarning() << "[live2d] 模型載入失敗:" << entry;
    // noquote：句子裡本來就有引號（格式名），再讓 QDebug 包一層會變成 \" 的跳脫地獄
    qWarning().noquote() << "[live2d]" << QString::fromStdString(lastLoadError_);
    qWarning().noquote() << "[live2d]" << QString::fromStdString(lastLoadHint_);
    // 載入失敗之後 paintGL 會在 `if (!controller_) return;` 就折返，而套形狀與清形狀
    // 兩條都排在那之後 —— 穿透遮罩因此會**永遠停在失敗前的那一刻**：切換失敗時留著
    // 上一隻模型的剪影，冷啟動失敗時則從頭到尾沒套過任何 region，於是那個看不見的
    // 透明視窗會把底下桌面的點擊整片吃掉、還拖得動。所以在這裡就地套一個**空的**
    // region —— 空 region ＝ 整窗不可點（見 platform/window_effects_win.cpp）；
    // clearClickableRegion 是相反的「整窗可點」，這裡千萬不能用它。
    l2m::platform::applyClickableRegion(this, QRegion());
    lastRegion_ = QRegion();
    regionApplied_ = true;
    lastRegionVersion_ = -1;  // 下一次載入成功時強制重算，別跟失敗前的版本號撞上
  }
  emit modelLoaded(ok);
}

bool CharacterWindow::isOpaqueAt(const QPointF& localPos) const {
  if (!hitMask_ || !hitMask_->ready()) return true;
  return hitMask_->isOpaque(localPos.x() / std::max(1, width()), localPos.y() / std::max(1, height()));
}

std::optional<double> CharacterWindow::modelBottomNormalized() const {
  if (!hitMask_) return std::nullopt;
  return hitMask_->bottomNormalizedY();
}

std::optional<double> CharacterWindow::modelTopNormalized() const {
  if (!hitMask_) return std::nullopt;
  return hitMask_->topNormalizedY();
}

void CharacterWindow::setFocusFromLocal(const QPointF& localPos) {
  if (!controller_) return;
  // 視線焦點用「視窗邊緣＝滿偏」的線性映射，刻意不走 screenToView ——
  // 那條路會除以投影縮放：寬畫布模型（draw() 的 SetWidth(2) 分支）垂直縮放
  // 只有 vw/vh，游標走到離中心 2/3 處焦點就被 clamp 成滿偏（游標還沒到視窗底
  // 頭已經看到底）；一般比例的模型水平方向反而除以 vh/vw，焦點最大只有
  // ±0.67，頭永遠轉不到底。這裡讓焦點恰好在視窗邊緣達到 ±1：每像素的轉頭量
  // 變小、手感變慢，但游標貼邊時仍能轉到底。點擊的命中測試照舊走
  // screenToView —— 那邊要的是模型 view 座標，不能跟著改。
  const float nx = static_cast<float>(localPos.x() / std::max(1, width())) * 2.0f - 1.0f;
  const float ny = -(static_cast<float>(localPos.y() / std::max(1, height())) * 2.0f - 1.0f);
  controller_->setFocus(std::clamp(nx, -1.0f, 1.0f), std::clamp(ny, -1.0f, 1.0f));
}

void CharacterWindow::initializeGL() {
  // Framework 的 GL renderer 用 glew 取函式指標
  glewExperimental = GL_TRUE;
  const GLenum glewResult = glewInit();
  if (glewResult != GLEW_OK) {
    qWarning() << "[live2d] glewInit 失敗:" << reinterpret_cast<const char*>(glewGetErrorString(glewResult));
  }
  glGetError();  // 清掉 glewInit 可能留下的無害錯誤

  l2m::CubismRuntime::initialize();
  hitMask_ = std::make_unique<l2m::AlphaHitMask>();

  glReady_ = true;
  loadPendingModel();

  frameClock_.start();
  runClock_.start();
}

void CharacterWindow::paintGL() {
  l2m::ProfileScope frameScope(l2m::FrameProfiler::StageFrame);

  // 透明視窗的關鍵：clear 成全透明，讓沒畫到的地方露出桌面
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  if (!controller_) return;

  // 半透明漸層在某些驅動上會被 dithering 弄出棋盤紋（GL 預設開啟）
  glDisable(GL_DITHER);

  // 淡入淡出的時鐘在「真的要畫的這一幀」才起算（見 pendingFade_）
  const double nowMs = double(runClock_.elapsed());
  if (pendingFade_) {
    fade_.begin(*pendingFade_ ? 1.0 : 0.0, nowMs);
    pendingFade_.reset();
  }

  const float dt = std::min(frameClock_.restart() / 1000.0f, 0.1f);
  {
    l2m::ProfileScope scope(l2m::FrameProfiler::StageUpdate);
    controller_->update(dt);
  }

  const qreal ratio = devicePixelRatio();
  const int vw = static_cast<int>(width() * ratio);
  const int vh = static_cast<int>(height() * ratio);
  glViewport(0, 0, vw, vh);
  // 離屏 buffer 跟視窗同尺寸，混合模式的離屏合成才不會有縮放紋
  if (vw != lastRenderTargetW_ || vh != lastRenderTargetH_) {
    controller_->setRenderTargetSize(vw, vh);
    lastRenderTargetW_ = vw;
    lastRenderTargetH_ = vh;
  }
  const float fadeAlpha = static_cast<float>(fade_.alphaAt(nowMs));
  {
    l2m::ProfileScope scope(l2m::FrameProfiler::StageDraw);
    // 只有這一次 draw 帶 fade alpha。命中遮罩要的是 alpha=1 的剪影，
    // 跟著淡的話形狀與腳底量測會一起消失 —— 所以淡化期間遮罩不吃這張畫布，
    // 改由 drawSilhouette() 自己重畫一份（見下面的 Source::usable）。
    controller_->draw(vw, vh, fadeAlpha);
  }

  // 點擊穿透（形狀視窗）：視窗形狀 = 角色本體，區域外的點擊直接落到下層。
  // 游標不在視窗上時不套形狀（畫面完整無裁切）。
  // 診斷：L2M_FORCE_REGION 時當作游標一直在視窗上，量測形狀更新的最壞路徑
  static const bool forceRegion = qEnvironmentVariableIsSet("L2M_FORCE_REGION");
  const bool wantRegion = (regionGate_ || forceRegion) && clickThroughEnabled && clickThroughEnabled();

  // 命中遮罩：把上面剛畫好的主畫布縮進小 FBO 並非同步讀回 alpha（內部節流）。
  // 形狀生效中就每幀更新（間隔 0）：形狀同時決定「哪裡看得見」，
  // 節流到 100ms 會讓角色動出舊形狀外的部分被裁掉，而且形狀是 10Hz 在跳。
  //
  // 來源是上面剛畫好的主畫布，不是把模型再畫一次 —— 改寫前那是這一幀最貴的一筆。
  // 實測數字、逐格比對與退回條件都寫在 live2d/hit_mask.h 的檔頭，這裡不抄第二份。
  l2m::AlphaHitMask::Source maskSource;
  maskSource.fbo = static_cast<unsigned int>(defaultFramebufferObject());
  maskSource.width = vw;
  maskSource.height = vh;
  // 淡化中的主畫布乘了 fadeAlpha，拿去當遮罩會讓形狀跟著縮成空。
  // 沒有淡化在跑時 alphaAt() 回的是精確的 target_（1.0），所以這條在平時恆為真。
  maskSource.usable = fadeAlpha >= 0.999f;
  hitMask_->update(*controller_, maskSource, double(runClock_.elapsed()), wantRegion ? 0.0 : 200.0);

  // 遮罩落地通知（腳底對齊用）。接收端沒有待處理的對齊時是一次整數比較 + 空 slot
  if (hitMask_->version() != lastNotifiedMaskVersion_) {
    lastNotifiedMaskVersion_ = hitMask_->version();
    emit maskUpdated();
  }

  if (wantRegion) {
    if (hitMask_->version() != lastRegionVersion_ || !regionApplied_) {
      lastRegionVersion_ = hitMask_->version();
      QRegion region;
      {
        l2m::ProfileScope scope(l2m::FrameProfiler::StageRegionBuild);
        region = hitMask_->clickableRegion(width(), height());
      }
      // 待機時相鄰兩幀的形狀常常完全一樣，比對過再套可以省下 DWM 重算
      if (region != lastRegion_ || !regionApplied_) {
        l2m::ProfileScope scope(l2m::FrameProfiler::StageRegionApply);
        l2m::platform::applyClickableRegion(this, region);
        lastRegion_ = region;
      }
      regionApplied_ = true;
    }
  } else if (regionApplied_) {
    l2m::platform::clearClickableRegion(this);
    lastRegion_ = QRegion();
    regionApplied_ = false;
  }

  // 診斷：L2M_TEST_OPAQUE 時在中央畫一塊純不透明紅色方塊，
  // 用來分辨「視窗層被減淡」還是「模型渲染 alpha 偏低」
  static const bool testOpaque = qEnvironmentVariableIsSet("L2M_TEST_OPAQUE");
  if (testOpaque) {
    glEnable(GL_SCISSOR_TEST);
    glScissor(vw / 2 - 100, vh / 2 - 100, 200, 200);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
  }

  // 診斷：設定 L2M_DUMP_FRAME 時，每 5 秒把渲染結果 dump 成 PNG（覆寫）
  static bool dumpScheduled = false;
  if (!dumpScheduled) {
    dumpScheduled = true;
    const QString dumpPath = qEnvironmentVariable("L2M_DUMP_FRAME");
    if (!dumpPath.isEmpty()) {
      auto* timer = new QTimer(this);
      connect(timer, &QTimer::timeout, this, [this, dumpPath] {
        grabFramebuffer().save(dumpPath);
        qInfo() << "已輸出 frame dump:" << dumpPath;
      });
      timer->start(5000);
    }
  }

  // 這一幀有模型了；等它 swap 上螢幕才算「第一幀畫好」
  if (awaitingFirstFrame_) {
    awaitingFirstFrame_ = false;
    firstFramePendingSwap_ = true;
  }

  if (l2m::FrameProfiler::enabled()) l2m::FrameProfiler::instance().tick();
}

void CharacterWindow::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) return;
  dragging_ = true;
  dragMoved_ = 0;
  pressPos_ = event->globalPosition().toPoint();
  lastScreenPos_ = pressPos_;
  if (onActivity) onActivity();
  if (onPointerEvent) onPointerEvent(true, event->position());
}

void CharacterWindow::mouseMoveEvent(QMouseEvent* event) {
  if (!dragging_ || !(event->buttons() & Qt::LeftButton)) return;
  const QPoint pos = event->globalPosition().toPoint();
  const QPoint delta = pos - lastScreenPos_;
  lastScreenPos_ = pos;
  dragMoved_ = std::max(dragMoved_, double((pos - pressPos_).manhattanLength()));
  if (dragMoved_ >= l2m::kTapThresholdPx && dragEnabled && dragEnabled() && onDragBy) {
    onDragBy(delta.x(), delta.y());
  }
}

void CharacterWindow::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) return;
  dragging_ = false;
  // 點擊動作不在這裡播 —— 單擊要等 OS 雙擊間隔確定沒有下一擊才算數
  //（否則連點三下會疊三個點擊動作再加一個 MultiTap 反應），
  // 判定與播放都在 AppController::onGesture。
  if (onPointerEvent) onPointerEvent(false, event->position());
}

void CharacterWindow::wheelEvent(QWheelEvent* event) {
  if (wheelZoomEnabled && !wheelZoomEnabled()) return;
  const int direction = event->angleDelta().y() > 0 ? 1 : -1;
  if (onWheelZoom) onWheelZoom(direction);
}
