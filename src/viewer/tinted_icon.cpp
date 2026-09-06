#include "tinted_icon.h"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QRectF>
#include <QSvgRenderer>

#include <utility>

namespace {

// 這個模式該用哪個顏色。每次畫都重問一次 QApplication::palette()，切換主題不必
// 通知這裡（重新指派 QIcon 的理由見標頭最後一段）。
QColor tintColor(QIcon::Mode mode) {
  const QPalette palette = QApplication::palette();
  // Selected 是 item view（QStyledItemDelegate）才會傳的模式，工具列這一頁走不到 ——
  // QMenu 也不傳它（QCommonStyle 的 CE_MenuItem 只在 Disabled／Active／Normal 之間選），
  // 所以連收進「»」延伸選單的那一項反白時也是 Active。順手接住而已，別照著它推論行為。
  const QPalette::ColorRole role = mode == QIcon::Selected ? QPalette::HighlightedText : QPalette::ButtonText;
  const QPalette::ColorGroup group = mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Normal;
  return palette.color(group, role);
}

class TintedSvgIconEngine : public QIconEngine {
public:
  explicit TintedSvgIconEngine(QString path) : path_(std::move(path)) {}

  void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
    // 刻意不直接畫進 painter：SourceIn 是整片重塗，painter 底下已經有的東西
    // （按鈕底色、凹下去的框）會被一起塗掉。一律先畫在自己的 pixmap 上再貼過去。
    painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, painter->device()->devicePixelRatioF()));
  }

  QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1.0); }

  QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State, qreal scale) override {
    if (size.isEmpty() || scale <= 0.0) return QPixmap();

    // svg 是向量的，這裡畫多少裝置像素就多清楚 —— 不必像預先烘好的點陣圖那樣
    // 在「猜一組尺寸」與「縮放糊掉」之間取捨。
    QPixmap pixmap(QSize(qMax(1, qRound(size.width() * scale)), qMax(1, qRound(size.height() * scale))));
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);

    // 每次重新解析。這九張都是單一 path、不到 600 位元組，而 QStyle 會把畫好的
    // 結果存進 QPixmapCache，同一個尺寸實際上只會走到這裡一次。
    QSvgRenderer renderer(path_);
    // 少了 Qt6::Svg（或它的 image plugin）就是走到這條 —— 回一張透明的空圖，
    // 跟原本 QIcon 讀不到 svg 的症狀一樣安靜，所以連結相依不能拿掉。
    if (!renderer.isValid()) return pixmap;

    // 設了 devicePixelRatio 之後 painter 的座標是裝置無關的，所以這裡一律用 size。
    const QRectF bounds(QPointF(0, 0), QSizeF(size));
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter, bounds);

    // 整片重塗。SourceIn 只保留目的地的 alpha，於是「圖形的形狀」原封不動、顏色
    // 整個換掉 —— svg 裡原本寫的是什麼顏色因此完全不重要。
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(bounds, tintColor(mode));
    painter.end();
    return pixmap;
  }

  QIconEngine* clone() const override { return new TintedSvgIconEngine(path_); }

  QString key() const override { return QStringLiteral("l2m-tinted-svg"); }

private:
  QString path_;
};

}  // namespace

QIcon tintedIcon(const QString& path) { return QIcon(new TintedSvgIconEngine(path)); }
