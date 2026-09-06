#include "persona_default.h"

#include <QFile>
#include <QString>

#include "i18n.h"

// static library 的資源初始化陷阱：qrc 編進靜態程式庫時，註冊資源的是一個
// static initializer；連結器只拉進「有解決到未定義符號」的 object file，
// 那個 TU 可能被整個丟掉，症狀是 QFile(":/personas/...") 在執行期一律
// exists() == false。這裡手動喚醒一次。Q_INIT_RESOURCE 在命名空間內展開會
// 找不到符號，所以包在全域命名空間的小函式裡。
// 真正的安全網是 tests/test_persona_default.cpp：五個語系都要讀得到非空內容。
static void l2mInitPersonasResource() { Q_INIT_RESOURCE(personas); }

namespace l2m {

std::string defaultPersonaMarkdown(const std::string& locale) {
  l2mInitPersonasResource();
  // locale 對應直接重用 i18n::matchLocale（任意字串 → 五個支援語系之一，
  // 對不到退回 en），不另寫一套比對規則
  const std::string matched = i18n::matchLocale(locale);
  QFile file(QStringLiteral(":/personas/Default.%1.md").arg(QString::fromStdString(matched)));
  if (!file.open(QIODevice::ReadOnly)) return {};
  return file.readAll().toStdString();
}

}  // namespace l2m
