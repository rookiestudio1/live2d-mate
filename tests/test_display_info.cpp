// cdi3.json（DisplayInfo）的解析：參數群組角色、物理輸入輸出、表情群組虛擬參數
#include <QtTest>

#include <filesystem>
#include <map>

#include "core/display_info.h"
#include "core/json_doc.h"

using namespace l2m;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = fs::u8path(L2M_FIXTURES_DIR);

fs::path dirOf(const char* name) { return kFixtures / name; }

// 對應測試裡的 MODEL3 常數
jsonu::Doc model3Json() { return *jsonu::Doc::parse(R"({"FileReferences":{"DisplayInfo":"param.cdi3.json","Physics":"param.physics3.json"}})"); }

std::map<std::string, ParameterInfo> byId(const fs::path& dir, yyjson_val* json) {
  std::map<std::string, ParameterInfo> result;
  for (const auto& p : describeParameters(dir, json)) result[p.id] = p;
  return result;
}

bool contains(const std::vector<std::string>& names, const char* name) { return std::find(names.begin(), names.end(), name) != names.end(); }

}  // namespace

class TestDisplayInfo : public QObject {
  Q_OBJECT

private slots:
  // === describeParameters ===

  // 名稱取自 cdi3，作者寫的中文才是 AI 看得懂的線索
  void namesFromCdi3() {
    const auto doc = model3Json();
    const auto params = byId(dirOf("ParamOnly"), doc.root());
    QVERIFY(params.count("expression6"));
    QVERIFY(params.at("expression6") == (ParameterInfo{"expression6", "生氣", "表情", ParameterRole::Free}));
  }

  // physics3 的 Output 參數標成 physics-output —— 寫進去每幀都會被物理蓋掉
  void physicsOutputRole() {
    const auto doc = model3Json();
    const auto params = byId(dirOf("ParamOnly"), doc.root());
    QCOMPARE(params.at("Param26").role, ParameterRole::PhysicsOutput);
    QCOMPARE(params.at("Param6").role, ParameterRole::PhysicsOutput);
  }

  // physics3 的 Input 參數標成 physics-input —— 要在物理演算之前寫才有效
  void physicsInputRole() {
    const auto doc = model3Json();
    const auto params = byId(dirOf("ParamOnly"), doc.root());
    QCOMPARE(params.at("ParamAngleX").role, ParameterRole::PhysicsInput);
    QCOMPARE(params.at("expression11").role, ParameterRole::PhysicsInput);
  }

  // 沒被物理碰到的參數是 free
  void freeRole() {
    const auto doc = model3Json();
    const auto params = byId(dirOf("ParamOnly"), doc.root());
    QCOMPARE(params.at("Param73").role, ParameterRole::Free);
    QCOMPARE(params.at("ParamCheek").role, ParameterRole::Free);
  }

  // 作者沒分組時 group 是空字串
  void ungroupedIsEmptyString() {
    const auto doc = model3Json();
    const auto params = byId(dirOf("ParamOnly"), doc.root());
    QCOMPARE(params.at("ParamAngleX").group, std::string(""));
  }

  // cdi3 的 Name 跟 Id 一樣代表作者沒取名，name 照樣填 id
  void unnamedFallsBackToId() {
    const auto doc = model3Json();
    const auto params = byId(dirOf("ParamOnly"), doc.root());
    QCOMPARE(params.at("Param99").name, std::string("Param99"));
  }

  // 模型沒有 DisplayInfo 時回空陣列，不丟例外
  void noDisplayInfoReturnsEmpty() {
    const auto doc = *jsonu::Doc::parse(R"({"FileReferences":{}})");
    QVERIFY(describeParameters(dirOf("Hiyori"), doc.root()).empty());
  }

  // DisplayInfo 指到不存在的檔案時回空陣列，不丟例外
  void missingDisplayInfoFileReturnsEmpty() {
    const auto doc = *jsonu::Doc::parse(R"({"FileReferences":{"DisplayInfo":"ghost.cdi3.json"}})");
    QVERIFY(describeParameters(dirOf("ParamOnly"), doc.root()).empty());
  }

  // === readPhysicsIO ===

  // 把所有 PhysicsSettings 的來源與目的地攤平成兩個集合
  void flattensInputsAndOutputs() {
    const auto doc = model3Json();
    const auto io = readPhysicsIO(dirOf("ParamOnly"), doc.root());
    QCOMPARE(io.inputs, (std::set<std::string>{"ParamAngleX", "expression11"}));
    QCOMPARE(io.outputs, (std::set<std::string>{"Param26", "Param6"}));
  }

  // 沒有 Physics 檔時回兩個空集合
  void noPhysicsReturnsEmptySets() {
    const auto doc = *jsonu::Doc::parse(R"({"FileReferences":{}})");
    const auto io = readPhysicsIO(dirOf("Hiyori"), doc.root());
    QCOMPARE(io.inputs.size(), size_t(0));
    QCOMPARE(io.outputs.size(), size_t(0));
  }

  // === expressionsFromDisplayInfo ===

  // 作者放在「表情」群組裡的參數變成虛擬表情
  void expressionGroupParamsBecomeVirtual() {
    const auto doc = model3Json();
    const auto found = expressionsFromDisplayInfo(describeParameters(dirOf("ParamOnly"), doc.root()));
    std::vector<std::string> names;
    for (const auto& e : found) names.push_back(e.name);

    QVERIFY(contains(names, "生氣"));
    QVERIFY(contains(names, "哭哭"));
    QVERIFY(contains(names, "呲牙表情"));

    // 虛擬表情帶著要寫的參數與值，套用時才知道該動誰
    const auto it = std::find_if(found.begin(), found.end(), [](const ParamExpressionRef& e) { return e.name == "生氣"; });
    QVERIFY(it != found.end());
    QVERIFY(*it == (ParamExpressionRef{"生氣", "expression6", 1}));

    // physics-input 的表情參數照樣收 —— 它只是要早一點寫，不是不能寫
    QVERIFY(contains(names, "換衣服"));

    // 名稱含「物理」的一律排除，那是給物理引擎用的補正參數
    QVERIFY(!contains(names, "瀏海物理補正"));

    // 不在表情群組的參數不會被誤認成表情
    QVERIFY(!contains(names, "手臂揮動"));
    QVERIFY(!contains(names, "右腿"));
    QVERIFY(!contains(names, "臉頰泛紅"));

    // 作者沒取名的參數不會變成表情，Param99 這種名字對 AI 毫無意義
    QVERIFY(!contains(names, "Param99"));
  }

  // 沒有參數資料時回空陣列
  void emptyInputReturnsEmpty() { QVERIFY(expressionsFromDisplayInfo({}).empty()); }
};

QTEST_APPLESS_MAIN(TestDisplayInfo)
#include "test_display_info.moc"
