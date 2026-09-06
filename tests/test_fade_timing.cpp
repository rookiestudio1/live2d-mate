// 角色出現與消失的 alpha 曲線，見 core/fade_timing.h。
#include <QtTest>

#include <cmath>

#include "core/fade_timing.h"

using namespace l2m;

namespace {

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

}  // namespace

class TestFadeTiming : public QObject {
  Q_OBJECT

private slots:
  // 常數就是需求本身：四個時機共用的 400 ms
  void durationIsFourHundredMs() { QVERIFY(near(kFadeDurationMs, 400.0)); }

  // 沒有動過的 Fade 是全不透明的 —— 沒有模型的視窗本來就什麼都不畫，
  // 預設值若是 0，之後任何忘了 begin() 的路徑都會變成永遠隱形
  void defaultsToOpaque() {
    Fade fade;
    QVERIFY(near(fade.alphaAt(0.0), 1.0));
    QVERIFY(near(fade.alphaAt(12345.0), 1.0));
    QVERIFY(fade.finished(0.0));
  }

  // 從全不透明淡出：起點 1、終點 0，剛好走滿 400 ms
  void fadeOutSpansFullDuration() {
    Fade fade;
    fade.begin(0.0, 1000.0);
    QVERIFY(near(fade.alphaAt(1000.0), 1.0));
    QVERIFY(near(fade.alphaAt(1400.0), 0.0));
    QVERIFY(!fade.finished(1399.0));
    QVERIFY(fade.finished(1400.0));
  }

  // 模型載入完成的用法：set(0) 之後第一幀就是全透明，再 begin(1) 淡入
  void fadeInFromPinnedZero() {
    Fade fade;
    fade.set(0.0);
    QVERIFY(near(fade.alphaAt(0.0), 0.0));
    QVERIFY(fade.finished(0.0));  // 只是定住，沒有動畫在跑
    fade.begin(1.0, 0.0);
    QVERIFY(near(fade.alphaAt(0.0), 0.0));
    QVERIFY(near(fade.alphaAt(400.0), 1.0));
  }

  // 單調：alpha 只會往目標走，不會抖回去
  void alphaIsMonotonic() {
    Fade fade;
    fade.begin(0.0, 0.0);
    double previous = 2.0;
    for (int i = 0; i <= 100; ++i) {
      const double alpha = fade.alphaAt(400.0 * i / 100.0);
      QVERIFY(alpha <= previous + 1e-12);
      previous = alpha;
    }
    QVERIFY(near(previous, 0.0));
  }

  // smoothstep：中點剛好在半途，但兩端明顯比等速慢（開場不會「啪」地掉一截）
  void easesAtBothEnds() {
    Fade fade;
    fade.begin(0.0, 0.0);
    QVERIFY(near(fade.alphaAt(200.0), 0.5));
    // 5% 的時間只走了 0.05² × (3 − 0.1) = 0.725% 的行程（等速會是 5%）
    QVERIFY(fade.alphaAt(20.0) > 0.99);
    QVERIFY(fade.alphaAt(380.0) < 0.01);
  }

  // 中途反轉不跳：淡出到一半改成淡入，當下的 alpha 必須原地接上
  void reversalDoesNotJump() {
    Fade fade;
    fade.begin(0.0, 0.0);
    const double half = fade.alphaAt(200.0);
    QVERIFY(near(half, 0.5));
    fade.begin(1.0, 200.0);
    QVERIFY(near(fade.alphaAt(200.0), half));
  }

  // 反轉後只跑剩下的距離：0.5 → 1.0 花 200 ms，不是重新算滿 400 ms
  void reversalKeepsConstantSpeed() {
    Fade fade;
    fade.begin(0.0, 0.0);
    fade.begin(1.0, 200.0);
    QVERIFY(!fade.finished(399.0));
    QVERIFY(fade.finished(400.0));
    QVERIFY(near(fade.alphaAt(400.0), 1.0));
    QVERIFY(near(fade.alphaAt(300.0), 0.75));  // 中點：0.5 + 0.5 × 0.5
  }

  // 已經在目標上就沒有動畫（重複 setVisible(true) 是 no-op，不該再淡一次）
  void beginToSameTargetFinishesImmediately() {
    Fade fade;
    fade.begin(1.0, 500.0);
    QVERIFY(fade.finished(500.0));
    QVERIFY(near(fade.alphaAt(500.0), 1.0));
  }

  // 目標值一律夾在 0..1；呼叫端算錯不該讓模型變成負 alpha
  void targetIsClamped() {
    Fade fade;
    fade.begin(2.5, 0.0);
    QVERIFY(near(fade.target(), 1.0));
    fade.set(-3.0);
    QVERIFY(near(fade.alphaAt(0.0), 0.0));
  }

  // nowMs 早於起點（時鐘還沒 start）時停在起點，不是外推出去的 NaN／負值
  void beforeStartStaysAtStartValue() {
    Fade fade;
    fade.begin(0.0, 1000.0);
    const double early = fade.alphaAt(0.0);
    QVERIFY(!std::isnan(early));
    QVERIFY(near(early, 1.0));
  }
};

QTEST_APPLESS_MAIN(TestFadeTiming)
#include "test_fade_timing.moc"
