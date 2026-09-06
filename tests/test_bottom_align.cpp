// 由 alpha 遮罩量模型的視覺上下緣，以及切換模型時的腳底對齊計算（core/bottom_align.h）
#include <QtTest>

#include <vector>

#include "core/bottom_align.h"

using namespace l2m;

namespace {

// 與 AlphaHitMask 相同的門檻語意（> 12 才算不透明）
constexpr int kThreshold = 12;

// 全透明的 RGBA 緩衝；strideBytes 可大於 width*4，padding 塞 0xFF
// 驗證掃描不會越過一列的有效範圍去讀 padding
std::vector<uint8_t> makeBuffer(int width, int height, size_t strideBytes, uint8_t padding = 0xFF) {
  std::vector<uint8_t> buf(strideBytes * size_t(height), 0);
  for (int row = 0; row < height; ++row) {
    for (size_t b = size_t(width) * 4; b < strideBytes; ++b) {
      buf[size_t(row) * strideBytes + b] = padding;
    }
  }
  return buf;
}

void setAlpha(std::vector<uint8_t>& buf, size_t strideBytes, int row, int x, uint8_t alpha) { buf[size_t(row) * strideBytes + size_t(x) * 4 + 3] = alpha; }

}  // namespace

class TestBottomAlign : public QObject {
  Q_OBJECT

private slots:
  // 全透明緩衝找不到不透明列
  void fullyTransparentReturnsNullopt() {
    const auto buf = makeBuffer(8, 6, 8 * 4);
    QVERIFY(!firstOpaqueRow(buf.data(), 8, 6, 8 * 4, kThreshold).has_value());
  }

  // 只有 row 0 不透明（bottom-up：即視窗最底）
  void findsRowZero() {
    auto buf = makeBuffer(8, 6, 8 * 4);
    setAlpha(buf, 8 * 4, 0, 3, 255);
    QCOMPARE(firstOpaqueRow(buf.data(), 8, 6, 8 * 4, kThreshold), std::optional<int>(0));
  }

  // 只有最後一列（height-1）不透明
  void findsLastRow() {
    auto buf = makeBuffer(8, 6, 8 * 4);
    setAlpha(buf, 8 * 4, 5, 3, 255);
    QCOMPARE(firstOpaqueRow(buf.data(), 8, 6, 8 * 4, kThreshold), std::optional<int>(5));
  }

  // 多列不透明時回最小列索引（視覺最低）
  void returnsLowestRow() {
    auto buf = makeBuffer(8, 6, 8 * 4);
    setAlpha(buf, 8 * 4, 4, 1, 255);
    setAlpha(buf, 8 * 4, 2, 6, 255);
    QCOMPARE(firstOpaqueRow(buf.data(), 8, 6, 8 * 4, kThreshold), std::optional<int>(2));
  }

  // 門檻邊界：alpha == threshold 不算，threshold+1 才算（釘住 > 語意，同 isOpaque）
  void thresholdIsExclusive() {
    auto buf = makeBuffer(8, 6, 8 * 4);
    setAlpha(buf, 8 * 4, 1, 0, kThreshold);
    QVERIFY(!firstOpaqueRow(buf.data(), 8, 6, 8 * 4, kThreshold).has_value());
    setAlpha(buf, 8 * 4, 1, 0, kThreshold + 1);
    QCOMPARE(firstOpaqueRow(buf.data(), 8, 6, 8 * 4, kThreshold), std::optional<int>(1));
  }

  // stride > width*4：padding 全塞 0xFF，掃描不可以把 padding 當像素
  void ignoresRowPadding() {
    const size_t stride = 8 * 4 + 12;
    auto buf = makeBuffer(8, 6, stride);
    QVERIFY(!firstOpaqueRow(buf.data(), 8, 6, stride, kThreshold).has_value());
    setAlpha(buf, stride, 3, 7, 255);
    QCOMPARE(firstOpaqueRow(buf.data(), 8, 6, stride, kThreshold), std::optional<int>(3));
  }

  // 單一不透明像素在第 0 欄與最後一欄都抓得到
  void findsEdgeColumns() {
    auto buf = makeBuffer(8, 6, 8 * 4);
    setAlpha(buf, 8 * 4, 2, 0, 255);
    QCOMPARE(firstOpaqueRow(buf.data(), 8, 6, 8 * 4, kThreshold), std::optional<int>(2));
    auto buf2 = makeBuffer(8, 6, 8 * 4);
    setAlpha(buf2, 8 * 4, 2, 7, 255);
    QCOMPARE(firstOpaqueRow(buf2.data(), 8, 6, 8 * 4, kThreshold), std::optional<int>(2));
  }

