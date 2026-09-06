#pragma once

// perform 工具的步驟執行器。
//
// 步驟之間有非同步等待（wait、speak{wait:true}），所以做成狀態機而不是迴圈：
// 每一步做完（或它的非同步回呼觸發）才推進下一步。wait 靠 QTimer，
// speak{wait:true} 靠 SpeechController 的完成回呼，其餘都是同步的。
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
  void finishStep(const CommandResult& result);
  void finish(CommandResult result);

  AppController& controller_;
  std::vector<PerformStep> steps_;
  std::function<void(CommandResult)> done_;
  size_t index_ = 0;
  bool finished_ = false;
  bool bestEffort_ = false;
  std::optional<int> defaultPriority_;
  std::function<bool()> moveGate_;
};

}  // namespace l2m
