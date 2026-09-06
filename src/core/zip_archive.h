#pragma once

// zip 壓縮檔的唯讀存取（miniz 包裝）。
//
// 這是全專案唯一 include <miniz.h> 的地方。資料來自 core/byte_source.h 的
// ByteSource，所以「本機檔案」與未來的「HTTP Range」對這一層是同一件事。
//
// 幾個刻意的行為，都是踩過才知道的：
//
//  1. **查找一律大小寫不敏感**。zip 是大小寫敏感的容器，但 model3.json 裡的引用
//     常常跟實際檔名大小寫對不上（"Motions/Idle.motion3.json" vs
//     "motions/idle.motion3.json"）。這種模型在 NTFS 上完全正常，一壓成 zip
//     就整組動作／貼圖讀不到，而且錯誤是靜默的 —— 模型照樣載入，只是什麼都不會動。
//
//  2. **條目名稱會先正規化**：反斜線換成 '/'、去掉開頭的 "./" 與 '/'；目錄項、
//     絕對路徑、含 "." 或 ".." 的路徑段一律當成不合法。這是「路徑合不合法」。
//
//  3. **條目名稱一律先解碼成 UTF-8**。ZIP 的 general purpose bit 11 若有設，
//     檔名就是 UTF-8；沒設的話規格說是 CP437，但實務上是兩種：一半的工具寫 UTF-8
//     卻忘了標記，另一半（**Windows 檔案總管的「壓縮成 ZIP 檔」**）寫的是系統
//     ANSI 碼頁的位元組（正體中文 Windows 就是 CP950／Big5）。
//     不解碼的後果實測過三種，而且一個比一個難查：
//       (a) model3.json 的內容是 UTF-8，引用的中文檔名跟 zip 裡的 Big5 位元組
//           完全對不上 → 貼圖、動作、physics 全部靜默讀不到；
//       (b) 設定補全會把掃到的檔名寫回 model3.json，非法 UTF-8 讓 yyjson 解析失敗
//           → model_scanner 直接略過整個模型（「資料夾掃得到、zip 掃不到」）；
//       (c) 那些位元組拿去建 std::filesystem::path，MSVC 會丟例外 → 整輪掃描 abort。
//
//  4. **「合不合法」與「列不列出」是兩回事**，跟資料夾模型一致：
//     entries()（也就是 list() 與找入口檔的依據）會濾掉 "__MACOSX/" 與以 '.' 開頭的
//     隱藏檔，對齊 core/model_scanner.cpp 掃資料夾時的規則；但 read() / contains()
//     照樣抓得到它們 —— 因為 DirModelAssets 那邊就是這樣（列舉會跳過隱藏檔，
//     直接開檔卻讀得到）。兩邊只要有一點不一樣，就會出現「解壓縮就好、壓起來就壞」。
//     隱藏檔規則的唯一例外也是共用的：stem 空的資產檔名（整個檔名就是
//     ".model3.json"）照樣列出，見 core/model_scanner.h 的 isEmptyStemAssetName。
//
// 執行緒：**不是**執行緒安全的。miniz 的 reader 內部有共用緩衝，
// 同一個 ZipArchive 不能同時在兩條執行緒上 read()。
// （見 live2d/model_controller.h 的貼圖載入註記。）

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "byte_source.h"

namespace l2m {

class ZipArchive {
public:
  ~ZipArchive();
  ZipArchive(const ZipArchive&) = delete;
  ZipArchive& operator=(const ZipArchive&) = delete;

  // 開啟。source 為 nullptr、不是 zip、中央目錄壞掉或被截斷都回 nullptr。
  // 刻意不丟例外：使用者的 models 目錄裡本來就可能放不相干的 zip。
  static std::unique_ptr<ZipArchive> open(std::unique_ptr<ByteSource> source);

  // 所有檔案條目（正規化後的 POSIX 相對路徑，已依名稱排序、不含目錄項）
  const std::vector<std::string>& entries() const { return entries_; }

  bool contains(const std::string& path) const;

  // 解出整個條目。找不到、解壓縮失敗、或超過大小上限都回 nullopt。
  std::optional<std::string> read(const std::string& path) const;

  // 只解出前 maxBytes 個位元組（走 mz_zip_reader_extract_iter，不會整包展開）。
  // 用途是「讀 PNG 檔頭拿尺寸」—— 8192² 的貼圖整包 inflate 是幾十毫秒，
  // 讀 IHDR 只要前 33 個位元組。
  std::optional<std::string> readPrefix(const std::string& path, std::size_t maxBytes) const;

private:
  struct Impl;

  ZipArchive();

  // 正規化後的相對路徑 → impl_ 內的 zip 條目索引；key 是小寫（大小寫不敏感查找）
  std::optional<unsigned int> locate(const std::string& path) const;

  std::unique_ptr<Impl> impl_;
  std::vector<std::string> entries_;
};

// zip 條目名稱轉成 UTF-8（理由見本檔開頭第 3 點）。
//   utf8Flag 為 true（general purpose bit 11 有設）→ 直接相信它
//   否則已經是合法 UTF-8 → 原樣沿用（**不能**再解一次，會變成雙重編碼的亂碼）
//   否則 → 當成系統 ANSI 碼頁的位元組解碼
std::string decodeZipEntryName(const std::string& raw, bool utf8Flag);

// zip 條目名稱的正規化。不合格（目錄項、絕對路徑、含 "." 或 ".." 的路徑段）
// 一律回空字串。公開出來是為了讓 core/model_assets.cpp 與測試共用同一份規則。
std::string normalizeZipEntry(const std::string& raw);

// 這個（已正規化的）條目該不該出現在 entries() 裡。
// 讀取不受此限，理由見本檔開頭第 4 點。
bool isListableZipEntry(const std::string& normalized);

}  // namespace l2m
