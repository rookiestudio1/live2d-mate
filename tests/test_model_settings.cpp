// model3.json 的設定補全（enrichCubism4Settings）：VTube Studio 模型、搬過位置的檔案、
// pose 修復、Layout 的處理，以及原始文件不被改動
#include <QtTest>

#include <filesystem>
#include <vector>

#include "core/json_doc.h"
#include "core/model_settings.h"

using namespace l2m;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = fs::u8path(L2M_FIXTURES_DIR);

fs::path dirOf(const char* name) { return kFixtures / name; }

struct ExpRef {
  std::string name;
  std::string file;
  bool operator==(const ExpRef& o) const { return name == o.name && file == o.file; }
};

// 把補全結果的 FileReferences 攤成好比對的結構
struct Refs {
  std::vector<ExpRef> expressions;
  bool hasExpressions = false;
  std::vector<std::pair<std::string, std::vector<std::string>>> motions;  // 群組 → File 清單
  bool hasMotions = false;
  std::string pose;
  bool hasPose = false;
};

Refs refsOf(const EnrichedSettings& enriched) {
  Refs result;
  const auto doc = *jsonu::Doc::parse(enriched.json());
  yyjson_val* refs = jsonu::get(doc.root(), "FileReferences");

  yyjson_val* expressions = jsonu::get(refs, "Expressions");
  if (yyjson_is_arr(expressions)) {
    result.hasExpressions = true;
    size_t idx, max;
    yyjson_val* e;
    yyjson_arr_foreach(expressions, idx, max, e) { result.expressions.push_back({jsonu::getString(e, "Name"), jsonu::getString(e, "File")}); }
  }

  yyjson_val* motions = jsonu::get(refs, "Motions");
  if (yyjson_is_obj(motions)) {
    result.hasMotions = true;
    size_t idx, max;
    yyjson_val* k;
    yyjson_val* v;
    yyjson_obj_foreach(motions, idx, max, k, v) {
      std::vector<std::string> files;
      if (yyjson_is_arr(v)) {
        size_t i, m;
        yyjson_val* item;
        yyjson_arr_foreach(v, i, m, item) files.push_back(jsonu::getString(item, "File"));
      }
      result.motions.push_back({yyjson_get_str(k), files});
    }
  }

  yyjson_val* pose = jsonu::get(refs, "Pose");
  if (pose && yyjson_is_str(pose)) {
    result.hasPose = true;
    result.pose = yyjson_get_str(pose);
  }
  return result;
}

// 依群組名取動作檔清單
const std::vector<std::string>* motionsOf(const Refs& refs, const std::string& group) {
  for (const auto& [name, files] : refs.motions) {
    if (name == group) return &files;
  }
  return nullptr;
}

}  // namespace

class TestModelSettings : public QObject {
  Q_OBJECT

private slots:
  // === VTube Studio 模型 ===

  void vtubeStudioModel() {
    const auto enriched = loadCubism4Settings(dirOf("VTubeStudio") / "vts.model3.json");
    const Refs refs = refsOf(enriched);

    // 表情名稱取自 hotkey，作者寫的中文比檔名好懂
    std::vector<std::string> names;
    for (const auto& e : refs.expressions) names.push_back(e.name);
    QCOMPARE(names, (std::vector<std::string>{"愛心眼", "臉紅"}));

    // 沒有 File 的 hotkey（例如「歸零」）不會變成表情
    QVERIFY(std::find(names.begin(), names.end(), "歸零") == names.end());

    // hotkey 指到不存在的檔案時直接略過，不留下按了會壞的項目
    QVERIFY(std::find(names.begin(), names.end(), "缺檔的") == names.end());

    // IdleAnimation 放進 Idle 群組，其餘動作各自成組
    QCOMPARE(refs.motions.size(), size_t(2));
    QVERIFY(motionsOf(refs, "Idle"));
    QCOMPARE(*motionsOf(refs, "Idle"), (std::vector<std::string>{"idle_loop.motion3.json"}));
    QVERIFY(motionsOf(refs, "extra_dance"));
    QCOMPARE(*motionsOf(refs, "extra_dance"), (std::vector<std::string>{"extra_dance.motion3.json"}));

    // 回報來源是 vtube
    QCOMPARE(enriched.report.source, EnrichReport::Source::VTube);
    QCOMPARE(enriched.report.addedExpressions, 2);
    QCOMPARE(enriched.report.addedMotions, 2);
  }

  // === 檔案搬進子資料夾但沒改 model3.json ===

  void movedFilesRepaired() {
    const auto enriched = loadCubism4Settings(dirOf("MovedFiles") / "moved.model3.json");
    const Refs refs = refsOf(enriched);

    // 斷掉的表情路徑會被接回去，名稱維持作者寫的
    QCOMPARE(refs.expressions, (std::vector<ExpRef>{
                                 {"微笑", "exp/smile.exp3.json"},
                                 {"生氣", "exp/angry.exp3.json"},
                                 {"根本不存在", "ghost.exp3.json"},
                               }));
    QCOMPARE(enriched.report.repairedPaths, 2);

    // 找不到同名檔就原樣留著，不亂猜
    QCOMPARE(refs.expressions[2].file, std::string("ghost.exp3.json"));

    // 子資料夾裡的動作也補得進來
    QCOMPARE(refs.motions.size(), size_t(1));
    QCOMPARE(*motionsOf(refs, "wave"), (std::vector<std::string>{"motions/wave.motion3.json"}));
  }

  // === 沒有東西可補的模型 ===

