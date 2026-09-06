#pragma once

// LLM 行為大腦的非同步橋接：決定「這一輪歸 LLM 還是規則版」、組 prompt、
// 發請求、把回覆解析成 PerformStep 交回去。
//
// 這是 AppController::behaviorPlanner 那個掛鉤（app_controller.h）的 LLM 版
// 接線對象 —— 掛鉤那側承諾「LLM 版隔一段網路往返再呼叫 done，一個字都不用改」，
// 這裡就是兌現的地方。純函式部分（prompt 組裝、回應解析、gate 判斷）都在
// core/llm_behavior_prompt.h，這裡只剩非同步搬運。
//
// 產品規則：
//  * **LLM 永遠不准讓桌寵僵住** —— 未啟用、設定不完整、cooldown 中、請求失敗、
//    回覆解析不出來，全部退回呼叫端給的 fallback（IdleDirector）。
//  * occasion == "petted" 一律不上 LLM：被摸的當下等網路往返很怪，
//    台詞走本機池（persona 的 # Petted 區）。
//  * cooldown 只管 "idle"（每分鐘來一次的那種）；welcome / breakReminder
//    本來就稀疏，不吃 cooldown。雲端計費的節流就靠這一條。

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/idle_director.h"
#include "core/perform_step.h"
#include "llm_manager.h"

namespace l2m {

class LlmBehaviorPlanner {
public:
  struct Deps {
    LlmManager* llm = nullptr;
    std::function<LlmConfig()> getConfig;
    // 單調毫秒（cooldown 用）
    std::function<double()> nowMs;
    // 套用中角色的 # Character Description（現讀；空字串＝沒有角色）
    std::function<std::string()> personaDescription;
    // 套用中角色的長期記憶（memory/<角色>.md，已 clamp；空字串＝沒有記憶檔）。
    // 現讀 —— 使用者拿記事本改完，下一輪就吃到新的
    std::function<std::string()> memoryText;
    // 現在時刻 0~23（prompt 的 time_of_day 用）
    std::function<int()> hourNow;
    // 現在的天氣，一行英文（core/weather_alert.h 的 weatherContextLine）。
    // **每一輪都帶**：使用者要的「天氣當背景知識」就是這個 —— 角色本來就要
    // 講話時自然帶到，而不是為了報天氣特地開口。空字串＝沒有資料，整段省略。
    // 這一則預警的摘要走 IdlePlanOptions::weatherAlert（那是「這一輪的情境」）。
    std::function<std::string()> weatherLine;
  };
  explicit LlmBehaviorPlanner(Deps deps);
  // 解構時取消還在飛的請求：main() 收尾後不准再有回呼落在已解構的物件上
  ~LlmBehaviorPlanner();

  // 回 true：LLM 接手，done 稍後被呼叫**恰好一次**（成功給 LLM 的步驟，
  // 失敗給 fallback() 的結果）。回 false：這一輪不歸 LLM，呼叫端自己走規則版
  //（done 完全沒被碰）。
  bool plan(const IdleWorld& world, const IdlePlanOptions& options, std::function<std::vector<PerformStep>()> fallback, std::function<void(std::vector<PerformStep>)> done);

private:
  // 發出（或重發）一次請求。allowMove 一路帶著（＝送出當下的 IdleWorld::canMove，
  // 也就是「隨機移動／鎖定位置／拖曳移動」三個開關的結論）：重試要用同一份 context
  // 的答案，prompt 說的話才不會跟過濾規則對不起來。它是快照 —— 「設定在往返途中被
  // 改掉」由 AppController::runAutonomous() 執行前現讀一次收尾，這裡不必也不該去追。
  // 暫時性失敗（llmErrorLooksTransient：5xx／429／
  // 連線層 —— Ollama 冷載入模型時直接回 500 "llm server loading model"）
  // 會隔幾秒自動重試，retriesLeft 用完才退回 fallback：立刻放棄等於每次
  // 冷啟動都白白錯過一輪。
  void send(std::vector<LlmMessage> messages, bool allowMove, std::function<std::vector<PerformStep>()> fallback, std::function<void(std::vector<PerformStep>)> done, int retriesLeft);

  Deps deps_;
  std::unique_ptr<LlmRequestHandle> inFlight_;
  double lastIdlePlanMs_ = -1;
  // 回呼落地時 this 可能已經走了（解構取消不掉「已經排進事件迴圈」的那一刻），
  // 用共享旗標護住 —— 與 FunctionLlmHandle 的 cancelled 同一套做法
  std::shared_ptr<bool> alive_;
  // 每呼叫一次 plan() 加一。排在計時器裡的重試要比對送出時的世代 ——
  // 新一輪已經開跑時，過期的重試直接作廢，兩個請求才不會互搶 inFlight_
  int generation_ = 0;
  // LLM 最近幾輪自己講過的台詞（餵回 prompt 防重複；只放記憶體，
  // 重啟歸零 —— 跨 session 的記憶是 memory/<角色>.md 的事）
  std::vector<std::string> recentLines_;
};

}  // namespace l2m
