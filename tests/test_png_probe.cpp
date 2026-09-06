// PNG 檔頭的判定與解析。判錯的兩個方向都是靜默失敗（見 core/png_probe.h），
// 所以這裡把「哪些該認、哪些不該認」逐條釘住。
#include <QtTest>

#include <vector>

#include "core/png_probe.h"

using namespace l2m;

namespace {

// 組一份合法的 PNG 前 29 個位元組：簽章 + IHDR 長度 + "IHDR" + 13 個欄位。
// 只到 IHDR 為止 —— readPngHeader 本來就不該需要看更後面的東西。
std::vector<unsigned char> makeHeader(uint32_t width, uint32_t height, unsigned char bitDepth, unsigned char colorType, unsigned char interlace = 0) {
  std::vector<unsigned char> bytes = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  const auto pushBe32 = [&bytes](uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value >> 24));
    bytes.push_back(static_cast<unsigned char>(value >> 16));
    bytes.push_back(static_cast<unsigned char>(value >> 8));
    bytes.push_back(static_cast<unsigned char>(value));
  };
  pushBe32(13);
  bytes.insert(bytes.end(), {'I', 'H', 'D', 'R'});
  pushBe32(width);
  pushBe32(height);
  bytes.push_back(bitDepth);
  bytes.push_back(colorType);
  bytes.push_back(0);  // compression
  bytes.push_back(0);  // filter
  bytes.push_back(interlace);
  return bytes;
}

}  // namespace

class TestPngProbe : public QObject {
  Q_OBJECT

private slots:
  // 一般的 8-bit RGBA 貼圖：本專案的貼圖九成九長這樣
  void readsPlainRgbaHeader() {
    const auto bytes = makeHeader(8192, 16384, 8, 6);
    const auto header = readPngHeader(bytes.data(), bytes.size());
    QVERIFY(header.has_value());
    QCOMPARE(header->width, 8192u);
    QCOMPARE(header->height, 16384u);
    QCOMPARE(header->bitDepth, uint8_t(8));
    QCOMPARE(header->colorType, uint8_t(6));
    QCOMPARE(header->interlace, uint8_t(0));
  }

  // 寬高是大端序的。位移或位元組序寫錯的實作會在這裡把 258 讀成 33488896 之類的值
  void parsesBigEndianDimensions() {
    const auto bytes = makeHeader(258, 1, 8, 6);
    const auto header = readPngHeader(bytes.data(), bytes.size());
    QVERIFY(header.has_value());
    QCOMPARE(header->width, 258u);
    QCOMPARE(header->height, 1u);
  }

