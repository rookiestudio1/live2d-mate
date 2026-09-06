#include "perform_runner.h"

#include <QDebug>
#include <QPointer>
#include <QTimer>

#include <algorithm>

#include "app_controller.h"

namespace l2m {

PerformRunner::PerformRunner(AppController& controller, std::vector<PerformStep> steps, std::function<void(CommandResult)> done, QObject* parent)
  : QObject(parent), controller_(controller), steps_(std::move(steps)), done_(std::move(done)) {}

void PerformRunner::start() {
  if (steps_.empty()) {
    finish(CommandResult::failure("No steps given", "steps is an array of { action, ... }; see the tool schema."));
    return;
  }
  if (steps_.size() > kMaxPerformSteps) {
    finish(CommandResult::failure("Too many steps: " + std::to_string(steps_.size()),
                                  "perform accepts at most " + std::to_string(kMaxPerformSteps) + " steps; split the sequence or use schedule for the later part."));
    return;
  }
  runNext();
}

void PerformRunner::runNext() {
  if (finished_) return;
  if (index_ >= steps_.size()) {
    finish(CommandResult::success("{\"completed\":" + std::to_string(steps_.size()) + "}"));
    return;
  }

  const PerformStep& step = steps_[index_];

  if (step.action == "motion") {
    finishStep(controller_.playMotion(step.group, step.index.value_or(-1), defaultPriority_));
    return;
  }
  if (step.action == "expression") {
    finishStep(controller_.setExpression(step.name, step.holdMs));
    return;
  }
  if (step.action == "move") {
    // 自主表演的移動閘門，每一步現讀（見 setMoveGate）。擋下來的步驟算成
    // 「做完了、什麼都沒做」而不是失敗：它不是錯誤，而且算成失敗會在
    // 非 bestEffort 的呼叫端把整段表演中止。
    if (moveGate_ && !moveGate_()) {
      finishStep(CommandResult::success());
      return;
    }
    // 自主表演的微移動：補間滑行過去（MCP 的 move 沒有 glideMs，永遠瞬移）
    if (step.glideMs && step.x && step.y) {
      QPointer<PerformRunner> self(this);
      controller_.glideTo(*step.x, *step.y, *step.glideMs, [self](CommandResult result) {
        if (self) self->finishStep(result);
      });
      return;
    }
    finishStep(controller_.moveTo(step.x, step.y, step.preset));
    return;
  }
  if (step.action == "parameters") {
    finishStep(controller_.setParameters(step.params));
    return;
  }
  if (step.action == "animate") {
    finishStep(controller_.animate(step.keyframes, step.animateOptions));
    return;
  }
  if (step.action == "wait") {
    const int ms = static_cast<int>(std::clamp(step.ms, 0.0, kMaxWaitMs));
    QPointer<PerformRunner> self(this);
    QTimer::singleShot(ms, this, [self] {
      if (self) self->finishStep(CommandResult::success());
    });
    return;
  }
  if (step.action == "speak") {
    QPointer<PerformRunner> self(this);
    // 自主表演的 bubble 模式：只出氣泡不經過 TTS（MCP 的 speak 永遠走下面那條）
    if (step.bubbleOnly) {
      controller_.mutter(step.text, [self](CommandResult result) {
        if (self) self->finishStep(result);
      });
      return;
    }
    AppController::SpeakRequest request;
    request.text = step.text;
    request.voice = step.voice;
    request.wait = step.speakWait;
    request.thinking = step.thinking;
    controller_.speak(request, [self](CommandResult result) {
      if (self) self->finishStep(result);
    });
    return;
  }

  finishStep(CommandResult::failure("Unknown step action: " + step.action));
}

void PerformRunner::finishStep(const CommandResult& result) {
  if (finished_) return;

  if (!result.ok) {
    const PerformStep& step = steps_[index_];
    // bestEffort（自主表演）：這一步失敗就跳過，剩下的照跑。
    // MCP 的 perform 不走這裡 —— AI 要靠「第幾步失敗」的錯誤自我修正。
    if (bestEffort_) {
      qDebug() << "[idle] 自主表演步驟失敗，跳過:" << index_ + 1 << QString::fromStdString(step.action) << QString::fromStdString(result.error);
      ++index_;
      runNext();
      return;
    }
    finish(CommandResult::failure("Step " + std::to_string(index_ + 1) + " (" + step.action + ") failed: " + result.error, result.hint));
    return;
  }

  ++index_;
  runNext();
}

void PerformRunner::finish(CommandResult result) {
  if (finished_) return;
  finished_ = true;
  if (done_) done_(std::move(result));
  deleteLater();
}

}  // namespace l2m
