// zip 唯讀存取層（core/zip_archive.h）。
//
// 重點在「不合格的輸入不會炸」與「條目名稱的正規化規則跟掃資料夾時一致」，
// 因為 models 目錄裡本來就可能躺著跟模型無關的 zip。
#include <QtTest>

#include <filesystem>
#include <fstream>

#include "core/string_util.h"
#include "core/zip_archive.h"
#include "zip_builder.h"

using namespace l2m;
namespace fs = std::filesystem;

namespace {

// 「符玄」的 Big5(CP950) 位元組。這不是合法的 UTF-8（0xB2 當前導位元組就違法），
// 用來模擬 Windows 檔案總管壓出來的中文檔名。
// 解碼結果會隨機器的 ANSI 碼頁而不同（CP950 的機器上就是「符玄」），所以測試
// 只斷言「一定是合法 UTF-8」與「entries() 拿到的名字餵回 read() 一定找得到」
// 這兩條與碼頁無關的不變量。
//
// 寫成數值而不是 "\xB2..." 字串跳脫：這個檔本身是 UTF-8，跳脫序列很容易在編輯
// 過程被重新編碼成 U+00B2 那種「看起來一樣、其實是合法 UTF-8」的東西，測試就
// 悄悄失去意義了（實際踩過一次）。
const std::string kBig5Name{static_cast<char>(0xB2), static_cast<char>(0xC5), static_cast<char>(0xA5), static_cast<char>(0xC8)};

// 可壓縮的長字串，用來驗證 readPrefix 真的沒有整包解開
std::string longText() {
  std::string text;
  for (int i = 0; i < 20000; ++i) text += "0123456789";
  return text;
}

std::unique_ptr<ZipArchive> openAt(const fs::path& path) { return ZipArchive::open(openFileByteSource(path)); }

}  // namespace

