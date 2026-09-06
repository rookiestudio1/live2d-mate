#pragma once

// 拖曳搖晃：把「視窗實際套用的位移樣本」濾成平滑速度，再換算成
// (A) 物理求值前的頭／身體角度偏移 與 (B) CubismPhysics 的風力向量。
//
// 為什麼放 core：與 core/parameter_tracks.h 同一個切法 —— 時間一律由呼叫端
// 以毫秒傳入（AppController 用 QElapsedTimer，測試直接餵固定時間點），
// 不碰 Qt GUI/GL，衰減曲線與 clamp 才驗得到。
//
// 濾波模型刻意用「EMA + 指數衰減」而不是彈簧-阻尼：回彈震盪是模型
// physics3.json 作者調好的本職，這裡只負責給物理一個平滑、放手後自然
// 歸零的輸入訊號。衰減以「距最後一次輸入的閒置時間」現算，不寫回狀態，
// 所以放開滑鼠後即使沒有任何事件也會歸零 —— 不需要拖曳結束通知，
// 而 mouseReleaseEvent 對非 tap 的拖曳本來就不往下傳（character_window.cpp）。

#include <algorithm>

namespace l2m {

// 調校常數集中一處，方便實機實驗；各值的理由寫在行尾
struct DragSwingTuning {
  double emaTauMs = 60;            // 速度 EMA 時間常數；滑鼠事件 8~16ms 一發，60ms 剛好抹平抖動
  double releaseTauMs = 180;       // 無新樣本時的衰減時間常數；不到 1 秒歸零，殘餘震盪交給物理
  double newRoundGapMs = 250;      // 輸入間隔超過此值視為新一輪拖曳，舊速度先衰減到位
  double assumedDtMs = 16;         // 第一筆樣本沒有前一筆可算 dt，用典型幀間隔充當
  double maxSpeedPxPerSec = 2500;  // 速度模長上限；快甩實測約 1500~2000
  double deadZonePxPerSec = 25;    // 低於此值輸出 inactive，完全不干擾模型
  // 角度換算（度 / (px/s)）與各自貢獻上限（度）。
  // 呼吸底噪是 AngleX ±7.5 / BodyAngleX ±2，上限取其 2~3 倍才看得出效果。
  double angleXPerPxPerSec = 0.012;
  double angleXMaxDeg = 20;
  double angleYPerPxPerSec = 0.008;
  double angleYMaxDeg = 10;
  double angleZPerPxPerSec = 0.006;
  double angleZMaxDeg = 10;
  double bodyXPerPxPerSec = 0.003;
  double bodyXMaxDeg = 6;
  // 風力換算（physics rig 位置空間 / (px/s)）。量級依模型差異大（典型 ±10~100），
  // 這裡給保守起點，實機調整。實機調校後：風向與拖曳同向（相對風的 -v 在
  // 實際模型上看起來方向不對）、強度逐輪調降（0.02 → 0.012 → 0.0005 ——
  // 頭髮對風極敏感）。windMax 在目前的強度下觸不到（速度上限 2500 × 0.0005
  // = 1.25），留著當保險絲；測試的風力期望值一律由這組常數導出。
  // 注意這只是「拖曳來源自己的」上限 —— 與環境風相加後的總風力另有一顆
  // 保險絲（core/ambient_wind.h 的 kWindMax，clamp 在 model_controller 相加之後）。
  double windPerPxPerSec = 0.0005;
  double windMax = 25;
};

class DragSwing {
public:
  explicit DragSwing(DragSwingTuning tuning = {}) : tuning_(tuning) {}

  // 餵一次「實際套用」的視窗位移（px，螢幕座標，y 向下為正）。
  // nowMs 為單調毫秒。dt 只看上一筆輸入樣本，不受 sample() 影響 ——
  // 渲染幀與滑鼠事件交錯時，瞬時速度才不會被過短的 dt 灌水。
  void addSample(double dxPx, double dyPx, double nowMs);

  // 這一刻要疊加給模型的貢獻
  struct Output {
    double angleXDeg = 0;  // ParamAngleX 的疊加貢獻（度）
    double angleYDeg = 0;  // ParamAngleY
    double angleZDeg = 0;  // ParamAngleZ
    double bodyXDeg = 0;   // ParamBodyAngleX
    double windX = 0;      // CubismPhysics Options.Wind（rig 空間）
    double windY = 0;
    bool active = false;  // 速度低於死區時 false，呼叫端可整段跳過
  };

  // 每幀取樣。衰減是閒置時間的純函數，同一 nowMs 重複呼叫結果相同。
  Output sample(double nowMs);

  // 切模型／關開關時歸零
  void reset();

private:
  DragSwingTuning tuning_;
  double vx_ = 0;  // EMA 濾波後速度（px/s）
  double vy_ = 0;
  double lastInputMs_ = -1;  // 最後一筆 addSample 的時間；-1 = 尚未有輸入
};

}  // namespace l2m
