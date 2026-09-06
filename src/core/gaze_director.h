#pragma once

// 視線追蹤的「該不該看游標」決策 —— 輸出一個 0..1 的權重，呼叫端拿去在
// 「正前方」與「游標」之間插值：focus = center + (cursor - center) * weight。
//
// 為什麼是權重而不是開關：
// 舊版是「lookAt 開著就每輪把游標座標直接寫進 focus」，視線永遠黏著滑鼠。
// 改成「滑鼠動才追、靜止 holdMs 後回正」之後，兩個方向都必須平滑，而
// 「說話期間正視前方」也要壓住追蹤 —— 這兩件事都在寫同一個 focus。
// 各自實作的話會在 25 Hz 的游標輪詢上逐幀互相覆寫（原本 speaking_gaze.h 的
// FaceFront／Restore 是硬切 focus），所以把所有「該不該看游標」的理由
// —— 滑鼠靜止、說話中、MCP 釘住、總開關關閉 —— 全部匯進這一個權重。
//
// 與 core/drag_swing.h、core/ambient_wind.h 同一個切法：純邏輯、不碰 Qt/GL，
// 時間由呼叫端以毫秒傳入，同一組輸入結果相同，測試才驗得到。
//
// 為什麼還要自己做緩動（Cubism 不是有了嗎）：
// CubismTargetPoint（_dragManager）自帶約 0.35 秒的慣性，但那是「頭轉過去的
// 速度」，參數寫死在 Framework 內改不了。0.35 秒的回正對「慢慢失去興趣」來說
// 太快、太機械，讀起來像被打斷而不是放空。兩層疊起來只會更柔。
//
// 從 speaking_gaze.h 繼承下來、不能弄丟的三條：
//  * 以 **suppress 的組合**（而非 speaking 的邊緣）為準。suppressed 只帶
//    「說話中 && speakFacingFront」，enabled 與 pinned 分開餵 —— 這樣
//    「說話中被釘住」「說話中把開關關掉」都自動落在正確分支，不用另外列舉。
//  * 權重 0 只要把 focus 設回視窗中心即可。視線在 ModelController::update()
//    第 5 步是 AddParameterValue，focus 歸零時貢獻自然歸零，
//    呼吸／動作／頭髮物理全部保留；與 look_at(reset) 走同一條路。
//  * **本狀態機必須排在 pollCursor 的 2px 死區 return 之前**。游標靜止時
//    死區以下整段不執行，回正動畫排在後面就永遠不會推進 ——
//    舊版「說完話恢復追蹤」踩過的就是這個坑。

namespace l2m {

// 手感常數。照 AmbientWindTuning／DragSwingTuning 的慣例集中一個 struct
// 並寫明理由，刻意不進 config：這是調校，不是使用者決策。
struct GazeDirectorTuning {
  // 游標靜止多久算「不再注意」。太短會在打字的字間空檔就回正、看起來很躁；
  // 3 秒約等於「換個視窗想一下」的停頓，還在注意力範圍內。
  double holdMs = 3000;
  // 正前方 → 追蹤的過渡時間
  double engageMs = 500;
  // 追蹤 → 正前方的過渡時間。刻意比 engage 慢四倍：
  // 「被吸引」該快、「失去興趣」該慢，不對稱才像有情緒。
  double releaseMs = 2000;
  // 單輪 dt 上限。休眠喚醒、視窗久未顯示、或除錯中斷之後，nowMs 會一次跳
  // 好幾秒，不夾住就會在一輪內走完整條曲線 —— 畫面上就是瞬間硬切，
  // 平滑白做。250 ms 是 40 ms 輪詢的 6 倍餘裕，正常抖動碰不到。
  double maxStepMs = 250;
};

struct GazeInput {
  double nowMs = 0;
  bool enabled = true;       // interaction.lookAt（視線追蹤總開關）
  bool pinned = false;       // MCP look_at 釘住中
  bool suppressed = false;   // 說話中 && interaction.speakFacingFront
  bool cursorMoved = false;  // 本輪位移是否達 kCursorThresholdPx
};

struct GazeOutput {
  bool write = false;  // 這一輪要不要寫 focus（false = 一個位元組都不要碰）
  double weight = 0;   // 0 = 正前方，1 = 游標；已過 smoothstep
};

class GazeDirector {
public:
  explicit GazeDirector(GazeDirectorTuning tuning = {}) : tuning_(tuning) {}

  // 每輪游標輪詢呼叫一次
  GazeOutput tick(const GazeInput& in);

  // 未經 smoothstep 的線性進度（測試與診斷用；實際套用的是 GazeOutput::weight）
  double weight() const { return weight_; }

  // 權重歸零並清掉時間基準。視窗隱藏時呼叫 —— 重新顯示要從正面開始，
  // 而不是接續幾分鐘前的權重，也不能讓那段空白算成一次巨大的 dt。
  void reset();

private:
  GazeDirectorTuning tuning_;
  double weight_ = 0;
  double lastTickMs_ = -1;
  double lastMoveMs_ = -1;
  // 已經停在正前方且寫過一次。少了它，收尾的 weight 會停在 0.003 之類的
  // 殘值上（頭永遠歪一點點）；有了它又不能一路不寫，所以是「歸零那一輪
  // 仍要寫，之後才閉嘴」。
  bool settledFront_ = false;
};

}  // namespace l2m
