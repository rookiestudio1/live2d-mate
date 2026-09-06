#pragma once

// 啟動／換模型時的等待畫面：圓角卡片 + 底部細光條。
//
// 只在 --splash 子行程裡使用（見 app/splash_process.h）。主行程不建這個視窗 ——
// 模型載入會把主行程的 GUI 執行緒整個卡住數秒（光是 CreateRenderer 的 shader
// 編譯就 2.3 秒），畫在那邊的動畫一定是凍的。這裡跑在自己的行程，
// 有自己的訊息迴圈，主行程卡多久都跟它無關。
//
// 為什麼不用 QSplashScreen：它沒有圓角、沒有淡入淡出、沒有進度指示，
// finish(QWidget*) 的收尾條件也對不上「等主行程斷線」。要覆寫的比繼承到的多。
//
// 光條刻意是 indeterminate 的：主行程在 CreateRenderer 那 2.3 秒裡完全沒有
// 事件迴圈，就算設計了進度回傳的協定，那段期間也一個數字都送不出來。
// 與其做一條會停住的 determinate 進度條，不如讓它一直掃 —— 反正真正要
// 傳達的訊息只有「還在動，沒當掉」。

#include <QElapsedTimer>
#include <QPixmap>
#include <QRect>
#include <QWidget>

class QTimer;
class QVariantAnimation;

namespace l2m {

class SplashWindow : public QWidget {
  Q_OBJECT

public:
  // Startup：程式啟動，整張 520×200 的 banner，游標所在螢幕置中。
  // Compact：執行期換模型，只有一條光條的小卡片，貼在角色身上。
  //          換模型是使用者主動的小操作，注意力就在角色上，
  //          彈一張大圖到螢幕正中央太重，視線還得跑一趟。
  enum class Mode { Startup, Compact };

  explicit SplashWindow(Mode mode = Mode::Startup, QWidget* parent = nullptr);
  ~SplashWindow() override;

  // Compact 模式：置中於這個螢幕矩形（主行程傳來的角色視窗 geometry）。
  // 給空矩形或不呼叫就退回「游標所在螢幕置中」。
  void setAnchor(const QRect& screenRect) { anchor_ = screenRect; }

  // 定位、顯示、開始淡入與光條動畫
  void begin();

  // 收尾：撐滿最短顯示時間後淡出，淡完發 closed()。
  // one-shot —— 逾時保險與主行程斷線可能同時到，重複呼叫無效。
  void finish();

signals:
  // 淡出真的跑完、視窗已經 hide 了才發。子行程靠它決定什麼時候結束自己。
  void closed();

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void positionSelf();
  // 把卡片（Startup 是裁圓角的圖，Compact 是純色底）連陰影一起快取成 pixmap。
  // dpr 沒變就直接 return。
  void rebuildCard();
  QSize bodySize() const;
  QRectF bodyRect() const;
  QRectF trackRect() const;
  int cornerRadius() const;
  int minVisibleMs() const;
  int fadeMs() const;

  const Mode mode_;
  QRect anchor_;
  QPixmap card_;         // 已套圓角＋陰影的整張卡片（含 devicePixelRatio）
  qreal cardDpr_ = 0.0;  // card_ 是用哪個 dpr 算的；0 = 還沒算過
  QElapsedTimer clock_;  // 光條相位與最短顯示時間共用同一個時鐘
  QTimer* tick_ = nullptr;
  QVariantAnimation* fade_ = nullptr;
  bool finished_ = false;
};

}  // namespace l2m
