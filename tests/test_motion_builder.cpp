// 由關鍵影格組出 motion3.json（buildMotion3）：曲線、段數、Meta、排序與輸入驗證
#include <QtTest>

#include <stdexcept>

#include "core/motion_builder.h"

using namespace l2m;

namespace {

// 依 Cubism 規格重新數一遍段數與點數，用來驗 Meta 沒有唬人
struct Tally {
  int segments = 0;
  int points = 0;
};

Tally tally(const Motion3& motion) {
  Tally t;
  for (const auto& curve : motion.curves) {
    // 開頭兩個數字是第一個點，之後每段是「型別 + 兩個數字」
    t.points += 1;
    for (size_t i = 2; i < curve.segments.size(); i += 3) {
      t.segments += 1;
      t.points += 1;
    }
  }
  return t;
}

const Motion3Curve* curveOf(const Motion3& motion, const std::string& id) {
  for (const auto& c : motion.curves) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

Keyframe kf(double at, std::vector<std::pair<std::string, double>> params) { return Keyframe{at, std::move(params)}; }

// 驗證丟出的例外訊息含有指定子字串
template <typename Fn>
bool throwsWithMessage(Fn fn, const char* fragment) {
  try {
    fn();
  } catch (const std::runtime_error& e) {
    return std::string(e.what()).find(fragment) != std::string::npos;
  }
  return false;
}

}  // namespace

class TestMotionBuilder : public QObject {
  Q_OBJECT

  Motion3 wave_ = buildMotion3({
    kf(0, {{"Param73", 0}}),
    kf(250, {{"Param73", 1}}),
    kf(500, {{"Param73", 0}}),
  });

private slots:
  // === buildMotion3 ===

  // 每個參數變成一條 Parameter 曲線
  void eachParamBecomesOneCurve() {
    QCOMPARE(wave_.curves.size(), size_t(1));
    QCOMPARE(wave_.curves[0].id, std::string("Param73"));
  }

  // Segments 用線性段編碼：開頭是第一個點，之後每段是「0, 時間, 值」
  void segmentsEncodedAsLinear() { QCOMPARE(wave_.curves[0].segments, (std::vector<double>{0, 0, 0, 0.25, 1, 0, 0.5, 0})); }

  // Duration 是最後一個 keyframe 的秒數
  void durationIsLastKeyframe() { QCOMPARE(wave_.meta.duration, 0.5); }

  // Meta 的三個計數跟實際資料對得起來，Cubism 靠它們配置陣列
  void metaCountsMatchData() {
    QCOMPARE(wave_.meta.curveCount, int(wave_.curves.size()));
    QCOMPARE(wave_.meta.totalSegmentCount, tally(wave_).segments);
    QCOMPARE(wave_.meta.totalPointCount, tally(wave_).points);
  }

  // 版本與必要的 Meta 欄位齊全
  void versionAndMetaFields() {
    QCOMPARE(wave_.meta.fps, 30);
    QCOMPARE(wave_.meta.userDataCount, 0);
    QCOMPARE(wave_.meta.totalUserDataSize, 0);
  }

  // keyframe 順序顛倒也照樣照時間排好
  void shuffledKeyframesSorted() {
    const auto shuffled = buildMotion3({
      kf(500, {{"Param73", 0}}),
      kf(0, {{"Param73", 0}}),
      kf(250, {{"Param73", 1}}),
    });
    QCOMPARE(shuffled.curves[0].segments, wave_.curves[0].segments);
  }

  // === 曲線補平 ===

  // 只出現在中間的參數，頭尾各補一個同值的點
  void midOnlyParamPaddedBothEnds() {
    const auto partial = buildMotion3({
      kf(0, {{"A", 0}}),
      kf(250, {{"A", 1}, {"B", 5}}),
      kf(500, {{"A", 0}}),
    });
    // B 只在 250ms 寫了 5，補成 0ms 就是 5、500ms 還是 5
    QVERIFY(curveOf(partial, "B"));
    QCOMPARE(curveOf(partial, "B")->segments, (std::vector<double>{0, 5, 0, 0.25, 5, 0, 0.5, 5}));

    // 補平後每條曲線都至少有兩個點，不會產出零段曲線
    for (const auto& curve : partial.curves) QVERIFY(curve.segments.size() >= 5);

    // 已經涵蓋頭尾的參數不會被多補點
    QCOMPARE(curveOf(partial, "A")->segments, (std::vector<double>{0, 0, 0, 0.25, 1, 0, 0.5, 0}));
  }