  // 頂端掃描：全透明一樣回 nullopt
  void topFullyTransparentReturnsNullopt() {
    const auto buf = makeBuffer(8, 6, 8 * 4);
    QVERIFY(!lastOpaqueRow(buf.data(), 8, 6, 8 * 4, kThreshold).has_value());
  }

  // 多列不透明時回**最大**列索引（bottom-up 緩衝裡那是視覺最高點），
  // 與 firstOpaqueRow 恰好是同一份資料的另一端
  void returnsHighestRow() {
    auto buf = makeBuffer(8, 6, 8 * 4);
    setAlpha(buf, 8 * 4, 4, 1, 255);
    setAlpha(buf, 8 * 4, 2, 6, 255);
    QCOMPARE(lastOpaqueRow(buf.data(), 8, 6, 8 * 4, kThreshold), std::optional<int>(4));
    QCOMPARE(firstOpaqueRow(buf.data(), 8, 6, 8 * 4, kThreshold), std::optional<int>(2));
  }

  // 門檻與 padding 的規則兩個方向一模一樣
  void topSharesThresholdAndPaddingRules() {
    const size_t stride = 8 * 4 + 12;
    auto buf = makeBuffer(8, 6, stride);
    QVERIFY(!lastOpaqueRow(buf.data(), 8, 6, stride, kThreshold).has_value());
    setAlpha(buf, stride, 3, 7, kThreshold);
    QVERIFY(!lastOpaqueRow(buf.data(), 8, 6, stride, kThreshold).has_value());
    setAlpha(buf, stride, 3, 7, kThreshold + 1);
    QCOMPARE(lastOpaqueRow(buf.data(), 8, 6, stride, kThreshold), std::optional<int>(3));
  }

  // 最後一列的上緣就是視窗最頂（0.0）；row 0 的上緣離頂端一格高
  void rowToNormalizedTop() {
    QCOMPARE(bottomUpRowToNormalizedTop(287, 288), 0.0);
    QCOMPARE(bottomUpRowToNormalizedTop(0, 288), 287.0 / 288);
  }

  // 上下緣是同一格的兩條邊：只有一列不透明時，兩者相差正好一格
  void topAndBottomOfOneRowDifferByOneCell() {
    auto buf = makeBuffer(8, 288, 8 * 4);
    setAlpha(buf, 8 * 4, 100, 3, 255);
    const auto low = firstOpaqueRow(buf.data(), 8, 288, 8 * 4, kThreshold);
    const auto high = lastOpaqueRow(buf.data(), 8, 288, 8 * 4, kThreshold);
    QCOMPARE(low, high);
    QCOMPARE(bottomUpRowToNormalizedBottom(*low, 288) - bottomUpRowToNormalizedTop(*high, 288), 1.0 / 288);
  }

  // row 0 的下緣是視窗最底（1.0）；最頂列的下緣是一格高
  void rowToNormalized() {
    QCOMPARE(bottomUpRowToNormalizedBottom(0, 288), 1.0);
    QCOMPARE(bottomUpRowToNormalizedBottom(287, 288), 1.0 / 288);
  }

  // lround：四捨五入而不是截斷
  void offsetRounds() {
    // 0.5 / 288 * 600 = 1.0417 → 1；287.6 格 → 599.17 → 599
    QCOMPARE(bottomOffsetPx(0.5 / 288, 600), 1);
    QCOMPARE(bottomOffsetPx(287.6 / 288, 600), 599);
    QCOMPARE(bottomOffsetPx(1.0, 600), 600);
  }

  // 冪等：新舊模型底部相同（同一份量測）時視窗 y 不變
  void identicalBottomsKeepWindowY() {
    const double bottom = bottomUpRowToNormalizedBottom(40, 288);
    const int windowY = 123;
    const int windowH = 600;
    const int targetBottomScreenY = windowY + bottomOffsetPx(bottom, windowH);
    QCOMPARE(windowYForBottom(targetBottomScreenY, bottomOffsetPx(bottom, windowH)), windowY);
  }

  // 新模型視窗內留白較多（底部偏高）→ 視窗要往下移補齊
  void movesWindowDownForShorterModel() {
    const int windowY = 100;
    const int windowH = 600;
    const int oldOffset = 590;  // 舊模型幾乎貼視窗底
    const int newOffset = 550;  // 新模型底部高了 40px
    const int target = windowY + oldOffset;
    QCOMPARE(windowYForBottom(target, newOffset), windowY + 40);
  }
};

QTEST_APPLESS_MAIN(TestBottomAlign)
#include "test_bottom_align.moc"
