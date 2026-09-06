#pragma once

// 關鍵影格 → motion3.json 的編譯器。
//
// 讓 AI 自己編動作最省事的做法，是把它給的關鍵影格編成一份標準 motion3.json，
// 再丟給模型原本就在用的 CubismMotion 播 —— 淡入淡出、循環、優先權、
// 「現在正在播哪一段」的回報全部照舊運作，不必另外寫一套時間軸。
//
// 純函式，不碰檔案系統也不碰模型。

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace l2m {

// 一個關鍵影格：在 at 毫秒時，這些參數要是這些值。
// params 用 vector 而不是 map，保留呼叫端給的寫入順序。
struct Keyframe {
  double at = 0;
  std::vector<std::pair<std::string, double>> params;
};

struct BuildMotionOptions {
  bool loop = false;
  std::optional<double> fadeInMs;
  std::optional<double> fadeOutMs;
};

struct Motion3Curve {
  // Target 固定是 "Parameter"
  std::string id;
  // [t0, v0, 型別, t1, v1, 型別, t2, v2, ...]；型別 0 是線性
  std::vector<double> segments;
};

struct Motion3 {
  // Version 固定是 3
  struct Meta {
    double duration = 0;
    int fps = 30;
    bool loop = false;
    bool areBeziersRestricted = true;
    int curveCount = 0;
    int totalSegmentCount = 0;
    int totalPointCount = 0;
    int userDataCount = 0;
    int totalUserDataSize = 0;
    double fadeInTime = 0;
    double fadeOutTime = 0;
  } meta;
  std::vector<Motion3Curve> curves;
};

// 把關鍵影格編成 motion3.json。
//
// 每個出現過的參數各自成為一條曲線，點就是它實際被寫到的 (時間, 值)。
// 曲線頭尾沒蓋到整段時長時會用同一個值補平 —— 想要「某參數中途才開始動」，
// 就在第一個影格明確把它寫成起始值，不要靠省略。
//
// 輸入不合法時丟 std::runtime_error。
Motion3 buildMotion3(const std::vector<Keyframe>& keyframes, const BuildMotionOptions& options = {});

// 把 Motion3 序列化成 motion3.json 的 JSON 文字（給 CubismMotion 從記憶體載入）
std::string toMotion3Json(const Motion3& motion);

// AI 合成動作專用的群組名稱。
//
// 這個群組只存在於執行期的 motionManager 裡，model3.json 與掃描結果都沒有它，
// 所以 list_motions 本來就看不到、play_motion 也叫不動 —— 這是刻意的。
inline constexpr const char* kAiMotionGroup = "AIMotion";

// 挑下一個要用的 AI 動作插槽。
//
// 動作管理對「同群組同索引已經在播」是直接拒絕的，每次都寫同一格的話，
// 前一段動畫還沒播完時再呼叫 animate，會回報成功卻什麼都不做。
// 輪流換格就永遠不會撞到那個判斷。playingSlot 為 nullopt 代表「沒有在播」。
int nextAiMotionSlot(std::optional<int> playingSlot);

}  // namespace l2m
