// ParameterTracks：參數覆寫的 tween、hold、淡回與釋放的生命週期
#include <QtTest>

#include "core/parameter_tracks.h"

using namespace l2m;

namespace {

// 假的模型讀值：目前值與「放掉之後要回去的值」
ParameterTracks::ProbeFn probe(double value, double base = 0) {
  return [value, base](const std::string&) { return ParameterProbe{value, base}; };
}

std::optional<double> valueOf(ParameterTracks& tracks, double now, const std::string& id) {
  for (const auto& p : tracks.sample(now)) {
    if (p.id == id) return p.value;
  }
  return std::nullopt;
}

SetParameterRequest req(std::string id, double value) {
  SetParameterRequest r;
  r.id = std::move(id);
  r.value = value;
  return r;
}

}  // namespace

class TestParameterTracks : public QObject {
  Q_OBJECT

private slots:
  // === 立即設定 ===

  // 沒給 duration 就直接到位
  void immediateWithoutDuration() {
    ParameterTracks tracks;
    tracks.set({req("A", 1)}, probe(0), 1000);
    QCOMPARE(valueOf(tracks, 1000, "A").value(), 1.0);
  }

  // 預設是覆寫模式，不是疊加
  void defaultModeIsSet() {
    ParameterTracks tracks;
    tracks.set({req("A", 1)}, probe(0), 1000);
    QCOMPARE(tracks.sample(1000)[0].mode, ParameterMode::Set);
  }

  // 可以指定疊加模式，讓值跟內建的呼吸視線混在一起
  void addModeSupported() {
    ParameterTracks tracks;
    auto r = req("A", 1);
    r.mode = ParameterMode::Add;
    tracks.set({r}, probe(0), 1000);
    QCOMPARE(tracks.sample(1000)[0].mode, ParameterMode::Add);
  }

  // === 補間 ===

  // 從呼叫當下的值出發，不是從零／中間點線性插值／時間到停在目標值／不衝過頭
  void tweenLifecycle() {
    ParameterTracks tracks;
    auto r = req("A", 10);
    r.durationMs = 1000;
    tracks.set({r}, probe(0), 1000);
    QCOMPARE(valueOf(tracks, 1000, "A").value(), 0.0);
    QCOMPARE(valueOf(tracks, 1500, "A").value(), 5.0);
    QCOMPARE(valueOf(tracks, 2000, "A").value(), 10.0);
    QCOMPARE(valueOf(tracks, 9999, "A").value(), 10.0);
  }

  // 補間途中重設會從當下的值接續，不會跳回起點
  void retargetContinuesFromCurrent() {
    ParameterTracks t;
    auto r1 = req("A", 10);
    r1.durationMs = 1000;
    t.set({r1}, probe(0), 0);
    // 呼叫端在 500ms 讀到的現值是 5，重設成 0
    auto r2 = req("A", 0);
    r2.durationMs = 1000;
    t.set({r2}, probe(5), 500);
    QCOMPARE(valueOf(t, 500, "A").value(), 5.0);
    QCOMPARE(valueOf(t, 1000, "A").value(), 2.5);
  }

  // === 保持與自動歸位 ===

  // holdMs 省略時一直保持，不會自己消失
  void holdForeverWithoutHoldMs() {
    ParameterTracks tracks;
    tracks.set({req("A", 1)}, probe(0), 0);
    QCOMPARE(valueOf(tracks, 60000, "A").value(), 1.0);
  }

  // holdMs 到期後開始淡回 base（預設 500ms 淡回；smoothstep 的半程剛好也是 0.5）
  void fadesBackAfterHold() {
    ParameterTracks tracks;
    auto r = req("A", 1);
    r.holdMs = 1000;
    tracks.set({r}, probe(0), 0);
    QCOMPARE(valueOf(tracks, 1000, "A").value(), 1.0);
    QVERIFY(qFuzzyCompare(valueOf(tracks, 1250, "A").value(), 0.5));
  }

  // 淡回的最後一格剛好寫回 base，不會留下 0.01 的殘值
  void finalFrameWritesExactBase() {
    ParameterTracks tracks;
    auto r = req("A", 1);
    r.holdMs = 1000;
    tracks.set({r}, probe(0), 0);
    const auto result = tracks.sample(1500);
    QCOMPARE(result.size(), size_t(1));
    QVERIFY(result[0] == (ActiveParameter{"A", 0, ParameterMode::Set}));
  }

