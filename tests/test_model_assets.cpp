// 模型資源存取層（core/model_assets.h）。
//
// 這一支最重要的是 dirAndZipAgree()：拿同一份 fixture 做成「資料夾」與「zip」
// 兩份，逐一斷言 read / exists / list 的輸出一字不差相同。兩條路徑一旦分歧，
// 症狀就是「解壓縮就好、壓起來就壞」那種最難查的 bug。
#include <QtTest>

#include <filesystem>
#include <fstream>

#include "core/model_assets.h"
#include "zip_builder.h"

using namespace l2m;
namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = fs::u8path(L2M_FIXTURES_DIR);

const char* kModel3 = R"({"Version":3,"FileReferences":{"Moc":"m.moc3"}})";

}  // namespace

class TestModelAssets : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  fs::path root_;

  fs::path makeZip(const std::string& name, const std::vector<ZipEntry>& entries) {
    const fs::path path = root_ / name;
    [&] { QVERIFY(writeZip(path, entries)); }();
    return path;
  }

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    root_ = fs::u8path(dir_.path().toStdString());
  }

  // === 兩種容器的行為必須完全一致 ===

  // MovedFiles 這個 fixture 剛好同時有根目錄的檔、exp/ 與 motions/ 子資料夾，
  // 涵蓋 list() 的深度規則
  void dirAndZipAgree() {
    const fs::path entry = kFixtures / "MovedFiles" / "moved.model3.json";
    const auto dirAssets = openModelAssets(entry);
    QVERIFY(dirAssets);

    const fs::path zipPath = root_ / "MovedFiles.zip";
    QVERIFY(zipDirectory(kFixtures / "MovedFiles", zipPath));
    const auto zipAssets = openModelAssets(zipPath);
    QVERIFY(zipAssets);

    QCOMPARE(zipAssets->entryName(), dirAssets->entryName());

    for (const char* suffix : {".exp3.json", ".motion3.json", ".json"}) {
      for (int depth = 1; depth <= 3; ++depth) {
        QCOMPARE(zipAssets->list(suffix, depth), dirAssets->list(suffix, depth));
      }
    }

    for (const auto& rel : dirAssets->list(".json", 3)) {
      QCOMPARE(zipAssets->exists(rel), dirAssets->exists(rel));
      QCOMPARE(zipAssets->read(rel), dirAssets->read(rel));
      QCOMPARE(zipAssets->readPrefix(rel, 16), dirAssets->readPrefix(rel, 16));
    }

    QCOMPARE(zipAssets->exists("nope.json"), dirAssets->exists("nope.json"));
    QCOMPARE(zipAssets->read("nope.json"), dirAssets->read("nope.json"));
    QCOMPARE(zipAssets->readPrefix("nope.json", 4), dirAssets->readPrefix("nope.json", 4));
  }

  // 隱藏檔：兩邊都是「不列出，但直接指名就讀得到」。
  // 這一條是刻意補的 —— 原本 zip 版把「不列出」寫進了路徑正規化，
  // 於是同一個模型放資料夾讀得到、壓成 zip 就讀不到。
  void hiddenFilesAgree() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    const fs::path modelDir = dir / "Hidden";
    fs::create_directories(modelDir);
    {
      std::ofstream(modelDir / "Hidden.model3.json", std::ios::binary) << kModel3;
      std::ofstream(modelDir / ".secret.motion3.json", std::ios::binary) << "{}";
    }
    const auto dirAssets = openModelAssets(modelDir / "Hidden.model3.json");
    QVERIFY(dirAssets);

    const fs::path zipPath = dir / "Hidden.zip";
    QVERIFY(writeZip(zipPath, {{"Hidden.model3.json", kModel3}, {".secret.motion3.json", "{}"}}));
    const auto zipAssets = openModelAssets(zipPath);
    QVERIFY(zipAssets);

    // 不列出
    QCOMPARE(zipAssets->list(".motion3.json", 2), dirAssets->list(".motion3.json", 2));
    QVERIFY(dirAssets->list(".motion3.json", 2).empty());
    // 但指名讀得到
    QCOMPARE(zipAssets->exists(".secret.motion3.json"), dirAssets->exists(".secret.motion3.json"));
    QVERIFY(dirAssets->exists(".secret.motion3.json"));
    QCOMPARE(zipAssets->read(".secret.motion3.json"), dirAssets->read(".secret.motion3.json"));
  }

  // stem 空的檔名（整個檔名就是副檔名）不是隱藏檔，兩種容器都要當成模型資產。
  // zip 那邊原本連入口檔都被隱藏檔規則濾掉，症狀是同一隻模型
  // 「資料夾開得起來、壓成 zip 就說不是模型包」。
  // zip 刻意多包一層 model/（Windows 檔案總管壓出來的形狀）。
  void emptyStemEntryAgrees() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    const fs::path modelDir = dir / "model";
    fs::create_directories(modelDir);
    {
      std::ofstream(modelDir / ".model3.json", std::ios::binary) << kModel3;
      std::ofstream(modelDir / ".motion3.json", std::ios::binary) << "{}";
    }
    const auto dirAssets = openModelAssets(modelDir / ".model3.json");
    QVERIFY(dirAssets);

    const fs::path zipPath = dir / "model.zip";
    QVERIFY(writeZip(zipPath, {{"model/.model3.json", kModel3}, {"model/.motion3.json", "{}"}}));
    const auto zipAssets = openModelAssets(zipPath);
    QVERIFY(zipAssets);

    QCOMPARE(zipAssets->entryName(), std::string(".model3.json"));
    QCOMPARE(zipAssets->entryName(), dirAssets->entryName());
    QCOMPARE(zipAssets->read(".model3.json"), dirAssets->read(".model3.json"));
    QCOMPARE(zipAssets->list(".motion3.json", 2), dirAssets->list(".motion3.json", 2));
    QCOMPARE(dirAssets->list(".motion3.json", 2), (std::vector<std::string>{".motion3.json"}));
  }

  // list 的 maxDepth 是「最多幾個路徑段」：1 只有模型根目錄，2 再加一層子資料夾
  void listDepthCountsSegments() {
    const fs::path path = makeZip("depth.zip", {
                                                 {"a.model3.json", kModel3},
                                                 {"root.motion3.json", "{}"},
                                                 {"motions/one.motion3.json", "{}"},
                                                 {"motions/deep/two.motion3.json", "{}"},
                                               });
    const auto assets = openModelAssets(path);
    QVERIFY(assets);
    QCOMPARE(assets->list(".motion3.json", 1), (std::vector<std::string>{"root.motion3.json"}));
    QCOMPARE(assets->list(".motion3.json", 2), (std::vector<std::string>{"motions/one.motion3.json", "root.motion3.json"}));
    QCOMPARE(assets->list(".motion3.json", 3), (std::vector<std::string>{"motions/deep/two.motion3.json", "motions/one.motion3.json", "root.motion3.json"}));
  }

  // === zip 內入口檔的判定 ===

  void findsEntryAtZipRoot() {
    const fs::path path = makeZip("root.zip", {
                                                {"Hiyori.model3.json", kModel3},
                                                {"motions/idle.motion3.json", "{}"},
                                              });
    const auto assets = openModelAssets(path);
    QVERIFY(assets);
    QCOMPARE(assets->entryName(), std::string("Hiyori.model3.json"));
    QVERIFY(assets->exists("motions/idle.motion3.json"));
  }

  // Windows 檔案總管右鍵「壓縮成 ZIP 檔」會多包一層，這是最常見的形態。
  // 那一層由 ZipModelAssets 吸收成前綴，上層看到的相對路徑跟資料夾版一模一樣。
  void drillsIntoSingleTopFolder() {
    const fs::path path = makeZip("wrapped.zip", {
                                                   {"Hiyori/Hiyori.model3.json", kModel3},
                                                   {"Hiyori/motions/idle.motion3.json", "{}"},
                                                 });
    const auto assets = openModelAssets(path);
    QVERIFY(assets);
    QCOMPARE(assets->entryName(), std::string("Hiyori.model3.json"));
    QVERIFY(assets->exists("motions/idle.motion3.json"));
    QCOMPARE(assets->list(".motion3.json", 2), (std::vector<std::string>{"motions/idle.motion3.json"}));
    QCOMPARE(assets->read("Hiyori.model3.json").value(), std::string(kModel3));
  }

  // 兩個頂層資料夾代表這不是單一模型包，無從決定要哪個
  void refusesTwoTopFolders() {
    const fs::path path = makeZip("two.zip", {
                                               {"A/a.model3.json", kModel3},
                                               {"B/b.model3.json", kModel3},
                                             });
    QVERIFY(!openModelAssets(path));
  }

  // 根目錄有入口檔時就不再往資料夾裡鑽
  void rootWinsOverTopFolder() {
    const fs::path path = makeZip("both.zip", {
                                                {"outer.model3.json", kModel3},
                                                {"Inner/inner.model3.json", kModel3},
                                              });
    const auto assets = openModelAssets(path);
    QVERIFY(assets);
    QCOMPARE(assets->entryName(), std::string("outer.model3.json"));
  }

  // 同一層有多個 model3.json 時取排序後的第一個（同資料夾模型的 dedupe 規則）
  void picksFirstSortedEntry() {
    const fs::path path = makeZip("multi.zip", {
                                                 {"zebra.model3.json", kModel3},
                                                 {"alpha.model3.json", kModel3},
                                               });
    const auto assets = openModelAssets(path);
    QVERIFY(assets);
    QCOMPARE(assets->entryName(), std::string("alpha.model3.json"));
  }

  // 裸命名入口（model.json / index.json）要讀內容才知道是不是 Cubism 4
  void genericEntrySniffedInZip() {
    const fs::path good = makeZip("generic.zip", {{"model.json", kModel3}});
    const auto assets = openModelAssets(good);
    QVERIFY(assets);
    QCOMPARE(assets->entryName(), std::string("model.json"));

    // Cubism 2 的入口沒有 FileReferences，不收
    const fs::path legacy = makeZip("legacy.zip", {{"model.json", R"({"version":"Sample 1.0.0","model":"a.moc"})"}});
    QVERIFY(!openModelAssets(legacy));
  }

  // *.model3.json 優先於裸命名入口
  void model3WinsOverGeneric() {
    const fs::path path = makeZip("mix.zip", {
                                               {"index.json", kModel3},
                                               {"real.model3.json", kModel3},
                                             });
    const auto assets = openModelAssets(path);
    QVERIFY(assets);
    QCOMPARE(assets->entryName(), std::string("real.model3.json"));
  }

  // 不相干的 zip 只是開不起來，不會丟例外（models 目錄裡本來就可能有）
  void nonModelZipReturnsNull() {
    QVERIFY(!openModelAssets(makeZip("junk.zip", {{"readme.txt", "hello"}})));
    QVERIFY(!openModelAssets(makeZip("blank.zip", {})));
    QVERIFY(!openModelAssets(root_ / "does-not-exist.zip"));
  }

  // === 資料夾模型 ===

  void directoryEntryNameIsFileName() {
    const auto assets = openModelAssets(kFixtures / "Hiyori" / "Hiyori.model3.json");
    QVERIFY(assets);
    QCOMPARE(assets->entryName(), std::string("Hiyori.model3.json"));
    QVERIFY(assets->exists("Hiyori.model3.json"));
    QVERIFY(!assets->exists("nope.json"));
  }

  // 資料夾不存在時不會崩，只是什麼都讀不到
  void missingDirectoryIsEmpty() {
    const auto assets = openModelAssets(root_ / "nowhere" / "x.model3.json");
    QVERIFY(assets);
    QVERIFY(!assets->read("x.model3.json").has_value());
    QVERIFY(assets->list(".json", 2).empty());
  }
};

QTEST_APPLESS_MAIN(TestModelAssets)
#include "test_model_assets.moc"
