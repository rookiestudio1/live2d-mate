#include "llm_behavior_planner.h"

#include <QDebug>
#include <QTimer>

#include <utility>

#include "core/llm_behavior_prompt.h"
#include "core/llm_http.h"

namespace l2m {

namespace {

// 暫時性失敗的重試：Ollama 冷載入一個 7B 模型實測要幾秒到二十幾秒，
// 15 秒後再試一次通常就熱好了；重試一次就夠 —— 下一輪閒置 60 秒後本來就會再來
constexpr int kTransientRetryDelayMs = 15000;
constexpr int kTransientRetries = 1;

// 防重複要記幾句：8 句配預設 5 分鐘 cooldown 約蓋住 40 分鐘的「剛講過」，
// 再多只是燒 token —— 一小時前講過類似的話不算重複
constexpr size_t kRecentLinesMax = 8;

}  // namespace

LlmBehaviorPlanner::LlmBehaviorPlanner(Deps deps) : deps_(std::move(deps)), alive_(std::make_shared<bool>(true)) {}

LlmBehaviorPlanner::~LlmBehaviorPlanner() {
  *alive_ = false;
  if (inFlight_) inFlight_->cancel();
}

bool LlmBehaviorPlanner::plan(const IdleWorld& world, const IdlePlanOptions& options, std::function<std::vector<PerformStep>()> fallback, std::function<void(std::vector<PerformStep>)> done) {
  const LlmConfig config = deps_.getConfig ? deps_.getConfig() : LlmConfig{};
  // 為什麼沒走 LLM 要講清楚（enabled 關閉除外 —— 那是使用者明確的選擇，
  // 每分鐘提醒一次只是噪音）；「感覺沒在用」的除錯全靠這幾行。
  if (!config.enabled) return false;
  if (!config.driveIdle) {
    qDebug() << "[llm] driveIdle 關閉，閒置規劃走規則版（設定 → LLM → 行為）";
    return false;
  }
  if (!deps_.llm) return false;
  if (const std::string issue = llmConfigIssue(config); !issue.empty()) {
    qDebug() << "[llm] 設定不完整，走規則版:" << QString::fromStdString(issue);
    return false;
  }
  // 被摸的當下等網路往返很怪，台詞走本機池
  if (options.occasion == "petted") return false;

  const double now = deps_.nowMs ? deps_.nowMs() : 0;
  if (options.occasion == "idle") {
    if (lastIdlePlanMs_ >= 0 && now - lastIdlePlanMs_ < config.idleLlmCooldownMs) {
      // cooldown 中，這一輪歸規則版（雲端計費節流）
      qDebug() << "[llm] cooldown 中（還剩" << static_cast<int>((config.idleLlmCooldownMs - (now - lastIdlePlanMs_)) / 1000) << "秒），這一輪走規則版";
      return false;
    }
    lastIdlePlanMs_ = now;
  }

  // 上一輪還在飛（閒置間隔 60 秒，慢端點跑不完是常態）：取消它，
  // 舊 context 生出來的表演這時已經過期了
  if (inFlight_) inFlight_->cancel();

  LlmBehaviorPromptInput input;
  input.world = world;
  input.options = options;
  input.personaDescription = deps_.personaDescription ? deps_.personaDescription() : "";
  input.hour = deps_.hourNow ? deps_.hourNow() : 12;
  input.memory = deps_.memoryText ? deps_.memoryText() : "";
  input.recentLines = recentLines_;
  input.weather = deps_.weatherLine ? deps_.weatherLine() : "";
  input.weatherAlert = options.weatherAlert;

  qDebug() << "[llm] 行為大腦接手這一輪:" << QString::fromStdString(options.occasion) << "provider=" << QString::fromStdString(config.provider)
           << "model=" << QString::fromStdString(config.provider == "anthropic" ? config.anthropicModel : config.model);

  // prompt 與回覆全文都印出來（noquote 才不會被逃逸成一整行）——
  // 「LLM 為什麼選了這個動作／為什麼一直安靜」不看原文根本查不了。
  // 頻率最高也就是每個 cooldown 一次，量不成問題。
  const std::vector<LlmMessage> messages = buildBehaviorPromptMessages(input);
  for (const auto& message : messages) {
    qDebug().noquote() << QStringLiteral("[llm] prompt(%1):\n%2").arg(QString::fromStdString(message.role), QString::fromStdString(message.text));
  }

  ++generation_;
  // canMove 跟著這一輪的 context 走 —— prompt 裡的 can_move 擋不住模型，
  // 真正的關卡是 parseBehaviorSteps 收到的這個旗標
  send(messages, world.canMove, std::move(fallback), std::move(done), kTransientRetries);
  return true;
}

void LlmBehaviorPlanner::send(std::vector<LlmMessage> messages, bool allowMove, std::function<std::vector<PerformStep>()> fallback, std::function<void(std::vector<PerformStep>)> done,
                              int retriesLeft) {
  std::shared_ptr<bool> alive = alive_;
  const int generation = generation_;
  inFlight_ = deps_.llm->chatBuffered(
    messages, LlmChatOptions{}, [this, alive, generation, messages, allowMove, fallback = std::move(fallback), done = std::move(done), retriesLeft](std::string text, std::string error) mutable {
      if (!*alive) return;
      if (!error.empty()) {
        // 暫時性失敗（模型冷載入中的 500、rate limit、連線抖動）：
        // 隔幾秒再試，額度用完才放棄
        if (retriesLeft > 0 && llmErrorLooksTransient(error)) {
          qDebug() << "[llm] 暫時性失敗，" << kTransientRetryDelayMs / 1000 << "秒後重試（剩" << retriesLeft << "次）:" << QString::fromStdString(error);
          QTimer::singleShot(kTransientRetryDelayMs,
                             [this, alive, generation, messages = std::move(messages), allowMove, fallback = std::move(fallback), done = std::move(done), retriesLeft]() mutable {
                               if (!*alive) return;
                               // 計時器躺著的期間新一輪已經開跑：這個重試過期了，直接作廢
                               //（沒有 done 要收尾 —— 新一輪自己會呼叫它自己的 done）
                               if (generation != generation_) return;
                               send(std::move(messages), allowMove, std::move(fallback), std::move(done), retriesLeft - 1);
                             });
          return;
        }
        qWarning() << "[llm] 行為大腦請求失敗，退回規則版:" << QString::fromStdString(error);
        if (done) done(fallback ? fallback() : std::vector<PerformStep>{});
        return;
      }
      qDebug().noquote() << QStringLiteral("[llm] response:\n%1").arg(QString::fromStdString(text));
      std::string parseError;
      auto steps = parseBehaviorSteps(text, allowMove, &parseError);
      if (!steps) {
        qWarning() << "[llm] 行為大腦回覆解析失敗，退回規則版:" << QString::fromStdString(parseError);
        if (done) done(fallback ? fallback() : std::vector<PerformStep>{});
        return;
      }
      qDebug() << "[llm] 行為大腦產出" << steps->size() << "步";
      // 記下這一輪講的台詞，下一輪餵回 prompt 防重複（無狀態 API 不會自己記得）
      for (const auto& step : *steps) {
        if (step.action != "speak") continue;
        recentLines_.push_back(step.text);
        if (recentLines_.size() > kRecentLinesMax) recentLines_.erase(recentLines_.begin());
      }
      if (done) done(std::move(*steps));
    });
}

}  // namespace l2m
