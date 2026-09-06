#include "bubble_window.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTextOption>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "../platform/window_effects.h"
#include "core/bubble_font.h"
#include "window_manager.h"

namespace l2m {

namespace {

// 字級固定：角色縮到 0.2 倍時氣泡也要看得清楚
constexpr int kFontPixelSize = 14;
constexpr int kFadeMs = 220;
// 外框粗細。畫在填色之上，所以線寬有一半落在路徑外面 ——
// core/bubble_shape.h 的 kOutlineMargin 要蓋得住那一半。
constexpr double kOutlineWidth = 2.0;

// 漫畫感來自「不透明白底＋黑框＋黑字」這一組顏色，所以**刻意不跟系統深色模式走**，
// 底色也永遠不透明 —— 半透明在漫畫裡不存在，加上去就變回一般的 UI 提示框。
// 投影則是後來補的一個設定（tts.bubbleShadow）：印刷的氣泡確實常墊一片硬邊的位移
// 剪影，那就是 BubbleShadow::Hard；柔邊那一檔給偏好一般 UI 浮起感的人，預設不是它。
const QColor kBubbleFill(255, 255, 255);
const QColor kBubbleInk(26, 26, 26);

// 依語系挑字型，日韓才不會被套成中文字形（對應 CSS 的 system-ui + lang）。
//
// **一定要是單一家族**。這裡曾經用 QFont::setFamilies() 給候選清單，看起來很合理，
// 但只要清單不只一個家族，Qt 就會為它建 fallback chain，在 Windows 上等於
// 跑一輪 DirectWrite 字型後援解析：第一次 QFontMetrics 實測 1398 ms，
// 整個卡在 GUI 執行緒上 —— 也就是「按下測試語音後動畫凍住一秒多」的那一下。
// 改成先問 QFontDatabase 誰真的裝了（5.2 ms）再用單數的 setFamily()（7.6 ms）。
// 完整的實測數字與規則在 core/bubble_font.h。
QFont bubbleFont(const std::string& locale) {
  // 系統 UI 字型永遠是這台機器上真的存在的那一個，拿它當保底
  QFont font = QGuiApplication::font();

  std::vector<std::string> candidates = bubbleFontCandidates(locale);
  candidates.push_back(font.family().toStdString());

  const QStringList families = QFontDatabase::families();
  std::vector<std::string> installed;
  installed.reserve(static_cast<size_t>(families.size()));
  for (const QString& family : families) installed.push_back(family.toStdString());

  // 挑不到就維持 Qt 的預設字型 —— 硬塞一個不存在的家族一樣會觸發後援解析
  const std::string chosen = pickInstalledFamily(candidates, installed);
  if (!chosen.empty()) font.setFamily(QString::fromStdString(chosen));

  font.setPixelSize(kFontPixelSize);
  return font;
}

// 陰影的剪影：與氣泡本體**同一條路徑**、同樣填色加描框，只是換成半透明的墨色再整個
// 往右下位移。描框不能省 —— 外框的線寬有一半落在路徑外面，只填不描的話影子會比看得見
// 的氣泡瘦一圈，邊緣就會露出一條白邊。
void fillShadowPath(QPainter& painter, const QPainterPath& path, const BubbleShadowSpec& spec) {
  QColor color = kBubbleInk;
  color.setAlpha(spec.alpha);
  painter.save();
  painter.translate(spec.offset, spec.offset);
  painter.setBrush(color);
  painter.setPen(QPen(color, kOutlineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawPath(path);
  painter.restore();
}

// 一條線的滑動平均。src 是連續的 count×4 位元組複本，dst 每一格相隔 stride 位元組
//（水平走一列時是 4，垂直走一行時是整列的長度）—— 同一支函式跑兩個方向。
// 兩端夾住邊界的像素，不然圖的四周會被當成透明而把陰影吸淡一圈。
void blurLine(const uchar* src, uchar* dst, int count, int stride, int radius) {
  const int window = radius * 2 + 1;
  const auto at = [&](int i) { return src + static_cast<size_t>(std::clamp(i, 0, count - 1)) * 4; };

  int sum[4] = {0, 0, 0, 0};
  for (int i = -radius; i <= radius; ++i) {
    const uchar* p = at(i);
    for (int c = 0; c < 4; ++c) sum[c] += p[c];
  }
  for (int i = 0; i < count; ++i) {
    uchar* out = dst + static_cast<size_t>(i) * stride;
    for (int c = 0; c < 4; ++c) out[c] = static_cast<uchar>(sum[c] / window);
    const uchar* leaving = at(i - radius);
    const uchar* entering = at(i + radius + 1);
    for (int c = 0; c < 4; ++c) sum[c] += entering[c] - leaving[c];
  }
}

// 箱型模糊（吃 QImage::Format_ARGB32_Premultiplied）。陰影是單一顏色，premultiplied
// 之下四個通道都是「顏色 × 覆蓋率」的線性量，四個一起糊就是對的。
//
// 跑兩趟：一趟的箱型核衰減是直線，邊緣看得出一條折線；兩趟疊起來是三角核。
// 代價是糊開的距離變成 2×radius —— core/bubble_shape.h 的 spec.blur 指的正是這個
// 總距離（視窗的右下餘裕也是照它留的），所以呼叫端傳進來的是 blur / 2。
void boxBlur(QImage& image, int radius) {
  if (radius <= 0 || image.isNull()) return;
  const int w = image.width();
  const int h = image.height();
  const qsizetype bpl = image.bytesPerLine();
  uchar* bits = image.bits();
  std::vector<uchar> line(static_cast<size_t>(std::max(w, h)) * 4);

  for (int pass = 0; pass < 2; ++pass) {
    for (int y = 0; y < h; ++y) {
      uchar* row = bits + static_cast<size_t>(y) * bpl;
      std::memcpy(line.data(), row, static_cast<size_t>(w) * 4);
      blurLine(line.data(), row, w, 4, radius);
    }
    for (int x = 0; x < w; ++x) {
      uchar* column = bits + static_cast<size_t>(x) * 4;
      for (int y = 0; y < h; ++y) std::memcpy(line.data() + static_cast<size_t>(y) * 4, column + static_cast<size_t>(y) * bpl, 4);
      blurLine(line.data(), column, h, static_cast<int>(bpl), radius);
    }
  }
}

// 走一趟 QTextLayout：widths 給每一行允許的寬度（橢圓的形狀就在這一組數字裡），
// 回傳每一行實際佔掉的寬度 —— 這就是 core/ellipse_text_fit.h 要的 EllipseTextMeasure。
//
// tops 非空時順便定位，而且是**每一行各自水平置中**：行寬本來就都不一樣，
// QTextOption 的 AlignHCenter 是「整塊對齊同一個寬度」，對不上這裡。
std::vector<double> runLayoutPass(QTextLayout& layout, const std::vector<double>& widths, const std::vector<double>* tops) {
  std::vector<double> actual;
  layout.clearLayout();
  layout.beginLayout();
  for (;;) {
    QTextLine line = layout.createLine();
    if (!line.isValid()) break;
    const size_t index = actual.size();
    // 行數超過 widths 時補最後一個，這是 EllipseTextMeasure 白紙黑字的契約
    const double allowed = widths.empty() ? 1.0 : (index < widths.size() ? widths[index] : widths.back());
    line.setLineWidth(std::max(allowed, 1.0));
    if (tops) line.setPosition(QPointF(-line.naturalTextWidth() / 2, index < tops->size() ? (*tops)[index] : 0.0));
    actual.push_back(line.naturalTextWidth());
  }
  layout.endLayout();
  return actual;
}

}  // namespace

BubbleWindow::BubbleWindow(QWidget* parent)
  : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint) {
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setFocusPolicy(Qt::NoFocus);

  font_ = bubbleFont("en");

  fade_ = new QVariantAnimation(this);
  fade_->setDuration(kFadeMs);
  connect(fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { setWindowOpacity(value.toReal()); });
  connect(fade_, &QVariantAnimation::finished, this, [this] {
    // 淡出結束才真的收起來；淡入結束不做事
    if (windowOpacity() <= 0.01) QWidget::hide();
  });
}

BubbleWindow::~BubbleWindow() = default;

void BubbleWindow::setLocale(const std::string& locale) {
  // 設定一有變動就會呼叫進來（拖曳角色時很頻繁），語系沒變就不要重建 QFont
  if (locale == locale_) return;
  locale_ = locale;
  font_ = bubbleFont(locale);
  layoutValid_ = false;
  if (isVisible()) {
    reposition();
    update();
  }
}

BubbleWindow::Layout BubbleWindow::buildLayout() {
  const QFontMetrics metrics(font_);

  // 不換行時的單行寬度。**只拿來估種子橢圓有多大**，不是版面的一部分
  //（收斂規則在 core/ellipse_text_fit.h ③）
  constexpr int kUnbounded = 100000;
  const QRect single = metrics.boundingRect(QRect(0, 0, kUnbounded, 0), Qt::TextSingleLine, text_);

  QTextOption option;
  // 斷不開的長 token（網址、檔案路徑）就地斷開。以前是讓它整段溢出橢圓、
  // 再靠 TextDontClip 不裁切，畫面上是「一顆超大的空氣泡配一條長長的字」；
  // 現在寧可把它斷掉，氣泡才回得到正常大小。
  option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  textLayout_.setText(text_);
  textLayout_.setFont(font_);
  textLayout_.setTextOption(option);

  // 排版參數全部用 EllipseTextTuning 的預設值 —— **氣泡的內距（文字塊到
  // 橢圓輪廓）要調就調 core/ellipse_text_fit.h 的 paddingX／paddingY**，這裡不再
  // 抄一份：同一組數字寫兩處，下場就是改了一邊、另一邊還寫著舊值。
  const EllipseTextLayout fit = fitTextInEllipse(single.width(), metrics.lineSpacing(), [this](const std::vector<double>& widths) { return runLayoutPass(textLayout_, widths, nullptr); });

  // 最後再走一次同一組寬度，這次順便定位。EllipseTextMeasure 的契約保證
  // 行數與剛才那一趟一模一樣 —— 量到的橢圓與畫出來的字因此是同一件事。
  runLayoutPass(textLayout_, fit.lineWidths, &fit.lineTops);

  Layout out;
  out.ellipse = QSizeF(fit.ellipse.width, fit.ellipse.height);
  // 尾巴那一側要留滿尾巴的長度，另外三邊只要外框（core/bubble_placement.h 的
  // kBubblePadding）—— 兩邊一樣厚的話短句氣泡會離角色太遠
  out.tailExtent = bubbleTailExtent(style_, fit.ellipse);
  out.window = QSize(static_cast<int>(std::ceil(fit.ellipse.width)) + kBubblePadding * 2, static_cast<int>(std::ceil(fit.ellipse.height + out.tailExtent)) + kBubblePadding);
  return out;
}

QRectF BubbleWindow::ellipseRect() const {
  const double top = side_ == BubbleSide::Above ? kBubblePadding : layout_.tailExtent;
  return QRectF(kBubblePadding, top, layout_.ellipse.width(), layout_.ellipse.height());
}

void BubbleWindow::reposition() {
  if (text_.isEmpty() || characterBounds_.isEmpty()) return;

  // 文字沒變就不要重排 —— 拖曳角色時 follow() 每一幀都會叫進來，
  // 而找最小橢圓要反覆斷行好幾輪
  if (!layoutValid_ || laidOutText_ != text_ || laidOutStyle_ != style_) {
    layout_ = buildLayout();
    laidOutText_ = text_;
    laidOutStyle_ = style_;
    layoutValid_ = true;
  }
  const QSize size = layout_.window;

  QScreen* screen = QGuiApplication::screenAt(characterBounds_.center());
  if (!screen) screen = QGuiApplication::primaryScreen();
  const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);

