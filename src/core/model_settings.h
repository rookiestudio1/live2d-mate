#pragma once

// model3.json 的設定補全。
//
// 有兩種模型包會讓動作／表情「明明檔案都在卻掃不出來」：
//
//  1. VTube Studio 模型：VTS 把綁定存在自己的 *.vtube.json（IdleAnimation 與 Hotkeys），
//     model3.json 的 FileReferences 保持乾淨，所以照 Cubism 規格讀就是空的。
//  2. 重新整理過的模型包：檔案被搬進 exp/、motions/ 子資料夾，model3.json 的路徑卻沒跟著改。
//
// 這裡在讀取當下把缺的補回去、把斷掉的路徑接回來，磁碟上的檔案完全不動。
// model-scanner（系統匣清單）與模型載入端都走這支，兩邊看到的設定才會是同一份。
//
// 檔案一律透過 core/model_assets.h 的 ModelAssets 取得，所以「資料夾模型」與
// 「zip 模型」走的是同一份補全邏輯。吃 std::filesystem::path 的多載是便利包裝，
// 內部就是建一個 DirModelAssets。

#include <yyjson.h>

#include <filesystem>
#include <optional>
#include <string>

#include "json_doc.h"

namespace l2m {

class ModelAssets;

// 補全時做了哪些事，用來寫 log 與跑測試
struct EnrichReport {
  enum class Source { None, VTube, Scan };

  // 補進去的表情數（原本沒宣告）
  int addedExpressions = 0;
  // 補進去的動作數（原本沒宣告）
  int addedMotions = 0;
  // 修好的斷掉路徑數
  int repairedPaths = 0;
  // 有沒有補上磁碟裡有、model3.json 卻沒引用的 pose3.json
  bool addedPose = false;
  // 名稱是從哪來的
  Source source = Source::None;
};

// 補全結果：doc 的 root 是補全後的 model3.json（原輸入不會被改到）
struct EnrichedSettings {
  jsonu::MutDoc doc;
  EnrichReport report;

  // 序列化成 JSON 文字。
  // 一律 pretty-print：CubismJson 的數字解析只接受「換行或逗號」作結尾，
  // 緊湊格式的 `"a":3}` 會被它判成解析錯誤（官方檔案都有排版所以沒踩到）。
  std::string json() const { return doc.write(true); }
};

// 讀模型裡的 VTube Studio 設定檔；沒有或壞掉都回傳 nullopt
std::optional<jsonu::Doc> readVTubeConfig(const ModelAssets& assets);
std::optional<jsonu::Doc> readVTubeConfig(const std::filesystem::path& modelDir);

// 補全一份已解析的 model3.json（原值不會被改到）。
// 只在原本「沒有宣告」時才補；作者有寫的東西一律尊重，只修斷掉的路徑。
EnrichedSettings enrichCubism4Settings(yyjson_val* json, const ModelAssets& assets);
EnrichedSettings enrichCubism4Settings(yyjson_val* json, const std::filesystem::path& modelDir);

// 判斷解析後的 JSON 是不是 Cubism 4/5 的模型設定。
// 入口檔叫 model.json / index.json 這種裸命名時，檔名分不出版本，只能靠內容判斷：
// Cubism 4 一定有 FileReferences，Cubism 2 則是把 moc 路徑放在頂層的 model 欄位。
bool isCubism4Json(yyjson_val* json);

// 讀取並補全一份 model3.json。解析失敗時丟 std::runtime_error，
// 交給呼叫端決定要略過還是回錯誤。
//
// 吃 path 的多載會自己判斷是資料夾模型還是 *.zip（見 openModelAssets），
// 所以 model_scanner 與 model_controller 都不必分兩條路走。
EnrichedSettings loadCubism4Settings(const ModelAssets& assets);
EnrichedSettings loadCubism4Settings(const std::filesystem::path& entryPath);

}  // namespace l2m
