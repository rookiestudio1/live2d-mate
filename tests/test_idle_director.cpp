// 閒置表演的決策層。用注入的定值亂數序列驅動，釘住兩條產品規則：
// 「有意義的動作會先被挑」「沒有具名表情時完全不碰表情」。
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>

#include "core/idle_director.h"

using namespace l2m;

namespace {

// 定值亂數序列：依序吐出佇列裡的值，用完退回 0.5
struct FakeRandom {
  std::deque<double> values;
  double next() {
    if (values.empty()) return 0.5;
    const double v = values.front();
    values.pop_front();
    return v;
  }
};

IdleDirector makeDirector(FakeRandom& random, double nowMs = 0) {
  IdleDirector::Deps deps;
  deps.random = [&random] { return random.next(); };
  deps.nowMs = [nowMs] { return nowMs; };
  return IdleDirector(deps);
}

// 兩個動作群組（beat 有命名意義）＋ 兩個表情（smile 有意義）的典型世界
IdleWorld richWorld() {
  IdleWorld world;
  world.motions.push_back({"beat", 2, {}});
  world.motions.push_back({"plain", 1, {}});
  world.expressions = {"smile", "F04"};
  world.annotations.motions["beat"] = "隨節奏打拍子";
  world.annotations.expressions["smile"] = "微笑";
  return world;
}

}  // namespace

class TestIdleDirector : public QObject {
  Q_OBJECT

private slots:
  // === idleLevelFor ===

  // 門檻 = base ×1 / ×3 / ×10（預設 180000 → 3 分 / 9 分 / 30 分）
  void levelThresholds() {
    const double base = 180000;
    QCOMPARE(idleLevelFor(base, base), IdleLevel::Fidget);
    QCOMPARE(idleLevelFor(base * 3 - 1, base), IdleLevel::Fidget);
    QCOMPARE(idleLevelFor(base * 3, base), IdleLevel::Bored);
    QCOMPARE(idleLevelFor(base * 10 - 1, base), IdleLevel::Bored);
    QCOMPARE(idleLevelFor(base * 10, base), IdleLevel::Sleepy);
    // base 沒設好時不炸，維持最低檔
    QCOMPARE(idleLevelFor(999999, 0), IdleLevel::Fidget);
  }

  // === plan ===

  // 空世界 → 空 vector
  void emptyWorldPlansNothing() {
    FakeRandom random;
    IdleDirector director = makeDirector(random);
    QVERIFY(director.plan(IdleWorld{}, {IdleLevel::Bored, "idle"}).empty());
  }

  // Fidget 只出 motion（不換表情）
  void fidgetOnlyEmitsMotion() {
    FakeRandom random;
    random.values = {0.0};
    IdleDirector director = makeDirector(random);
    const auto steps = director.plan(richWorld(), {IdleLevel::Fidget, "idle"});
    QCOMPARE(steps.size(), size_t(1));
    QCOMPARE(steps[0].action, std::string("motion"));
  }

  // Bored 才可能出 expression；自主表情一定帶 holdMs（自動退回，不依賴 idle.autoReset）
  void boredMayEmitExpressionWithHold() {
    FakeRandom random;
    // 消耗順序：要不要表情(0.0 < 0.5 → 要) → 挑表情 → 挑動作
    random.values = {0.0, 0.0, 0.0};
    IdleDirector director = makeDirector(random);
    const auto steps = director.plan(richWorld(), {IdleLevel::Bored, "idle"});
    QCOMPARE(steps.size(), size_t(2));
    QCOMPARE(steps[0].action, std::string("expression"));
    QCOMPARE(steps[0].name, std::string("smile"));
    QVERIFY(steps[0].holdMs.has_value());
    QVERIFY(*steps[0].holdMs > 0);
    QCOMPARE(steps[1].action, std::string("motion"));
  }

