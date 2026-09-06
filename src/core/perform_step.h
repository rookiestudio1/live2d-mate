#pragma once

// perform 工具的單一步驟。
//
// 用「帶標籤的結構」而不是 std::variant：yyjson 解析出來就是逐欄位填，
// variant 反而要多寫一整套 visitor，對這個規模不划算。
// 欄位名稱維持 MCP 那側的 snake_case 語意（at_ms、fade_in_ms…）在解析層轉換。

#include <yyjson.h>

#include <optional>
#include <string>
#include <vector>

#include "motion_builder.h"
#include "parameter_tracks.h"

namespace l2m {

// wait 步驟的上限（毫秒）
inline constexpr double kMaxWaitMs = 60000;

// 一次 perform 最多幾步：一個沒收斂的序列會把
// MCP 的長呼叫名額佔住，PerformRunner 明確擋掉並在錯誤裡講清楚。
// 放這裡而不是 perform_runner.h：IdleDirector（l2m_core）產步驟時也要遵守同一個上限。
inline constexpr size_t kMaxPerformSteps = 20;

struct PerformStep {
  // motion | expression | speak | move | wait | parameters | animate
  std::string action;

  // action == "motion"
  std::string group;
  std::optional<int> index;

  // action == "expression"
  std::string name;
  // 表情自動退回的毫秒數。目前只有自主表演（IdleDirector）會設 ——
  // parsePerformSteps 刻意不吐這個欄位，對 MCP 的 perform 零影響
  std::optional<double> holdMs;

  // action == "speak"
  std::string text;
  std::optional<std::string> voice;
  // 預設 true：一句講完才進下一步，不然整段台詞會全部疊在一起
  bool speakWait = true;
  // true 時這一句走 think 的管線：思考泡泡＋殘響＋嘴巴不動（AppController::SpeakRequest::thinking）。
  // 做成 speak 步驟的旗標而不是新增一個 "think" action —— 底層本來就只差這一個旗標
  //（mcp/mcp_tools.cpp 的 speak／think 共用同一個 if），多開一個 action 等於把
  //「等不等這一句講完」的時序推理再抄一份，遲早分岔。
  // 與 bubbleOnly 互斥：bubbleOnly 走 mutter，根本不經過 TTS 也就沒有殘響可言。
  bool thinking = false;
  // true 時只出氣泡、不經過 TTS（AppController::mutter）。只有自主表演
  //（autonomy.speech == "bubble"）會設 —— parsePerformSteps 刻意不吐這個欄位，
  // MCP 的 speak 永遠走完整的合成路徑
  bool bubbleOnly = false;

  // action == "move"
  std::optional<double> x;
  std::optional<double> y;
  std::optional<std::string> preset;
  // 帶了就用補間滑行過去（毫秒），不帶維持瞬移。只有自主表演的微移動會設 ——
  // parsePerformSteps 刻意不吐，MCP 的 move 永遠瞬移（AI 等的是結果不是動畫）
  std::optional<double> glideMs;

  // action == "wait"
  double ms = 0;

  // action == "parameters"
  std::vector<SetParameterRequest> params;

  // action == "animate"
  std::vector<Keyframe> keyframes;
  BuildMotionOptions animateOptions;
};

// 解析 MCP 傳來的 steps 陣列；失敗時回 nullopt 並在 error 裡放原因。
std::optional<std::vector<PerformStep>> parsePerformSteps(yyjson_val* steps, std::string* error);

// 解析 MCP 的 params 陣列（parameterSchema）
std::optional<std::vector<SetParameterRequest>> parseParameterRequests(yyjson_val* params, std::string* error);

// 解析 MCP 的 keyframes 陣列（keyframeSchema，at_ms → at）
std::optional<std::vector<Keyframe>> parseKeyframes(yyjson_val* keyframes, std::string* error);

}  // namespace l2m
