// 「動作播完之後接哪一段」的挑選規則。挑錯／挑不到的後果不是不好看而是定格：
// 沒有動作在驅動模型時，restoreBaseline 會把它還原成 moc3 的預設狀態，
// 而那對三分之一的碧藍航線模型來說是「所有肢體變體一起開著」（4 隻腳／4 隻手）
#include <QtTest>

#include "core/idle_motion_pick.h"

using namespace l2m;

class TestIdleMotionPick : public QObject {
  Q_OBJECT

private slots:
  // 官方慣例：群組名就叫 Idle → 維持「群組內隨機」的既有行為
  void idleGroupWinsAndStaysRandom() {
    const std::vector<MotionGroupFiles> groups{
      {"TapBody", {"motions/tap.motion3.json"}},
      {"Idle", {"motions/idle_01.motion3.json", "motions/idle_02.motion3.json"}},
    };
    const auto slot = pickIdleMotion(groups);
    QVERIFY(slot.has_value());
    QCOMPARE(slot->group, std::string("Idle"));
    QCOMPARE(slot->index, -1);
  }

  // 群組名的比對不分大小寫（作者寫 idle／IDLE 的都有）
  void idleGroupNameIsCaseInsensitive() {
    const auto slot = pickIdleMotion({{"idle", {"a.motion3.json"}}});
    QVERIFY(slot.has_value());
    QCOMPARE(slot->group, std::string("idle"));
  }

  // 碧藍航線那批：15 支動作全塞在同一個空字串群組，待機只以檔名存在。
  // 這一條沒接住的話 GetMotionCount("Idle") 永遠是 0，動作播完就再也沒人驅動模型。
  void fileNamedIdleInUnnamedGroup() {
    const std::vector<MotionGroupFiles> groups{{"",
                                                {
                                                  "motions/complete.motion3.json",
                                                  "motions/home.motion3.json",
                                                  "motions/idle.motion3.json",
                                                  "motions/login.motion3.json",
                                                }}};
    const auto slot = pickIdleMotion(groups);
    QVERIFY(slot.has_value());
    QCOMPARE(slot->group, std::string(""));
    QCOMPARE(slot->index, 2);  // 指名到 idle.motion3.json，不是群組內隨機
  }

  // 精確的檔名要贏過模糊的群組名：touch_idle 是點擊的待機變體，不是待機動畫
  void exactFileBeatsFuzzyGroupName() {
    const std::vector<MotionGroupFiles> groups{
      {"touch_idle", {"motions/touch_idle.motion3.json"}},
      {"", {"motions/idle.motion3.json"}},
    };
    const auto slot = pickIdleMotion(groups);
    QVERIFY(slot.has_value());
    QCOMPARE(slot->group, std::string(""));
    QCOMPARE(slot->index, 0);
  }

  // 精確比對都落空時才輪到模糊的群組名（Idle2、idle_loop…）
  void fuzzyGroupNameIsTheThirdChoice() {
    const std::vector<MotionGroupFiles> groups{
      {"Tap", {"motions/tap.motion3.json"}},
      {"Idle2", {"motions/a.motion3.json", "motions/b.motion3.json"}},
    };
    const auto slot = pickIdleMotion(groups);
    QVERIFY(slot.has_value());
    QCOMPARE(slot->group, std::string("Idle2"));
    QCOMPARE(slot->index, -1);
  }

  // 最後才是模糊的檔名（idle_01 這種帶編號的寫法）
  void fuzzyFileNameIsTheLastChoice() {
    const std::vector<MotionGroupFiles> groups{
      {"Tap", {"motions/tap.motion3.json"}},
      {"Main", {"motions/main.motion3.json", "motions/idle_01.motion3.json"}},
    };
    const auto slot = pickIdleMotion(groups);
    QVERIFY(slot.has_value());
    QCOMPARE(slot->group, std::string("Main"));
    QCOMPARE(slot->index, 1);
  }

  // 目錄與副檔名不算進比對：motions/IDLE.motion3.json 也要認得
  void pathAndExtensionAreStripped() {
    const auto slot = pickIdleMotion({{"", {"some/deep/dir/IDLE.motion3.json"}}});
    QVERIFY(slot.has_value());
    QCOMPARE(slot->index, 0);
  }

  // 空群組不算數：挑到了 startMotion 也會被 GetMotionCount 當場回絕
  void emptyGroupsAreSkipped() {
    const std::vector<MotionGroupFiles> groups{
      {"Idle", {}},
      {"Main", {"motions/main.motion3.json"}},
    };
    QVERIFY(!pickIdleMotion(groups).has_value());
  }

  // 規則 1 與規則 2 同時命中時，群組名 Idle 要贏（否則「群組內隨機」的既有行為
  // 會被別的群組裡一支叫 idle 的檔案搶走）
  void idleGroupBeatsIdleFileElsewhere() {
    const std::vector<MotionGroupFiles> groups{
      {"", {"motions/idle.motion3.json"}},
      {"Idle", {"motions/a.motion3.json"}},
    };
    const auto slot = pickIdleMotion(groups);
    QVERIFY(slot.has_value());
    QCOMPARE(slot->group, std::string("Idle"));
    QCOMPARE(slot->index, -1);
  }

  // 同一群組裡兩支都叫 idle 時取第一支（挑選必須是決定性的，
  // 不然每次載入模型接回的待機動畫都可能不一樣）
  void firstMatchingFileWins() {
    const std::vector<MotionGroupFiles> groups{{"", {"a/idle.motion3.json", "b/idle.motion3.json"}}};
    const auto slot = pickIdleMotion(groups);
    QVERIFY(slot.has_value());
    QCOMPARE(slot->index, 0);
  }

  // 挑不出來要老實回 nullopt —— 呼叫端靠這個決定「連 baseline 都不要還原」
  void noIdleAtAllReturnsNullopt() {
    const std::vector<MotionGroupFiles> groups{
      {"TapBody", {"motions/tap.motion3.json"}},
      {"Main", {"motions/main_1.motion3.json", "motions/main_2.motion3.json"}},
    };
    QVERIFY(!pickIdleMotion(groups).has_value());
    QVERIFY(!pickIdleMotion({}).has_value());
  }
};

QTEST_APPLESS_MAIN(TestIdleMotionPick)
#include "test_idle_motion_pick.moc"
