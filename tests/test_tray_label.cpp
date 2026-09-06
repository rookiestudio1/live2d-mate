// 系統匣選單的標籤格式（formatNamedLabel）。
// 放在 core/tray_label 才連得進只依賴 l2m_core 的測試執行檔。
#include <QtTest>

#include <filesystem>

#include "core/tray_label.h"
#include "core/i18n.h"

using namespace l2m;
namespace fs = std::filesystem;

class TestTrayLabel : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() { i18n::setMessagesDir(fs::u8path(L2M_I18N_DIR)); }

  // 沒有命名時就是原始名稱
  void plainWhenUnnamed() { QCOMPARE(formatNamedLabel("zh-TW", "", "TapBody"), std::string("TapBody")); }

  // 有命名時意義在前、原始名稱在括號裡
  void meaningFirstRawInBrackets() { QCOMPARE(formatNamedLabel("zh-TW", "被戳身體", "TapBody"), std::string("被戳身體（TapBody）")); }

  // 後綴接在原始名稱後面，不會被意義拆散
  void suffixStaysWithRaw() {
    QCOMPARE(formatNamedLabel("zh-TW", "待機", "Idle", "，9 個"), std::string("待機（Idle，9 個）"));
    QCOMPARE(formatNamedLabel("zh-TW", "", "Idle", "，9 個"), std::string("Idle，9 個"));
  }

  // 模型允許空字串群組名，選單不能顯示成一片空白
  void emptyGroupNameHasPlaceholder() {
    QCOMPARE(formatNamedLabel("zh-TW", "", ""), std::string("(未命名群組)"));
    QCOMPARE(formatNamedLabel("zh-TW", "打招呼", "", "，1 個"), std::string("打招呼（(未命名群組)，1 個）"));
  }

  // 空字串的意義視為沒有命名
  void blankMeaningIsUnnamed() { QCOMPARE(formatNamedLabel("zh-TW", "", "F01"), std::string("F01")); }

  // 英文語系用半形括號，不會沿用中文的全形括號
  void englishUsesHalfWidthBrackets() {
    QCOMPARE(formatNamedLabel("en", "waving", "Idle", ", 9 motions"), std::string("waving (Idle, 9 motions)"));
    QCOMPARE(formatNamedLabel("en", "", ""), std::string("(unnamed group)"));
  }
};

QTEST_APPLESS_MAIN(TestTrayLabel)
#include "test_tray_label.moc"
