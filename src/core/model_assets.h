#pragma once

// 一個模型的資源存取層。
//
// 模型可以是「資料夾」也可以是「zip 壓縮檔」，上層（model_settings 的補全、
// display_info 的 cdi3／physics3、model_scanner 的掃描、live2d/model_controller
// 的載入）全部只透過這個介面拿檔案，兩種容器因此走同一條程式碼路徑。
//
// **這是最重要的設計約束**：只要 DirModelAssets 與 ZipModelAssets 的
// read / exists / list 有一點行為不一致，症狀就會是「解壓縮就好、壓起來就壞」
// 那種最難查的 bug。tests/test_model_assets.cpp 就是拿同一份內容做成資料夾與
// zip 兩份，逐一斷言三個方法的輸出一字不差相同。
//
// 路徑一律是「相對於模型根目錄的 POSIX 路徑」，跟 model3.json 裡 FileReferences
// 的寫法一致（"motions/idle.motion3.json"）。zip 內多包一層資料夾時，那一層由
// ZipModelAssets 自己吸收成前綴，對上層完全透明。
//
// 執行緒：實作**不保證**執行緒安全（zip 版底下的 miniz reader 有共用緩衝）。
// 所有呼叫都必須在同一條執行緒上。

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zip_archive.h"

namespace l2m {

class ModelAssets {
public:
  virtual ~ModelAssets() = default;

  // 入口檔（model3.json）相對於模型根目錄的路徑；一律沒有目錄部分
  virtual const std::string& entryName() const = 0;

  // 寫 log 與錯誤訊息用的人類可讀位置
  virtual std::string describe() const = 0;

  virtual std::optional<std::string> read(const std::string& rel) const = 0;

  // 只讀前 maxBytes 個位元組。zip 版不會整包解開（見 zip_archive.h）。
  virtual std::optional<std::string> readPrefix(const std::string& rel, std::size_t maxBytes) const = 0;

  virtual bool exists(const std::string& rel) const = 0;

  // 列出符合副檔名的檔案（POSIX 相對路徑，已排序）。
  // maxDepth 是「最多幾個路徑段」：1 = 只有模型根目錄下的檔案，
  // 2 = 再加上 exp/、motions/ 這類一層子資料夾（model_settings 的補全用 2）。
  virtual std::vector<std::string> list(const std::string& suffix, int maxDepth) const = 0;
};

// 依 entryPath 決定實作：
//   *.zip           → ZipModelAssets（開檔、找入口、決定內部前綴）
//   其餘（model3.json 之類的入口檔）→ DirModelAssets，根目錄是它的上層資料夾
//
// 開不起來（zip 壞掉、裡面沒有合格的入口檔）回 nullptr，不丟例外 ——
// 使用者的 models 目錄裡本來就可能放不相干的 zip。
std::unique_ptr<ModelAssets> openModelAssets(const std::filesystem::path& entryPath);

// 直接以一個資料夾為模型根目錄建立存取層（永遠成功）。
// 給「已經知道模型根目錄、不需要入口檔」的舊介面用 ——
// model_settings 與 display_info 都有一組吃 modelDir 的多載，
// 那些呼叫端（含既有測試）因此完全不必改。
std::unique_ptr<ModelAssets> openDirectoryAssets(const std::filesystem::path& modelDir, const std::string& entryName = {});

// 在 zip 裡找模型入口檔，回傳它在 zip 內的完整相對路徑。
//
// 判定順序刻意對齊 model_scanner.cpp 的 collectCandidates：
//   1. 根目錄的 *.model3.json（排序後第一個）
//   2. 根目錄的裸命名入口 model.json / index.json，且內容判定為 Cubism 4
//   3. 兩者都沒有時，若 zip 裡**剛好只有一個**頂層資料夾就鑽進去重跑 1、2
//      —— Windows 檔案總管右鍵「壓縮成 ZIP 檔」產生的就是這種多包一層的結構。
//      兩個以上頂層資料夾代表這不是單一模型包，直接放棄。
std::optional<std::string> findZipEntry(const ZipArchive& zip);

}  // namespace l2m
