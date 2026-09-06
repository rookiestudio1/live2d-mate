#include "idle_director.h"

#include <algorithm>

#include "string_util.h"

namespace l2m {

namespace {

// 閒置表演不挑待機群組 —— 那個本來就會自己接（與 AppController 的同名常數一致）
constexpr const char* kIdleGroup = "Idle";

// 有寫命名意義的動作權重 ×3：使用者肯花時間寫意義的，通常是「拿得出手」的那些；
// 沒寫的常常是過場、口型或半成品
constexpr double kAnnotatedMotionWeight = 3;

// 自主表情的自動退回時間（PerformStep::holdMs）。
// 不依賴 idle.autoReset 有沒有開 —— 表演自己套的表情自己收
constexpr double kIdleExpressionHoldMs = 12000;

// Bored / Sleepy 這一輪順便換個表情的機率（心情好時升到 cheerful 那一檔）
constexpr double kExpressionChance = 0.5;
constexpr double kExpressionChanceCheerful = 0.8;

// Sleepy 這一輪什麼都不做的機率：睡著的角色不該每分鐘準時動一下
constexpr double kSleepyQuietChance = 0.6;
constexpr double kSleepyQuietChanceCheerful = 0.4;

// 閒置時順便嘀咕一句的機率。預設間隔 60 秒 → 平均約 4 分鐘一句，
// 再頻繁就從「有生命」變成「很吵」。心情好時話多一點；
// 熟悉度每一級再墊 0.05（「它記得我」的感覺，永不鎖回去 —— core/mood.h）
constexpr double kIdleSpeakChance = 0.25;
constexpr double kIdleSpeakChanceCheerful = 0.4;
constexpr double kIdleSpeakChancePerFamiliarity = 0.05;
constexpr double kIdleSpeakChanceMax = 0.6;

// 微移動：偶爾往旁邊挪一小步（水平緩滑）。
// 只動水平：桌寵沿工作列滑比較自然，上下漂移看起來像 bug。
// Fidget 也放行（機率減半）—— 原本只給 Bored 以上，但閒置分級吃的是
// 「app 閒置與 OS 真閒置取小」：使用者人在電腦前 OS 閒置永遠很短，
// 等級到不了 Bored；真的離座夠久又會被 pauseWhenAway 跳過整輪，
// 結果是預設設定下散步一次都不會發生。
constexpr double kMoveChance = 0.15;
constexpr double kMoveChanceFidget = 0.08;
constexpr double kMoveMaxRatioStep = 0.08;
constexpr double kMoveGlideMs = 1500;

// 動作群組不重複最近 2 個：動作少的模型連播同一段看起來像卡住
constexpr int kMotionNoRepeat = 2;

// 表情冷卻 5 分鐘＋不重複上一個：表情變化本來就該比動作稀疏
constexpr double kExpressionCooldownMs = 5 * 60 * 1000;

bool motionIsAnnotated(const ModelAnnotations& annotations, const MotionGroupInfo& group) {
  const auto meaningful = [&annotations](const std::string& key) {
    const auto it = annotations.motions.find(key);
    return it != annotations.motions.end() && !it->second.empty();
  };
  if (meaningful(motionKey(group.name))) return true;
  for (int i = 0; i < group.count; ++i) {
    if (meaningful(motionKey(group.name, i))) return true;
  }
  return false;
}

}  // namespace

IdleLevel idleLevelFor(double idleForMs, double baseMs) {
  if (baseMs <= 0) return IdleLevel::Fidget;
  if (idleForMs < baseMs * 3) return IdleLevel::Fidget;
  if (idleForMs < baseMs * 10) return IdleLevel::Bored;
  return IdleLevel::Sleepy;
}

IdleDirector::IdleDirector(Deps deps)
  : deps_(std::move(deps)),
    motionPicker_({/*cooldownMs=*/0, /*noRepeatLast=*/kMotionNoRepeat}),
    expressionPicker_({kExpressionCooldownMs, /*noRepeatLast=*/1}),
    linePicker_({/*cooldownMs=*/5 * 60 * 1000, /*noRepeatLast=*/2}) {}

namespace {

PerformStep speakStep(std::string line) {
  PerformStep step;
  step.action = "speak";
  step.text = std::move(line);
  // speakWait 維持預設 true：這一句講完 PerformRunner 才收尾，
  // idleSuppress_ 才會蓋住整段說話期間
  return step;
}

}  // namespace

std::vector<PerformStep> IdleDirector::plan(const IdleWorld& world, const IdlePlanOptions& options) {
  std::vector<PerformStep> steps;
  const double now = deps_.nowMs ? deps_.nowMs() : 0;
  const auto random = [this] { return deps_.random ? deps_.random() : 0.5; };

  // 台詞候選（world.lines 已由呼叫端依 occasion 從角色 .md 對應區塊填好）
  std::vector<PickCandidate> lineCandidates;
  if (world.canSpeak) {
    for (const auto& line : world.lines) lineCandidates.push_back({line, 1});
  }

  // 歡迎詞／久坐提醒／摸摸反應／天氣預警：講一句本身就是這一輪的目的 ——
  // 有台詞就講一句，不做動作與表情（剛回座就手舞足蹈反而突兀；摸摸的表情回應由
  // AppController 就地處理，這裡只負責台詞）。沒有台詞就整輪安靜。
  if (options.occasion == "welcome" || options.occasion == "breakReminder" || options.occasion == "petted" || options.occasion == "weatherAlert") {
    if (!lineCandidates.empty()) {
      if (const auto picked = linePicker_.pick(lineCandidates, now, random())) {
        steps.push_back(speakStep(*picked));
      }
    }
    return steps;
  }

  // 睡著的角色偶爾翻個身就好，大多數輪次什麼都不做。
  // 亂數的消耗順序是固定的（quiet → 要不要表情 → 挑表情 → 挑動作 →
  // 要不要散步 → 挑步幅 → 要不要嘀咕 → 挑台詞），測試以注入的定值序列驅動，
  // 順序一變斷言就會亂掉。
  const double sleepyQuiet = options.cheerful ? kSleepyQuietChanceCheerful : kSleepyQuietChance;
  if (options.level == IdleLevel::Sleepy && random() < sleepyQuiet) return steps;

  // 動作候選先算出來（不消耗亂數）：表情的出場條件要看它是不是空的
  std::vector<PickCandidate> motions;
  for (const auto& group : world.motions) {
    if (group.count <= 0) continue;
    if (strutil::equalsInsensitive(group.name, kIdleGroup)) continue;
    motions.push_back({group.name, motionIsAnnotated(world.annotations, group) ? kAnnotatedMotionWeight : 1});
  }

  // 表情：只挑有寫意義的（見標頭的產品規則）。Fidget 是小動作，原則上不換表情
  // —— 但**模型一個可用動作群組都沒有時例外**（VTube Studio 出身的模型常常把
  // 全部動作歸在 Idle、或根本沒有動作，表情是它唯一能表演的東西；不放行的話
  // 這類模型的閒置表演永遠一片死寂）。
  if (options.level != IdleLevel::Fidget || motions.empty()) {
    std::vector<PickCandidate> expressions;
    for (const auto& name : world.expressions) {
      const auto it = world.annotations.expressions.find(name);
      if (it != world.annotations.expressions.end() && !it->second.empty()) {
        expressions.push_back({name, 1});
      }
    }
    const double expressionChance = options.cheerful ? kExpressionChanceCheerful : kExpressionChance;
    if (!expressions.empty() && random() < expressionChance) {
      if (const auto picked = expressionPicker_.pick(expressions, now, random())) {
        PerformStep step;
        step.action = "expression";
        step.name = *picked;
        step.holdMs = kIdleExpressionHoldMs;
        steps.push_back(std::move(step));
      }
    }
  }

  // 動作：排掉待機群組（本來就會自己接），有命名意義的權重 ×3
  if (!motions.empty()) {
    if (const auto picked = motionPicker_.pick(motions, now, random())) {
      PerformStep step;
      step.action = "motion";
      step.group = *picked;
      steps.push_back(std::move(step));
    }
  }

  // 微移動：有位置資訊且沒被鎖定才動；Fidget 機率減半（見 kMoveChanceFidget）
  const double moveChance = options.level == IdleLevel::Fidget ? kMoveChanceFidget : kMoveChance;
  if (world.canMove && world.windowXRatio && world.windowYRatio && random() < moveChance) {
    PerformStep step;
    step.action = "move";
    step.x = std::clamp(*world.windowXRatio + (random() * 2 - 1) * kMoveMaxRatioStep, 0.0, 1.0);
    step.y = *world.windowYRatio;
    step.glideMs = kMoveGlideMs;
    steps.push_back(std::move(step));
  }

  // 台詞：偶爾嘀咕一句。放在最後 —— 動作先起跑，氣泡跟著出現；
  // speakWait=true 讓整段說話期間都在 idleSuppress_ 的保護傘下
  const double speakChance = std::min(kIdleSpeakChanceMax, (options.cheerful ? kIdleSpeakChanceCheerful : kIdleSpeakChance) + kIdleSpeakChancePerFamiliarity * options.familiarityLevel);
  if (!lineCandidates.empty() && random() < speakChance) {
    if (const auto picked = linePicker_.pick(lineCandidates, now, random())) {
      steps.push_back(speakStep(*picked));
    }
  }

  // 目前最多 2 步，離 kMaxPerformSteps 很遠；保個險，之後加步驟種類也不會超
  if (steps.size() > kMaxPerformSteps) steps.resize(kMaxPerformSteps);
  return steps;
}

void IdleDirector::reset() {
  motionPicker_.reset();
  expressionPicker_.reset();
  linePicker_.reset();
}

}  // namespace l2m
