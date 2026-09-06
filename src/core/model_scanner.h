#pragma once

// 模型目錄掃描。
//
// 兩個刻意的決定：
//
//  1. 本專案不支援 Cubism 2.1，*.model.json 與內容判定為 Cubism 2 的裸命名入口
//     （model.json / index.json）一律略過，不再出現在掃描結果裡。
//  2. 另外收 *.zip：掃到壓縮檔就打開來看看裡面是不是模型包，是的話當成一般模型
//     列出（磁碟上不解壓縮，讀取走 core/model_assets.h）。**一個 zip = 一個模型**，
//     id 就是那個 zip 的相對路徑（"Foo.zip"）、名稱是檔名去掉副檔名。
//     不是模型包的 zip 靜靜略過 —— models 目錄裡本來就可能有不相干的壓縮檔。

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "model_types.h"

namespace l2m {

// 掃描結果的預設 URL 前綴；host 固定為 models，之後接相對路徑
inline constexpr const char* kModelUrlBase = "live2d://models";

// 裸命名入口檔（model.json / index.json）的判定；
// 這種檔沒有前綴名稱，得另外讀內容判斷是不是 Cubism 4。
bool isGenericEntryName(const std::string& filename);

// 「stem 空」的資產檔名 —— 整個檔名就是副檔名（".model3.json"、".motion3.json"…）。
//
// 列舉檔案時**開頭是點的一律略過**（隱藏目錄，以及 macOS 打包留下的
// ._Foo.model3.json —— 那是 AppleDouble 中繼資料而不是模型，撿到它就是
// 「拖進來卻說解析失敗」）。但有一種模型會被這條規則整隻誤傷：匯出時 stem 留空的
// 模型，一整組檔案就叫 .model3.json / .moc3 / .cdi3.json / .physics3.json
// （MementoMori 的 Characters/CHR_*/model 全是這個形狀）。三條路一起中招，
// 而且**全部是靜默失敗**：拖那個資料夾進 Viewer 完全沒反應（findDirectoryEntry
// 回 nullopt 就等於「這不是模型資料夾」，連錯誤對話框都不會出現）、放進 models
// 目錄掃不到、壓成 zip 連入口檔都找不到（「略過不是模型包的 zip」）。
//
// 兩者分得開的地方只有一個：**整個檔名就是副檔名**（"._Foo.model3.json" 與
// ".secret.motion3.json" 的 stem 都不是空的）。所以規則是「開頭是點就略過，
// 檔名剛好等於某個已知資產副檔名時放行」。
//
// 清單刻意只列**列舉時**會用到的那幾種：入口檔（.model3.json）與 ModelAssets::list
// 的三個目標（.motion3.json／.exp3.json／.pose3.json）加上 VTube Studio 的
// .vtube.json。.moc3／.cdi3.json／physics／貼圖都是拿 model3.json 裡寫的路徑
// 直接 read()／exists()，本來就不經過任何隱藏檔規則，列進來只是多一份要維護的真相。
bool isEmptyStemAssetName(const std::string& filename);

// 掃描模型目錄，回傳可載入的模型清單。
//
// 純函式（只碰檔案系統），方便直接用 fixture 目錄做單元測試。
// 解析失敗的模型會被略過而不是讓整個掃描中斷。
std::vector<ModelInfo> scanModels(const std::filesystem::path& modelsDir, const std::string& urlBase = kModelUrlBase);

// 描述**單一**模型入口：資料夾裡的 model3.json，或整個 *.zip。
//
// 存在的理由是 Live2D Viewer（src/viewer/）：它是使用者直接挑一個檔案打開，
// 沒有 models 目錄可掃，但需要跟桌寵一模一樣的那份 ModelInfo ——
// 執行期補全、cdi3 參數表、虛擬表情、內建動作／表情、命名檔全都要在。
// scanModels 內部就是對每個候選跑同一段程式碼，兩條路因此不會分岔。
//
// 內建動作／表情**一律照樣附上**（與 scanModels 一字不差是這支的合約，
// tests/test_model_scanner.cpp 釘住）。檢視器不想要那些，是它自己收下之後再用
// core/builtin_actions.h 的 removeBuiltinActions() 拿掉 —— 「掃到什麼」與
// 「要顯示什麼」是兩件事，混在一起會讓桌寵那條路跟著多一個開關。
//
// 與 scanModels 的三個欄位差異（其餘完全相同）：
//   id   ＝ entryPath 本身（POSIX 斜線），仍然是唯一鍵
//   name ＝ zip 取檔名去掉 .zip，其餘取所在資料夾名
//   url  ＝ **空字串**。單一入口不隸屬任何 models 目錄，沒有 live2d:// 可言，
//          與其編一個載不回來的位址不如留空。
//
// 開不起來、解析不了、或不是 Cubism 4 模型時回 nullopt（不丟例外）。
std::optional<ModelInfo> describeModel(const std::filesystem::path& entryPath);

// 把一個**資料夾**解析成它第一層的模型入口檔（找不到就回 nullopt）。
//
// 存在的理由同樣是 Live2D Viewer：使用者手上的模型十之八九是一個解壓縮出來的
// 資料夾，拖進視窗的自然是那個資料夾本身，而不是埋在裡面的 model3.json。
//
// 三個刻意的決定：
//   1. **只看第一層，不遞迴** —— 再往下找的話，拖一個裝了十隻模型的目錄進來會
//      靜靜開了其中一隻，那不是使用者的意思。「一個目錄底下有哪些模型」是
//      scanModels 的工作，兩支的定位不一樣。
//   2. 同一層有多個入口時取名稱排序的第一個（同 scanModels 的候選收集），
//      選中誰才不會隨檔案系統的列舉順序而變。
//   3. *.model3.json 一律優先於裸命名入口（model.json / index.json），
//      後者還要讀內容確認是 Cubism 4 才收 —— 與 scanModels 同一套規則。
//
// 不是資料夾（含不存在、開不起來）一律回 nullopt，呼叫端不必先自己 stat。
std::optional<std::filesystem::path> findDirectoryEntry(const std::filesystem::path& dir);

}  // namespace l2m
