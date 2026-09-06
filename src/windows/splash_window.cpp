#include "splash_window.h"

#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScreen>
#include <QStyleHints>
#include <QTimer>
#include <QVariantAnimation>

#include "core/splash_timing.h"

namespace l2m {

namespace {

// Startup：卡片本體 = splash.png 的邏輯尺寸（520×200，2.6:1）
constexpr int kCardWidth = 520;
constexpr int kCardHeight = 200;
constexpr int kCornerRadius = 18;

// Compact：整個視窗就是一條膠囊形的進度條。
// 一開始做成 220×56 的小卡片、光條貼底部，實際疊在角色身上很怪 ——
// 卡片中間是空的，看起來不像「載入中」，只像一塊白色色塊。
// 膠囊本身就是軌道，光條在裡面掃，訊息單一也不佔位置。
constexpr int kCompactWidth = 220;
constexpr int kCompactHeight = 18;
constexpr int kCompactRadius = 9;

// 卡片外圈留給陰影的空白。設 0 就退化成一張純圓角卡片（視窗也跟著縮小）
constexpr int kShadowPadding = 12;

// 淡入淡出。膠囊要短得多：它得等子行程起來（實測 450 ms 以上）才出現，
// 再用 300 ms 慢慢淡入的話，遇到小模型往往還沒到全不透明就要收了，
// 看起來就像「根本沒顯示」。
constexpr int kFadeMs = 300;
constexpr int kCompactFadeMs = 120;

// 最短顯示時間（從 begin() 起算，含淡入）。
// 啟動時給得長一點：小模型 0.2 秒就載完，splash 一閃而過比不顯示還難看。
// 換模型時要短：那是使用者主動的操作，等待感越短越好。
constexpr int kMinVisibleMs = 800;
constexpr int kCompactMinVisibleMs = 500;

// 底部光條
constexpr int kBarHeight = 4;
constexpr double kSweepSegmentRatio = 0.28;  // 光條寬度佔軌道的比例
constexpr int kSweepCycleMs = 1200;

// 重繪節奏
constexpr int kTickMs = 16;

// 配色取自圖本身：淡紫底 + 紫藍描邊的 wordmark + 桃紅／青色幾何點綴。
// 半透明白在這種淺底上幾乎看不見，所以軌道用低透明度的深紫當「凹槽」。
const QColor kTrackColor(60, 40, 110, 40);
const QColor kSweepFrom(108, 76, 224);  // #6C4CE0，wordmark 的紫
const QColor kSweepTo(226, 85, 176);    // #E255B0，點綴的桃紅
const QColor kShadowColor(40, 24, 80, 12);

// Compact 沒有圖可以襯，得自己畫底。取 splash 圖的淡紫調，
// 深色模式下換成深紫，才不會在暗桌布上刺眼。
bool darkMode() { return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark; }

}  // namespace

SplashWindow::SplashWindow(Mode mode, QWidget* parent)
  : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint), mode_(mode) {
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setFocusPolicy(Qt::NoFocus);
  const QSize body = bodySize();
  resize(body.width() + kShadowPadding * 2, body.height() + kShadowPadding * 2);

  fade_ = new QVariantAnimation(this);
  fade_->setDuration(fadeMs());
  connect(fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { setWindowOpacity(value.toReal()); });
  connect(fade_, &QVariantAnimation::finished, this, [this] {
    // 淡出結束才真的收起來；淡入結束不做事
    if (windowOpacity() > 0.01) return;
    QWidget::hide();
    emit closed();
  });
}

SplashWindow::~SplashWindow() = default;

QSize SplashWindow::bodySize() const { return mode_ == Mode::Compact ? QSize(kCompactWidth, kCompactHeight) : QSize(kCardWidth, kCardHeight); }

QRectF SplashWindow::bodyRect() const {
  const QSize body = bodySize();
  return QRectF(kShadowPadding, kShadowPadding, body.width(), body.height());
}

int SplashWindow::cornerRadius() const { return mode_ == Mode::Compact ? kCompactRadius : kCornerRadius; }

int SplashWindow::minVisibleMs() const { return mode_ == Mode::Compact ? kCompactMinVisibleMs : kMinVisibleMs; }