  // 模型沒有任何可用動作群組時，Fidget 也放行（有命名的）表情 ——
  // VTS 出身的模型常常沒有動作，表情是它唯一能表演的東西
  void motionlessModelMayEmitExpressionAtFidget() {
    IdleWorld world;
    world.expressions = {"smile"};
    world.annotations.expressions["smile"] = "微笑";
    FakeRandom random;
    random.values = {0.0, 0.0};  // 要表情 → 挑表情
    IdleDirector director = makeDirector(random);
    const auto steps = director.plan(world, {IdleLevel::Fidget, "idle"});
    QCOMPARE(steps.size(), size_t(1));
    QCOMPARE(steps[0].action, std::string("expression"));
    QCOMPARE(steps[0].name, std::string("smile"));
  }

  // 沒有任何表情有寫意義時完全不出 expression 步驟（亂套虛擬表情會讓角色看起來壞掉）
  void unannotatedExpressionsNeverUsed() {
    IdleWorld world = richWorld();
    world.annotations.expressions.clear();
    FakeRandom random;
    random.values = {0.0, 0.0, 0.0};
    IdleDirector director = makeDirector(random);
    for (int i = 0; i < 5; ++i) {
      for (const auto& step : director.plan(world, {IdleLevel::Bored, "idle"})) {
        QVERIFY(step.action != "expression");
      }
    }
  }

  // 有 annotation 的動作權重 ×3：兩個群組（beat ×3、plain ×1），
  // unit 0.7 在等權下會挑到第二個（0.7×2=1.4），加權後仍落在 beat（0.7×4=2.8 < 3）
  void annotatedMotionIsPreferred() {
    FakeRandom random;
    random.values = {0.7};
    IdleDirector director = makeDirector(random);
    const auto steps = director.plan(richWorld(), {IdleLevel::Fidget, "idle"});
    QCOMPARE(steps.size(), size_t(1));
    QCOMPARE(steps[0].group, std::string("beat"));
  }

  // 連續兩輪的動作群組不會相同（現況的均勻 bounded() 會 —— 最直觀的 before/after）
  void consecutiveRoundsDoNotRepeatMotion() {
    IdleWorld world;
    world.motions.push_back({"a", 1, {}});
    world.motions.push_back({"b", 1, {}});
    world.motions.push_back({"c", 1, {}});
    FakeRandom random;
    random.values = {0.0, 0.0, 0.0, 0.0};  // 每輪都指向第一個候選
    IdleDirector director = makeDirector(random);
    const auto first = director.plan(world, {IdleLevel::Fidget, "idle"});
    const auto second = director.plan(world, {IdleLevel::Fidget, "idle"});
    QCOMPARE(first.size(), size_t(1));
    QCOMPARE(second.size(), size_t(1));
    QVERIFY(first[0].group != second[0].group);
  }

  // 待機群組不參與挑選（它本來就會自己接）；只剩 Idle 的模型這輪就安靜
  void idleGroupIsExcluded() {
    IdleWorld world;
    world.motions.push_back({"Idle", 3, {}});
    FakeRandom random;
    IdleDirector director = makeDirector(random);
    QVERIFY(director.plan(world, {IdleLevel::Fidget, "idle"}).empty());
  }

  // Sleepy 有一定比例回空 vector（睡著的角色不該每分鐘準時動一下）
  void sleepySometimesPlansNothing() {
    FakeRandom quiet;
    quiet.values = {0.0};  // < kSleepyQuietChance → 這輪什麼都不做
    IdleDirector calm = makeDirector(quiet);
    QVERIFY(calm.plan(richWorld(), {IdleLevel::Sleepy, "idle"}).empty());

    FakeRandom active;
    active.values = {0.99, 0.99, 0.0};  // 不安靜、不換表情、挑動作
    IdleDirector lively = makeDirector(active);
    QVERIFY(!lively.plan(richWorld(), {IdleLevel::Sleepy, "idle"}).empty());
  }

  // === 台詞 ===

  // canSpeak 且有台詞時偶爾嘀咕一句；speak 步驟排在最後（動作先起跑）
  void idleMaySpeakALine() {
    IdleWorld world = richWorld();
    world.canSpeak = true;
    world.lines = {"今天也在偷懶嘛", "喝口水吧"};
    FakeRandom random;
    // Fidget：挑動作 → 要不要嘀咕(0.0 < 0.25 → 要) → 挑台詞
    random.values = {0.0, 0.0, 0.0};
    IdleDirector director = makeDirector(random);
    const auto steps = director.plan(world, {IdleLevel::Fidget, "idle"});
    QCOMPARE(steps.size(), size_t(2));
    QCOMPARE(steps[0].action, std::string("motion"));
    QCOMPARE(steps[1].action, std::string("speak"));
    QCOMPARE(steps[1].text, std::string("今天也在偷懶嘛"));
    QCOMPARE(steps[1].speakWait, true);
  }

