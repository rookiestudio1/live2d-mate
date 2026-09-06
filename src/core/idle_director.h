#pragma once

// 閒置表演的決策層：由模型能力＋命名意義＋閒置分級，產出一段
// std::vector<PerformStep> 交給既有的 PerformRunner 執行。
//
// 為什麼重用 PerformStep：它的 action 集合（motion / expression / speak / move /
// wait / …）與「多模態閒置」要的完全重疊，而且**未來 LLM 大腦吐的就是 MCP
// perform 的同一份 schema** —— 共用同一個執行器、同一個資料型別，
// AppController::behaviorPlanner 那個 std::function 就是唯一要換的東西。
//
// 這一層不認識 Qt、不認識 ModelController：世界（IdleWorld）由呼叫端組好純資料
// 丟進來，時間與亂數由 Deps 注入，整份決策才測得到。
//
// 產出的步驟種類：motion / expression / speak（台詞來自角色 .md 的分區，
// 見 core/persona_doc.h；bubble 模式的改寫在 AppController::runAutonomous）。
// move 是階段 3 的題目（IdleWorld 的 canMove 欄位先佔位）。

#include <functional>
#include <string>
#include <vector>

#include "annotations.h"
#include "behavior_pick.h"
#include "model_types.h"
#include "perform_step.h"

namespace l2m {

// 分級閒置。門檻 = base ×1 / ×3 / ×10 —— 預設 performAfterMs 180000 →
// 3 分 / 9 分 / 30 分。三個絕對門檻沒有人調得出來，但「多久開始表演」
// 使用者真的有感，所以只留 idle.performAfterMs 這一個旋鈕。
enum class IdleLevel { Fidget, Bored, Sleepy };
IdleLevel idleLevelFor(double idleForMs, double baseMs);

// 決策時看得到的世界。呼叫端組好純資料丟進來。
struct IdleWorld {
  std::vector<MotionGroupInfo> motions;  // count > 0 的群組
  std::vector<std::string> expressions;
  ModelAnnotations annotations;    // 唯一的語意來源
  std::vector<std::string> lines;  // 依 occasion 從角色 .md 對應區塊取出的候選台詞
                                   //（idle → # Dialogue List、welcome → # Welcome Text）
  bool canSpeak = false;           // autonomy.speech 關掉、或正在說話時為 false
  bool canMove = false;            // autonomy.move / lockPosition / dragMove 決定
  // 視窗目前在工作區的比例位置（0..1）；缺了就不產 move 步驟
  std::optional<double> windowXRatio;
  std::optional<double> windowYRatio;
};

struct IdlePlanOptions {
  IdleLevel level = IdleLevel::Fidget;
  // 這一輪的「主題」：idle / welcome / breakReminder / petted.head…
  // 之後接 LLM 時，這就是 prompt 的情境欄位。
  std::string occasion = "idle";
  // 心情餘韻（core/mood.h）：被摸過之後那幾分鐘比較活潑 ——
  // 表情機率升、嘀咕機率升、Sleepy 的安靜比例降
  bool cheerful = false;
  // 熟悉度級距 0~3（core/mood.h 的 familiarityLevel）：只墊高說話機率
  int familiarityLevel = 0;
  // occasion == "weatherAlert" 時，這一則預警的英文摘要（WeatherAlert::summary）。
  // 規則版用不到（它只從 world.lines 挑一句），但 LLM 版要靠它才知道要提醒什麼；
  // 這是「這一輪的情境」而不是「世界狀態」，所以跟 occasion 放在一起。
  std::string weatherAlert;
};

class IdleDirector {
public:
  struct Deps {
    // [0,1) 亂數與單調毫秒。測試注定值序列與固定時鐘。
    std::function<double()> random;
    std::function<double()> nowMs;
  };
  explicit IdleDirector(Deps deps);

  // 產出這一輪的表演。**回空 vector 是合法且常見的結果**：
  // 每 60 秒硬要動一下，看久了比不動還煩，「這輪什麼都不做」本身就是設計。
  //
  // 兩條產品規則（tests/test_idle_director.cpp 釘住）：
  //  * 使用者肯花時間寫命名意義的動作，通常是「拿得出手」的那些 —— 權重 ×3。
  //  * 表情更嚴格：**只挑有寫意義的**。VTuber 模型的虛擬表情很可能叫「右腿」
  //    「星星移動1」，亂套上去只會讓角色看起來壞掉；整個模型一個表情意義都
  //    沒寫就完全不碰表情。這也順便給了使用者填命名的理由。
  std::vector<PerformStep> plan(const IdleWorld& world, const IdlePlanOptions& options);

  // 換模型時把冷卻與不重複的記憶歸零
  void reset();

private:
  Deps deps_;
  BehaviorPicker motionPicker_;
  BehaviorPicker expressionPicker_;
  BehaviorPicker linePicker_;
};

}  // namespace l2m
