// 一支動作會驅動哪些參數／部件。restoreBaseline 靠這份清單決定「哪些值留給
// 新動作自己接手」—— 漏收就是淡入那一秒看得到模型的編輯狀態（多出來的手腳），
// 多收就是上一段動作的道具留在畫面上，兩種都沒有錯誤訊息
#include <QtTest>

#include "core/motion_curve_ids.h"

using namespace l2m;

class TestMotionCurveIds : public QObject {
  Q_OBJECT

private slots:
  // Parameter 與 PartOpacity 兩種 Target 分開收（碧藍航線的肢體切換兩種都用）
  void separatesParameterAndPartTargets() {
    const std::string json = R"({
      "Version": 3,
      "Meta": {"Duration": 1.0},
      "Curves": [
        {"Target": "Parameter", "Id": "ParamFootRWalk", "Segments": [0, 1, 0, 1, 1]},
        {"Target": "PartOpacity", "Id": "Part24", "Segments": [0, 0, 0, 1, 0]},
        {"Target": "Parameter", "Id": "ParamFootLWalk", "Segments": [0, 1, 0, 1, 1]}
      ]
    })";
    const MotionDrivenIds ids = motionDrivenIds(json);
    QCOMPARE(ids.parameterIds, (std::vector<std::string>{"ParamFootRWalk", "ParamFootLWalk"}));
    QCOMPARE(ids.partIds, (std::vector<std::string>{"Part24"}));
  }

  // Target 是 Model 的效果曲線（LipSync／EyeBlink）刻意不收：它驅動的是
  // SetEffectIds 另外指定的一組 id，這裡看不到
  void modelTargetIsIgnored() {
    const std::string json = R"({
      "Curves": [
        {"Target": "Model", "Id": "LipSync", "Segments": [0, 0, 0, 1, 1]},
        {"Target": "Parameter", "Id": "ParamAngleX", "Segments": [0, 0, 0, 1, 1]}
      ]
    })";
    const MotionDrivenIds ids = motionDrivenIds(json);
    QCOMPARE(ids.parameterIds, (std::vector<std::string>{"ParamAngleX"}));
    QVERIFY(ids.partIds.empty());
  }

  // 重複的 id 只留第一次，保留出現順序
  void duplicatesAreCollapsed() {
    const std::string json = R"({
      "Curves": [
        {"Target": "Parameter", "Id": "ParamA", "Segments": [0, 0]},
        {"Target": "Parameter", "Id": "ParamB", "Segments": [0, 0]},
        {"Target": "Parameter", "Id": "ParamA", "Segments": [0, 0]}
      ]
    })";
    QCOMPARE(motionDrivenIds(json).parameterIds, (std::vector<std::string>{"ParamA", "ParamB"}));
  }

  // 壞掉的輸入一律回空清單 ＝「什麼都不保留」＝ 修改前的舊行為，安全的那一邊
  void malformedInputYieldsEmptyList() {
    for (const std::string& json : {std::string("not json at all"), std::string("{}"), std::string(R"({"Curves": 3})"), std::string()}) {
      const MotionDrivenIds ids = motionDrivenIds(json);
      QVERIFY(ids.parameterIds.empty());
      QVERIFY(ids.partIds.empty());
    }
  }

  // 型別對不上的欄位也不能炸：Curves 的元素不是物件、Target 不是字串。
  // 這條是釘住「yyjson 的取值對錯型別回 NULL」這個前提，改用別的解析器時會擋下來。
  void wronglyTypedFieldsAreSkipped() {
    QVERIFY(motionDrivenIds(R"({"Curves": [1, 2, null]})").parameterIds.empty());
    const std::string mixed = R"({
      "Curves": [
        {"Target": 3, "Id": "ParamBad", "Segments": [0, 0]},
        {"Target": "Parameter", "Id": 7, "Segments": [0, 0]},
        {"Target": "Parameter", "Id": "ParamOk", "Segments": [0, 0]}
      ]
    })";
    QCOMPARE(motionDrivenIds(mixed).parameterIds, (std::vector<std::string>{"ParamOk"}));
  }

  // 缺 Target 或 Id 的曲線跳過，不要塞一個空字串進去
  void curvesWithoutTargetOrIdAreSkipped() {
    const std::string json = R"({
      "Curves": [
        {"Id": "ParamNoTarget", "Segments": [0, 0]},
        {"Target": "Parameter", "Segments": [0, 0]},
        {"Target": "Parameter", "Id": "", "Segments": [0, 0]},
        {"Target": "Parameter", "Id": "ParamOk", "Segments": [0, 0]}
      ]
    })";
    QCOMPARE(motionDrivenIds(json).parameterIds, (std::vector<std::string>{"ParamOk"}));
  }
};

QTEST_APPLESS_MAIN(TestMotionCurveIds)
#include "test_motion_curve_ids.moc"
