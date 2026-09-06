// 「跟著聲音一起演」的押後規則：perform 的哪些步驟要等 TTS 真的出聲才執行。
// 判錯的兩個方向都是靜默的 —— 少押後就是「動作先演完、兩秒半後才出聲」原封不動，
// 多押後（例如把 wait 也吃進去）就是 AI 明寫的節奏被悄悄抹掉、動作全擠在同一拍。
#include <QtTest>

#include "core/perform_sync.h"

using namespace l2m;

namespace {

PerformStep stepOf(const std::string& action) {
  PerformStep step;
  step.action = action;
  return step;
}

PerformStep bubbleSpeak() {
  PerformStep step = stepOf("speak");
  step.bubbleOnly = true;
  return step;
}

}  // namespace

class TestPerformSync : public QObject {
  Q_OBJECT

private slots:
  // 伴奏步驟就是「角色長什麼樣」那四個。move 是位置變更、wait 是明寫的節奏，
  // 兩個都不算；speak 自己更不算（它才是被等的那一個）
  void companionActionsAreTheFourVisualOnes() {
    QVERIFY(isSpeechCompanionAction("motion"));
    QVERIFY(isSpeechCompanionAction("expression"));
    QVERIFY(isSpeechCompanionAction("parameters"));
    QVERIFY(isSpeechCompanionAction("animate"));

    QVERIFY(!isSpeechCompanionAction("move"));
    QVERIFY(!isSpeechCompanionAction("wait"));
    QVERIFY(!isSpeechCompanionAction("speak"));
    QVERIFY(!isSpeechCompanionAction("nonsense"));
  }

  // 最典型的那一串：動作押後，speak 自己不押後
  void defersWhenSpeakComesNext() {
    const std::vector<PerformStep> steps{stepOf("motion"), stepOf("speak")};
    QVERIFY(deferUntilSpeech(steps, 0));
    QVERIFY(!deferUntilSpeech(steps, 1));
  }

  // AI 最常寫的 [expression, motion, speak]：中間隔著同類步驟照樣整組押後
  void defersAcrossOtherCompanionSteps() {
    const std::vector<PerformStep> steps{stepOf("expression"), stepOf("motion"), stepOf("speak")};
    QVERIFY(deferUntilSpeech(steps, 0));
    QVERIFY(deferUntilSpeech(steps, 1));
    QVERIFY(!deferUntilSpeech(steps, 2));
  }

  // wait 是 AI 明寫的節奏（「動一下、頓半秒、再開口」），押後等於把它抹掉
  void waitBreaksTheGroup() {
    const std::vector<PerformStep> steps{stepOf("motion"), stepOf("wait"), stepOf("speak")};
    QVERIFY(!deferUntilSpeech(steps, 0));
  }

  // move 會自己照原順序執行，押後的視覺步驟跳過它就變成真的亂序
  void moveBreaksTheGroup() {
    const std::vector<PerformStep> steps{stepOf("motion"), stepOf("move"), stepOf("speak")};
    QVERIFY(!deferUntilSpeech(steps, 0));
  }

  // 後面根本沒有 speak：一個都不押後，不然那些動作永遠等不到放行的訊號
  void noSpeakMeansNoDeferral() {
    const std::vector<PerformStep> steps{stepOf("motion"), stepOf("expression")};
    QVERIFY(!deferUntilSpeech(steps, 0));
    QVERIFY(!deferUntilSpeech(steps, 1));
  }

  // 嘀咕（bubbleOnly）走 mutter，氣泡當場就出來、根本沒有那段空窗
  void bubbleOnlySpeakIsNotAGate() {
    const std::vector<PerformStep> steps{stepOf("motion"), bubbleSpeak()};
    QVERIFY(!deferUntilSpeech(steps, 0));
  }

  // speak 之後的視覺步驟不往回找 —— 它本來就落在開口之後
  void stepsAfterTheSpeakAreNotDeferred() {
    const std::vector<PerformStep> steps{stepOf("speak"), stepOf("motion")};
    QVERIFY(!deferUntilSpeech(steps, 1));
  }

  // 多段台詞：每一組各自認自己後面那一句
  void eachSpeakGathersItsOwnGroup() {
    const std::vector<PerformStep> steps{stepOf("motion"), stepOf("speak"), stepOf("expression"), stepOf("speak")};
    QVERIFY(deferUntilSpeech(steps, 0));
    QVERIFY(!deferUntilSpeech(steps, 1));
    QVERIFY(deferUntilSpeech(steps, 2));
    QVERIFY(!deferUntilSpeech(steps, 3));
  }

  // 越界與空陣列不能爆 —— 呼叫端是逐步推進的迴圈，邊界一定會走到
  void outOfRangeIsFalse() {
    const std::vector<PerformStep> steps{stepOf("motion")};
    QVERIFY(!deferUntilSpeech(steps, 1));
    QVERIFY(!deferUntilSpeech({}, 0));
  }
};

QTEST_GUILESS_MAIN(TestPerformSync)
#include "test_perform_sync.moc"