  // 寫完最後一格才清掉軌道，之後不再插手這個參數
  void removedAfterFinalFrame() {
    ParameterTracks tracks;
    auto r = req("A", 1);
    r.holdMs = 1000;
    tracks.set({r}, probe(0), 0);
    tracks.sample(1500);
    QVERIFY(tracks.sample(1600).empty());
    QCOMPARE(tracks.size(), size_t(0));
  }

  // holdMs 從補間結束才開始算，不是從呼叫當下
  void holdStartsAfterTween() {
    ParameterTracks tracks;
    auto r = req("A", 1);
    r.durationMs = 500;
    r.holdMs = 1000;
    tracks.set({r}, probe(0), 0);
    QCOMPARE(valueOf(tracks, 1400, "A").value(), 1.0);
    QCOMPARE(valueOf(tracks, 1500, "A").value(), 1.0);
    QVERIFY(qFuzzyCompare(valueOf(tracks, 1750, "A").value(), 0.5));
  }

  // === 釋放 ===

  // 指定 id 時只放掉那幾個
  void releaseOnlySpecified() {
    ParameterTracks tracks;
    tracks.set({req("A", 1), req("B", 1)}, probe(1), 0);
    tracks.release(std::vector<std::string>{"A"}, 0);
    tracks.sample(600);
    const auto remaining = tracks.sample(700);
    QCOMPARE(remaining.size(), size_t(1));
    QCOMPARE(remaining[0].id, std::string("B"));
  }

  // 不指定 id 就全部放掉
  void releaseAllWhenUnspecified() {
    ParameterTracks tracks;
    tracks.set({req("A", 1), req("B", 1)}, probe(1), 0);
    tracks.release(std::nullopt, 0);
    tracks.sample(600);
    QVERIFY(tracks.sample(700).empty());
  }

  // 釋放是淡出不是瞬斷，中途還讀得到值
  void releaseIsFadeNotCut() {
    ParameterTracks tracks;
    tracks.set({req("A", 1)}, probe(0), 0);
    tracks.release(std::vector<std::string>{"A"}, 0);
    QVERIFY(qFuzzyCompare(valueOf(tracks, 250, "A").value(), 0.5));
  }

  // 淡回是緩動不是線性：四分之一時間走的距離要比線性的少（兩端斜率為 0）。
  // 這一條釘的是「放開的那一刻不會憑空生出速度」—— 少了它，把時間拉長
  // 只是把同一個瞬斷拉成等速直線，看起來還是像被抽掉。
  void releaseEasesInsteadOfLinear() {
    ParameterTracks tracks;
    tracks.set({req("A", 1)}, probe(0), 0);
    tracks.release(std::vector<std::string>{"A"}, 0);
    // 線性會是 0.75；smoothstep(0.25) = 0.15625，所以還留在 0.84375
    QVERIFY(qFuzzyCompare(valueOf(tracks, 125, "A").value(), 0.84375));
    // 對稱地，四分之三時也比線性的 0.25 更接近終點
    QVERIFY(qFuzzyCompare(valueOf(tracks, 375, "A").value(), 0.15625));
  }

  // 放掉不存在的 id 不會爆炸
  void releaseUnknownIdIsSafe() {
    ParameterTracks tracks;
    tracks.release(std::vector<std::string>{"nope"}, 0);
    QCOMPARE(tracks.size(), size_t(0));
  }

  // clear 立刻清空，切換模型時不留殘影
  void clearRemovesEverything() {
    ParameterTracks tracks;
    tracks.set({req("A", 1)}, probe(0), 0);
    tracks.clear();
    QCOMPARE(tracks.size(), size_t(0));
    QVERIFY(tracks.sample(0).empty());
  }

  // === 診斷用資訊 ===

  // 列得出目前被接管的參數，用來查「為什麼臉沒變」
  void listsTakenOverParameters() {
    ParameterTracks tracks;
    tracks.set({req("B", 1), req("A", 0.5)}, probe(0), 0);
    auto ids = tracks.ids();
    std::sort(ids.begin(), ids.end());
    QCOMPARE(ids, (std::vector<std::string>{"A", "B"}));
    QCOMPARE(tracks.size(), size_t(2));
  }
};

QTEST_APPLESS_MAIN(TestParameterTracks)
#include "test_parameter_tracks.moc"
