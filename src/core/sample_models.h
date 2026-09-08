#pragma once

// 一隻模型都沒有時，去哪裡拿免費模型。
//
// 這個 app 不附任何模型 —— Cubism 模型各有各的授權，而 repo 是 MIT。
// 所以全新安裝的第一次啟動，models 目錄是空的、舞台上什麼都沒有，
// 使用者看到的是一個「開起來像壞掉」的桌寵。在這之前唯一的線索是
// `AppController::start()` 那行 qWarning，等於沒有。
//
// 官方的免費模型集正是為這件事存在的：Live2D 自己放了一批樣本模型免費下載，
// 拿來試玩剛剛好。所以第一次遇到空目錄時問一句「要不要去官網抓一隻」，
// 是這個狀態下唯一有用的回應。
//
// **刻意不自動下載**，兩個理由：
//   ① 那個頁面在下載前要求先看過「無償提供マテリアル使用許諾契約書」與
//      「Live2D Cubism サンプルデータ利用規約」（2026-09 實測頁面上兩份都列著），
//      繞過它直接抓 zip 等於替使用者按下同意。
//   ② 各模型的 zip 網址沒有任何穩定性保證，寫死在程式裡遲早爛掉，
//      而症狀會是「按了下載完全沒反應」—— 最難查的那一種。
// 開瀏覽器到官方頁面則永遠不會過期，授權流程也留在該在的地方。
//
// 下載回來的 zip **不必解壓縮**：掃描器本來就吃 zip（core/model_assets.h），
// 整包丟進 models 目錄即可。所以對話框會把那個目錄一起打開 ——
// 它在 %APPDATA% 底下，叫使用者自己找比什麼都不做還煩。
//
// 語系對應是逐一實測出來的，不是照 app 自己那五個語系直接套前綴：
//   ja    → **沒有語系前綴的根路徑**（/learn/sample/，標題
//           「Live2D サンプルデータ集（無料配布）」）
//   ko    → /ko/learn/sample/
//   zh-CN → /zh-CHS/learn/sample/
//   zh-TW → **官網沒有 zh-CHT 這個版本**（/zh-CHT/learn/sample/ 實測回 404），
//           所以跟其餘語系一起退回 /en/。寧可給看得懂的外語，也不要給 404。
// 換句話說，這張表**不能**用「locale 直接當路徑前綴」的通則生成 ——
// 五個語系裡有三個是例外。

#include <string>

namespace l2m {

// 該語系對應的官方免費模型頁面。對不到的語系一律回英文版。
std::string sampleModelsUrl(const std::string& uiLocale);

// 要不要跳出「去抓一隻模型吧」的引導。三個條件缺一不可：
//   ① 真的一隻模型都沒有；
//   ② 還沒問過（config 的 app.sampleModelsPrompted）—— 只記「問過」不記答案，
//      每次啟動都彈一個對話框比沒有模型更煩人；
//   ③ 不是 --hidden 啟動 —— 那次啟動使用者要的是安靜地縮在系統匣，
//      對著一個看不見的角色彈 modal 完全違反那個意圖。這種情況**不記旗標**，
//      留到下次正常啟動再問。
//      （開機自動啟動**已經不走這條**了，見 core/autostart_command.h：自啟就是
//       一次普通的啟動，角色會顯示，所以沒有模型時本來就該當場問。）
bool shouldOfferSampleModels(bool anyModelInstalled, bool alreadyPrompted, bool startedHidden);

}  // namespace l2m