  // === 選項 ===

  // loop 與淡入淡出時間寫進 Meta，單位換成秒
  void optionsWrittenToMeta() {
    const std::vector<Keyframe> frames{kf(0, {{"A", 0}}), kf(1000, {{"A", 1}})};
    BuildMotionOptions options;
    options.loop = true;
    options.fadeInMs = 200;
    options.fadeOutMs = 300;
    const auto motion = buildMotion3(frames, options);
    QCOMPARE(motion.meta.loop, true);
    QCOMPARE(motion.meta.fadeInTime, 0.2);
    QCOMPARE(motion.meta.fadeOutTime, 0.3);

    // 沒指定時不循環，淡入淡出用預設值
    const auto defaults = buildMotion3(frames);
    QCOMPARE(defaults.meta.loop, false);
    QCOMPARE(defaults.meta.fadeInTime, 0.2);
    QCOMPARE(defaults.meta.fadeOutTime, 0.2);
  }

  // === 輸入檢查 ===

  // 沒有 keyframe／只有一個 keyframe 就丟例外，訊息要講得出該怎麼改
  void rejectsTooFewKeyframes() {
    QVERIFY(throwsWithMessage([] { buildMotion3({}); }, "at least two keyframes"));
    QVERIFY(throwsWithMessage([] { buildMotion3({kf(0, {{"A", 1}})}); }, "at least two keyframes"));
  }

  // 所有 keyframe 都在同一個時間點，總長度是零，丟例外
  void rejectsZeroDuration() {
    QVERIFY(throwsWithMessage([] { buildMotion3({kf(0, {{"A", 0}}), kf(0, {{"A", 1}})}); }, "duration"));
  }

  // 負的時間點丟例外，免得算出長度是負數的動作
  void rejectsNegativeTime() {
    QVERIFY(throwsWithMessage([] { buildMotion3({kf(-100, {{"A", 0}}), kf(500, {{"A", 1}})}); }, "negative"));
  }

  // 一個參數都沒寫的 keyframe 組合丟例外
  void rejectsNoParameters() {
    QVERIFY(throwsWithMessage([] { buildMotion3({kf(0, {}), kf(500, {})}); }, "no parameters"));
  }

  // === nextAiMotionSlot ===

  // 沒有 AI 動作在播時用第 0 格；第 0 格在播就換第 1 格；第 1 格在播就換回第 0 格
  void slotAlternates() {
    QCOMPARE(nextAiMotionSlot(std::nullopt), 0);
    QCOMPARE(nextAiMotionSlot(0), 1);
    QCOMPARE(nextAiMotionSlot(1), 0);

    // 連續呼叫時每一次都跟前一次不同格
    int slot = nextAiMotionSlot(std::nullopt);
    std::vector<int> used{slot};
    for (int i = 0; i < 5; ++i) {
      slot = nextAiMotionSlot(slot);
      used.push_back(slot);
    }
    for (size_t i = 1; i < used.size(); ++i) QVERIFY(used[i] != used[i - 1]);
  }

  // === 參數保持不動的寫法（工具說明裡的錯／對範例）===
  // 這兩個是特徵測試：把 animate 工具說明與 README 講的語意釘住，
  // 之後有人改動內插規則時，文件會跟著一起亮紅燈。

  // 只在頭尾被提到的參數會整段慢慢漂移 —— 最容易踩的坑
  void headTailOnlyParamDrifts() {
    const auto motion = buildMotion3({
      kf(0, {{"ParamArmLB", 0}}),
      kf(9000, {{"ParamArmLB", 5}}),
    });
    // 只有兩個點、中間沒有轉折，所以 9 秒內一路內插
    QCOMPARE(curveOf(motion, "ParamArmLB")->segments, (std::vector<double>{0, 0, 0, 9, 5}));
  }

  // 在該段開始前再壓一次原值，前面那段就會是平的
  void rePressBeforeSegmentKeepsFlat() {
    const auto motion = buildMotion3({
      kf(0, {{"ParamArmLB", 0}}),
      kf(8200, {{"ParamArmLB", 0}}),
      kf(9000, {{"ParamArmLB", 5}}),
    });
    QCOMPARE(curveOf(motion, "ParamArmLB")->segments, (std::vector<double>{0, 0, 0, 8.2, 0, 0, 9, 5}));
  }
};

QTEST_APPLESS_MAIN(TestMotionBuilder)
#include "test_motion_builder.moc"