  // canSpeak 為 false（autonomy.speech = off、或正在說話）→ 永遠不出 speak
  void mutedWorldNeverSpeaks() {
    IdleWorld world = richWorld();
    world.canSpeak = false;
    world.lines = {"不該被講出來"};
    FakeRandom random;
    random.values = {0.0, 0.0, 0.0, 0.0};
    IdleDirector director = makeDirector(random);
    for (const auto& step : director.plan(world, {IdleLevel::Fidget, "idle"})) {
      QVERIFY(step.action != "speak");
    }
  }

  // welcome：打招呼本身就是目的 —— 只出一個 speak 步驟，不做動作與表情
  void welcomeSpeaksOneLineOnly() {
    IdleWorld world = richWorld();
    world.canSpeak = true;
    world.lines = {"歡迎回來", "老闆我在"};
    FakeRandom random;
    random.values = {0.0};
    IdleDirector director = makeDirector(random);
    const auto steps = director.plan(world, {IdleLevel::Fidget, "welcome"});
    QCOMPARE(steps.size(), size_t(1));
    QCOMPARE(steps[0].action, std::string("speak"));
    QCOMPARE(steps[0].text, std::string("歡迎回來"));
  }

  // welcome 但沒有台詞（# Welcome Text 空）→ 整輪安靜，不硬湊
  void welcomeWithoutLinesIsSilent() {
    IdleWorld world = richWorld();
    world.canSpeak = true;
    FakeRandom random;
    IdleDirector director = makeDirector(random);
    QVERIFY(director.plan(world, {IdleLevel::Fidget, "welcome"}).empty());
  }

  // 連續打招呼不重複同一句（linePicker 的不重複記憶）
  void consecutiveWelcomesVaryLines() {
    IdleWorld world;
    world.canSpeak = true;
    world.lines = {"a", "b", "c"};
    FakeRandom random;
    random.values = {0.0, 0.0};
    IdleDirector director = makeDirector(random);
    const auto first = director.plan(world, {IdleLevel::Fidget, "welcome"});
    const auto second = director.plan(world, {IdleLevel::Fidget, "welcome"});
    QCOMPARE(first.size(), size_t(1));
    QCOMPARE(second.size(), size_t(1));
    QVERIFY(first[0].text != second[0].text);
  }

  // === 心情與熟悉度偏壓 ===

  // cheerful：同一個亂數在平常不夠格嘀咕（0.3 ≥ 0.25），心情好時夠（0.3 < 0.4）
  void cheerfulMoodRaisesSpeakChance() {
    IdleWorld world = richWorld();
    world.canSpeak = true;
    world.lines = {"哼哼♪"};

    FakeRandom neutral;
    neutral.values = {0.0, 0.3};  // 挑動作 → 嘀咕判定
    IdleDirector calm = makeDirector(neutral);
    IdlePlanOptions plain{IdleLevel::Fidget, "idle"};
    for (const auto& step : calm.plan(world, plain)) QVERIFY(step.action != "speak");

    FakeRandom happy;
    happy.values = {0.0, 0.3, 0.0};
    IdleDirector lively = makeDirector(happy);
    IdlePlanOptions cheerful{IdleLevel::Fidget, "idle"};
    cheerful.cheerful = true;
    const auto steps = lively.plan(world, cheerful);
    QVERIFY(std::any_of(steps.begin(), steps.end(), [](const PerformStep& s) { return s.action == "speak"; }));
  }