  const Rect windowRect{static_cast<double>(characterBounds_.x()), static_cast<double>(characterBounds_.y()), static_cast<double>(characterBounds_.width()),
                        static_cast<double>(characterBounds_.height())};
  // 貼的是模型的視覺上下緣而不是整個角色視窗 —— 畫布上方留白大的模型，拿視窗頂端
  // 當錨點的話氣泡會浮在頭頂老遠的地方（換算與退回舊行為的規則見 bubbleAnchorRect）
  const Rect character = bubbleAnchorRect(windowRect, modelTop_, modelBottom_);
  const Rect area{static_cast<double>(available.x()), static_cast<double>(available.y()), static_cast<double>(available.width()), static_cast<double>(available.height())};

  // 模型比例：角色視窗的寬度就是 kBaseWidth × scale（見 windows/window_manager.h）
  const double scale = characterBounds_.width() > 0 ? static_cast<double>(characterBounds_.width()) / kBaseWidth : 1.0;

  const BubblePlacement placement = placeBubble(character, {static_cast<double>(size.width()), static_cast<double>(size.height())}, area, kBubbleGap, offsetY_, scale);
  side_ = placement.side;
  tailX_ = placement.tailX;
  mirrored_ = placement.mirrored;

  // 陰影只往右下長，所以視窗在那兩邊各多要一點空間。**placeBubble() 仍然吃沒有陰影的
  // 尺寸**，多出來的餘裕全加在右下 —— 橢圓與尾巴是從左上的 kBubblePadding 量的
  //（見 ellipseRect），座標因此一格都不動，換陰影檔位時氣泡不會跟著位移。
  const int extra = static_cast<int>(std::ceil(bubbleShadowExtent(shadow_)));

