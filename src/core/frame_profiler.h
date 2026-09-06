#pragma once

// 幀時間探針（診斷用）。
//
// 設定環境變數 L2M_PROFILE 後啟用：每 2 秒把各階段耗時的
// 平均／中位數／p95／最大值與實際幀率印到 qDebug。
// 未啟用時 ProfileScope 只做一次 static bool 判斷，成本可忽略。
//
// 注意：GL 指令是非同步的，這裡量到的是 CPU 端提交時間；
// 真正的 GPU 停頓會集中反映在 StageMaskRead（glReadPixels 會等管線排空）。

#include <QDebug>
#include <QElapsedTimer>
#include <QString>

#include <algorithm>
#include <array>
#include <vector>

namespace l2m {

class FrameProfiler {
public:
  enum Stage {
    StageUpdate,       // 模型參數更新（motion / physics / breath）
    StageDraw,         // 主畫面繪製指令提交
    StageMaskDraw,     // 命中遮罩的離屏重繪
    StageMaskRead,     // 命中遮罩的 glReadPixels 回讀（含 GPU 同步停頓）
    StageRegionBuild,  // QRegion 產生（門檻 + 膨脹 + 逐列合併）
    StageRegionApply,  // QRegion → HRGN → SetWindowRgn
    StageFrame,        // 整個 paintGL
    StageCount
  };

  static bool enabled() {
    static const bool on = qEnvironmentVariableIsSet("L2M_PROFILE");
    return on;
  }

  static FrameProfiler& instance() {
    static FrameProfiler profiler;
    return profiler;
  }

  void add(Stage stage, double ms) { samples_[stage].push_back(ms); }

  // 形狀落後一幀時會被裁掉的格數 / 角色總格數
  void addClipSample(int outsideCells, int totalCells) {
    clipOutside_ += outsideCells;
    clipTotal_ += totalCells;
    clipWorst_ = std::max(clipWorst_, outsideCells);
    clipSamples_++;
  }

  // 每幀結尾呼叫；累積滿一個報告週期就輸出並清空
  void tick() {
    if (!wall_.isValid()) {
      wall_.start();
      return;
    }
    if (wall_.elapsed() < kReportIntervalMs) return;
    report(wall_.restart());
  }

private:
  static constexpr qint64 kReportIntervalMs = 2000;

  static const char* stageName(Stage stage) {
    switch (stage) {
      case StageUpdate:
        return "update";
      case StageDraw:
        return "draw";
      case StageMaskDraw:
        return "mask.draw";
      case StageMaskRead:
        return "mask.read";
      case StageRegionBuild:
        return "rgn.build";
      case StageRegionApply:
        return "rgn.apply";
      case StageFrame:
        return "FRAME";
      default:
        return "?";
    }
  }

  void report(qint64 windowMs) {
    const auto frames = samples_[StageFrame].size();
    qInfo().noquote() << QStringLiteral("[perf] ── %1 幀 / %2 ms = %3 fps ──").arg(frames).arg(windowMs).arg(windowMs > 0 ? frames * 1000.0 / windowMs : 0.0, 0, 'f', 1);
    for (int s = 0; s < StageCount; ++s) {
      auto& values = samples_[s];
      if (values.empty()) continue;
      std::sort(values.begin(), values.end());
      double sum = 0;
      for (double v : values) sum += v;
      const auto p95 = values[std::min(values.size() - 1, size_t(values.size() * 95 / 100))];
      qInfo().noquote() << QStringLiteral("[perf]   %1 n=%2 avg=%3 p50=%4 p95=%5 max=%6 ms")
                             .arg(QString::fromLatin1(stageName(Stage(s))), -10)
                             .arg(values.size(), 4)
                             .arg(sum / values.size(), 7, 'f', 2)
                             .arg(values[values.size() / 2], 7, 'f', 2)
                             .arg(p95, 7, 'f', 2)
                             .arg(values.back(), 7, 'f', 2);
      values.clear();
    }
    if (clipSamples_ > 0) {
      qInfo().noquote() << QStringLiteral("[perf]   clip       n=%1 平均落後格數=%2 最差=%3 佔角色=%4%")
                             .arg(clipSamples_, 4)
                             .arg(double(clipOutside_) / clipSamples_, 7, 'f', 1)
                             .arg(clipWorst_, 5)
                             .arg(clipTotal_ > 0 ? 100.0 * clipOutside_ / clipTotal_ : 0.0, 5, 'f', 2);
      clipOutside_ = 0;
      clipTotal_ = 0;
      clipWorst_ = 0;
      clipSamples_ = 0;
    }
  }

  std::array<std::vector<double>, StageCount> samples_;
  long long clipOutside_ = 0;
  long long clipTotal_ = 0;
  int clipWorst_ = 0;
  int clipSamples_ = 0;
  QElapsedTimer wall_;
};

// RAII 計時區塊：建構開始計時，解構時把耗時記進對應階段
class ProfileScope {
public:
  explicit ProfileScope(FrameProfiler::Stage stage) : stage_(stage) {
    if (FrameProfiler::enabled()) timer_.start();
  }
  ~ProfileScope() {
    if (FrameProfiler::enabled()) FrameProfiler::instance().add(stage_, timer_.nsecsElapsed() / 1000000.0);
  }

  ProfileScope(const ProfileScope&) = delete;
  ProfileScope& operator=(const ProfileScope&) = delete;

private:
  FrameProfiler::Stage stage_;
  QElapsedTimer timer_;
};

}  // namespace l2m
