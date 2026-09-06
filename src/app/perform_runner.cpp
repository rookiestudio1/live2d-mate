#include "perform_runner.h"

#include <QDebug>
#include <QPointer>
#include <QTimer>

#include <algorithm>

#include "app_controller.h"
#include "core/perform_sync.h"

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

  // 押後到「真的開口」那一刻（見標頭與 core/perform_sync.h）。
  // 這裡只排隊不執行，所以直接推進到下一步。
  if (deferUntilSpeech(steps_, index_)) {
    deferred_.push_back(index_);
    ++index_;
    runNext();
    return;
  }

  // 後面沒有接 speak 的視覺步驟（或中間隔著 move／wait）照舊當場執行
  if (isSpeechCompanionAction(step.action)) {
    finishStep(runCompanionStep(step));
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
    // 押後的視覺步驟在「真的開口」那一刻放行 —— 跟氣泡同一拍
    request.onSpeechStart = [self] {
      if (self) self->flushDeferred();
    };
    controller_.speak(request, [self](CommandResult result) {
      if (!self) return;
      // 保險：onSpeechStart 一次都沒觸發也不能把步驟弄丟（見標頭）
      self->flushDeferred();
      self->finishStep(result);
    });
    return;
  }

  finishStep(CommandResult::failure("Unknown step action: " + step.action));
}

CommandResult PerformRunner::runCompanionStep(const PerformStep& step) {
  if (step.action == "motion") return controller_.playMotion(step.group, step.index.value_or(-1), defaultPriority_);
  if (step.action == "expression") return controller_.setExpression(step.name, step.holdMs);
  if (step.action == "parameters") return controller_.setParameters(step.params);
  if (step.action == "animate") return controller_.animate(step.keyframes, step.animateOptions);
  return CommandResult::failure("Unknown step action: " + step.action);
}

void PerformRunner::flushDeferred() {
  if (finished_ || deferred_.empty()) return;
  // 先整份搬走：某一步失敗時 finish() 會沿著 done_ 走出去，那條路上不該
  // 再看到一份清到一半的清單
  std::vector<size_t> pending;
  pending.swap(deferred_);

  for (size_t i : pending) {
    const PerformStep& step = steps_[i];
    const CommandResult result = runCompanionStep(step);
    if (result.ok) continue;
    if (bestEffort_) {
      qDebug() << "[idle] 自主表演步驟失敗，跳過:" << i + 1 << QString::fromStdString(step.action) << QString::fromStdString(result.error);
      continue;
    }
    // 報的是「第幾步」而不是「第幾個押後的」—— AI 看到的步號要跟它自己送的
    // 陣列對得起來，押後純粹是內部的時序調整
    finish(CommandResult::failure("Step " + std::to_string(i + 1) + " (" + step.action + ") failed: " + result.error, result.hint));
    return;
  }
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
