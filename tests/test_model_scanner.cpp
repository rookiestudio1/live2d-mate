// 模型掃描（core/model_scanner.h）：資料夾與 zip 模型的列舉、動作／表情／參數的整理。
//
// 本專案不支援 Cubism 2.1，所以有一組驗的是「Cubism 2 模型被略過」
// （Legacy / MixedCase2 / GenericV2 / GenericIndex）；命名檔用 Annotated fixture 驗。
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "core/model_scanner.h"
#include "core/string_util.h"
#include "zip_builder.h"

using namespace l2m;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = fs::u8path(L2M_FIXTURES_DIR);

const char* kModel3 = R"({"Version":3,"FileReferences":{"Moc":"m.moc3"}})";

// 「符玄」的 Big5(CP950) 位元組。Windows 檔案總管壓出來的中文檔名就是長這樣：
// 不是合法的 UTF-8，而且 zip 沒設 UTF-8 旗標。寫成數值而不是字串跳脫，
// 是因為這個檔本身是 UTF-8，跳脫序列很容易在編輯過程被重新編碼成
// 「看起來一樣、其實是合法 UTF-8」的東西，測試就悄悄失去意義了。
const std::string kBig5Name{static_cast<char>(0xB2), static_cast<char>(0xC5), static_cast<char>(0xA5), static_cast<char>(0xC8)};

// 暫存目錄裡擺一個檔案（findDirectoryEntry 的多入口情境要現場組資料夾）
void writeFile(const fs::path& path, const std::string& content) {
  std::ofstream file(path, std::ios::binary);
  file << content;
}

const ModelInfo* byName(const std::vector<ModelInfo>& models, const std::string& name) {
  for (const auto& m : models) {
    if (m.name == name) return &m;
  }
  return nullptr;
}

}  // namespace

class TestModelScanner : public QObject {
  Q_OBJECT

  std::vector<ModelInfo> models_ = scanModels(kFixtures);

private slots:
  // 掃到 Cubism 4 模型並解析動作與表情
  void scansCubism4() {
    const auto* hiyori = byName(models_, "Hiyori");
    QVERIFY(hiyori);
    QCOMPARE(hiyori->motions, (std::vector<MotionGroupInfo>{
                                {"Idle", 2, {"idle_01.motion3.json", "idle_02.motion3.json"}},
                                {"TapBody", 1, {"tap_01.motion3.json"}},
                              }));
    QCOMPARE(hiyori->expressions, (std::vector<std::string>{"Smile", "Angry"}));
  }

  // Cubism 2 模型一律略過（本專案不支援 2.1）
  void cubism2Skipped() {
    QVERIFY(!byName(models_, "Legacy"));
    QVERIFY(!byName(models_, "MixedCase2"));
    QVERIFY(!byName(models_, "GenericV2"));
    QVERIFY(!byName(models_, "GenericIndex"));
  }

  // 同一個資料夾同時有兩種入口時只留 Cubism 4
  void dualEntryKeepsCubism4() {
    int count = 0;
    for (const auto& m : models_) {
      if (m.name == "Both") count++;
    }
    QCOMPARE(count, 1);
    QCOMPARE(byName(models_, "Both")->id, std::string("Both/dual.model3.json"));
  }

  // 略過壞掉的模型而不是整個掃描失敗
  void brokenModelSkipped() {
    QVERIFY(!byName(models_, "Broken"));
    QVERIFY(!models_.empty());
  }

  // 最多往下找 3 層
  void depthLimitedToThree() {
    QVERIFY(byName(models_, "c"));   // models/a/b/c/Deep.model3.json
    QVERIFY(!byName(models_, "d"));  // models/a/b/c/d/TooDeep.model3.json
  }

  // 入口檔副檔名不分大小寫，Foo.Model3.json 也掃得到
  void mixedCaseEntryFound() { QVERIFY(byName(models_, "MixedCase4")); }

