// 貼圖格式的診斷：副檔名比對與歸咎範圍的邊界。
// 這支的輸出就是使用者與 AI 看到的那句話（「webp 這個建置解不動」），比錯一格就是
// 給出誤導的建議 —— 叫人去修一個沒壞的檔案，或反過來叫人去裝一個用不上的外掛。
// l2m_live2d 連不進測試，只有純函式驗得到。
#include <QtTest>

#include "core/texture_format.h"

using namespace l2m;

namespace {

// 沒裝 Qt Image Formats 模組時的實測清單（少了 webp/tga/tiff 那一組）
const std::vector<std::string> kStockQt = {"bmp", "gif", "ico", "jpeg", "jpg", "png", "svg"};

bool contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

}  // namespace

class TestTextureFormat : public QObject {
  Q_OBJECT

private slots:
  // 實際踩到的那一隻（adaerbote_3）：兩張 webp，Qt 沒裝 Image Formats 模組
  void namesTheUnsupportedFormat() {
    const auto issue = inspectTextureFailure({"textures/texture_00.webp", "textures/texture_01.webp"}, kStockQt, 2);
    QCOMPARE(issue.unsupported.size(), size_t(1));
    QCOMPARE(issue.unsupported[0], std::string("webp"));
    QVERIFY(contains(issue.error, "All 2 textures failed to load"));
    QVERIFY(contains(issue.error, "\"webp\""));
    // 兩條使用者做得到的路都要講：轉檔不必動程式，裝外掛不必動模型
    QVERIFY(contains(issue.hint, "PNG"));
    QVERIFY(contains(issue.hint, "Qt Image Formats"));
    // 建議裡要列出「那這台到底解得動什麼」，不然使用者只知道不能用 webp
    QVERIFY(contains(issue.hint, "png"));
  }

  // 副檔名比對不分大小寫：Windows 上 .WEBP 與 .webp 是同一個檔案
  void matchesExtensionCaseInsensitively() {
    const auto issue = inspectTextureFailure({"textures/TEXTURE_00.WEBP"}, kStockQt, 1);
    QCOMPARE(issue.unsupported.size(), size_t(1));
    QCOMPARE(issue.unsupported[0], std::string("webp"));
    // 只有一張時不說「All 1 texture」
    QCOMPARE(issue.error, std::string("The texture failed to load: the image format \"webp\" is not supported by this build."));
  }

  // 支援清單本身也不分大小寫（Qt 回的是小寫，但注入端不該被綁死）
  void acceptsSupportedListInAnyCase() {
    const auto issue = inspectTextureFailure({"textures/a.PNG"}, {"PNG", "JPG"}, 1);
    QVERIFY(issue.unsupported.empty());
  }

  // 同一種格式只講一次：三張 webp 說三遍只是把建議洗掉
  void dedupesRepeatedFormats() {
    const auto issue = inspectTextureFailure({"a.webp", "b.webp", "c.webp"}, kStockQt, 3);
    QCOMPARE(issue.unsupported.size(), size_t(1));
    QCOMPARE(issue.unsupported[0], std::string("webp"));
  }

  // 多種格式時用複數句型，順序照 model3.json 的出現順序
  void listsSeveralFormatsInOrder() {
    const auto issue = inspectTextureFailure({"a.tga", "b.webp", "c.tga"}, kStockQt, 3);
    QCOMPARE(issue.unsupported.size(), size_t(2));
    QCOMPARE(issue.unsupported[0], std::string("tga"));
    QCOMPARE(issue.unsupported[1], std::string("webp"));
    QVERIFY(contains(issue.error, "formats \"tga\", \"webp\" are not supported"));
  }

  // 格式明明認得卻還是載不起來：那是檔案的問題，不能叫人去裝外掛
  void fallsBackToCorruptFileWhenFormatIsKnown() {
    const auto issue = inspectTextureFailure({"textures/texture_00.png"}, kStockQt, 1);
    QVERIFY(issue.unsupported.empty());
    QCOMPARE(issue.error, std::string("The texture failed to load."));
    QVERIFY(contains(issue.hint, "truncated, corrupt, or too large"));
    QVERIFY(!contains(issue.hint, "Qt Image Formats"));
  }

  // **只歸咎真的失敗的那幾張**。模型有一張副檔名寫成 .tga 但其實是 PNG 的圖
  //（QImage 靠內容嗅探照樣解得出來）＋一張截斷的 png：只有後者進得來，
  // 訊息就不該提 tga，更不該叫人去裝一個用不到的外掛。
  void blamesOnlyTheFilesThatActuallyFailed() {
    const auto issue = inspectTextureFailure({"textures/broken.png"}, kStockQt, 2);
    QVERIFY(issue.unsupported.empty());
    QCOMPARE(issue.error, std::string("1 of 2 textures failed to load."));
    QVERIFY(contains(issue.hint, "truncated, corrupt, or too large"));
  }

  // 沒有副檔名、點落在目錄名上（正／反斜線都算）、或結尾就是一個點：
  // 拿目錄名去比對支援清單只會胡說八道
  void ignoresNamesWithoutRealExtension() {
    const auto issue = inspectTextureFailure({"textures/atlas", "tex.d/atlas", "tex.d\\atlas", "atlas."}, kStockQt, 4);
    QVERIFY(issue.unsupported.empty());
    QVERIFY(contains(issue.hint, "truncated, corrupt, or too large"));
  }

  // 檔名裡有好幾個點時取最後一段；反斜線目錄也要認得出真正的副檔名
  void findsExtensionAfterSeveralDotsAndBackslashes() {
    QCOMPARE(inspectTextureFailure({"textures/tex.2048.webp"}, kStockQt, 1).unsupported, std::vector<std::string>{"webp"});
    QCOMPARE(inspectTextureFailure({"textures\\tex.webp"}, kStockQt, 1).unsupported, std::vector<std::string>{"webp"});
  }

  // 部分失敗：模型還是畫得出來一半，句子要說清楚是幾張中的幾張
  void reportsPartialFailure() {
    const auto issue = inspectTextureFailure({"b.webp"}, kStockQt, 3);
    QVERIFY(contains(issue.error, "1 of 3 textures failed to load"));
    QVERIFY(contains(issue.error, "\"webp\""));
  }

  // 沒有失敗就必須完全不出聲：空的 error 是 ModelController::load() 用來判定
  //「有貼圖而且出過事」的依據，這裡給一句「0 張失敗」會讓模型載入被誤判成失敗。
  void staysSilentWhenNothingFailed() {
    QVERIFY(inspectTextureFailure({}, kStockQt, 2).error.empty());
    QVERIFY(inspectTextureFailure({"a.webp"}, kStockQt, 0).error.empty());
  }
};

QTEST_GUILESS_MAIN(TestTextureFormat)
#include "test_texture_format.moc"
