// 環境風該吹哪些 PhysicsSetting（core/wind_targets.h）。
//
// 這裡釘住的是「開了環境風，身體搖得比頭髮還大」那個 bug 的兩條判別規則。
// 最重要的是 bodyFollowerWithManyParticlesIsExcluded：只看粒子數的舊規則
// 在那一條上是綠燈的，而畫面上身體正在大幅上下擺動。
#include <QtTest>

#include "core/wind_targets.h"

using namespace l2m;

namespace {

// 頭髮鏈：5 節，輸出全是作者自訂的參數
constexpr const char* kHairJson = R"JSON({
  "PhysicsSettings": [
    {
      "Id": "PhysicsSetting1",
      "Input": [{"Source": {"Id": "ParamAngleX"}}],
      "Output": [{"Destination": {"Id": "Param17"}}, {"Destination": {"Id": "Param20"}}],
      "Vertices": [{}, {}, {}, {}, {}]
    }
  ]
})JSON";

// Gan Yu_LordHut_XJY_booth 的實際結構（縮短版）：
// 頭髮 7 節 → 自訂參數；身體跟隨 5 節 → ParamBodyAngleX／Y；ParamAngleZ 跟隨 2 節。
constexpr const char* kGanYuJson = R"JSON({
  "PhysicsSettings": [
    {
      "Id": "PhysicsSetting1",
      "Output": [{"Destination": {"Id": "Param17"}}],
      "Vertices": [{}, {}, {}, {}, {}, {}, {}]
    },
    {
      "Id": "PhysicsSetting2",
      "Output": [{"Destination": {"Id": "Param25"}}, {"Destination": {"Id": "ParamBodyAngleX"}}],
      "Vertices": [{}, {}, {}, {}, {}]
    },
    {
      "Id": "PhysicsSetting3",
      "Output": [{"Destination": {"Id": "Param29"}}, {"Destination": {"Id": "ParamBodyAngleY"}}],
      "Vertices": [{}, {}, {}, {}, {}]
    },
    {
      "Id": "PhysicsSetting4",
      "Output": [{"Destination": {"Id": "ParamBodyAngleZ"}}],
      "Vertices": [{}, {}]
    }
  ]
})JSON";

PhysicsSettingWind setting(int particles, std::vector<std::string> outputs) { return PhysicsSettingWind{particles, std::move(outputs)}; }

}  // namespace

class TestWindTargets : public QObject {
  Q_OBJECT

private slots:
  // 規則①：單節擺錘（2 粒子）不吃風，會飄的鏈（3 粒子以上）才吃
  void shortPendulumIsExcluded() {
    QVERIFY(!settingAcceptsWind(setting(1, {"Param01"})));
    QVERIFY(!settingAcceptsWind(setting(2, {"Param01"})));
    QVERIFY(settingAcceptsWind(setting(3, {"Param01"})));
    QVERIFY(settingAcceptsWind(setting(7, {"Param01", "Param02"})));
  }

  // 規則②：輸出寫進整體姿勢的標準參數就不吃風，粒子數再多也一樣。
  // 這一條就是 Gan Yu（5 粒子 → ParamBodyAngleY）「身體大幅上下擺動」的根因，
  // 只有規則①的話這裡會是綠燈。
  void bodyFollowerWithManyParticlesIsExcluded() {
    QVERIFY(!settingAcceptsWind(setting(5, {"Param29", "ParamBodyAngleY"})));
    QVERIFY(!settingAcceptsWind(setting(3, {"ParamBodyAngleX"})));
    for (const char* id : {"ParamAngleX", "ParamAngleY", "ParamAngleZ", "ParamBodyAngleX", "ParamBodyAngleY", "ParamBodyAngleZ"}) {
      QVERIFY2(!settingAcceptsWind(setting(6, {id})), id);
    }
  }

  // 標準 id 是精確比對：镜流 的 PhysicsSetting14 同時輸出 ParamBodyAngleZ 與
  // 自訂的 ParamBodyAngleZ2，前綴比對會把一堆 ParamAngleX2 之類的自訂參數也算進來
  void poseParamMatchIsExact() {
    QVERIFY(settingAcceptsWind(setting(4, {"ParamBodyAngleZ2"})));
    QVERIFY(settingAcceptsWind(setting(4, {"ParamAngleX2", "ParamAngleXX"})));
    QVERIFY(settingAcceptsWind(setting(4, {"parambodyanglex"})));
    QVERIFY(!settingAcceptsWind(setting(4, {"ParamBodyAngleZ2", "ParamBodyAngleZ"})));
  }

  // 沒有 Output 的 setting 只看粒子數
  void noOutputFallsBackToParticleCount() {
    QVERIFY(!settingAcceptsWind(setting(2, {})));
    QVERIFY(settingAcceptsWind(setting(3, {})));
  }

  // 遮罩的順序要跟 PhysicsSettings 陣列一致
  void maskFollowsSettingOrder() {
    const auto mask = windTargetMask(kGanYuJson);
    const std::vector<std::uint8_t> expected{1, 0, 0, 0};
    QCOMPARE(mask, expected);
  }

  // 只有頭髮的模型整份都吃風
  void hairOnlyModelIsAllWind() {
    const auto mask = windTargetMask(kHairJson);
    const std::vector<std::uint8_t> expected{1};
    QCOMPARE(mask, expected);
  }

  // 解析不出來一律回空 vector —— 呼叫端據此交 NULL 給 Framework，維持原行為
  void unusableJsonYieldsEmptyMask() {
    QVERIFY(windTargetMask("").empty());
    QVERIFY(windTargetMask("not json at all").empty());
    QVERIFY(windTargetMask("{}").empty());
    QVERIFY(windTargetMask(R"JSON({"PhysicsSettings": []})JSON").empty());
    QVERIFY(windTargetMask(R"JSON({"PhysicsSettings": 3})JSON").empty());
  }
};

QTEST_GUILESS_MAIN(TestWindTargets)
#include "test_wind_targets.moc"