  // 但 placeBubble() 夾好的是**沒有陰影的**那個尺寸，撐大之後可能又頂出工作區
  //（角色貼著螢幕邊緣時）。**只往左／上收回來**，收的量最多就是 extra；視窗比工作區
  // 還大就別收，那是 placeBubble() 自己也放棄的情形。
  // 這是上面那句「座標一格都不動」的唯一例外：氣泡本來就已經被夾在邊上時，開陰影
  // 會讓它再往回挪最多 extra px —— 比讓影子跑到螢幕外面或壓在工作列上好。
  const int maxX = available.x() + available.width() - (size.width() + extra);
  const int maxY = available.y() + available.height() - (size.height() + extra);
  const int x = maxX >= available.x() ? std::min(placement.x, maxX) : placement.x;
  const int y = maxY >= available.y() ? std::min(placement.y, maxY) : placement.y;
  const QRect target(x, y, size.width() + extra, size.height() + extra);
  // 位置沒變就不要再 setGeometry —— 每次都設會讓氣泡在角色被拖曳時閃爍
  if (geometry() != target) setGeometry(target);
}

void BubbleWindow::showText(const QString& text, BubbleStyle style) {
  text_ = text;
  // 外型會改變量測結果（思考的圓點串比尾巴長），一定要在 reposition() 之前定案
  style_ = style;
  if (text_.isEmpty() || !characterVisible_) {
    hideBubble();
    return;
  }

  reposition();
  update();

  fade_->stop();
  fade_->setStartValue(windowOpacity());
  fade_->setEndValue(1.0);
  if (!isVisible()) {
    setWindowOpacity(0.0);
    fade_->setStartValue(0.0);
    // 不搶焦點：show() 之後角色仍然是可以被拖曳的
    show();
    // 氣泡沒有角色那支看門狗（它只在說話時存在），而系統同樣會把它的置頂拔掉。
    // 每次現身時補一刀最省：Qt 的 show() 不會重下 SetWindowPos(HWND_TOPMOST)，
    // 少了這行就會出現「角色在最上面、氣泡被別的視窗蓋住」的分家畫面。
    if (alwaysOnTop_) platform::applyTopmost(windowHandle(), true);
  }
  fade_->start();
}

void BubbleWindow::hideBubble() {
  if (!isVisible()) {
    text_.clear();
    return;
  }
  text_.clear();
  fade_->stop();
  fade_->setStartValue(windowOpacity());
  fade_->setEndValue(0.0);
  fade_->start();
}

void BubbleWindow::setOffsetY(int px) {
  // 與 setLocale 同理：設定一有變動就會呼叫進來，值沒變就不要白重算一次位置
  if (px == offsetY_) return;
  offsetY_ = px;
  if (isVisible()) reposition();
}

void BubbleWindow::setModelExtent(double topNormalized, double bottomNormalized) {
  // 同 setOffsetY：切模型才會叫進來一次，值沒變就不要白重算一次位置
  if (topNormalized == modelTop_ && bottomNormalized == modelBottom_) return;
  modelTop_ = topNormalized;
  modelBottom_ = bottomNormalized;
  if (isVisible()) reposition();
}

void BubbleWindow::setShadow(BubbleShadow shadow) {
  // 與 setLocale 同理：設定一有變動就會呼叫進來，沒換檔就不要白重算一次
  if (shadow == shadow_) return;
  shadow_ = shadow;
  // 視窗的右下餘裕是照陰影留的，換檔一定要重新定位（reposition 會重設 geometry）
  if (isVisible()) {
    reposition();
    update();
  }
}

void BubbleWindow::follow(const QRect& characterBounds) {
  characterBounds_ = characterBounds;
  if (isVisible() && !text_.isEmpty()) reposition();
}

void BubbleWindow::setCharacterVisible(bool visible) {
  characterVisible_ = visible;
  if (!visible) hideBubble();
}

void BubbleWindow::setAlwaysOnTop(bool enabled) {
  alwaysOnTop_ = enabled;
  const bool wasVisible = isVisible();
  setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
  // setWindowFlag 會重建原生視窗，本來看得見的話要自己補顯示回來
  if (wasVisible) show();
  // 跟角色視窗同一個理由：QWidget::setWindowFlags 在 flag 沒變時直接 return，
  // 系統把置頂拔掉之後這條路是空包彈（見 core/topmost_watchdog.h）
  platform::applyTopmost(windowHandle(), enabled);
}

void BubbleWindow::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);
  if (text_.isEmpty() || layout_.window.isEmpty()) return;

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const QRectF body = ellipseRect();
  const Rect ellipse{body.x(), body.y(), body.width(), body.height()};

  QPainterPath path;
  if (style_ == BubbleStyle::Thought) {
    path.addEllipse(body);
    // 圓點是分離的子路徑 —— 思考泡泡的圓點本來就不碰本體，所以這一邊完全
    // 沒有「接縫」的問題（說話那一邊有，見下）
    for (const ThoughtDot& dot : thoughtDots(ellipse, tailX_, side_, mirrored_)) {
      path.addEllipse(QPointF(dot.center.x, dot.center.y), dot.radius, dot.radius);
    }
  } else {
    // 本體與尾巴一定要是**同一條連續路徑**：橢圓與尾巴是相切的，
    // 拿兩條路徑做布林聯集會在切點留下毛邊 —— 有了黑外框之後那條毛邊會直接畫出來。
    // 所以走法是「從 baseB 沿橢圓繞一整圈到 baseA，再兩段貝茲收回 baseB」。
    const SpeechTail tail = speechTail(ellipse, tailX_, side_, mirrored_);
    path.arcMoveTo(body, tail.arcStartDeg);
    path.arcTo(body, tail.arcStartDeg, tail.arcSpanDeg);
    path.quadTo(QPointF(tail.ctrlA.x, tail.ctrlA.y), QPointF(tail.tip.x, tail.tip.y));
    path.quadTo(QPointF(tail.ctrlB.x, tail.ctrlB.y), QPointF(tail.baseB.x, tail.baseB.y));
    path.closeSubpath();
  }

  // 陰影畫在本體之前（會被不透明的白底蓋住一部分，露出來的就是外緣那一圈）。
  // 用的是同一條 path，所以思考泡泡的圓點串自動也有影子。
  const BubbleShadowSpec shadow = bubbleShadowSpec(shadow_);
  if (shadow.alpha > 0) {
    if (shadow.blur <= 0) {
      fillShadowPath(painter, path, shadow);
    } else {
      // 柔邊：先把剪影畫進一張同尺寸的圖再糊開。在視窗上重複描粗邊也能做出漸層，
      // 但那是一圈一圈的同心環，邊緣會看得出分層。
      // 糊一次的成本是「每句一次」而不是「每幀一次」，所以不必為它做快取。
      //
      // 量到的次數（Qt 6.8.3 / Windows 11）：220 ms 的淡入 14 個動畫 tick、**0 次**
      // paintEvent，淡出同樣 0 次；**同尺寸**的 setGeometry 120 次也是 0 次 —— 拖曳角色
      // 走的就是這一種；只有換了尺寸才重畫（20 次換尺寸得到 19 次）。而換句子必然換
      // 尺寸，於是「每句一次」。
      // 兩個但書，別把這組數字讀得比它實際的強：
      //  * 量的是**另外建的一個空視窗**，只帶了與這裡相同的 flag 與屬性，不是這個類別
      //    本身。所以哪天這裡多了子視窗、改了 WA_ 屬性、或在非 resize 的路徑上呼叫
      //    update()，這組數字就不再成立，得重量一次。
      //  * **機制沒有驗證過**：推測是 setWindowOpacity 在分層視窗上只重新混色而不發
      //    WM_PAINT，但沒有人去看過。決策靠的是上面那組次數，不是這個解釋。
      const qreal dpr = devicePixelRatioF();
      QImage layer((QSizeF(size()) * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
      layer.setDevicePixelRatio(dpr);
      layer.fill(Qt::transparent);
      {
        QPainter layerPainter(&layer);
        layerPainter.setRenderHint(QPainter::Antialiasing, true);
        fillShadowPath(layerPainter, path, shadow);
      }
      // **一定要無條件捨去**：半徑進位會讓糊開的距離超過位移，core/bubble_shape.h 那條
      // 「blur ≤ offset」就破了 —— dpr 1.25 時 lround(2×1.25) ＝ 3，糊開 6 device px
      // ＝ 4.8 邏輯 px，比 4 px 的位移還多，影子會往左上溢出去（而左上沒有留餘裕）。
      boxBlur(layer, static_cast<int>(std::floor(shadow.blur / 2 * dpr)));
      painter.drawImage(0, 0, layer);
    }
  }

  // 一次填滿並描框。思考泡泡的三顆圓點是各自獨立的子路徑，所以會各自帶到
  // 自己的外框 —— 那正是參考圖的樣子。
  painter.setBrush(kBubbleFill);
  painter.setPen(QPen(kBubbleInk, kOutlineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawPath(path);

  // 字在 buildLayout() 就排好了 —— 每一行的寬度與位置都躺在 textLayout_ 裡，
  // 座標相對橢圓中心，所以這裡把原點移到橢圓中心畫就好。
  // 在 paint 裡重排一次會讓每一幀都付一次排版的錢。
  painter.setPen(kBubbleInk);
  textLayout_.draw(&painter, body.center());
}

}  // namespace l2m
