// 命名檔（annotations）：鍵的解析與組裝、預設名稱補齊、磁碟讀寫與壞檔備份
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <fstream>

#include "core/annotations.h"
#include "core/model_assets.h"
#include "zip_builder.h"

using namespace l2m;
namespace fs = std::filesystem;

namespace {

void writeText(const fs::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
}

bool fileExists(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

}  // namespace

class TestAnnotations : public QObject {
  Q_OBJECT

private slots:
  // === motionKey / parseMotionKey ===

  // 沒有索引時就是群組名本身
  void keyWithoutIndex() {
    QCOMPARE(motionKey("Idle"), std::string("Idle"));
    const auto parts = parseMotionKey("Idle");
    QCOMPARE(parts.group, std::string("Idle"));
    QCOMPARE(parts.index, -1);
  }

  // 有索引時用 # 接在後面，且可以還原
  void keyWithIndexRoundTrips() {
    QCOMPARE(motionKey("TapBody", 2), std::string("TapBody#2"));
    const auto parts = parseMotionKey("TapBody#2");
    QCOMPARE(parts.group, std::string("TapBody"));
    QCOMPARE(parts.index, 2);
  }

  // 群組名本身含 # 時以最後一個 # 為準
  void lastHashWins() {
    const auto parts = parseMotionKey("a#b#3");
    QCOMPARE(parts.group, std::string("a#b"));
    QCOMPARE(parts.index, 3);
  }

  // # 後面不是合法索引時整串當成群組名
  void invalidIndexTreatedAsGroup() {
    QCOMPARE(parseMotionKey("weird#name").group, std::string("weird#name"));
    QCOMPARE(parseMotionKey("weird#name").index, -1);
    QCOMPARE(parseMotionKey("weird#-1").group, std::string("weird#-1"));
    QCOMPARE(parseMotionKey("weird#-1").index, -1);
  }

  // 空字串群組名也能正常往返（模型允許這種命名）
  void emptyGroupRoundTrips() {
    QCOMPARE(motionKey("", 0), std::string("#0"));
    const auto parts = parseMotionKey("#0");
    QCOMPARE(parts.group, std::string(""));
    QCOMPARE(parts.index, 0);
  }

  // === annotationsPathFor ===

  // Cubism 4 入口檔旁邊
  void pathBesideCubism4Entry() { QCOMPARE(annotationsPathFor(fs::path("m") / "Hiyori" / "Hiyori.model3.json"), fs::path("m") / "Hiyori" / "Hiyori.annotations.json"); }

  // 裸命名入口檔（model.json / index.json）也推得出乾淨的檔名
  void pathForGenericEntries() {
    QCOMPARE(annotationsPathFor(fs::path("m") / "Bare" / "model.json"), fs::path("m") / "Bare" / "model.annotations.json");
    QCOMPARE(annotationsPathFor(fs::path("m") / "Bare" / "index.json"), fs::path("m") / "Bare" / "index.annotations.json");
  }

  // === applyMeaning ===

  // 設定並去掉頭尾空白
  void setsAndTrims() {
    const auto next = applyMeaning({}, AnnotationKind::Motions, "Idle#1", "  揮手  ");
    QCOMPARE(next.motions.at("Idle#1"), std::string("揮手"));
  }

  // 空字串代表清除
  void emptyStringClears() {
    const auto first = applyMeaning({}, AnnotationKind::Expressions, "F01", "微笑");
    const auto second = applyMeaning(first, AnnotationKind::Expressions, "F01", "   ");
    QVERIFY(second.expressions.empty());
  }

  // 不會改到傳進去的物件
  void inputNotMutated() {
    const ModelAnnotations original;
    applyMeaning(original, AnnotationKind::Motions, "Idle", "待機");
    QVERIFY(original.motions.empty());
  }

  // === fillDefaultNames ===

  // 群組填群組名、多動作群組的每一段填檔名（去掉 .motion3.json）、表情填自己
  void defaultNamesFillEverything() {
    ModelInfo model;
    model.motions.push_back({"Idle", 1, {"idle_01.motion3.json"}});
    model.motions.push_back({"TapBody", 2, {"tap_a.motion3.json", "tap_b.motion3.json"}});
    model.expressions = {"F01", "F02"};

    const auto next = fillDefaultNames(model, {});
    QCOMPARE(next.motions.at("Idle"), std::string("Idle"));
    QCOMPARE(next.motions.at("TapBody"), std::string("TapBody"));
    QCOMPARE(next.motions.at("TapBody#0"), std::string("tap_a"));
    QCOMPARE(next.motions.at("TapBody#1"), std::string("tap_b"));
    QCOMPARE(next.expressions.at("F01"), std::string("F01"));
    QCOMPARE(next.expressions.at("F02"), std::string("F02"));
  }

  // 對齊命名區的列：單一動作的群組畫面上沒有 #索引 列，這裡也不替它填
  void defaultNamesSkipIndexRowsOfSingleMotionGroups() {
    ModelInfo model;
    model.motions.push_back({"Idle", 1, {"idle_01.motion3.json"}});

    const auto next = fillDefaultNames(model, {});
    QCOMPARE(next.motions.size(), size_t(1));
    QVERIFY(next.motions.count("Idle#0") == 0);
  }

