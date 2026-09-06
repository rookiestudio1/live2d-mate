#pragma once

// perform 工具的步驟執行器。
//
// 步驟之間有非同步等待（wait、speak{wait:true}），所以做成狀態機而不是迴圈：
// 每一步做完（或它的非同步回呼觸發）才推進下一步。wait 靠 QTimer，
// speak{wait:true} 靠 SpeechController 的完成回呼，其餘都是同步的。
//
// **視覺步驟會押後到「真的開口」那一刻**（哪些算，見 core/perform_sync.h）。
// AI 的典型序列 [expression, motion, speak] 嚴格照順序跑的話，動作在 t=0
// 就演完了，聲音卻要等 TTS 的一整趟往返 —— 實測 custom 引擎 2.35／2.48 秒，
// 中間既沒有聲音也沒有氣泡，看起來像對不上嘴的配音。所以
// motion／expression／parameters／animate 後面若緊接著 speak，先押進
// deferred_ 不執行，等 SpeakRequest::onSpeechStart 觸發（跟氣泡同一個閘門，
// 見 app/speech_controller.h）才整批放出來。
// **押後不等於可以弄丟**：speak 的 done 回呼上還有一次 flushDeferred()，
// 涵蓋 onSpeechStart 一次都沒觸發的路徑（stop_speaking 中途打斷、
// 沒有接上 speakHandler 的建置）。押後是時序調整，不是「有機會就跳過」。
//
// 自己管生命週期：跑完（或失敗）呼叫 done 之後 deleteLater()，
// 呼叫端不必持有它。app 結束時 QObject 的親子關係會一起收掉。

#include <QObject>

#include <functional>
#include <optional>
#include <vector>

#include "core/command_result.h"
#include "core/perform_step.h"

namespace l2m {

class AppController;

class PerformRunner : public QObject {
  Q_OBJECT

public:
  PerformRunner(AppController& controller, std::vector<PerformStep> steps, std::function<void(CommandResult)> done, QObject* parent = nullptr);

  // 開始執行。第一步會在本次呼叫中同步跑掉。
  void start();

  // 失敗的步驟跳過而不是整段中止。給自主表演用 —— 表情名剛好不存在，
  // 不該讓整段閒置表演消失。**預設 false 維持現行 MCP 語意**
  //（AI 要靠「第幾步失敗」的錯誤自我修正）。
  void setBestEffort(bool enabled) { bestEffort_ = enabled; }

  // motion 步驟未指定 priority 時的預設值。nullopt 維持現行行為
  //（playMotion 的預設 PriorityForce —— 明確點名的動作不該被待機擋下）；
  // 自主表演傳 PriorityNormal：使用者或 AI 正在進行的動作優先，表演只是填空檔。
  void setDefaultPriority(std::optional<int> priority) { defaultPriority_ = priority; }

  // 自主表演專用：每次**要執行** move 步驟時現讀一次閘門，回 false 就跳過那一步。
  // 為什麼不能只在 start() 之前濾一輪（AppController::runAutonomous 做的那件事）：
  // 一段夾著 wait 的表演可以跑上好幾分鐘，使用者完全來得及在中途把「隨機移動」關掉
  // 或把位置鎖起來，而 move 步驟走的 glideTo()／moveTo() 兩支都不看設定 ——
  // 症狀就是「開關明明關著，桌寵還是自己走了一步」。
  // **預設不設 = 不檢查**，維持 MCP 的語意：AI 明確下的 move 是使用者的主動要求，
  // 不是自主行為。
  void setMoveGate(std::function<bool()> gate) { moveGate_ = std::move(gate); }

private:
  void runNext();
  // 同步執行一個視覺步驟（motion／expression／parameters／animate）。
  // 押後與不押後兩條路共用同一支，才不會有一條偷偷少傳 defaultPriority_
  CommandResult runCompanionStep(const PerformStep& step);
  // 放行押後的步驟。失敗的處理與 finishStep 一致：bestEffort 跳過，否則整段中止
  void flushDeferred();
  void finishStep(const CommandResult& result);
  void finish(CommandResult result);

  AppController& controller_;
  std::vector<PerformStep> steps_;
  std::function<void(CommandResult)> done_;
  size_t index_ = 0;
  // 押後到「開口那一刻」的視覺步驟，存的是 steps_ 的索引（錯誤訊息要報第幾步）
  std::vector<size_t> deferred_;
  bool finished_ = false;
  bool bestEffort_ = false;
  std::optional<int> defaultPriority_;
  std::function<bool()> moveGate_;
};

}  // namespace l2m