  // 入口檔叫 model.json 這種裸命名也掃得到，版本由內容判斷（Cubism 4 才收）
  void genericEntrySniffed() {
    const auto* v4 = byName(models_, "GenericV4");
    QVERIFY(v4);
    QCOMPARE(v4->motions, (std::vector<MotionGroupInfo>{{"Idle", 1, {"idle.motion3.json"}}}));
  }

  // 叫 model.json 但內容不是模型設定時略過
  void junkGenericSkipped() { QVERIFY(!byName(models_, "GenericJunk")); }

  // 產生可直接載入的 live2d:// URL 且路徑經過編碼
  void urlIsLoadable() { QCOMPARE(byName(models_, "Hiyori")->url, std::string("live2d://models/Hiyori/Hiyori.model3.json")); }

  // 依名稱排序，方便選單直接列出來
  void sortedByName() {
    for (size_t i = 1; i < models_.size(); ++i) {
      QVERIFY(models_[i - 1].name <= models_[i].name ||
              // localeLess 是大小寫不敏感優先，只驗大小寫摺疊後的順序
              QString::compare(QString::fromStdString(models_[i - 1].name), QString::fromStdString(models_[i].name), Qt::CaseInsensitive) <= 0);
    }
  }

  // 動作檔名只留檔名，且索引與動作一一對應
  void motionFilesMatchIndices() {
    const auto* hiyori = byName(models_, "Hiyori");
    const auto& idle = hiyori->motions[0];
    QCOMPARE(idle.files.size(), size_t(idle.count));
    QCOMPARE(idle.files[1], std::string("idle_02.motion3.json"));
  }

  // 順便讀進模型資料夾裡的命名檔（用 Annotated fixture）
  void annotationsLoadedAlongside() {
    const auto* annotated = byName(models_, "Annotated");
    QVERIFY(annotated);
    QCOMPARE(annotated->annotations.motions.at("idle"), std::string("站著發呆"));
    QCOMPARE(annotated->annotations.motions.at("tap_body#1"), std::string("被戳肚子會笑"));
    QCOMPARE(annotated->annotations.expressions.at("f01"), std::string("微笑"));
  }

  // 沒有命名檔的模型拿到空命名
  void missingAnnotationsAreEmpty() { QVERIFY(byName(models_, "Hiyori")->annotations == ModelAnnotations{}); }

  // VTube Studio 模型的動作與表情靠 vtube.json 補回來。
  // 這支只看模型自己的項目，內建合成的另外由 builtinActionsAppended 驗。
  void vtubeStudioEnriched() {
    const auto* vts = byName(models_, "VTubeStudio");
    QVERIFY(vts);
    std::vector<std::string> groups;
    for (const auto& g : vts->motions) {
      if (std::find(vts->builtinMotions.begin(), vts->builtinMotions.end(), g.name) != vts->builtinMotions.end()) continue;
      groups.push_back(g.name);
    }
    QCOMPARE(groups, (std::vector<std::string>{"extra_dance", "Idle"}));
    QCOMPARE(vts->expressions, (std::vector<std::string>{"愛心眼", "臉紅"}));
  }

  // 內建動作接在模型自己的動作後面（排前面會讓 resolveMotion 先命中合成的那些），
  // 並且登記進 builtinMotions。VTubeStudio 的 cdi3 只有 ParamAngleX，
  // 所以只拿得到吃這一個參數的 shake 與 look_away。
  // 附加順序只看得出在 builtinMotions 上 —— model.motions 在掃描最後會依名稱排序，
  // 用 back() 驗「接在後面」是驗不到的（原本會過只是因為 shake 剛好排在最後）。
  void builtinActionsAppended() {
    const auto* vts = byName(models_, "VTubeStudio");
    QVERIFY(vts);
    QCOMPARE(vts->builtinMotions, (std::vector<std::string>{"shake", "look_away"}));
    for (const char* name : {"shake", "look_away"}) {
      const auto it = std::find_if(vts->motions.begin(), vts->motions.end(), [name](const MotionGroupInfo& g) { return g.name == name; });
      QVERIFY2(it != vts->motions.end(), name);
      QCOMPARE(it->count, 1);
    }
    // 表情要 MouthForm／EyeLOpen／MouthOpenY 才撐得起來，這隻一個都沒有
    QVERIFY(vts->builtinExpressions.empty());
  }