  // 已經填過的命名一律保留 —— 這顆按鈕是補空缺，不是整份覆蓋
  void defaultNamesKeepExistingMeanings() {
    ModelInfo model;
    model.motions.push_back({"TapBody", 2, {"tap_a.motion3.json", "tap_b.motion3.json"}});
    model.expressions = {"F01"};

    ModelAnnotations current;
    current.motions["TapBody"] = "拍拍";
    current.expressions["F01"] = "微笑";

    const auto next = fillDefaultNames(model, current);
    QCOMPARE(next.motions.at("TapBody"), std::string("拍拍"));
    QCOMPARE(next.motions.at("TapBody#0"), std::string("tap_a"));
    QCOMPARE(next.expressions.at("F01"), std::string("微笑"));
  }

  // 空群組名與缺檔名資訊的段落沒有可用的預設名稱，跳過而不是寫進空字串
  void defaultNamesSkipEmptyNames() {
    ModelInfo model;
    model.motions.push_back({"", 2, {}});

    const auto next = fillDefaultNames(model, {});
    QVERIFY(next.motions.empty());
  }

  // 檔名不是 .motion3.json 結尾時原樣沿用（不硬剝副檔名）
  void defaultNamesKeepUnrecognizedFileNames() {
    ModelInfo model;
    model.motions.push_back({"G", 2, {"a.mtn", "b.motion3.json"}});

    const auto next = fillDefaultNames(model, {});
    QCOMPARE(next.motions.at("G#0"), std::string("a.mtn"));
    QCOMPARE(next.motions.at("G#1"), std::string("b"));
  }

  // === 讀寫模型資料夾裡的命名檔 ===

  void readWriteInModelFolder() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    fs::create_directories(dir / "Hiyori");
    const fs::path entry = dir / "Hiyori" / "Hiyori.model3.json";
    writeText(entry, "{}");

    // 沒有命名檔時回傳空的
    QVERIFY(readAnnotations(entry) == ModelAnnotations{});

    // 寫進模型自己的資料夾，再讀得回來
    const auto data = applyMeaning({}, AnnotationKind::Motions, "Idle#1", "伸懶腰");
    writeAnnotations(entry, data);
    QVERIFY(fileExists(dir / "Hiyori" / "Hiyori.annotations.json"));
    QCOMPARE(readAnnotations(entry).motions.at("Idle#1"), std::string("伸懶腰"));

    // 內容全空時不留下空檔案
    writeAnnotations(entry, ModelAnnotations{});
    QVERIFY(!fileExists(annotationsPathFor(entry)));
  }

  // 壞掉的 JSON 會被備份，並以空命名繼續
  void brokenJsonBackedUp() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    fs::create_directories(dir / "Hiyori");
    const fs::path entry = dir / "Hiyori" / "Hiyori.model3.json";
    writeText(entry, "{}");

