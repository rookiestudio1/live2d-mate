// model3.json 的 Layout 能不能信。每一條分支錯了都只會表現成「某一隻模型的構圖怪怪的」，
// 沒有任何錯誤訊息，所以實際踩到的兩隻直接用真實數值釘住。
#include <QtTest>

#include <vector>

#include "core/layout_fit.h"

using namespace l2m;

class TestLayoutFit : public QObject {
  Q_OBJECT

private slots:
  // 沒有 Layout 的模型（手邊 29 隻裡的 27 隻）什麼都不該被否決
  void emptyLayoutChangesNothing() {
    const LayoutPlan plan = planLayout({}, 1.0, 1.7561, 0.0, 0.0);
    QVERIFY(!plan.specifiesSize);
    QVERIFY(!plan.specifiesPosition);
    QVERIFY(plan.usable);
  }

  // 《原神》可莉的實際 Layout。畫布 1.0 × 1.7561 單位、原點在正中央。
  // height 2.6 → 縮放 1.4806，畫布高變成 2.6；top 0.3（**蓋掉前面的 bottom 2.0**）
  // → 畫布落在 [-1.0, 1.6]，上緣爆出視野 0.6。症狀就是「頭被裁掉」。
  //
  // 注意這一支**只釘得住整體結果**：height 2.6 自己就讓半高變成 1.3 而超標，
  // 位置鍵刪掉答案一樣。位置算式本身由底下 appliesVerticalPositionKeys()／
  // appliesHorizontalPositionKeys()／lastKeyOnTheSameAxisWins() 分別釘。
  void rejectsKeliLayout() {
    const std::vector<LayoutEntry> layout = {{"height", 2.6}, {"bottom", 2.0}, {"top", 0.3}};
    const LayoutPlan plan = planLayout(layout, 1.0, 1.7561, 0.0, 0.0);
    QVERIFY(plan.specifiesSize);
    QVERIFY(plan.specifiesPosition);
    QVERIFY(!plan.usable);
  }

  // 《原神》派蒙的實際 Layout。畫布 1.0 × 1.3968、原點在正中央。
  // height 2.2 → 畫布高 2.2；bottom 2.3 → 平移 2.3 - 2.2 = 0.1
  // → 畫布 [-1.0, 1.2]。上下都超出，症狀是「上下被裁掉」。
  void rejectsPaimonLayout() {
    const std::vector<LayoutEntry> layout = {{"height", 2.2}, {"bottom", 2.3}};
    const LayoutPlan plan = planLayout(layout, 1.0, 1.3968, 0.0, 0.0);
    QVERIFY(plan.specifiesSize);
    QVERIFY(plan.specifiesPosition);
    QVERIFY(!plan.usable);
  }

  // 同一軸寫了兩個鍵時**後面那個說了算**（Framework 的 TranslateY 是絕對指派）。
  // 把可莉的 top 與 bottom 對調，最後生效的變成 bottom 2.0 → 平移 -0.6
  // → 畫布 [-1.9, 0.7]，這次是下緣爆出去。順序沒照 model3.json 走的話這兩種
  // 情況會算成同一個答案
  void lastKeyOnTheSameAxisWins() {
    // 畫布高 2.0、height 2.0 → 縮放 1、畫布半高 1。
    // top 0.0 → 平移 0（畫布 [-1, 1]，剛好貼齊）；bottom 2.5 → 平移 0.5（畫布 [-0.5, 1.5]，爆出去）。
    // 同一組鍵值只是換順序，答案就必須相反 —— 照 map 排序而不是照出現順序讀的實作會在這裡兩邊一樣
    QVERIFY(planLayout({{"height", 2.0}, {"bottom", 2.5}, {"top", 0.0}}, 1.0, 2.0, 0.0, 0.0).usable);
    QVERIFY(!planLayout({{"height", 2.0}, {"top", 0.0}, {"bottom", 2.5}}, 1.0, 2.0, 0.0, 0.0).usable);
  }