  // 沒有 cdi3.json 的模型讀不到參數表，一個內建項目都不會有
  void modelWithoutDisplayInfoGetsNoBuiltins() {
    const auto* hiyori = byName(models_, "Hiyori");
    QVERIFY(hiyori);
    QVERIFY(hiyori->parameters.empty());
    QVERIFY(hiyori->builtinMotions.empty());
    QVERIFY(hiyori->builtinExpressions.empty());
  }

  // 搬進子資料夾的動作與斷掉的表情路徑都救得回來
  void movedFilesRecovered() {
    const auto* moved = byName(models_, "MovedFiles");
    QVERIFY(moved);
    QCOMPARE(moved->motions, (std::vector<MotionGroupInfo>{{"wave", 1, {"wave.motion3.json"}}}));
    QCOMPARE(moved->expressions, (std::vector<std::string>{"微笑", "生氣", "根本不存在"}));
  }

  // 目錄不存在時回傳空陣列而不是丟例外
  void missingDirReturnsEmpty() { QVERIFY(scanModels(kFixtures / "does-not-exist").empty()); }

  // === 只有參數、沒有表情檔的 VTuber 模型 ===

  // 參數清單帶著 cdi3 的名稱與物理角色
  void paramOnlyParameterRoles() {
    const auto* paramOnly = byName(models_, "ParamOnly");
    QVERIFY(paramOnly);
    bool found = false;
    for (const auto& p : paramOnly->parameters) {
      if (p == ParameterInfo{"expression6", "生氣", "表情", ParameterRole::Free}) found = true;
    }
    QVERIFY(found);
    for (const auto& p : paramOnly->parameters) {
      if (p.id == "Param26") QCOMPARE(p.role, ParameterRole::PhysicsOutput);
      if (p.id == "ParamAngleX") QCOMPARE(p.role, ParameterRole::PhysicsInput);
    }
  }

  // 作者放在表情群組的開關參數升級成虛擬表情，且併進 expressions
  void paramOnlyVirtualExpressions() {
    const auto* paramOnly = byName(models_, "ParamOnly");
    std::vector<std::string> names;
    for (const auto& e : paramOnly->paramExpressions) names.push_back(e.name);
    QCOMPARE(names, (std::vector<std::string>{"哭哭", "生氣", "換衣服", "呲牙表情"}));
    QCOMPARE(paramOnly->expressions, (std::vector<std::string>{"哭哭", "生氣", "換衣服", "呲牙表情"}));
  }

  // === 已經有真表情檔的模型 ===

  // 不會被虛擬表情汙染 —— 作者做了 exp3 就以 exp3 為準；參數層照樣讀得到
  void realExpressionsNotPolluted() {
    const auto* vts = byName(models_, "VTubeStudio");
    QVERIFY(vts->paramExpressions.empty());
    QCOMPARE(vts->expressions, (std::vector<std::string>{"愛心眼", "臉紅"}));

    std::vector<std::string> ids;
    for (const auto& p : vts->parameters) ids.push_back(p.id);
    QCOMPARE(ids, (std::vector<std::string>{"expression1", "ParamAngleX"}));
  }

  // === zip 模型 ===

  // zip 當成一般模型列出：id 是 zip 的相對路徑，名稱是檔名去掉 .zip
  void zipModelScanned() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path models = fs::u8path(tempDir.path().toStdString());
    QVERIFY(
      writeZip(models / "Hiyori.zip", {{"Hiyori.model3.json", kModel3}, {"motions/idle.motion3.json", R"({"Meta":{},"Curves":[]})"}, {"exp/smile.exp3.json", R"({"Type":"Live2D Expression"})"}}));

