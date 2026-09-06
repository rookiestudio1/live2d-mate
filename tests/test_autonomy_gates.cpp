// 自主移動的閘門：三個開關的真值表。
// 這條規則要在相隔一整趟網路往返的兩個地方各判一次（規劃前算成 IdleWorld::canMove、
// 執行前由 runAutonomous 現讀），寫成兩份字面量遲早會漂移，所以收斂成一支純函式。
#include <QtTest>

#include "core/autonomy_gates.h"

using namespace l2m;

class TestAutonomyGates : public QObject {
  Q_OBJECT

private slots:
  // 預設就是不准動 —— 隨機移動是使用者要主動打開的東西，不是預設行為
  void defaultsToNotAllowed() {
    const AppConfig config = defaultConfig();
    QCOMPARE(config.autonomy.move, false);
    QVERIFY(!autonomousMoveAllowed(config));
  }

  // 三個條件全部成立才放行；任何一個倒下就整個不准
  void allowedOnlyWhenAllThreeAgree() {
    AppConfig config = defaultConfig();
    config.autonomy.move = true;
    config.interaction.lockPosition = false;
    config.interaction.dragMove = true;
    QVERIFY(autonomousMoveAllowed(config));

    // 位置鎖著時誰都不准動它
    config.interaction.lockPosition = true;
    QVERIFY(!autonomousMoveAllowed(config));
    config.interaction.lockPosition = false;

    // 連手拖都不給拖的人，更不會想要它自己走
    config.interaction.dragMove = false;
    QVERIFY(!autonomousMoveAllowed(config));
    config.interaction.dragMove = true;

    // 隨機移動關掉：其餘兩個再怎麼開也沒用
    config.autonomy.move = false;
    QVERIFY(!autonomousMoveAllowed(config));
  }
};

QTEST_GUILESS_MAIN(TestAutonomyGates)
#include "test_autonomy_gates.moc"