  // `width` 走的是**畫布寬**，`height` 走畫布高。兩者拿錯除數是最容易寫錯的一行，
  // 而上面那些測試全都只用 height，一個都抓不到。
  // 畫布 2.0 × 1.0：width 2.0 → 縮放 1（半寬剛好 1，採用）；
  // 誤用畫布高當除數的話縮放變成 2、半寬 2，就會被否決
  void widthKeyDividesByCanvasWidth() {
    QVERIFY(planLayout({{"width", 2.0}}, 2.0, 1.0, 0.0, 0.0).usable);
    QVERIFY(!planLayout({{"width", 2.2}}, 2.0, 1.0, 0.0, 0.0).usable);
  }

  // 垂直位置的三個鍵。刻意挑一個**大小本身沒問題**的 Layout（畫布 2.0、height 1.0
  // → 半高 0.5），這樣答案就完全由平移決定 —— 否則像可莉那樣光是 height 就超標，
  // 位置算式寫成什麼都驗不出來
  void appliesVerticalPositionKeys() {
    // top 是絕對指派：0.4 + 0.5 = 0.9 進得去，0.9 + 0.5 = 1.4 出界
    QVERIFY(planLayout({{"height", 1.0}, {"top", 0.4}}, 1.0, 2.0, 0.0, 0.0).usable);
    QVERIFY(!planLayout({{"height", 1.0}, {"top", 0.9}}, 1.0, 2.0, 0.0, 0.0).usable);
    // bottom 要減掉整個縮放後的高度：1.0 - 1.0 = 0 進得去，1.9 - 1.0 = 0.9 出界。
    // 漏掉那一項的話兩個都會算成 top 而答案相反
    QVERIFY(planLayout({{"height", 1.0}, {"bottom", 1.0}}, 1.0, 2.0, 0.0, 0.0).usable);
    QVERIFY(!planLayout({{"height", 1.0}, {"bottom", 1.9}}, 1.0, 2.0, 0.0, 0.0).usable);
    // center_y 要**減**半個高度：-0.4 - 0.5 = -0.9 → 出界。
    // 正負號寫反（-0.4 + 0.5 = 0.1）會變成進得去
    QVERIFY(!planLayout({{"height", 1.0}, {"center_y", -0.4}}, 1.0, 2.0, 0.0, 0.0).usable);
    QVERIFY(planLayout({{"height", 1.0}, {"center_y", 0.0}}, 1.0, 2.0, 0.0, 0.0).usable);
  }

  // 水平位置的四個鍵。畫布 1.0 × 1.0、width 1.0 → 半寬半高都是 0.5，
  // 垂直方向永遠安全，所以答案只由水平平移決定
  void appliesHorizontalPositionKeys() {
    // left／x 都是絕對指派
    QVERIFY(planLayout({{"width", 1.0}, {"left", 0.4}}, 1.0, 1.0, 0.0, 0.0).usable);
    QVERIFY(!planLayout({{"width", 1.0}, {"left", 0.6}}, 1.0, 1.0, 0.0, 0.0).usable);
    QVERIFY(!planLayout({{"width", 1.0}, {"x", 0.6}}, 1.0, 1.0, 0.0, 0.0).usable);
    // right 要減掉整個縮放後的寬度：1.0 - 1.0 = 0 進得去。
    // 寫成加的話變成 2.0 而出界
    QVERIFY(planLayout({{"width", 1.0}, {"right", 1.0}}, 1.0, 1.0, 0.0, 0.0).usable);
    QVERIFY(!planLayout({{"width", 1.0}, {"right", 0.4}}, 1.0, 1.0, 0.0, 0.0).usable);
    // center_x 要減半個寬度：-0.4 - 0.5 = -0.9 → 出界；正負號寫反會變成進得去
    QVERIFY(!planLayout({{"width", 1.0}, {"center_x", -0.4}}, 1.0, 1.0, 0.0, 0.0).usable);
    QVERIFY(planLayout({{"width", 1.0}, {"center_x", 0.0}}, 1.0, 1.0, 0.0, 0.0).usable);
  }