    const auto models_ = scanModels(models);
    const auto* hiyori = byName(models_, "Hiyori");
    QVERIFY(hiyori);
    QCOMPARE(hiyori->id, std::string("Hiyori.zip"));
    // model3.json 沒宣告動作／表情，補全機制在 zip 裡照樣運作
    QCOMPARE(hiyori->motions, (std::vector<MotionGroupInfo>{{"idle", 1, {"idle.motion3.json"}}}));
    QCOMPARE(hiyori->expressions, (std::vector<std::string>{"smile"}));
  }

  // 放在子資料夾裡的 zip，名稱仍然取檔名而不是所在資料夾名
  void zipNameComesFromFileName() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path models = fs::u8path(tempDir.path().toStdString());
    fs::create_directories(models / "sub");
    QVERIFY(writeZip(models / "sub" / "Alice.zip", {{"Alice.model3.json", kModel3}}));

    const auto models_ = scanModels(models);
    QCOMPARE(models_.size(), size_t(1));
    QCOMPARE(models_[0].name, std::string("Alice"));
    QCOMPARE(models_[0].id, std::string("sub/Alice.zip"));
  }

  // 多包一層資料夾的 zip（Windows 右鍵「壓縮成 ZIP 檔」的產物）也認得
  void wrappedZipScanned() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path models = fs::u8path(tempDir.path().toStdString());
    QVERIFY(writeZip(models / "Bob.zip", {{"Bob/Bob.model3.json", kModel3}, {"Bob/motions/wave.motion3.json", R"({"Meta":{},"Curves":[]})"}}));

    const auto models_ = scanModels(models);
    QCOMPARE(models_.size(), size_t(1));
    QCOMPARE(models_[0].name, std::string("Bob"));
    QCOMPARE(models_[0].motions, (std::vector<MotionGroupInfo>{{"wave", 1, {"wave.motion3.json"}}}));
  }

  // 不是模型包的 zip 略過，而且不影響同一輪掃到的其他模型
  void nonModelZipSkipped() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path models = fs::u8path(tempDir.path().toStdString());
    QVERIFY(writeZip(models / "junk.zip", {{"readme.txt", "hello"}}));
    QVERIFY(writeZip(models / "Ok.zip", {{"Ok.model3.json", kModel3}}));
    // 根本不是壓縮檔的 .zip
    {
      std::ofstream file(models / "fake.zip", std::ios::binary);
      file << "這其實是純文字，只是副檔名叫 zip 而已，不該讓整輪掃描壞掉。";
    }

    const auto models_ = scanModels(models);
    QCOMPARE(models_.size(), size_t(1));
    QCOMPARE(models_[0].name, std::string("Ok"));
  }

  // zip 的 dedupe 鍵是 zip 檔本身，不會跟同一層的資料夾模型互相吃掉
  void zipDoesNotCollideWithSiblingEntry() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path models = fs::u8path(tempDir.path().toStdString());
    QVERIFY(writeZip(models / "Zipped.zip", {{"Zipped.model3.json", kModel3}}));
    QVERIFY(writeZip(models / "Other.zip", {{"Other.model3.json", kModel3}}));
    {
      std::ofstream file(models / "Loose.model3.json", std::ios::binary);
      file << kModel3;
    }

    const auto models_ = scanModels(models);
    std::vector<std::string> names;
    for (const auto& m : models_) names.push_back(m.name);
    QCOMPARE(names, (std::vector<std::string>{"Loose", "Other", "Zipped"}));
  }

  // Windows 檔案總管壓出來的中文檔名是系統 ANSI 碼頁（CP950）而且沒設 UTF-8 旗標。
  // 這是真實災情：「藿藿.zip」跟「藿藿」資料夾內容一模一樣，資料夾掃得到、zip 掃不到。
  // 根因是那些位元組被當成 UTF-8：補全把非法 UTF-8 塞進 model3.json，
  // yyjson 解析失敗 → model_scanner 靜靜 continue 掉整個模型。
  void ansiNamedZipScanned() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path models = fs::u8path(tempDir.path().toStdString());
    // model3.json 沒宣告動作，逼補全去掃資料夾 —— 補進去的就是那個非法 UTF-8 檔名
    QVERIFY(writeZip(models / "Legacy.zip",
                     {
                       {"Legacy.model3.json", kModel3},
                       {kBig5Name + ".motion3.json", R"({"Meta":{},"Curves":[]})"},
                     },
                     /*markUtf8=*/false));

    const auto models_ = scanModels(models);
    QCOMPARE(models_.size(), size_t(1));
    QCOMPARE(models_[0].name, std::string("Legacy"));
    // 補全把那個動作找回來了，而且群組名是解碼後的合法 UTF-8
    QCOMPARE(models_[0].motions.size(), size_t(1));
    QVERIFY(strutil::isValidUtf8(models_[0].motions[0].name));
  }

  // 一個壞掉的模型不准把整輪掃描帶走。
  // （之前 readAnnotations 拿非法 UTF-8 去建 std::filesystem::path，
  //   MSVC 丟例外又沒人接，整個行程 abort 成 0xC0000409。）
  void badZipDoesNotAbortScan() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path models = fs::u8path(tempDir.path().toStdString());
    QVERIFY(writeZip(models / "Weird.zip",
                     {
                       {kBig5Name + ".model3.json", kModel3},
                       {kBig5Name + ".moc3", "MOC3"},
                     },
                     /*markUtf8=*/false));
    QVERIFY(writeZip(models / "Sane.zip", {{"Sane.model3.json", kModel3}}));

    const auto models_ = scanModels(models);
    std::vector<std::string> names;
    for (const auto& m : models_) names.push_back(m.name);
    QVERIFY(names.size() >= 1);
    QVERIFY(std::find(names.begin(), names.end(), "Sane") != names.end());
  }

  // zip 旁邊的 sidecar 命名檔在掃描時就讀進來
  void zipAnnotationsSidecarLoaded() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path models = fs::u8path(tempDir.path().toStdString());
    QVERIFY(writeZip(models / "Named.zip", {{"Named.model3.json", kModel3}}));
    {
      std::ofstream file(models / "Named.annotations.json", std::ios::binary);
      file << R"({"motions":{"Idle":"站著發呆"},"expressions":{}})";
    }

    const auto models_ = scanModels(models);
    QCOMPARE(models_.size(), size_t(1));
    QCOMPARE(models_[0].annotations.motions.at("Idle"), std::string("站著發呆"));
  }

  // ── describeModel（Live2D Viewer 用的單一入口） ─────────────────

  // 內容部分必須跟掃描完全一樣 —— 兩條路共用同一支 describeContents，
  // 分岔的話 Viewer 會列出跟桌寵不一樣的動作／表情清單。
  void describeModelMatchesScan() {
    const auto* scanned = byName(models_, "Hiyori");
    QVERIFY(scanned);

    const auto single = describeModel(kFixtures / "Hiyori" / "Hiyori.model3.json");
    QVERIFY(single.has_value());
    QCOMPARE(single->motions, scanned->motions);
    QCOMPARE(single->expressions, scanned->expressions);
    QCOMPARE(single->parameters, scanned->parameters);
    QCOMPARE(single->paramExpressions, scanned->paramExpressions);
    QCOMPARE(single->builtinMotions, scanned->builtinMotions);
    QCOMPARE(single->builtinExpressions, scanned->builtinExpressions);
    QCOMPARE(single->annotations, scanned->annotations);
  }

  // 名稱取所在資料夾（＝掃描對巢狀模型的規則），url 刻意留空
  void describeModelNamesByFolder() {
    const auto single = describeModel(kFixtures / "Hiyori" / "Hiyori.model3.json");
    QVERIFY(single.has_value());
    QCOMPARE(single->name, std::string("Hiyori"));
    QVERIFY(single->url.empty());
    QVERIFY(!single->id.empty());
  }

  // zip 也吃（一個 zip = 一個模型），名稱取檔名去掉 .zip
  void describeModelAcceptsZip() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path zipPath = fs::u8path(tempDir.path().toStdString()) / "Foo.zip";
    QVERIFY(writeZip(zipPath, {{"Foo.model3.json", kModel3}}));

    const auto single = describeModel(zipPath);
    QVERIFY(single.has_value());
    QCOMPARE(single->name, std::string("Foo"));
  }

  // 不是模型的檔案回 nullopt 而不是丟例外（Viewer 會被拖進任何東西）
  void describeModelRejectsNonModel() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path junk = fs::u8path(tempDir.path().toStdString()) / "Junk.zip";
    {
      std::ofstream file(junk, std::ios::binary);
      file << "not a zip at all";
    }
    QVERIFY(!describeModel(junk).has_value());
    QVERIFY(!describeModel(fs::u8path(tempDir.path().toStdString()) / "Missing.model3.json").has_value());
  }

  // ── findDirectoryEntry（Viewer 的資料夾拖放） ─────────────────

  // 資料夾 → 第一層的 *.model3.json。使用者拖進 Viewer 的十次有九次是這個形狀
  void directoryEntryFound() {
    const auto entry = findDirectoryEntry(kFixtures / "Hiyori");
    QVERIFY(entry.has_value());
    QCOMPARE(entry->filename().u8string(), std::string("Hiyori.model3.json"));
  }

  // **只看第一層**：模型埋在子資料夾裡的不算（那是 scanModels 的工作，
  // 遞迴下去會讓「拖一個裝了十隻模型的目錄」靜靜開了其中一隻）
  void directoryEntryNotRecursive() { QVERIFY(!findDirectoryEntry(kFixtures / "a").has_value()); }

  // 檔案本身與不存在的路徑一律 nullopt —— 呼叫端（looksLikeModelPath）
  // 對每個拖進來的東西都直接問，不必先自己 stat
  void directoryEntryRejectsNonDirectory() {
    QVERIFY(!findDirectoryEntry(kFixtures / "Hiyori" / "Hiyori.model3.json").has_value());
    QVERIFY(!findDirectoryEntry(kFixtures / "does-not-exist").has_value());
  }

  // 裸命名入口（model.json / index.json）要讀內容確認是 Cubism 4，與掃描同一套規則
  void directoryEntryGenericSniffed() {
    QVERIFY(findDirectoryEntry(kFixtures / "GenericV4").has_value());
    QVERIFY(!findDirectoryEntry(kFixtures / "GenericV2").has_value());
    QVERIFY(!findDirectoryEntry(kFixtures / "GenericIndex").has_value());
    QVERIFY(!findDirectoryEntry(kFixtures / "GenericJunk").has_value());
  }

  // *.model3.json 優先於裸命名入口，而且不是靠檔名排序贏的
  // （zoo 排在 model.json 後面，照樣要選 zoo）
  void directoryEntryPrefersModel3() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    writeFile(dir / "model.json", kModel3);
    writeFile(dir / "zoo.model3.json", kModel3);

    const auto entry = findDirectoryEntry(dir);
    QVERIFY(entry.has_value());
    QCOMPARE(entry->filename().u8string(), std::string("zoo.model3.json"));
  }

  // 同一層有多個 *.model3.json 時取名稱排序的第一個 ——
  // 選中誰不可以隨檔案系統的列舉順序而變
  void directoryEntryPicksFirstByName() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    writeFile(dir / "Beta.model3.json", kModel3);
    writeFile(dir / "Alpha.model3.json", kModel3);

    const auto entry = findDirectoryEntry(dir);
    QVERIFY(entry.has_value());
    QCOMPARE(entry->filename().u8string(), std::string("Alpha.model3.json"));
  }

  // 開頭是點的略過：macOS 打包留下的 ._Foo.model3.json 是 AppleDouble 中繼資料，
  // 而且排序還排在真檔案前面，撿到它就是「拖進來卻說解析失敗」
  void directoryEntrySkipsDotFiles() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    writeFile(dir / "._Alpha.model3.json", "not a model at all");
    writeFile(dir / "Alpha.model3.json", kModel3);

    const auto entry = findDirectoryEntry(dir);
    QVERIFY(entry.has_value());
    QCOMPARE(entry->filename().u8string(), std::string("Alpha.model3.json"));
  }

  // ── stem 空的入口檔（隱藏檔規則的唯一例外） ─────────────────

  // 例外只認「整個檔名就是副檔名」—— AppleDouble 與一般隱藏檔的 stem 都不是空的
  void emptyStemAssetNames() {
    // 入口檔與 ModelAssets::list 的四個目標，少一個就是「表情／動作靜靜地不見了」
    QVERIFY(isEmptyStemAssetName(".model3.json"));
    QVERIFY(isEmptyStemAssetName(".MODEL3.JSON"));
    QVERIFY(isEmptyStemAssetName(".motion3.json"));
    QVERIFY(isEmptyStemAssetName(".exp3.json"));
    QVERIFY(isEmptyStemAssetName(".pose3.json"));
    QVERIFY(isEmptyStemAssetName(".vtube.json"));

    // 白名單刻意不是「所有 Live2D 副檔名」：這幾種是拿 model3.json 裡寫的路徑
    // 直接 read()／exists()，從來不經過隱藏檔規則，列進來只是多一份要維護的真相
    QVERIFY(!isEmptyStemAssetName(".moc3"));
    QVERIFY(!isEmptyStemAssetName(".cdi3.json"));
    QVERIFY(!isEmptyStemAssetName(".physics3.json"));

    QVERIFY(!isEmptyStemAssetName("._.model3.json"));
    QVERIFY(!isEmptyStemAssetName("._Foo.model3.json"));
    QVERIFY(!isEmptyStemAssetName(".secret.motion3.json"));
    QVERIFY(!isEmptyStemAssetName("Foo.model3.json"));
    QVERIFY(!isEmptyStemAssetName(".DS_Store"));
    QVERIFY(!isEmptyStemAssetName(""));
  }

  // 匯出時 stem 留空的模型整組檔案就叫 .model3.json / .moc3 / .cdi3.json
  // （MementoMori 的 Characters/CHR_*/model），拖那個資料夾進 Viewer 原本
  // 完全沒反應。AppleDouble 的 "._.model3.json"（排序還排在前面）照樣要擋掉。
  void directoryEntryAcceptsEmptyStem() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    writeFile(dir / "._.model3.json", "not a model at all");
    writeFile(dir / ".model3.json", kModel3);

    const auto entry = findDirectoryEntry(dir);
    QVERIFY(entry.has_value());
    QCOMPARE(entry->filename().u8string(), std::string(".model3.json"));
  }

  // 壞掉的是**掃描**這一條（桌寵的 models 目錄整隻掃不到）；describeModel 從頭到尾
  // 是指名讀取、不經過列舉，本來就通。順帶釘住兩條路對同一隻模型的一致性。
  void scansEmptyStemModel() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path models = fs::u8path(tempDir.path().toStdString());
    const fs::path modelDir = models / "CHR_000004" / "model";
    fs::create_directories(modelDir);
    writeFile(modelDir / ".model3.json", kModel3);

    const auto found = scanModels(models);
    QCOMPARE(found.size(), size_t(1));
    QCOMPARE(found[0].name, std::string("model"));
    QCOMPARE(found[0].id, std::string("CHR_000004/model/.model3.json"));

    QVERIFY(describeModel(modelDir / ".model3.json").has_value());
  }

  // 直接放在 models 根目錄時沒有資料夾名可用，而 ".model3.json" 剝掉副檔名剝成空字串。
  // 空名稱會變成清單上一列空白、modelListHint 裡的一個空欄位，
  // 而且 resolveModel 拿 name 比對時會被空字串靜默命中。
  void emptyStemModelAtRootKeepsName() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path models = fs::u8path(tempDir.path().toStdString());
    writeFile(models / ".model3.json", kModel3);

    const auto found = scanModels(models);
    QCOMPARE(found.size(), size_t(1));
    QCOMPARE(found[0].name, std::string(".model3.json"));
  }
};

QTEST_APPLESS_MAIN(TestModelScanner)
#include "test_model_scanner.moc"
