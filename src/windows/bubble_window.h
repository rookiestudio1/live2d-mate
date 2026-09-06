#pragma once

// 對話氣泡：獨立的透明、點擊穿透、不搶焦點的視窗，跟著角色走。
//
// 為什麼是獨立視窗而不是畫在角色視窗裡：角色視窗會依 alpha 遮罩裁形狀
// （點擊穿透靠它），氣泡畫進去會被一起裁掉；而且氣泡常常要超出角色視窗的
// 400×600 範圍才擺得下。
//
// 版面同步量完再擺，第一次 show() 位置就是對的，
// 不會先以錯的位置顯示再搬過去而閃一下。
//
// 外型有兩種（BubbleStyle）：說話是漫畫式的橢圓＋月牙尾巴，思考是橢圓＋
// 一串遞減的小圓。幾何全在 core/bubble_shape.h 算好，這裡只負責把點餵給
// QPainterPath —— 那裡的標頭寫了尺寸關係，改動之前要先讀。
//
// 文字是「上下短、中間長」排進橢圓裡的：每一行允許的寬度不一樣，
// 由 core/ellipse_text_fit.h 算出來，再交給 QTextLayout 逐行斷行與定位。
// QPainter::drawText 只吃一個固定寬度，做不來這件事。
//
// 量測與繪製的分工要留意：**視窗大小不能依賴尾巴的水平位置**。tailX 是
// placeBubble() 依視窗大小算出來的，反過來依賴就成了環，所以尾巴那一側的
// 餘裕取的是與 tailX 無關的保守上界（bubbleTailExtent）。
//
// 陰影（設定 tts.bubbleShadow，三檔：無／硬邊／柔邊）畫在本體之前，用的是**同一條
// 路徑**位移之後再填一次，所以思考泡泡的圓點串自動也有影子。要留意的是視窗餘裕：
// 四邊的厚度是精算過的（尾巴那一側只剩 kOutlineMargin ＝ 4 px），影子直接畫會被裁掉。
// 作法是**版面與 placeBubble() 都照舊吃沒有陰影的尺寸**，只在最後 setGeometry 時把視窗
// 往右下各撐大 bubbleShadowExtent() —— 橢圓、尾巴與 tailX 的座標因此一格都不動，
// 換陰影檔位時氣泡不會跟著位移。
//
// 模型比例是從 characterBounds_ 反推的（寬 ÷ kBaseWidth），不另外接一條設定線 ——
// 角色視窗的大小本來就是 sizeForScale(scale) 算出來的，follow() 每次都會帶最新的。

#include <QColor>
#include <QFont>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTextLayout>
#include <QWidget>

#include <string>

#include "core/bubble_placement.h"
#include "core/bubble_shape.h"
#include "core/ellipse_text_fit.h"

class QVariantAnimation;

namespace l2m {

class BubbleWindow : public QWidget {
  Q_OBJECT

public:
  explicit BubbleWindow(QWidget* parent = nullptr);
  ~BubbleWindow() override;

  // 顯示一句話。會依角色目前的位置決定擺上面還是下面。
  // style 決定外型：思考（think 工具）用圓點串，其餘都是月牙尾巴。
  void showText(const QString& text, BubbleStyle style = BubbleStyle::Speech);
  // 淡出後才真的隱藏
  void hideBubble();

  // 角色視窗的位置／大小變了：跟著移動（內容不變）
  void follow(const QRect& characterBounds);
  // 角色被隱藏時氣泡也要跟著消失
  void setCharacterVisible(bool visible);
  void setAlwaysOnTop(bool enabled);
  // 語系決定字型挑選（日韓不能被套成中文字形）
  void setLocale(const std::string& locale);
  // 與角色的間距微調（px，設定裡的 tts.bubbleOffsetY）。上下鏡像，
  // 換算與夾限都在 core/bubble_placement.h，這裡只是把值帶進去。
  void setOffsetY(int px);
  // 模型在角色視窗裡的視覺上下緣（normalized 0..1，0=視窗頂）。
  // AppController 在每次載入模型、遮罩落地時量一次餵進來（見 core/bottom_align.h），
  // 於是氣泡貼的是模型的頭頂而不是視窗頂端。**刻意只在載入時量一次**：
  // 遮罩每 200ms 更新，跟著逐次重擺的話舉手、甩頭都會讓氣泡上下跳。
  // 換算與退回舊行為的規則在 core/bubble_placement.h 的 bubbleAnchorRect。
  void setModelExtent(double topNormalized, double bottomNormalized);
  // 陰影樣式（設定 tts.bubbleShadow）。換檔會改變視窗尺寸，所以要重新定位。
  void setShadow(BubbleShadow shadow);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  // 一次量完的版面。reposition() 算好放著，paintEvent() 直接用 ——
  // 在 paint 裡重量一次字會讓每一幀都付一次排版的錢。
  struct Layout {
    QSize window;           // 視窗大小（含餘裕）
    QSizeF ellipse;         // 橢圓本體
    double tailExtent = 0;  // 尾巴那一側的餘裕
  };

  // 量一次版面，順便把 textLayout_ 排好（行寬與位置都定案）。
  // **不是 const**：找最小橢圓的過程要反覆拿同一個 QTextLayout 斷行。
  Layout buildLayout();
  // 橢圓本體在視窗內的位置：尾巴在下時貼上緣，在上時貼下緣
  QRectF ellipseRect() const;
  void reposition();

  QString text_;
  QRect characterBounds_;
  bool characterVisible_ = true;
  // 建構時的 flag 就帶了 WindowStaysOnTopHint，預設值要跟它一致
  bool alwaysOnTop_ = true;
  BubbleSide side_ = BubbleSide::Above;
  BubbleStyle style_ = BubbleStyle::Speech;
  // 尾巴／圓點串要左右鏡射嗎（**角色在螢幕左半邊時為真** —— placeBubble 的判定是
  // centerX <= screenCenterX，理由與 kBubbleSideShift 的連動寫在 core/bubble_placement.h）
  bool mirrored_ = false;
  // 尾巴在視窗內的水平位置（px）。**固定是視窗正中，不追著角色跑** ——
  // 追著跑會抵銷掉 kBubbleSideShift 的斜度（算式與理由在 core/bubble_placement.cpp）
  int tailX_ = 0;
  // 使用者設定的間距微調（px，可為負）
  int offsetY_ = 0;
  // 模型的視覺上下緣（normalized）。預設 0/1 ＝ 整個視窗，也就是量到之前
  // 與量測失敗時的舊行為
  double modelTop_ = 0;
  double modelBottom_ = 1;
  // 陰影樣式。預設值要跟 config 的預設（"hard"）一致
  BubbleShadow shadow_ = BubbleShadow::Hard;
  Layout layout_;
  // 排好的文字，坐標相對橢圓中心
  QTextLayout textLayout_;
  // 上一次排版的輸入。**快取不能省** —— reposition() 在拖曳角色時是
  // 每一幀都跑的，而文字根本沒變；排版要反覆斷行好幾輪，沒快取就是
  // 把那些錢乘上幀數付出去。
  QString laidOutText_;
  BubbleStyle laidOutStyle_ = BubbleStyle::Speech;
  bool layoutValid_ = false;

  // 目前套用的語系；setLocale 靠它避免重複重建字型
  std::string locale_;
  QFont font_;
  QVariantAnimation* fade_ = nullptr;
};

}  // namespace l2m