int SplashWindow::fadeMs() const { return mode_ == Mode::Compact ? kCompactFadeMs : kFadeMs; }

void SplashWindow::begin() {
  positionSelf();
  clock_.start();

  // 先設 0 再 show，第一幀才不會閃現全不透明（同 bubble_window.cpp 的做法）
  setWindowOpacity(0.0);
  show();  // WA_ShowWithoutActivating，不搶焦點

  // 淡入交給動畫就好 —— 這個行程的事件迴圈馬上就會開始跑。
  //（內嵌在主行程時不行：那邊 app.exec() 還沒開始，動畫拿不到任何 tick，
  //  當初得靠一個同步迴圈手動推。搬成子行程之後那套就不需要了。）
  fade_->stop();
  fade_->setStartValue(0.0);
  fade_->setEndValue(1.0);
  fade_->start();

  tick_ = new QTimer(this);
  tick_->setInterval(kTickMs);
  connect(tick_, &QTimer::timeout, this, [this] { update(); });
  tick_->start();
}

void SplashWindow::positionSelf() {
  // Compact：貼在角色身上（主行程傳來的角色視窗 geometry）
  if (mode_ == Mode::Compact && !anchor_.isEmpty()) {
    move(anchor_.center() - QPoint(width() / 2, height() / 2));
    return;
  }
  // 三層退路同 bubble_window.cpp 的螢幕解析：游標所在 → 主螢幕 → 保底常數
  QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
  if (!screen) screen = QGuiApplication::primaryScreen();
  // availableGeometry 而不是 geometry：避開工作列
  const QRect area = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
  move(area.center() - QPoint(width() / 2, height() / 2));
}

void SplashWindow::finish() {
  if (finished_ || !isVisible()) return;
  finished_ = true;

  const int hold = int(splashRemainingHoldMs(double(clock_.elapsed()), minVisibleMs()));
  QTimer::singleShot(hold, this, [this] {
    if (tick_) tick_->stop();
    fade_->stop();
    fade_->setStartValue(windowOpacity());
    fade_->setEndValue(0.0);
    fade_->start();
  });
}

