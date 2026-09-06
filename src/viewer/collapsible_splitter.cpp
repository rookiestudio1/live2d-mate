#include "collapsible_splitter.h"

#include <QResizeEvent>
#include <QToolButton>

#include <algorithm>

namespace {

// 手把寬度。預設的 5~6 px 放不下箭頭按鈕，也不好按。
constexpr int kHandleWidth = 12;
// 按鈕沿著手把方向的長度上限；手把比這短時就整條都是按鈕
constexpr int kButtonLength = 48;
// 展開時至少要留給前一格的空間 —— 還原的寬度比視窗還大時，
// 不能把畫布壓成 0（那等於把收合方向反過來做一次）
constexpr int kKeepForPrevious = 80;

}  // namespace

CollapsibleSplitterHandle::CollapsibleSplitterHandle(Qt::Orientation orientation, CollapsibleSplitter* parent) : QSplitterHandle(orientation, parent) {
  button_ = new QToolButton(this);
  button_->setAutoRaise(true);
  // 按鈕不吃焦點：這是一個純滑鼠的小開關，搶走焦點只會讓清單的鍵盤操作斷掉
  button_->setFocusPolicy(Qt::NoFocus);
  // 手把整條是分隔游標，按鈕上要換回一般箭頭，才看得出「這裡是按的不是拖的」
  button_->setCursor(Qt::ArrowCursor);
  connect(button_, &QToolButton::clicked, parent, &CollapsibleSplitter::toggleSide);
  refresh();
}

void CollapsibleSplitterHandle::refresh() {
  auto* owner = qobject_cast<CollapsibleSplitter*>(splitter());
  if (!owner) return;
  const bool collapsed = owner->sideCollapsed();
  // 箭頭指的是「按下去之後那一格會往哪邊跑」
  if (orientation() == Qt::Horizontal) {
    button_->setArrowType(collapsed ? Qt::LeftArrow : Qt::RightArrow);
  } else {
    button_->setArrowType(collapsed ? Qt::UpArrow : Qt::DownArrow);
  }
  button_->setToolTip(owner->toggleTip());
}

void CollapsibleSplitterHandle::resizeEvent(QResizeEvent* event) {
  QSplitterHandle::resizeEvent(event);
  // 沿著手把方向置中，橫向佔滿整條寬度：按鈕以外的地方仍然拖得動
  if (orientation() == Qt::Horizontal) {
    const int length = std::min(kButtonLength, height());
    button_->setGeometry(0, (height() - length) / 2, width(), length);
  } else {
    const int length = std::min(kButtonLength, width());
    button_->setGeometry((width() - length) / 2, 0, length, height());
  }
}

CollapsibleSplitter::CollapsibleSplitter(Qt::Orientation orientation, QWidget* parent) : QSplitter(orientation, parent) {
  setHandleWidth(kHandleWidth);
  // 使用者自己把手把拖到底也算收合 —— 箭頭要跟著翻面，
  // 順便把「還沒收到底時的寬度」記下來，之後按按鈕才還原得回去。
  connect(this, &QSplitter::splitterMoved, this, [this](int, int) {
    const QList<int> current = sizes();
    if (!current.isEmpty() && current.last() > 0) restoreWidth_ = current.last();
    refreshHandles();
  });
}

QSplitterHandle* CollapsibleSplitter::createHandle() {
  // 第 0 個手把（第一格之前的那一個）永遠是隱藏的，它的按鈕因此也點不到，
  // 不必特別排除。
  return new CollapsibleSplitterHandle(orientation(), this);
}

bool CollapsibleSplitter::sideCollapsed() const {
  const QList<int> current = sizes();
  return current.size() >= 2 && current.last() == 0;
}

void CollapsibleSplitter::setToggleTips(const QString& collapse, const QString& expand) {
  collapseTip_ = collapse;
  expandTip_ = expand;
  refreshHandles();
}

QString CollapsibleSplitter::toggleTip() const { return sideCollapsed() ? expandTip_ : collapseTip_; }

void CollapsibleSplitter::toggleSide() {
  QList<int> current = sizes();
  if (current.size() < 2) return;
  const int last = current.size() - 1;

  if (current[last] > 0) {
    restoreWidth_ = current[last];
    current[last - 1] += current[last];
    current[last] = 0;
  } else {
    int width = restoreWidth_ > 0 ? restoreWidth_ : (orientation() == Qt::Horizontal ? widget(last)->sizeHint().width() : widget(last)->sizeHint().height());
    // 還原的寬度比現有空間還大時寧可少還一點，也不能把前一格壓沒
    width = std::clamp(width, 1, std::max(1, current[last - 1] - kKeepForPrevious));
    current[last - 1] -= width;
    current[last] = width;
  }

  setSizes(current);
  refreshHandles();
}

void CollapsibleSplitter::refreshHandles() {
  for (int i = 0; i < count(); ++i) {
    if (auto* h = qobject_cast<CollapsibleSplitterHandle*>(handle(i))) h->refresh();
  }
}
