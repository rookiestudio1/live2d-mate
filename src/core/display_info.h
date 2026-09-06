#pragma once

// cdi3.json（DisplayInfo）與 physics3.json 的解讀。
//
// 給臉部追蹤用的 VTuber 模型常常一個 .exp3.json 都沒有，語意全部藏在「參數」裡：
// 作者在 Cubism Editor 幫參數取的中文名（哭哭、生氣、手臂揮動）會存進 cdi3.json，
// 但 Cubism 規格裡沒有任何地方把它當成表情或動作，照規格讀就是一片空白。
//
// 這裡把 cdi3 的名稱與 physics3 的輸入／輸出關係合起來，讓上層知道
// 「這個參數叫什麼」以及「寫進去到底有沒有用」。
//
// 檔案一律透過 core/model_assets.h 的 ModelAssets 取得，所以資料夾模型與 zip 模型
// 走同一條路；吃 std::filesystem::path 的多載是便利包裝（內部建一個 DirModelAssets），
// 方便直接用 fixture 目錄做單元測試。

#include <yyjson.h>

#include <filesystem>
#include <set>
#include <vector>

#include "model_types.h"

namespace l2m {

class ModelAssets;

struct PhysicsIO {
  std::set<std::string> inputs;
  std::set<std::string> outputs;
};

// 讀 model3.json 指到的 *.physics3.json，攤平成輸入／輸出兩個參數集合
PhysicsIO readPhysicsIO(const ModelAssets& assets, yyjson_val* modelJson);
PhysicsIO readPhysicsIO(const std::filesystem::path& modelDir, yyjson_val* modelJson);

// 列出模型的參數與它們的名稱、群組、物理角色。
// 沒有 DisplayInfo（或檔案壞掉）時回空陣列 —— 這是常態，不是錯誤。
std::vector<ParameterInfo> describeParameters(const ModelAssets& assets, yyjson_val* modelJson);
std::vector<ParameterInfo> describeParameters(const std::filesystem::path& modelDir, yyjson_val* modelJson);

// 把「開關型的表情參數」升級成虛擬表情。
//
// 判定保守，寧可漏也不要亂認 —— 認錯了會在 list_expressions 裡塞一堆
// 「右腿」「星星移動1」這種按了看不出差別的項目，AI 只會更難挑。
std::vector<ParamExpressionRef> expressionsFromDisplayInfo(const std::vector<ParameterInfo>& params);

}  // namespace l2m
