// 參數指令（set_parameters／animate）的純邏輯：「只讀 ModelInfo」的規則
// 抽在 core/model_commands，這裡直接測那一層 —— 解析、互斥、擋錯、hint、報告合併。
#include <QtTest>

#include <algorithm>
#include <filesystem>

#include "core/json_doc.h"
#include "core/model_commands.h"
#include "core/motion_builder.h"
#include "core/model_scanner.h"

using namespace l2m;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> zeroedIds(const std::vector<SetParameterRequest>& params) {
  std::vector<std::string> ids;
  for (const auto& p : params) {
    if (p.value == 0) ids.push_back(p.id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

}  // namespace

class TestControllerParameters : public QObject {
  Q_OBJECT

private:
  std::vector<ModelInfo> models_;
  const ModelInfo* model_ = nullptr;

private slots:
  void initTestCase() {
    models_ = scanModels(fs::u8path(L2M_FIXTURES_DIR));
    model_ = resolveModel(models_, "ParamOnly");
    QVERIFY(model_ != nullptr);
  }

  // === 虛擬表情 ===

  // 套用時改寫參數，而不是去找根本不存在的 exp3 檔
  void virtualExpressionWritesParameters() {
    std::string match;
    QVERIFY(resolveExpression(*model_, "生氣", &match).ok);

    const auto params = virtualExpressionParams(*model_, match);
    const auto it = std::find_if(params.begin(), params.end(), [](const SetParameterRequest& p) { return p.id == "expression6"; });
    QVERIFY(it != params.end());
    QCOMPARE(it->value, 1.0);
  }

  // 同一批表情彼此互斥，套新的會把其他的歸零
  void virtualExpressionsAreMutuallyExclusive() {
    const auto params = virtualExpressionParams(*model_, std::string("生氣"));
    QCOMPARE(zeroedIds(params), (std::vector<std::string>{"Param80", "expression1", "expression11"}));
  }

  // 不帶名字代表全部歸零
  void clearingExpressionZeroesAll() {
    const auto params = virtualExpressionParams(*model_, std::nullopt);
    QCOMPARE(params.size(), size_t(4));
    for (const auto& p : params) QCOMPARE(p.value, 0.0);
  }

  // 照樣認得使用者寫的意義
  void resolvesByUserMeaning() {
    ModelInfo named = *model_;
    named.annotations.expressions["生氣"] = "angry face";

    std::string match;
    QVERIFY(resolveExpression(named, "angry face", &match).ok);
    QCOMPARE(match, std::string("生氣"));
  }

  // 名稱對不上時附上可用清單，AI 一次就能改對
  void unknownExpressionCarriesHint() {
    std::string match;
    const CommandResult result = resolveExpression(*model_, "不存在的表情", &match);
    QVERIFY(!result.ok);
    QVERIFY(result.hint.find("生氣") != std::string::npos);
  }

  // === setParameters 的驗證 ===

  // 合法的要求原封不動放行
  void validRequestPasses() {
    SetParameterRequest req;
    req.id = "Param73";
    req.value = 1;
    req.durationMs = 250;
    QVERIFY(validateSetParameters(*model_, {req}).ok);
  }

  // 模型裡沒有的參數當場擋下來，附上最接近的候選
  void unknownParameterIsRejectedWithHint() {
    SetParameterRequest req;
    req.id = "Param7300";
    req.value = 1;
    const CommandResult result = validateSetParameters(*model_, {req});
    QVERIFY(!result.ok);
    QVERIFY(result.hint.find("Param73") != std::string::npos);
  }

  // 寫進物理輸出參數時擋下來並說明原因，不然 AI 會一直重試
  void physicsOutputIsRejected() {
    SetParameterRequest req;
    req.id = "Param26";
    req.value = 1;
    const CommandResult result = validateSetParameters(*model_, {req});
    QVERIFY(!result.ok);
    QVERIFY(result.error.find("physics") != std::string::npos);
  }

  // 一次可以設多個
  void multipleParametersPass() {
    SetParameterRequest a;
    a.id = "Param73";
    a.value = 1;
    SetParameterRequest b;
    b.id = "ParamCheek";
    b.value = 0.8;
    QVERIFY(validateSetParameters(*model_, {a, b}).ok);
  }

  // 空清單擋掉
  void emptyRequestIsRejected() {
    const CommandResult result = validateSetParameters(*model_, {});
    QVERIFY(!result.ok);
    QCOMPARE(result.error, std::string("No parameters given"));
  }

  // === animate ===

  // 把關鍵影格編成 motion3.json 再送去播
  void animateCompilesKeyframes() {
    const std::vector<Keyframe> frames{{0, {{"Param73", 0}}}, {250, {{"Param73", 1}}}, {500, {{"Param73", 0}}}};
    QVERIFY(validateAnimateParams(*model_, uniqueKeyframeParams(frameMaps(frames))).ok);

    const Motion3 motion = buildMotion3(frames, {});
    QCOMPARE(motion.meta.duration, 0.5);
  }

  // 關鍵影格不合法時回可讀的錯誤，不會把爛資料丟下去
  void animateRejectsSingleKeyframe() {
    bool threw = false;
    try {
      buildMotion3({{0, {{"Param73", 1}}}}, {});
    } catch (const std::exception& err) {
      threw = true;
      const std::string message = err.what();
      QVERIFY(message.find("at least two keyframes") != std::string::npos);
    }
    QVERIFY(threw);
  }

  // 用到模型沒有的參數時擋下來並附上提示
  void animateRejectsUnknownParameter() {
    const std::vector<Keyframe> frames{{0, {{"Nope", 0}}}, {500, {{"Nope", 1}}}};
    const CommandResult result = validateAnimateParams(*model_, uniqueKeyframeParams(frameMaps(frames)));
    QVERIFY(!result.ok);
    QVERIFY(!result.hint.empty());
  }

  // === listParameters ===

  // 參數帶著名稱、群組與物理角色
  void parametersCarryNameGroupRole() {
    const auto find = [this](const std::string& id) -> const ParameterInfo* {
      for (const auto& p : model_->parameters) {
        if (p.id == id) return &p;
      }
      return nullptr;
    };

    const ParameterInfo* angry = find("expression6");
    QVERIFY(angry != nullptr);
    QCOMPARE(angry->name, std::string("生氣"));
    QCOMPARE(angry->group, std::string("表情"));
    QCOMPARE(std::string(roleName(angry->role)), std::string("free"));

    const ParameterInfo* arm = find("Param26");
    QVERIFY(arm != nullptr);
    QCOMPARE(std::string(roleName(arm->role)), std::string("physics-output"));
  }

  // === parameterReport ===

  // cdi3 的名稱角色與執行期的現值上下限合成一份，AI 一次就看得完
  void reportMergesStaticAndLive() {
    const std::vector<ParameterSnapshot> snapshots{{"expression6", 0.5, 0, 1, 0}, {"Param26", 3, -10, 10, 0}};
    const std::string json = buildParameterReportJson(*model_, true, snapshots, {"expression6"});

    auto doc = jsonu::Doc::parse(json);
    QVERIFY(doc.has_value());
    yyjson_val* entry = findParameter(doc->root(), "expression6");
    QVERIFY(entry != nullptr);
    QCOMPARE(jsonu::getString(entry, "name"), std::string("生氣"));
    QCOMPARE(jsonu::getString(entry, "group"), std::string("表情"));
    QCOMPARE(jsonu::getString(entry, "role"), std::string("free"));
    QCOMPARE(yyjson_get_num(jsonu::get(entry, "value")), 0.5);
    QCOMPARE(yyjson_get_num(jsonu::get(entry, "max")), 1.0);
  }

  // 帶上目前被接管的參數，用來診斷「寫了卻沒反應」
  void reportListsOverridden() {
    const std::string json = buildParameterReportJson(*model_, true, {}, {"expression6"});
    auto doc = jsonu::Doc::parse(json);
    QVERIFY(doc.has_value());
    yyjson_val* overridden = jsonu::get(doc->root(), "overridden");
    QVERIFY(overridden && yyjson_is_arr(overridden));
    QCOMPARE(yyjson_arr_size(overridden), size_t(1));
    QCOMPARE(jsonu::asString(yyjson_arr_get_first(overridden)), std::string("expression6"));
  }

  // 執行期還沒回報的參數保留靜態資訊，值的欄位留空而不是亂填 0
  void reportOmitsMissingValues() {
    const std::vector<ParameterSnapshot> snapshots{{"expression6", 0.5, 0, 1, 0}};
    const std::string json = buildParameterReportJson(*model_, true, snapshots, {});

    auto doc = jsonu::Doc::parse(json);
    QVERIFY(doc.has_value());
    yyjson_val* angle = findParameter(doc->root(), "ParamAngleX");
    QVERIFY(angle != nullptr);
    QCOMPARE(jsonu::getString(angle, "name"), std::string("角度 X"));
    QCOMPARE(jsonu::getString(angle, "role"), std::string("physics-input"));
    QVERIFY(jsonu::get(angle, "value") == nullptr);
  }

private:
  static std::vector<std::map<std::string, double>> frameMaps(const std::vector<Keyframe>& frames) {
    std::vector<std::map<std::string, double>> out;
    for (const auto& frame : frames) {
      std::map<std::string, double> one;
      for (const auto& entry : frame.params) one[entry.first] = entry.second;
      out.push_back(std::move(one));
    }
    return out;
  }

  static yyjson_val* findParameter(yyjson_val* root, const std::string& id) {
    yyjson_val* arr = jsonu::get(root, "parameters");
    if (!arr) return nullptr;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(arr, &iter);
    yyjson_val* item = nullptr;
    while ((item = yyjson_arr_iter_next(&iter))) {
      if (jsonu::getString(item, "id") == id) return item;
    }
    return nullptr;
  }
};

QTEST_APPLESS_MAIN(TestControllerParameters)
#include "test_controller_parameters.moc"