void SplashWindow::rebuildCard() {
  const qreal dpr = devicePixelRatioF();
  // cardDpr_ > 0 代表算過了；圖檔讀不到時也會設，才不會每幀重試
  if (cardDpr_ > 0.0 && qFuzzyCompare(cardDpr_, dpr)) return;
  cardDpr_ = dpr;

  const int radius = cornerRadius();
  QPixmap body;  // 卡片本體（不含陰影留白）

  if (mode_ == Mode::Compact) {
    // 膠囊本身就是軌道。底色要夠不透明，才不會被身後的角色與桌布吃掉輪廓；
    // 深色模式下換深紫，淺色桌布上則用淡紫白，兩邊都看得出這是個容器。
    const QSize size = bodySize();
    body = QPixmap(size * dpr);
    body.setDevicePixelRatio(dpr);
    body.fill(Qt::transparent);
    QPainter p(&body);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(darkMode() ? QColor(32, 24, 56, 230) : QColor(238, 234, 250, 235));
    QPainterPath shape;
    shape.addRoundedRect(QRectF(0, 0, size.width(), size.height()), radius, radius);
    p.drawPath(shape);
  } else {
    // QPixmap 不像 QIcon 會自動解析 @2x —— 自動 @Nx 只有 QIcon::addFile 的
    // qt_findAtNxFile 有（tray.cpp 是靠 QIcon 才白拿到 tray@2x.png）。
    // QPixmap(fileName) 走 QImageReader，不會去找 @2x，所以這裡手動挑檔。
    QPixmap art(dpr > 1.25 ? QStringLiteral(":/icons/splash@2x.png") : QStringLiteral(":/icons/splash.png"));
    if (art.isNull()) art.load(QStringLiteral(":/icons/splash.png"));
    if (art.isNull()) {
      qWarning() << "[splash] 讀不到 splash 圖檔，不顯示卡片";
      card_ = QPixmap();
      return;
    }
    // 反算 dpr，讓這張圖的邏輯尺寸固定是 520×200（1x 得 1.0、@2x 得 2.0）
    art.setDevicePixelRatio(art.width() / double(kCardWidth));

    // 把圖裁成圓角。
    // 不用 setMask（1-bit 遮罩，18px 圓角上是明顯階梯），也不用 setClipPath
    //（raster engine 的 clip 是 scanline coverage，開了 Antialiasing 也不會
    // 對 clip 邊緣抗鋸齒，一樣有階梯）。
    //
    // 順序是「先畫圓角實心形狀，再用 SourceIn 把圖蓋上去」，不是反過來用
    // DestinationIn 畫遮罩 —— 那樣是錯的：fillPath 只會處理 path 覆蓋到的
    // 像素，圓角**外側**的像素根本不被觸碰，所以四個角一個都裁不掉
    //（實測就是這樣，畫面上是一張方角的圖）。
    // SourceIn 這條路上 drawPixmap 覆蓋整個矩形，每個像素都會參與運算，
    // 圓角外 destination alpha = 0，結果就是 0。
    body = QPixmap(art.size());
    body.setDevicePixelRatio(art.devicePixelRatio());
    body.fill(Qt::transparent);
    QPainter p(&body);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    QPainterPath shape;
    shape.addRoundedRect(QRectF(0, 0, kCardWidth, kCardHeight), radius, radius);
    p.drawPath(shape);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    // dpr 1.5 的螢幕會挑 @2x 再縮到 0.75 倍，沒開這個會退成 nearest neighbor
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawPixmap(0, 0, art);
  }

  // 疊到含陰影留白的卡片上。陰影直接畫幾層半透明外框（**跟 BubbleWindow 的陰影不是同一套**：
  // 那邊是位移剪影或箱型模糊，見 core/bubble_shape.h），比拉一個 QGraphicsEffect 便宜得多。
  // 整張卡片只算這一次（dpr 變了才重來），paintEvent 因此只剩「一次 drawPixmap + 一條光條」。
  card_ = QPixmap(size() * dpr);
  card_.setDevicePixelRatio(dpr);
  card_.fill(Qt::transparent);
  QPainter p(&card_);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  QPainterPath path;
  path.addRoundedRect(bodyRect(), radius, radius);
  for (int i = 5; i >= 1; --i) {
    p.setPen(QPen(kShadowColor, i * 2.4));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path.translated(0, 3));
  }
  p.drawPixmap(kShadowPadding, kShadowPadding, body);
}

QRectF SplashWindow::trackRect() const {
  const QRectF body = bodyRect();
  // Compact：整條膠囊就是軌道，光條佔滿高度（兩端交給圓角 clip 收邊）
  if (mode_ == Mode::Compact) return body;
  // Startup：貼在卡片底部內緣，左右滿版
  return QRectF(body.left(), body.bottom() - kBarHeight, body.width(), kBarHeight);
}

void SplashWindow::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);
  rebuildCard();  // dpr 沒變是 no-op；也順便處理跨螢幕移動
  if (card_.isNull()) return;

  QPainter painter(this);
  painter.drawPixmap(0, 0, card_);

  const QRectF track = trackRect();
  if (track.isEmpty()) return;

  // 光條的左右下角要跟著卡片圓角收邊。這裡的 clip 是硬邊，但 4px 高的光條
  // 只碰到圓角的極短一段，肉眼看不出；真正吃抗鋸齒的卡片外框已經在
  // rebuildCard() 用 SourceIn（或直接畫圓角底）處理掉了。
  QPainterPath clip;
  clip.addRoundedRect(bodyRect(), cornerRadius(), cornerRadius());
  painter.setClipPath(clip);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);
  painter.setBrush(kTrackColor);
  painter.drawRect(track);

  QLinearGradient gradient(track.left(), 0, track.right(), 0);
  gradient.setColorAt(0.0, kSweepFrom);
  gradient.setColorAt(1.0, kSweepTo);
  painter.setBrush(gradient);

  const double elapsed = clock_.isValid() ? double(clock_.elapsed()) : 0.0;
  const double segment = track.width() * kSweepSegmentRatio;
  const double x = splashSweepOffset(elapsed, track.width(), segment, kSweepCycleMs);
  painter.drawRect(QRectF(track.left() + x, track.top(), segment, track.height()));
}

}  // namespace l2m
