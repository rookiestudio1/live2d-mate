// 點擊要播哪一段動作的挑選規則（pickTapMotion）
#include <QtTest>

#include "core/interaction_logic.h"

using namespace l2m;

namespace {

// 一個群組配一個同名的動作檔（官方模型的常見寫法）
MotionGroupInfo groupOf(const std::string& name) { return MotionGroupInfo{name, 1, {name + ".motion3.json"}}; }

std::vector<MotionGroupInfo> groupsOf(const std::vector<std::string>& names) {
  std::vector<MotionGroupInfo> out;
  for (const auto& name : names) out.push_back(groupOf(name));
  return out;
}

// 碧藍航線那種「全部塞在同一個空字串群組」的寫法（實測佔語料庫的 52%）
MotionGroupInfo nameless(const std::vector<std::string>& stems) {
  MotionGroupInfo group;
  group.name = "";
  group.count = static_cast<int>(stems.size());
  for (const auto& stem : stems) group.files.push_back(stem + ".motion3.json");
  return group;
}

}  // namespace

class TestInteractionLogic : public QObject {
  Q_OBJECT

  const std::vector<MotionGroupInfo> groups_ = groupsOf({"Idle", "TapBody", "TapHead", "Shake"});

private slots:
  // 作者標了 hit area 時優先比對 Tap + 區域名稱
  void tapPlusAreaWins() {
    QCOMPARE(pickTapMotion({"Head"}, BodyPart::Unknown, groups_).value(), (TapMotionPick{"TapHead", -1}));
    QCOMPARE(pickTapMotion({"Body"}, BodyPart::Unknown, groups_).value(), (TapMotionPick{"TapBody", -1}));
  }

  // 支援底線寫法與 touch 前綴的群組命名
  void underscoreAndTouchNaming() {
    QCOMPARE(pickTapMotion({"body"}, BodyPart::Unknown, groupsOf({"idle", "tap_body"})).value(), (TapMotionPick{"tap_body", -1}));
    QCOMPARE(pickTapMotion({"head"}, BodyPart::Unknown, groupsOf({"idle", "touch_head"})).value(), (TapMotionPick{"touch_head", -1}));
  }

  // 群組名稱就是區域名稱時直接用
  void groupNameEqualsArea() { QCOMPARE(pickTapMotion({"Shake"}, BodyPart::Unknown, groups_).value(), (TapMotionPick{"Shake", -1})); }

  // 沒有精確對應時退回包含關係
  void fallsBackToSubstring() { QCOMPARE(pickTapMotion({"head"}, BodyPart::Unknown, groupsOf({"Idle", "HeadPat"})).value(), (TapMotionPick{"HeadPat", -1})); }

  // 作者標了 area、但只有包含關係對得上時，**觸摸動作優先於同樣含有那個字的
  // 待機動作** —— body_idle 是很常見的命名，不分先後的話點身體會播待機搖擺
  void substringPrefersTouchPool() { QCOMPARE(pickTapMotion({"body"}, BodyPart::Unknown, groupsOf({"body_idle", "touch_body_01"})).value(), (TapMotionPick{"touch_body_01", -1})); }

  // 99% 的模型沒有 hit area，部位由幾何推算而來：碧藍航線的群組命名
  void bodyPartPicksTouchGroup() {
    const auto motions = groupsOf({"idle", "login", "touch_body", "touch_head", "touch_special", "wedding"});
    QCOMPARE(pickTapMotion({}, BodyPart::Head, motions).value(), (TapMotionPick{"touch_head", -1}));
    QCOMPARE(pickTapMotion({}, BodyPart::Body, motions).value(), (TapMotionPick{"touch_body", -1}));
    // 胸口對到 touch_special —— 那個字沒有部位含意，是排在真正的部位字之後的補救
    QCOMPARE(pickTapMotion({}, BodyPart::Chest, motions).value(), (TapMotionPick{"touch_special", -1}));
  }

  // **群組名是空字串、動作名只存在於檔名**時也要挑得到（語料庫佔比最高的那種）
  void matchesByMotionFileName() {
    const std::vector<MotionGroupInfo> motions{nameless({"complete", "home", "idle", "login", "touch_body", "touch_head", "touch_special", "wedding"})};
    QCOMPARE(pickTapMotion({}, BodyPart::Head, motions).value(), (TapMotionPick{"", 5}));
    QCOMPARE(pickTapMotion({}, BodyPart::Body, motions).value(), (TapMotionPick{"", 4}));
    QCOMPARE(pickTapMotion({}, BodyPart::Chest, motions).value(), (TapMotionPick{"", 6}));
  }

  // 沒有命中任何區域、也推不出部位時仍要挑到觸摸動作
  void fallsBackToAnyTouchMotion() { QCOMPARE(pickTapMotion({}, BodyPart::Unknown, groups_).value(), (TapMotionPick{"TapBody", -1})); }

  // touch_idle（待機變體）與 touch_drag（拖曳反應）不是點擊該播的東西，
  // 有別的觸摸動作時一律排在後面
  void idleAndDragVariantsComeLast() {
    const auto motions = groupsOf({"touch_drag1", "touch_idle1", "touch_head"});
    QCOMPARE(pickTapMotion({}, BodyPart::Unknown, motions).value(), (TapMotionPick{"touch_head", -1}));
    // 真的只剩它們的話還是要播 —— 點了沒反應比播錯一段更糟
    QCOMPARE(pickTapMotion({}, BodyPart::Unknown, groupsOf({"idle", "touch_drag1", "touch_idle1"})).value(), (TapMotionPick{"touch_drag1", -1}));
  }

  // 部位對不上時退回任一觸摸動作（模型只做了一種觸摸）
  void unmatchedPartStillPlaysTouch() { QCOMPARE(pickTapMotion({}, BodyPart::Leg, groupsOf({"idle", "touch_head"})).value(), (TapMotionPick{"touch_head", -1})); }

  // 沒有任何觸摸動作時回 nullopt，交給呼叫端退回隨機動作
  void noTouchMotionsReturnsNull() {
    QVERIFY(!pickTapMotion({}, BodyPart::Head, groupsOf({"Idle", "main_1", "wedding"})).has_value());
    QVERIFY(!pickTapMotion({"Head"}, BodyPart::Head, {}).has_value());
  }

  // 內建合成動作（core/builtin_actions.h）沒有 files，model3.json 裡也不存在
  // 這個群組 —— 挑到了只會被 GetMotionCount 當場回絕，所以一律略過
  void skipsSynthesizedBuiltins() {
    std::vector<MotionGroupInfo> motions = groupsOf({"Idle"});
    motions.push_back(MotionGroupInfo{"tap", 1, {}});
    QVERIFY(!pickTapMotion({}, BodyPart::Head, motions).has_value());
  }
};

QTEST_APPLESS_MAIN(TestInteractionLogic)
#include "test_interaction_logic.moc"