  // 「填滿視野」這種最常見的寫法要採用 —— 剛好貼齊邊界，浮點誤差不能把它擋掉
  void acceptsExactFullViewLayout() {
    const LayoutPlan plan = planLayout({{"height", 2.0}}, 1.0, 1.7561, 0.0, 0.0);
    QVERIFY(plan.specifiesSize);
    QVERIFY(!plan.specifiesPosition);
    QVERIFY(plan.usable);
  }

  // 作者刻意縮小一點也要照做
  void acceptsSmallerThanViewLayout() { QVERIFY(planLayout({{"height", 1.8}}, 1.0, 1.7561, 0.0, 0.0).usable); }

  // 只寫大小、但大過視野 → 畫布 [-1.1, 1.1]，一樣會被切，一樣不採用
  void rejectsOversizedSizeOnlyLayout() {
    const LayoutPlan plan = planLayout({{"height", 2.2}}, 1.0, 1.7561, 0.0, 0.0);
    QVERIFY(plan.specifiesSize);
    QVERIFY(!plan.specifiesPosition);
    QVERIFY(!plan.usable);
  }

  // 只寫位置沒寫大小時，起始縮放是 CubismModelMatrix 建構子的 SetHeight(2.0)，
  // **不是 1**。畫布高 4 單位 → 縮放 0.5 → 畫布高剛好 2，y=0 就是貼齊視野。
  // 誤用 1 當起始值的實作會算出畫布高 4 而把這一組判成不能用
  void positionOnlyLayoutStartsFromDefaultScale() {
    const LayoutPlan plan = planLayout({{"y", 0.0}}, 1.0, 4.0, 0.0, 0.0);
    QVERIFY(!plan.specifiesSize);
    QVERIFY(plan.specifiesPosition);
    QVERIFY(plan.usable);
  }

  // 原點不在畫布正中央的模型（實測 LiveroiD：畫布中心在原點上方 0.5164）——
  // Framework 的位置算式漏的就是這一項。少算的話這一組會被誤判成「剛好貼齊、可以用」，
  // 畫面上則是整個畫布往上偏了半個身子
  void accountsForOffCenterCanvasOrigin() {
    QVERIFY(planLayout({{"height", 2.0}}, 1.0, 1.7212, 0.0, 0.0).usable);
    QVERIFY(!planLayout({{"height", 2.0}}, 1.0, 1.7212, 0.0, 0.5164).usable);
  }

  // 橫幅畫布配 Layout 一定否決：它本來就塞不進正方形視野，
  // 退回「依視窗長寬比挑受限維度」的 fit 才是對它好的結果
  void rejectsLandscapeCanvasLayout() { QVERIFY(!planLayout({{"height", 2.0}}, 3.0, 1.0, 0.0, 0.0).usable); }

  // 畫布尺寸讀不到（PixelsPerUnit 是 0）時不否決任何東西 —— 不知道就別亂動
  void keepsLayoutWhenCanvasSizeUnknown() {
    QVERIFY(planLayout({{"height", 2.6}, {"top", 0.3}}, 0.0, 0.0, 0.0, 0.0).usable);
    QVERIFY(planLayout({{"height", 2.6}}, 1.0, -1.0, 0.0, 0.0).usable);
  }

  // 認得的鍵之外一律略過，不能讓未知的鍵改變縮放或平移
  void ignoresUnknownKeys() {
    const LayoutPlan plan = planLayout({{"height", 2.0}, {"depth", 99.0}}, 1.0, 1.7561, 0.0, 0.0);
    QVERIFY(plan.specifiesSize);
    QVERIFY(!plan.specifiesPosition);
    QVERIFY(plan.usable);
  }
};

QTEST_GUILESS_MAIN(TestLayoutFit)
#include "test_layout_fit.moc"
