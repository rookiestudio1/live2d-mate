// 以命名（annotations）指定動作與表情：解析規則在 core/model_commands，
// 這裡直接驗解析出來的群組與索引；命名檔的讀寫則走 core/annotations 與
// core/model_scanner。
#include <QtTest>

#include <filesystem>

#include "core/annotations.h"
#include "core/json_doc.h"
#include "core/model_commands.h"
#include "core/model_scanner.h"

using namespace l2m;
namespace fs = std::filesystem;

class TestControllerNaming : public QObject {
  Q_OBJECT

private:
  QTemporaryDir dir_;
  fs::path modelsDir_;
  std::vector<ModelInfo> models_;

  // 每個測試都從乾淨的 fixture 副本開始（命名會寫進模型資料夾）
  void reload() { models_ = scanModels(modelsDir_); }

  const ModelInfo* model(const std::string& name) { return resolveModel(models_, name); }

  // 解析成功時回傳群組與索引，失敗時把 CommandResult 帶回去
  ResolvedMotion resolve(const ModelInfo& m, const std::string& group, std::optional<int> index = std::nullopt, CommandResult* outResult = nullptr) {
    ResolvedMotion resolved;
    const CommandResult result = resolveMotion(m, group, index, &resolved);
    if (outResult) *outResult = result;
    return resolved;
  }