class TestZipArchive : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  fs::path root_;

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    root_ = fs::u8path(dir_.path().toStdString());
  }

  // === normalizeZipEntry 的規則 ===

  // 反斜線換成 '/'、去掉開頭的 "./" 與 '/'
  void normalizesSeparatorsAndPrefixes() {
    QCOMPARE(normalizeZipEntry("a.txt"), std::string("a.txt"));
    QCOMPARE(normalizeZipEntry("./a.txt"), std::string("a.txt"));
    QCOMPARE(normalizeZipEntry(".///./a.txt"), std::string("a.txt"));
    QCOMPARE(normalizeZipEntry("/a.txt"), std::string("a.txt"));
    QCOMPARE(normalizeZipEntry("sub\\a.txt"), std::string("sub/a.txt"));
  }

  // 目錄項、"."／".."、空段一律不是合法路徑
  void rejectsInvalidPaths() {
    QVERIFY(normalizeZipEntry("").empty());
    QVERIFY(normalizeZipEntry("sub/").empty());
    QVERIFY(normalizeZipEntry("../escape.txt").empty());
    QVERIFY(normalizeZipEntry("sub/../escape.txt").empty());
    QVERIFY(normalizeZipEntry("sub/./a.txt").empty());
    QVERIFY(normalizeZipEntry("sub//a.txt").empty());
  }

  // 隱藏檔與 __MACOSX/ 是「合法但不列出」——
  // 資料夾模型也是這樣（directory_iterator 跳過隱藏檔，直接開檔卻讀得到），
  // 把這兩件事混成一條規則就會讓 zip 比資料夾少讀得到東西
  void hiddenEntriesAreValidButNotListed() {
    QCOMPARE(normalizeZipEntry(".hidden"), std::string(".hidden"));
    QCOMPARE(normalizeZipEntry("sub/.hidden"), std::string("sub/.hidden"));
    QCOMPARE(normalizeZipEntry("__MACOSX/foo.txt"), std::string("__MACOSX/foo.txt"));

    QVERIFY(!isListableZipEntry(".hidden"));
    QVERIFY(!isListableZipEntry("sub/.hidden"));
    QVERIFY(!isListableZipEntry(".git/config"));
    QVERIFY(!isListableZipEntry("__MACOSX/foo.txt"));
    QVERIFY(isListableZipEntry("a.txt"));
    QVERIFY(isListableZipEntry("sub/a.txt"));

    // 例外：stem 空的資產檔名（整個檔名就是副檔名）是真的模型檔，不是隱藏檔。
    // 濾掉的話同一隻模型「資料夾開得起來、壓成 zip 就說不是模型包」。
    QVERIFY(isListableZipEntry(".model3.json"));
    QVERIFY(isListableZipEntry("model/.model3.json"));
    QVERIFY(!isListableZipEntry("._.model3.json"));
    QVERIFY(!isListableZipEntry(".hidden/.model3.json"));
  }

  // === 條目名稱的編碼 ===
  //
  // 真實災情：使用者用 Windows 檔案總管把「藿藿」資料夾壓成 zip，檔名被寫成
  // CP950 位元組而且沒設 UTF-8 旗標。我們原本直接把那些位元組當 UTF-8 用，
  // 結果 (1) model3.json 裡 UTF-8 的引用一個都對不上、
  // (2) 補全把非法 UTF-8 塞進 JSON 讓 yyjson 解析失敗、整個模型被略過、
  // (3) 拿那些位元組去建 std::filesystem::path 讓 MSVC 丟例外，整個掃描 abort。

  // 沒設 UTF-8 旗標又不是合法 UTF-8 → 用系統 ANSI 碼頁解碼成 UTF-8
  void decodesLegacyAnsiNames() {
    const std::string raw = kBig5Name + ".model3.json";
    QVERIFY(!strutil::isValidUtf8(raw));

    const std::string decoded = decodeZipEntryName(raw, false);
    QVERIFY(strutil::isValidUtf8(decoded));
    QVERIFY(decoded != raw);
    QVERIFY(decoded.size() > 4);
    QCOMPARE(decoded.substr(decoded.size() - 12), std::string(".model3.json"));
  }

  // 已經是合法 UTF-8 就原樣沿用，**不能**再丟進 ANSI 解碼一次。
  // 實測使用者的「镜流.zip」就是這種：名字是 UTF-8，旗標卻沒設。
  void doesNotDoubleDecodeUtf8() {
    const std::string utf8 = "鏡流.model3.json";  // 合法 UTF-8
    QCOMPARE(decodeZipEntryName(utf8, false), utf8);
    QCOMPARE(decodeZipEntryName(utf8, true), utf8);
    // 旗標有設就一律相信它，連驗證都不做
    QCOMPARE(decodeZipEntryName(kBig5Name, true), kBig5Name);
    QCOMPARE(decodeZipEntryName("ascii.json", false), std::string("ascii.json"));
  }

  // 整條路走完：Big5 檔名的 zip，entries() 一定是合法 UTF-8，
  // 而且拿 entries() 的名字回頭 read() 一定讀得到（往返一致）
  void ansiNamedZipRoundTrips() {
    const fs::path path = root_ / "big5.zip";
    QVERIFY(writeZip(path,
                     {
                       {kBig5Name + ".model3.json", "{}"},
                       {kBig5Name + ".4096/texture_00.png", "PNG"},
                       {"ascii.motion3.json", "{}"},
                     },
                     /*markUtf8=*/false));
    const auto zip = openAt(path);
    QVERIFY(zip);
    QCOMPARE(zip->entries().size(), size_t(3));
    for (const auto& entry : zip->entries()) {
      QVERIFY2(strutil::isValidUtf8(entry), entry.c_str());
      QVERIFY2(zip->contains(entry), entry.c_str());
      QVERIFY2(zip->read(entry).has_value(), entry.c_str());
    }
    // 原始的 Big5 位元組不該再查得到 —— 索引已經是解碼後的名字
    QVERIFY(!zip->contains(kBig5Name + ".model3.json"));
  }

  // === 開啟與列舉 ===

  // 條目依名稱排序，目錄項與該濾掉的東西不出現在清單裡
  void listsNormalizedSortedEntries() {
    const fs::path path = root_ / "list.zip";
    QVERIFY(writeZip(path, {
                             {"sub/", ""},
                             {"b.txt", "B"},
                             {"./a.txt", "A"},
                             {"sub\\c.txt", "C"},
                             {"__MACOSX/junk", "x"},
                             {".hidden", "x"},
                           }));
    const auto zip = openAt(path);
    QVERIFY(zip);
    QCOMPARE(zip->entries(), (std::vector<std::string>{"a.txt", "b.txt", "sub/c.txt"}));
    // 沒被列出，但照樣讀得到（同資料夾模型）
    QVERIFY(zip->contains(".hidden"));
    QCOMPARE(zip->read(".hidden").value(), std::string("x"));
  }

  // 查找一律大小寫不敏感：model3.json 的引用常跟實際檔名大小寫對不上，
  // 在 NTFS 上看不出來，壓成 zip 就整組資源靜默讀不到
  void lookupIsCaseInsensitive() {
    const fs::path path = root_ / "case.zip";
    QVERIFY(writeZip(path, {{"Motions/Idle.motion3.json", "{}"}}));
    const auto zip = openAt(path);
    QVERIFY(zip);
    QVERIFY(zip->contains("motions/idle.motion3.json"));
    QVERIFY(zip->contains("MOTIONS/IDLE.MOTION3.JSON"));
    QCOMPARE(zip->read("motions/IDLE.motion3.json").value(), std::string("{}"));
  }

  // 查找端的路徑也會先正規化，所以 "./" 與反斜線寫法都通
  void lookupNormalizesQuery() {
    const fs::path path = root_ / "query.zip";
    QVERIFY(writeZip(path, {{"sub/a.txt", "A"}}));
    const auto zip = openAt(path);
    QVERIFY(zip);
    QVERIFY(zip->contains("./sub/a.txt"));
    QVERIFY(zip->contains("sub\\a.txt"));
    QVERIFY(!zip->contains("nope.txt"));
    QVERIFY(!zip->read("nope.txt").has_value());
  }

  // === 讀取 ===

  void readsWholeEntry() {
    const fs::path path = root_ / "read.zip";
    const std::string text = longText();
    QVERIFY(writeZip(path, {{"big.txt", text}, {"empty.txt", ""}}));
    const auto zip = openAt(path);
    QVERIFY(zip);
    QCOMPARE(zip->read("big.txt").value(), text);
    // 零位元組的條目是合法的，不是「讀失敗」
    QCOMPARE(zip->read("empty.txt").value(), std::string());
  }

  // readPrefix 只解出前 n 個位元組（貼圖尺寸探測靠這個維持在微秒級）
  void readPrefixStopsEarly() {
    const fs::path path = root_ / "prefix.zip";
    const std::string text = longText();
    QVERIFY(writeZip(path, {{"big.txt", text}}));
    const auto zip = openAt(path);
    QVERIFY(zip);
    const auto head = zip->readPrefix("big.txt", 10);
    QVERIFY(head.has_value());
    QCOMPARE(*head, text.substr(0, 10));
    // 要的比實際內容多時短讀，不是錯誤
    QCOMPARE(zip->readPrefix("big.txt", text.size() + 100).value(), text);
    QCOMPARE(zip->readPrefix("big.txt", 0).value(), std::string());
    QVERIFY(!zip->readPrefix("nope.txt", 10).has_value());
  }

  // === 不合格的輸入 ===

  // 開不起來一律回 nullptr 而不是丟例外或崩掉
  void rejectsBadInput() {
    QVERIFY(!ZipArchive::open(nullptr));
    QVERIFY(!openAt(root_ / "does-not-exist.zip"));

    const fs::path notZip = root_ / "not-a-zip.bin";
    {
      std::ofstream file(notZip, std::ios::binary);
      file << "這不是壓縮檔，只是一段夠長的純文字而已，用來確認不會被誤判。";
    }
    QVERIFY(!openAt(notZip));

    const fs::path empty = root_ / "empty.bin";
    {
      std::ofstream file(empty, std::ios::binary);
    }
    QVERIFY(!openAt(empty));
  }

  // 被截斷的 zip（中央目錄不見了）也只是開不起來
  void rejectsTruncatedZip() {
    const fs::path good = root_ / "truncate-src.zip";
    QVERIFY(writeZip(good, {{"a.txt", longText()}}));
    QVERIFY(openAt(good));

    const fs::path cut = root_ / "truncated.zip";
    {
      std::ifstream in(good, std::ios::binary);
      std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      std::ofstream out(cut, std::ios::binary);
      out.write(all.data(), static_cast<std::streamsize>(all.size() / 2));
    }
    QVERIFY(!openAt(cut));
  }
};

QTEST_APPLESS_MAIN(TestZipArchive)
#include "test_zip_archive.moc"