    writeText(annotationsPathFor(entry), "{ not json");
    QVERIFY(readAnnotations(entry) == ModelAnnotations{});
    QVERIFY(fileExists(dir / "Hiyori" / "Hiyori.annotations.bak.json"));
  }

  // 欄位不合法的命名檔同樣被備份
  void invalidFieldsBackedUp() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    fs::create_directories(dir / "Hiyori");
    const fs::path entry = dir / "Hiyori" / "Hiyori.model3.json";
    writeText(entry, "{}");

    writeText(annotationsPathFor(entry), R"({"motions":"nope"})");
    QVERIFY(readAnnotations(entry) == ModelAnnotations{});
    QVERIFY(fileExists(dir / "Hiyori" / "Hiyori.annotations.bak.json"));
  }

  // === migrateLegacyAnnotations ===

  // 沒有舊檔時什麼都不做
  void noLegacyFileDoesNothing() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    QCOMPARE(migrateLegacyAnnotations(dir / "annotations.json", dir / "models", {"Hiyori/Hiyori.model3.json"}), 0);
  }

  // 把舊檔拆進各模型資料夾並改名保留
  void splitsIntoModelFolders() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    const fs::path modelsDir = dir / "models";
    const fs::path legacy = dir / "annotations.json";
    for (const char* name : {"Hiyori", "Haru"}) {
      fs::create_directories(modelsDir / name);
      writeText(modelsDir / name / (std::string(name) + ".model3.json"), "{}");
    }

    writeText(legacy, R"({"Hiyori/Hiyori.model3.json":{"motions":{"Idle":"待機"},"expressions":{}},)"
                      R"("Haru/Haru.model3.json":{"motions":{},"expressions":{"F01":"微笑"}}})");

    const std::vector<std::string> ids{"Hiyori/Hiyori.model3.json", "Haru/Haru.model3.json"};
    QCOMPARE(migrateLegacyAnnotations(legacy, modelsDir, ids), 2);
    QCOMPARE(readAnnotations(modelsDir / "Hiyori" / "Hiyori.model3.json").motions.at("Idle"), std::string("待機"));
    QCOMPARE(readAnnotations(modelsDir / "Haru" / "Haru.model3.json").expressions.at("F01"), std::string("微笑"));
    QVERIFY(!fileExists(legacy));
    QVERIFY(fileExists(dir / "annotations.migrated.json"));
  }

  // 對應不到的模型直接略過，不會炸掉整個搬移
  void unknownModelSkipped() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    const fs::path modelsDir = dir / "models";
    const fs::path legacy = dir / "annotations.json";
    fs::create_directories(modelsDir / "Hiyori");
    writeText(modelsDir / "Hiyori" / "Hiyori.model3.json", "{}");

    writeText(legacy, R"({"Gone/Gone.model3.json":{"motions":{"Idle":"不存在"},"expressions":{}},)"
                      R"("Hiyori/Hiyori.model3.json":{"motions":{"Idle":"待機"},"expressions":{}}})");

    QCOMPARE(migrateLegacyAnnotations(legacy, modelsDir, {"Hiyori/Hiyori.model3.json", "Haru/Haru.model3.json"}), 1);
    QCOMPARE(readAnnotations(modelsDir / "Hiyori" / "Hiyori.model3.json").motions.at("Idle"), std::string("待機"));
  }

  // 模型資料夾已經有命名檔時以它為準，不被舊檔蓋掉
  void existingAnnotationsWin() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    const fs::path modelsDir = dir / "models";
    const fs::path legacy = dir / "annotations.json";
    fs::create_directories(modelsDir / "Hiyori");
    const fs::path entry = modelsDir / "Hiyori" / "Hiyori.model3.json";
    writeText(entry, "{}");

    ModelAnnotations current;
    current.motions["Idle"] = "新的";
    writeAnnotations(entry, current);
    writeText(legacy, R"({"Hiyori/Hiyori.model3.json":{"motions":{"Idle":"舊的"},"expressions":{}}})");

    QCOMPARE(migrateLegacyAnnotations(legacy, modelsDir, {"Hiyori/Hiyori.model3.json"}), 0);
    QCOMPARE(readAnnotations(entry).motions.at("Idle"), std::string("新的"));
  }

  // === zip 模型的 sidecar ===

  // zip 是唯讀容器，命名檔推到 zip 旁邊（Foo.zip → Foo.annotations.json）
  void zipAnnotationsPathIsSidecar() {
    QCOMPARE(annotationsPathFor(fs::u8path("models/Foo.zip")), fs::u8path("models/Foo.annotations.json"));
    QCOMPARE(annotationsPathFor(fs::u8path("models/Foo.ZIP")), fs::u8path("models/Foo.annotations.json"));
  }

  // 沒有 sidecar 時讀得到作者打包進 zip 的那一份
  void zipInnerAnnotationsReadWhenNoSidecar() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    const fs::path zipPath = dir / "Foo.zip";
    QVERIFY(writeZip(zipPath, {{"Foo.model3.json", R"({"FileReferences":{}})"}, {"Foo.annotations.json", R"({"motions":{"Idle":"內建的"},"expressions":{}})"}}));

    const auto assets = openModelAssets(zipPath);
    QVERIFY(assets);
    QCOMPARE(readAnnotations(zipPath, *assets).motions.at("Idle"), std::string("內建的"));
  }

  // sidecar 存在就蓋掉 zip 內建的那一份，而且寫入永遠落在 zip 外面
  void zipSidecarWinsAndWritesOutside() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    const fs::path zipPath = dir / "Foo.zip";
    QVERIFY(writeZip(zipPath, {{"Foo.model3.json", R"({"FileReferences":{}})"}, {"Foo.annotations.json", R"({"motions":{"Idle":"內建的"},"expressions":{}})"}}));

    const auto assets = openModelAssets(zipPath);
    QVERIFY(assets);

    ModelAnnotations mine;
    mine.motions["Idle"] = "我改的";
    writeAnnotations(zipPath, mine);

    QVERIFY(fileExists(dir / "Foo.annotations.json"));
    QCOMPARE(readAnnotations(zipPath, *assets).motions.at("Idle"), std::string("我改的"));
    // zip 本身一個位元組都沒被動到
    const auto reopened = openModelAssets(zipPath);
    QVERIFY(reopened);
    QVERIFY(reopened->read("Foo.annotations.json").has_value());
  }

  // 資料夾模型的 sidecar 與「容器內」本來就是同一個檔案，兩個多載結果必須一致
  void directoryModelUnaffectedByOverload() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    const fs::path entry = dir / "Hiyori.model3.json";
    writeText(entry, R"({"FileReferences":{}})");

    ModelAnnotations data;
    data.expressions["f01"] = "微笑";
    writeAnnotations(entry, data);

    const auto assets = openModelAssets(entry);
    QVERIFY(assets);
    QCOMPARE(readAnnotations(entry, *assets).expressions.at("f01"), std::string("微笑"));
    QCOMPARE(readAnnotations(entry).expressions.at("f01"), std::string("微笑"));
  }
};

QTEST_APPLESS_MAIN(TestAnnotations)
#include "test_annotations.moc"