  void writeMeaning(const std::string& modelName, AnnotationKind kind, const std::string& key, const std::string& meaning) {
    const ModelInfo* m = model(modelName);
    QVERIFY(m != nullptr);
    const fs::path entry = modelsDir_ / fs::u8path(m->id);
    writeAnnotations(entry, applyMeaning(readAnnotations(entry), kind, key, meaning));
    reload();
  }

private slots:
  void init() {
    QVERIFY(dir_.isValid());
    modelsDir_ = fs::u8path(dir_.path().toStdString()) / "models";
    std::error_code ec;
    fs::remove_all(modelsDir_, ec);
    fs::copy(fs::u8path(L2M_FIXTURES_DIR), modelsDir_, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    QVERIFY2(!ec, ec.message().c_str());
    reload();
  }

  // === 依原始名稱呼叫 ===

  // 群組名稱大小寫不敏感
  void groupNameIsCaseInsensitive() {
    CommandResult result;
    const auto resolved = resolve(*model("Hiyori"), "idle", std::nullopt, &result);
    QVERIFY(result.ok);
    QCOMPARE(resolved.group, std::string("Idle"));
  }

  // 索引超出範圍會擋下來並說明範圍
  void indexOutOfRangeExplainsRange() {
    CommandResult result;
    resolve(*model("Hiyori"), "Idle", 5, &result);
    QVERIFY(!result.ok);
    QVERIFY(result.hint.find("0 ~ 1") != std::string::npos);
  }

  // === 依使用者命名的意義呼叫動作 ===

  // 沒有命名時，錯誤訊息只列出原始群組
  void hintWithoutNamesListsGroupsOnly() {
    CommandResult result;
    resolve(*model("Hiyori"), "開心地揮手", std::nullopt, &result);
    QVERIFY(!result.ok);
    QVERIFY(result.hint.find("Available motion groups") != std::string::npos);
    QVERIFY(result.hint.find("Named motions") == std::string::npos);
  }

  // 命名到某個索引時，會連索引一起帶入
  void meaningOnIndexCarriesIndex() {
    writeMeaning("Hiyori", AnnotationKind::Motions, "Idle#1", "開心地揮手");

    CommandResult result;
    const auto resolved = resolve(*model("Hiyori"), "開心地揮手", std::nullopt, &result);
    QVERIFY(result.ok);
    QCOMPARE(resolved.group, std::string("Idle"));
    QCOMPARE(resolved.index.value_or(-1), 1);
  }

  // 命名到整個群組時不指定索引，維持隨機播放
  void meaningOnGroupKeepsRandomIndex() {
    writeMeaning("Hiyori", AnnotationKind::Motions, "TapBody", "被戳身體");

    CommandResult result;
    const auto resolved = resolve(*model("Hiyori"), "被戳身體", std::nullopt, &result);
    QVERIFY(result.ok);
    QCOMPARE(resolved.group, std::string("TapBody"));
    QVERIFY(!resolved.index.has_value());
  }

  // 只講部分關鍵字也能命中
  void partialMeaningMatches() {
    writeMeaning("Hiyori", AnnotationKind::Motions, "Idle#0", "安靜地站著發呆");

    CommandResult result;
    const auto resolved = resolve(*model("Hiyori"), "發呆", std::nullopt, &result);
    QVERIFY(result.ok);
    QCOMPARE(resolved.group, std::string("Idle"));
    QCOMPARE(resolved.index.value_or(-1), 0);
  }

  // 呼叫端明確指定 index 時，不被命名的索引蓋掉
  void explicitIndexWinsOverNamedIndex() {
    writeMeaning("Hiyori", AnnotationKind::Motions, "Idle#1", "開心地揮手");

    CommandResult result;
    const auto resolved = resolve(*model("Hiyori"), "開心地揮手", 0, &result);
    QVERIFY(result.ok);
    QCOMPARE(resolved.group, std::string("Idle"));
    QCOMPARE(resolved.index.value_or(-1), 0);
  }

  // 原始名稱優先於意義，不會被命名綁架
  void rawNameWinsOverMeaning() {
    writeMeaning("Hiyori", AnnotationKind::Motions, "TapBody", "Idle");

    CommandResult result;
    const auto resolved = resolve(*model("Hiyori"), "Idle", std::nullopt, &result);
    QVERIFY(result.ok);
    QCOMPARE(resolved.group, std::string("Idle"));
  }

  // 找不到時的提示會附上已命名的項目，AI 才知道能怎麼講
  void hintIncludesNamedEntries() {
    writeMeaning("Hiyori", AnnotationKind::Motions, "Idle#1", "開心地揮手");

    CommandResult result;
    resolve(*model("Hiyori"), "倒立", std::nullopt, &result);
    QVERIFY(!result.ok);
    QVERIFY(result.hint.find("Idle#1=\"開心地揮手\"") != std::string::npos);
  }

  // === 依使用者命名的意義呼叫表情 ===

  // 用意義套用表情
  void expressionByMeaning() {
    writeMeaning("Hiyori", AnnotationKind::Expressions, "Smile", "開心的笑臉");

    std::string match;
    QVERIFY(resolveExpression(*model("Hiyori"), "開心的笑臉", &match).ok);
    QCOMPARE(match, std::string("Smile"));
  }

  // 找不到時提示會列出名稱與已命名的意義
  void expressionHintListsBoth() {
    writeMeaning("Hiyori", AnnotationKind::Expressions, "Angry", "生氣鼓臉頰");

    std::string match;
    const CommandResult result = resolveExpression(*model("Hiyori"), "哭哭", &match);
    QVERIFY(!result.ok);
    QVERIFY(result.hint.find("Smile") != std::string::npos);
    QVERIFY(result.hint.find("Angry=\"生氣鼓臉頰\"") != std::string::npos);
  }

  // 命名指到模型沒有的表情時視為找不到
  void meaningPointingOutsideModelFails() {
    writeMeaning("Hiyori", AnnotationKind::Expressions, "NotInModel", "幽靈表情");

    std::string match;
    QVERIFY(!resolveExpression(*model("Hiyori"), "幽靈表情", &match).ok);
  }

  // === 命名存放位置 ===

  // 命名寫進模型自己的資料夾，而不是集中在一處
  void meaningLandsInModelFolder() {
    writeMeaning("Hiyori", AnnotationKind::Motions, "Idle#1", "開心地揮手");

    const fs::path path = annotationsPathFor(modelsDir_ / "Hiyori" / "Hiyori.model3.json");
    QVERIFY(fs::exists(path));

    auto doc = jsonu::Doc::parseFile(path);
    QVERIFY(doc.has_value());
    yyjson_val* motions = jsonu::get(doc->root(), "motions");
    QCOMPARE(jsonu::getString(motions, "Idle#1"), std::string("開心地揮手"));
  }

  // 重新掃描後命名還在（掃描模型時一起讀進來）
  void meaningSurvivesRescan() {
    writeMeaning("Hiyori", AnnotationKind::Expressions, "Smile", "開心的笑臉");
    reload();
    QCOMPARE(model("Hiyori")->annotations.expressions.at("Smile"), std::string("開心的笑臉"));
  }

  // 讀得到模型資料夾裡本來就附的命名檔
  // （本專案不支援 Cubism 2.1，所以用 Annotated fixture 而不是 Legacy）
  void readsShippedAnnotations() {
    const ModelInfo* annotated = model("Annotated");
    QVERIFY(annotated != nullptr);
    QCOMPARE(annotated->annotations.motions.at("idle"), std::string("站著發呆"));
    QCOMPARE(annotated->annotations.motions.at("tap_body#1"), std::string("被戳肚子會笑"));
    QCOMPARE(annotated->annotations.expressions.at("f01"), std::string("微笑"));
  }

  // 模型附的命名可以直接拿來呼叫
  void shippedAnnotationsAreCallable() {
    const ModelInfo* annotated = model("Annotated");
    QVERIFY(annotated != nullptr);

    CommandResult result;
    const auto resolved = resolve(*annotated, "被戳肚子會笑", std::nullopt, &result);
    QVERIFY(result.ok);
    QCOMPARE(resolved.group, std::string("tap_body"));
    QCOMPARE(resolved.index.value_or(-1), 1);
  }

  // 每個模型看到的是自己的命名，不會沿用上一個
  void annotationsDoNotLeakBetweenModels() {
    writeMeaning("Hiyori", AnnotationKind::Motions, "Idle", "這是 Hiyori 的");

    QCOMPARE(model("Annotated")->annotations.motions.count("Idle"), size_t(0));
    QCOMPARE(model("Hiyori")->annotations.motions.at("Idle"), std::string("這是 Hiyori 的"));
  }

  // === 命名視窗的預覽 ===

  // previewMotionKey 會解析索引
  void previewKeyParsesIndex() {
    const MotionKeyParts parts = parseMotionKey("Idle#1");
    QCOMPARE(parts.group, std::string("Idle"));
    QCOMPARE(parts.index, 1);

    CommandResult result;
    const auto resolved = resolve(*model("Hiyori"), parts.group, parts.index < 0 ? std::nullopt : std::optional<int>(parts.index), &result);
    QVERIFY(result.ok);
    QCOMPARE(resolved.index.value_or(-1), 1);
  }

  // 只給群組名時播整個群組
  void previewKeyWithoutIndexPlaysGroup() {
    const MotionKeyParts parts = parseMotionKey("TapBody");
    QCOMPARE(parts.group, std::string("TapBody"));
    QCOMPARE(parts.index, -1);
  }
};

QTEST_APPLESS_MAIN(TestControllerNaming)
#include "test_controller_naming.moc"