  // 不會無中生有
  void bareModelUntouched() {
    const auto enriched = loadCubism4Settings(dirOf("Bare") / "bare.model3.json");
    const Refs refs = refsOf(enriched);
    QVERIFY(!refs.hasExpressions);
    QVERIFY(!refs.hasMotions);
    QCOMPARE(enriched.report.source, EnrichReport::Source::None);
  }

  // 作者已經宣告動作時完全不介入
  void declaredMotionsRespected() {
    const auto original = *jsonu::Doc::parse(R"({"FileReferences":{"Motions":{"Idle":[{"File":"a.motion3.json"}]}}})");
    const auto enriched = enrichCubism4Settings(original.root(), dirOf("Hiyori"));
    const Refs refs = refsOf(enriched);
    QCOMPARE(refs.motions.size(), size_t(1));
    QCOMPARE(*motionsOf(refs, "Idle"), (std::vector<std::string>{"a.motion3.json"}));
    QCOMPARE(enriched.report.addedMotions, 0);
  }

  // 原輸入不會被就地改掉（深拷貝後才改寫）
  void originalNotMutated() {
    const auto original = *jsonu::Doc::parse(R"({"FileReferences":{}})");
    enrichCubism4Settings(original.root(), dirOf("VTubeStudio"));
    yyjson_val* refs = jsonu::get(original.root(), "FileReferences");
    QCOMPARE(yyjson_obj_size(refs), size_t(0));
  }

  // === Pose 補全 ===

  // 磁碟有 pose3.json 但沒被引用時補上，手臂互斥才會生效
  void orphanPoseAdded() {
    const auto original = *jsonu::Doc::parse(R"({"FileReferences":{}})");
    const auto enriched = enrichCubism4Settings(original.root(), dirOf("PoseOrphan"));
    QCOMPARE(refsOf(enriched).pose, std::string("char.pose3.json"));
    QCOMPARE(enriched.report.addedPose, true);
  }

  // 作者宣告的 Pose 路徑斷掉時接回去
  void brokenPoseRepaired() {
    const auto original = *jsonu::Doc::parse(R"({"FileReferences":{"Pose":"old/char.pose3.json"}})");
    const auto enriched = enrichCubism4Settings(original.root(), dirOf("PoseOrphan"));
    QCOMPARE(refsOf(enriched).pose, std::string("char.pose3.json"));
    QCOMPARE(enriched.report.repairedPaths, 1);
    QCOMPARE(enriched.report.addedPose, false);
  }

  // 宣告完好時完全不介入
  void intactPoseUntouched() {
    const auto original = *jsonu::Doc::parse(R"({"FileReferences":{"Pose":"char.pose3.json"}})");
    const auto enriched = enrichCubism4Settings(original.root(), dirOf("PoseOrphan"));
    QCOMPARE(refsOf(enriched).pose, std::string("char.pose3.json"));
    QCOMPARE(enriched.report.repairedPaths, 0);
    QCOMPARE(enriched.report.addedPose, false);
  }

  // 沒有 pose 檔就不會無中生有
  void noPoseFileNoInvention() {
    const auto original = *jsonu::Doc::parse(R"({"FileReferences":{}})");
    const auto enriched = enrichCubism4Settings(original.root(), dirOf("Bare"));
    QVERIFY(!refsOf(enriched).hasPose);
    QCOMPARE(enriched.report.addedPose, false);
  }

  // === Layout ===

  // Cubism 2 時代的小寫 "layout"（haru 就是）刻意不搬成大寫 "Layout"：
  // 那些數值是寫給 Cubism 2 的 2 單位座標系，餵給 CubismModelMatrix 會把
  // 構圖推到視野外（實測全空）。
  void lowercaseLayoutLeftAlone() {
    const auto original = *jsonu::Doc::parse(R"({"FileReferences":{},"layout":{"width":5,"center_x":0,"center_y":-0.8}})");
    const auto enriched = enrichCubism4Settings(original.root(), dirOf("Bare"));
    const auto doc = *jsonu::Doc::parse(enriched.json());
    QVERIFY(!jsonu::get(doc.root(), "Layout"));
    // 原欄位原樣保留（enrich 承諾不動作者的東西）
    QCOMPARE(yyjson_get_num(jsonu::get(jsonu::get(doc.root(), "layout"), "width")), 5.0);
  }

  // 正牌大寫 "Layout"（鍵名本來就是 Framework 認得的小寫蛇形）原樣通過
  void canonicalLayoutUntouched() {
    const auto original = *jsonu::Doc::parse(R"({"FileReferences":{},"Layout":{"width":2,"center_y":-0.5}})");
    const auto enriched = enrichCubism4Settings(original.root(), dirOf("Bare"));
    const auto doc = *jsonu::Doc::parse(enriched.json());
    QCOMPARE(yyjson_get_num(jsonu::get(jsonu::get(doc.root(), "Layout"), "width")), 2.0);
  }

  // === readVTubeConfig ===

  // 讀得到同資料夾的 *.vtube.json
  void readsVTubeConfig() {
    const auto vtube = readVTubeConfig(dirOf("VTubeStudio"));
    QVERIFY(vtube.has_value());
    QCOMPARE(jsonu::getString(vtube->root(), "Name"), std::string("VTubeStudio"));
  }

  // 沒有就回 nullopt，不丟例外
  void missingVTubeConfigIsNull() {
    QVERIFY(!readVTubeConfig(dirOf("Hiyori")).has_value());
    QVERIFY(!readVTubeConfig(dirOf("does-not-exist")).has_value());
  }
};

QTEST_APPLESS_MAIN(TestModelSettings)
#include "test_model_settings.moc"