  // 熟悉度墊高說話機率：level 3 讓 0.3 過門檻（0.25 + 0.15 = 0.4）
  void familiarityRaisesSpeakChance() {
    IdleWorld world = richWorld();
    world.canSpeak = true;
    world.lines = {"老闆好"};
    FakeRandom random;
    random.values = {0.0, 0.3, 0.0};
    IdleDirector director = makeDirector(random);
    IdlePlanOptions options{IdleLevel::Fidget, "idle"};
    options.familiarityLevel = 3;
    const auto steps = director.plan(world, options);
    QVERIFY(std::any_of(steps.begin(), steps.end(), [](const PerformStep& s) { return s.action == "speak"; }));
  }

  // === 微移動 ===

  // Bored 且可移動：偶爾散步一小步（帶 glideMs 的 move 步驟，垂直不動）
  void boredMayWanderALittle() {
    IdleWorld world = richWorld();
    world.canMove = true;
    world.windowXRatio = 0.5;
    world.windowYRatio = 0.9;
    // Bored：表情判定(0.9 → 不換) → 挑動作(0.0) → 散步判定(0.0 → 要) → 步幅(0.99)
    FakeRandom random;
    random.values = {0.9, 0.0, 0.0, 0.99};
    IdleDirector director = makeDirector(random);
    const auto steps = director.plan(world, {IdleLevel::Bored, "idle"});
    const auto move = std::find_if(steps.begin(), steps.end(), [](const PerformStep& s) { return s.action == "move"; });
    QVERIFY(move != steps.end());
    QVERIFY(move->glideMs.has_value());
    QVERIFY(move->x.has_value());
    QVERIFY(std::abs(*move->x - 0.5) <= 0.08 + 1e-9);  // 步幅有上限
    QCOMPARE(*move->y, 0.9);                           // 垂直不動
  }

  // Fidget 也散步，但機率減半 —— 閒置分級吃「app 與 OS 閒置取小」，
  // 使用者在電腦前永遠到不了 Bored，只開放 Bored 的話散步根本看不到
  void fidgetMayWanderAtHalfChance() {
    IdleWorld world = richWorld();
    world.canMove = true;
    world.windowXRatio = 0.5;
    world.windowYRatio = 0.9;
    const auto hasMove = [](const std::vector<PerformStep>& steps) { return std::any_of(steps.begin(), steps.end(), [](const PerformStep& s) { return s.action == "move"; }); };

    // Fidget：挑動作(0.0) → 散步判定(0.05 < 0.08 → 要) → 步幅
    FakeRandom random;
    random.values = {0.0, 0.05, 0.99};
    IdleDirector director = makeDirector(random);
    QVERIFY(hasMove(director.plan(world, {IdleLevel::Fidget, "idle"})));

    // 同一個散步判定值（0.10）：Fidget（門檻 0.08）不動、Bored（0.15）會動
    FakeRandom fidgetRandom;
    fidgetRandom.values = {0.0, 0.10, 0.99};
    IdleDirector fidget = makeDirector(fidgetRandom);
    QVERIFY(!hasMove(fidget.plan(world, {IdleLevel::Fidget, "idle"})));
    FakeRandom boredRandom;
    boredRandom.values = {0.9, 0.0, 0.10, 0.99};
    IdleDirector bored = makeDirector(boredRandom);
    QVERIFY(hasMove(bored.plan(world, {IdleLevel::Bored, "idle"})));
  }

  // 鎖定位置（canMove=false）永遠不散步
  void lockedNeverMoves() {
    IdleWorld world = richWorld();
    world.canMove = false;
    world.windowXRatio = 0.5;
    world.windowYRatio = 0.9;
    FakeRandom random;
    random.values = {0.9, 0.0, 0.0, 0.0};
    IdleDirector locked = makeDirector(random);
    for (const auto& step : locked.plan(world, {IdleLevel::Bored, "idle"})) {
      QVERIFY(step.action != "move");
    }
  }

  // 步數永遠不超過 perform 的上限（與 MCP 共用同一個常數）
  void planNeverExceedsMaxSteps() {
    FakeRandom random;
    IdleDirector director = makeDirector(random);
    for (const auto level : {IdleLevel::Fidget, IdleLevel::Bored, IdleLevel::Sleepy}) {
      QVERIFY(director.plan(richWorld(), {level, "idle"}).size() <= kMaxPerformSteps);
    }
  }
};

QTEST_GUILESS_MAIN(TestIdleDirector)
#include "test_idle_director.moc"