  // 簽章不對就不是 PNG。JPEG 的開頭餵進來只能回 nullopt，
  // 不然就會把 JPEG 交給 spng 白跑一趟
  void rejectsNonPng() {
    const std::vector<unsigned char> jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1};
    QVERIFY(!isPngSignature(jpeg.data(), jpeg.size()));
    QVERIFY(!readPngHeader(jpeg.data(), jpeg.size()).has_value());
  }

  // 簽章對、但後面接的不是 IHDR。這種檔案交給 spng 只會在配置完記憶體之後才失敗
  void rejectsSignatureWithoutIhdr() {
    auto bytes = makeHeader(16, 16, 8, 6);
    bytes[13] = 'D';  // "IHDR" → "IDDR"
    QVERIFY(isPngSignature(bytes.data(), bytes.size()));
    QVERIFY(!readPngHeader(bytes.data(), bytes.size()).has_value());
  }

  // 型別是 IHDR、但宣告的長度不是規格寫死的 13。長度檢查與型別檢查是**兩件事**，
  // 只驗型別的話這個案例會過，而那正是「簽章對、後面是別的東西」的另一半
  void rejectsIhdrWithWrongLength() {
    auto bytes = makeHeader(16, 16, 8, 6);
    bytes[11] = 12;  // 長度欄的最低位元組：13 → 12
    QVERIFY(!readPngHeader(bytes.data(), bytes.size()).has_value());
    bytes[11] = 13;
    QVERIFY(readPngHeader(bytes.data(), bytes.size()).has_value());
  }

  // 規格 §11.2.2 的寬高上界是 2^31-1。超過的話呼叫端 static_cast<int> 會變成負數，
  // 接著被 std::max(1, ...) 夾成 1 而讓「解碼後多大」整個算錯
  void rejectsDimensionsAboveSpecLimit() {
    const auto tooWide = makeHeader(2147483648u, 16, 8, 6);
    const auto tooTall = makeHeader(16, 4294967295u, 8, 6);
    const auto atLimit = makeHeader(2147483647u, 2147483647u, 8, 6);
    QVERIFY(!readPngHeader(tooWide.data(), tooWide.size()).has_value());
    QVERIFY(!readPngHeader(tooTall.data(), tooTall.size()).has_value());
    QVERIFY(readPngHeader(atLimit.data(), atLimit.size()).has_value());
  }

  // 只讀到一半（zip 的 readPrefix 給的位元組不夠、或檔案截斷）一律回 nullopt，
  // 絕不能越界讀
  void rejectsTruncatedData() {
    const auto bytes = makeHeader(16, 16, 8, 6);
    for (size_t n = 0; n < bytes.size(); ++n) QVERIFY(!readPngHeader(bytes.data(), n).has_value());
    QVERIFY(readPngHeader(bytes.data(), bytes.size()).has_value());
    QVERIFY(!readPngHeader(nullptr, 64).has_value());
  }

  // 0 寬或 0 高不是合法的 PNG，而且它會一路變成 0 位元組的配置
  void rejectsZeroDimensions() {
    const auto zeroWidth = makeHeader(0, 16, 8, 6);
    const auto zeroHeight = makeHeader(16, 0, 8, 6);
    QVERIFY(!readPngHeader(zeroWidth.data(), zeroWidth.size()).has_value());
    QVERIFY(!readPngHeader(zeroHeight.data(), zeroHeight.size()).has_value());
  }

  // 位元深度與色彩型別的合法組合（規格表 11.2.2）：調色盤最多 8 位元、
  // 帶 alpha 的兩種至少 8 位元。放行不合法的組合等於把失敗延到配置之後
  void rejectsIllegalBitDepthAndColorTypeCombos() {
    const auto palette16 = makeHeader(16, 16, 16, 3);
    const auto rgba4 = makeHeader(16, 16, 4, 6);
    const auto grayAlpha4 = makeHeader(16, 16, 4, 4);
    const auto rgb1 = makeHeader(16, 16, 1, 2);
    const auto colorType5 = makeHeader(16, 16, 8, 5);
    const auto bitDepth3 = makeHeader(16, 16, 3, 0);
    QVERIFY(!readPngHeader(palette16.data(), palette16.size()).has_value());
    QVERIFY(!readPngHeader(rgba4.data(), rgba4.size()).has_value());
    QVERIFY(!readPngHeader(grayAlpha4.data(), grayAlpha4.size()).has_value());
    QVERIFY(!readPngHeader(rgb1.data(), rgb1.size()).has_value());
    QVERIFY(!readPngHeader(colorType5.data(), colorType5.size()).has_value());
    QVERIFY(!readPngHeader(bitDepth3.data(), bitDepth3.size()).has_value());
    // 合法的另一端：RGB 配 16 位元
    const auto rgb16 = makeHeader(16, 16, 16, 2);
    QVERIFY(readPngHeader(rgb16.data(), rgb16.size()).has_value());
  }

  // 合法但少見的幾種都要認得：1-bit 灰階、調色盤、Adam7 交錯。
  // 這些 spng 都解得動，誤判成「不是 PNG」只會安靜地退回慢路徑
  void acceptsUncommonButLegalVariants() {
    const auto gray1 = makeHeader(16, 16, 1, 0);
    const auto palette8 = makeHeader(16, 16, 8, 3);
    const auto interlaced = makeHeader(16, 16, 8, 6, 1);
    const auto gray16Alpha = makeHeader(16, 16, 16, 4);
    QVERIFY(readPngHeader(gray1.data(), gray1.size()).has_value());
    QVERIFY(readPngHeader(palette8.data(), palette8.size()).has_value());
    QVERIFY(readPngHeader(interlaced.data(), interlaced.size()).has_value());
    QVERIFY(readPngHeader(gray16Alpha.data(), gray16Alpha.size()).has_value());
  }

  // interlace 只有 0 與 1 兩個合法值
  void rejectsUnknownInterlaceMethod() {
    const auto bytes = makeHeader(16, 16, 8, 6, 2);
    QVERIFY(!readPngHeader(bytes.data(), bytes.size()).has_value());
  }

  // 解碼後大小：8192×16384×4 已經超過 32 位元，用 int 算會變成負數或小數字，
  // 而小數字會讓上限檢查放行、真正炸掉的地方在後面的配置
  void computesDecodedSizeWithoutOverflow() {
    PngHeader header;
    header.width = 8192;
    header.height = 16384;
    header.bitDepth = 8;
    header.colorType = 6;
    QCOMPARE(pngRgba8Bytes(header), 536870912ull);  // 512 MiB

    // PNG 規格的尺寸上限是 2^31-1，兩邊都拉滿時 w×h×4 是 1.84e19 —— 差一點點
    // 但**還在 uint64 之內**，所以這個邊界要算得出精確值而不是被守衛擋掉
    header.width = 2147483647;
    header.height = 2147483647;
    QCOMPARE(pngRgba8Bytes(header), 18446744056529682436ull);

    // 真正會溢位的是欄位本身的上限（uint32 塞得下、乘起來塞不下）。
    // 繞回去之後那個小數字會讓上限檢查一路放行，炸掉的地方在後面的配置
    header.width = 4294967295;
    header.height = 4294967295;
    QCOMPARE(pngRgba8Bytes(header), 0ull);

    header.width = 0;
    header.height = 16;
    QCOMPARE(pngRgba8Bytes(header), 0ull);
  }
};

QTEST_GUILESS_MAIN(TestPngProbe)
#include "test_png_probe.moc"
