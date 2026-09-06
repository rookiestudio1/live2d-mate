#pragma once

// 設定視窗的「模型」分頁。
//
// 左半是模型清單與一條按鈕列（載入模型／重新掃描／開啟模型資料夾），
// 取代原本系統匣的「模型」子選單；右半是「模型內建動作與表情命名」，
// 整份移植自原本的 windows/naming_window.*。
//
// 為什麼命名要跟模型清單放在一起：命名是綁在模型上的（寫進模型資料夾裡的
// <Model>.annotations.json），換模型就整份換掉。分開成兩個地方的話，
// 使用者得先在一處換模型、再到另一處命名。
//
// 兩個必須保留的行為（原本就踩過坑）：
//  1. **切換模型是按鈕，不是選取清單就切**。切換會先淡出 400 ms，接著
//     AppController::switchModel 底下的載入同步把 GUI 執行緒卡住數秒；
//     掛在 currentRowChanged 上的話，鍵盤上下鍵一路按過去就是連續好幾次凍結。
//     switchModel() 本身現在會立刻回傳（淡出還沒開始），所以「載入中」與
//     WaitCursor 一路撐到 AppController::modelSwitchFinished 才收，
//     不能接它的回傳值。
//  2. **命名區只在模型真的換了才重建**。存檔本身也會發 modelsChanged，
//     無條件重建會把使用者正在打字的輸入框連同焦點一起砍掉。
//
// 對應的表單是 model_page.ui。命名列全部由程式碼產生（列數跟著模型走）。
// 右半最上面的 currentModelName 是**目前載入中的模型名稱**，字級比
//「模型內建動作與表情命名」大一級：命名是寫進該模型資料夾的 annotations.json，
// 換個模型就是完全不同的一份，使用者得一眼看出自己正在命名哪一個。

#include <QWidget>

#include <functional>
#include <memory>
#include <string>

#include "core/command_result.h"
#include "core/model_types.h"
#include "settings_context.h"

class QVBoxLayout;

namespace Ui {
class ModelPage;
}

namespace l2m {

class ModelPage : public QWidget {
  Q_OBJECT

public:
  explicit ModelPage(const SettingsContext& context, QWidget* parent = nullptr);
  ~ModelPage() override;

  void retranslate();
  void refresh();
  void refreshIfActive();

private:
  QString tr2(const char* key) const;

  void rebuildModelList();
  // 收掉「載入中」與 WaitCursor。接 modelSwitchFinished，也用在
  // switchModel() 當場失敗（找不到模型、載入根本沒開始）的路徑上
  void finishLoading();
  // 命名區上方那行模型名稱。單純 setText，不會動到正在打字的輸入框，
  // 所以 refresh() 的「模型沒換就不重建」提前返回之前就能先更新它
  void updateCurrentModelName();
  // 命名區整份重建：內容跟著模型走，局部更新只會讓「哪一列對應哪個 key」
  // 慢慢對不上
  void rebuildNaming();
  // 命名區最下面那一區（畫面上叫「自定義動作與表情」）：core/builtin_actions.h
  // 依這隻模型的參數合成出來的通用動作與表情。
  //
  // **這一區沒有意義輸入框**：項目與名稱都是內定的，而那組名稱（wave／nod／sigh…）
  // 本身就是送到 AI 面前的語意，再讓使用者寫一次說明只會多出第二份真相 ——
  // 兩邊不一致時 AI 兩份都看得到，反而更難挑。所以這裡走 buildPresetRow，
  // 只有名稱與「試看」。
  //
  // **標題用 i18n 的本地化名稱（揮手／點頭…）而不是內部 id（wave／nod）**：
  // 那組英文 id 是 MCP 的鍵，不能跟著語系跑，但畫面上要讀得懂。
  // id 與實際對應到的參數放進 tooltip —— 槽位解析有靠 cdi3 名稱猜的成分，
  // 猜錯時仍然要查得到是哪幾個參數在動，但攤在副標上一列太長。
  // 一個都撐不起來時整區不畫。
  void buildBuiltinSection(QWidget* content, const ModelInfo& model);
  // 建一列：標題 ＋ 意義輸入框 ＋「試看」按鈕（模型自帶的動作與表情用）
  QWidget* buildRow(QWidget* parent, const QString& title, const QString& subtitle, const QString& placeholder, const std::string& meaning, std::function<void(const std::string&)> onSave,
                    std::function<void()> onPreview);
  // 建一列：只有標題與「試看」，沒有輸入框（自定義動作與表情用）
  QWidget* buildPresetRow(QWidget* parent, const QString& title, std::function<void()> onPreview);
  // 存檔結果：成功顯示「已儲存」，失敗把 error 與 hint 顯示出來
  //（模型放在唯讀位置時，使用者得知道自己白打了）
  void applySave(const CommandResult& result);

  void loadSelected();
  void setBusy(bool busy);
  std::string selectedModelId() const;

  SettingsContext ctx_;
  std::unique_ptr<Ui::ModelPage> ui_;
  QVBoxLayout* namingLayout_ = nullptr;
  // 命名區目前顯示的是哪個模型；只有它變了才重建
  std::string shownModelId_;
  bool updating_ = false;
  bool busy_ = false;
};

}  // namespace l2m
