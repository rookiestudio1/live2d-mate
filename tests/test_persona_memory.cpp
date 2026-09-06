// 長期記憶的注入前夾取：保留開頭、截斷落在 UTF-8 字元邊界。
#include <QtTest>

#include <string>

#include "core/persona_memory.h"
#include "core/string_util.h"

using namespace l2m;

class TestPersonaMemory : public QObject {
  Q_OBJECT

private slots:
  void shortMemoryPassesThrough() {
    QCOMPARE(clampPersonaMemory("  boss stays up late  \n"), std::string("boss stays up late"));
    QCOMPARE(clampPersonaMemory(""), std::string());
  }

  // 超限時保留開頭、丟掉尾巴（檔案開頭放最重要的事是唯一要教使用者的規則）
  void clampsKeepingHead() {
    const std::string head(kPersonaMemoryMaxChars, 'a');
    const std::string clamped = clampPersonaMemory(head + "TAIL");
    QCOMPARE(clamped, head);
  }

  // 截斷落在字元邊界：中文一字 3 位元組，夾完不該出現半個字的亂碼
  void clampsAtUtf8Boundary() {
    std::string text;
    for (int i = 0; i < kPersonaMemoryMaxChars + 10; ++i) text += "貓";
    const std::string clamped = clampPersonaMemory(text);
    QCOMPARE(strutil::utf8Length(clamped), static_cast<size_t>(kPersonaMemoryMaxChars));
    QVERIFY(strutil::isValidUtf8(clamped));
  }
};

QTEST_GUILESS_MAIN(TestPersonaMemory)
#include "test_persona_memory.moc"
