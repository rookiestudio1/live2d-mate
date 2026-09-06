#pragma once

// 手把上帶一顆「一鍵收合」按鈕的 QSplitter。
//
// 為什麼要自己做：QSplitter 本來就能把某一格拖到 0 寬（childrenCollapsible 預設
// 就是開的），但「拖到底」跟「按一下就收起來、再按一下回到原寬」是兩件事 ——
// 前者收得掉、卻沒有辦法原樣還原，使用者得自己重新拖到差不多的位置。
// 所以這裡多記一個 restoreWidth_：收合前的寬度存下來，展開時原樣放回去。
//
// 按鈕做在 QSplitterHandle 上（createHandle() 換成自家的子類別），而不是另外在
// 版面裡塞一條窄欄：手把本來就在那個位置、本來就會跟著拖曳移動，塞欄位等於自己
// 重做一次 QSplitter 的幾何。按鈕是手把的**子 widget**，所以它自己吃自己的滑鼠
// 事件，按鈕以外的手把區域照樣可以拖 —— 不必去攔截 mousePressEvent 分辨
//「這一下是要拖還是要按」。
//
// 收合的一律是**最後一格**（ViewerWindow 裡就是右邊的清單）。
//
// 使用者直接把手把拖到底時按鈕也要跟著翻面，所以 splitterMoved 一律重整箭頭方向；
// 同一個掛鉤順便把「還沒收合時的寬度」記起來，手動拖到 0 之後按按鈕也還原得回去。

#include <QSplitter>
#include <QSplitterHandle>
#include <QString>

class QResizeEvent;
class QToolButton;

class CollapsibleSplitter;

// 手把：原本的拖曳照舊，中央多一顆扁平的箭頭按鈕。
class CollapsibleSplitterHandle : public QSplitterHandle {
  Q_OBJECT

public:
  CollapsibleSplitterHandle(Qt::Orientation orientation, CollapsibleSplitter* parent);

  // 依目前的收合狀態換箭頭方向與提示字
  void refresh();

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  QToolButton* button_ = nullptr;
};

class CollapsibleSplitter : public QSplitter {
  Q_OBJECT

public:
  explicit CollapsibleSplitter(Qt::Orientation orientation, QWidget* parent = nullptr);

  // 最後一格現在是不是收合的（寬／高為 0）
  bool sideCollapsed() const;

  // 收合 ↔ 展開最後一格。展開時用收合前記下的寬度，沒有記錄才退回 sizeHint()。
  void toggleSide();

  // 按鈕的提示字。i18n 由呼叫端負責（這個類別不認識 l2m::i18n），
  // ViewerWindow::retranslate() 會餵進來。
  void setToggleTips(const QString& collapse, const QString& expand);
  QString toggleTip() const;

protected:
  QSplitterHandle* createHandle() override;

private:
  void refreshHandles();

  // 收合前最後一次看到的寬度；0 代表還沒記過
  int restoreWidth_ = 0;
  QString collapseTip_;
  QString expandTip_;
};
