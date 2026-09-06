#pragma once

// 指標事件序列 → 手勢（Tap / MultiTap / LongPress / Pet）。
//
// 「撫摸」定義成**放開狀態下，指標在同一個部位上來回滑動** —— 不是按著拖：
// 按著拖已經被 dragMove 佔走（那是移動視窗），兩者搶同一組事件只會互相干擾。
// 放開的 hover 撫摸還有一個好處：pollCursor() 本來就在追全域游標，
// 連 click-through 開著時都感覺得到（形狀視窗會吃掉 hover 事件，游標輪詢不會）。
//
// **單擊是遞延判定的**：放開之後先掛起，超過 multiTapWindowMs（app 層以 OS
// 雙擊間隔覆寫）沒有下一擊才發 Tap；時窗內連滿 multiTapCount 下則立刻發
// MultiTap 並吞掉掛起的單擊。這是連點與單擊表演不打架的關鍵 ——
// 以前每一下都立即播點擊動作，連點三下會疊三個動作再加一個 MultiTap 反應。
//
// **按壓位移用螢幕座標（screenX/screenY）算**，不能用視窗內座標：
// dragMove 期間視窗跟著游標跑，視窗內座標幾乎不動，用它判定會把
// 每一次拖曳的放開都誤判成 tap。
//
// 時間全由樣本帶入（nowMs），不碰 Qt —— 手勢的時窗與門檻才測得到。
// 事件驅動之外要靠 tick() 推進時間：長按與遞延單擊等的都是「沒有新事件」
// 那段時間，由既有的 40ms 游標輪詢帶動。

#include <string>
#include <vector>

#include "hit_area_semantics.h"

namespace l2m {

// 一筆指標樣本。press/release 由視窗事件餵，hover 由游標輪詢餵。
struct PointerSample {
  double x = 0;  // 視窗內像素座標（撫摸的來回滑動用這組）
  double y = 0;
  double screenX = 0;  // 螢幕座標（按壓位移用這組 —— 見檔頭 dragMove 的理由）
  double screenY = 0;
  double nowMs = 0;
  bool pressed = false;
  bool inside = false;             // 在角色的命中區上（hitTest 有結果）
  std::vector<std::string> areas;  // hitTest 命中的 area 名稱
};

enum class GestureKind { None, Tap, MultiTap, LongPress, Pet };

struct Gesture {
  GestureKind kind = GestureKind::None;
  BodyPart part = BodyPart::Unknown;
  int taps = 0;  // Tap＝時窗內累計的擊數（1 或 2）；MultiTap＝multiTapCount
  // 按下當下命中的 area 名稱（Tap / MultiTap 帶回）——
  // 點擊動作的挑選（pickTapMotion）吃原始名稱，BodyPart 對它不夠用
  std::vector<std::string> areas;
  // 按下當下的**視窗內**像素座標（Tap / MultiTap 帶回）。
  // 99% 的模型沒有填 HitAreas，areas 因此永遠是空的，「點在頭還是身上」
  // 只能靠這個座標去比對角色的外接框（core/model_regions.h）。
  // 刻意不是螢幕座標：那組是給位移門檻用的（見檔頭 dragMove 的理由）。
  double x = 0;
  double y = 0;
};

class GestureDetector {
public:
  struct Options {
    double tapMaxMovePx = 5;  // 沿用 interaction_logic.h 的點擊門檻
    // 兩下之間的最長間隔，同時也是單擊的遞延時間。
    // 預設 400 只是測試用的定值；app 層一律以 OS 的雙擊間隔覆寫
    //（QStyleHints::mouseDoubleClickInterval，Windows 預設 500）——
    // 使用者在系統設定調過雙擊速度，桌寵的判定就該跟著走。
    double multiTapWindowMs = 400;
    int multiTapCount = 3;  // 連點幾下算 MultiTap（2 會誤傷平常的連續點擊）
    double longPressMs = 700;
    double petMinTravelPx = 40;  // 來回滑動的最短總行程
    int petMinReversals = 3;     // 至少折返幾次（沒有折返只是滑過去）
    double petIdleMs = 600;      // 停這麼久算一次撫摸結束，重新累計
  };
  // 不寫 `Options options = {}` 的預設引數：預設引數屬於外層類別的 complete-class
  // context，會在類別本身還沒定義完成時就要用到 Options 的成員預設值 ——
  // clang 直接報錯（MSVC 放行），所以拆成兩個建構子。
  GestureDetector() = default;
  explicit GestureDetector(Options options) : options_(options) {}

  // 餵一筆樣本；大多數樣本回 None
  Gesture feed(const PointerSample& sample);
  // 沒有事件也要推進時間（長按成立、撫摸逾時）
  Gesture tick(double nowMs);

  void reset();

private:
  Gesture petFrom(const PointerSample& sample);
  void resetPress();
  void resetPendingTap();
  void resetPet();

  Options options_;

  // 按壓追蹤（Tap / MultiTap / LongPress）
  bool pressing_ = false;
  bool longPressFired_ = false;
  double pressStartMs_ = 0;
  double pressX_ = 0;  // 螢幕座標（見檔頭）
  double pressY_ = 0;
  double pressMovedPx_ = 0;
  BodyPart pressPart_ = BodyPart::Unknown;
  std::vector<std::string> pressAreas_;  // 按下當下的命中區，發手勢時帶回
  double pressLocalX_ = 0;               // 按下當下的視窗內座標，同樣帶回（部位推算用）
  double pressLocalY_ = 0;
  // 遞延中的單擊：tapCount_ > 0 表示有擊數掛著，等時窗過了才發 Tap
  int tapCount_ = 0;
  double lastTapMs_ = -1;
  BodyPart pendingTapPart_ = BodyPart::Unknown;
  std::vector<std::string> pendingTapAreas_;
  double pendingTapX_ = 0;
  double pendingTapY_ = 0;

  // 撫摸追蹤（放開狀態的 hover）
  bool petActive_ = false;
  bool petFired_ = false;
  BodyPart petPart_ = BodyPart::Unknown;
  double petLastX_ = 0;
  double petLastMoveMs_ = 0;
  double petTravelPx_ = 0;
  int petDirection_ = 0;  // -1 / 0 / +1，換向即折返
  int petReversals_ = 0;
};

}  // namespace l2m
