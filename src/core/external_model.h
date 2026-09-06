#pragma once

// 外部模型：不在 %APPDATA%/live2d_mate/models 底下、由 Live2D Viewer 直接指定的模型。
//
// 桌寵原本的世界觀是「模型一定在 models 目錄裡」—— ModelInfo::id 是相對路徑，
// AppController::entryPathOf() 就是 modelsDir_ / id。使用者在 Viewer 看完一隻
// 放在 E:\Downloads 的模型想直接拿來當桌寵，那條路走不通。
//
// 解法是讓 config 的 model.current **可以**是一條絕對路徑（只有這一格，
// 不另外存清單）。於是這裡要有三組規則，而且三組錯了都是靜默失敗，
// 所以全部放在 l2m_core 讓 QTest 釘住：
//
//  1. isExternalModelId —— 「這個 id 是相對還是絕對」。judged wrong 的話，
//     桌寵會拿絕對路徑去接 modelsDir_，載到一條不存在的路徑而毫無訊息。
//  2. normalizeModelPath —— id 同時是模型清單的唯一鍵。同一個檔案用
//     "E:/a/b.json" 與 "E:\a\.\b.json" 兩種寫法進來，清單上會多出一隻分身。
//  3. Instance 訊息 —— Viewer 與桌寵之間唯一的協定（走 SingleInstance 那條
//     QLocalSocket）。路徑含空白是常態（"C:/Program Files/..."），
//     所以 verb 只取到第一個空白，其餘整段都是路徑。

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace l2m {

// ── 模型 id ─────────────────────────────────────────────

// 這個 id 是不是外部模型（絕對路徑）。
//
// 刻意用字串判定而不是 std::filesystem::path::is_absolute()：後者在 Windows 上
// 對 "/home/u/x" 回 false，同一份 config 在兩個平台會得到不同答案。
bool isExternalModelId(const std::string& id);

// 正規化成模型 id 的形式：POSIX 斜線、收掉 "." 與 ".."、去掉尾端斜線
//（根目錄的那一條斜線保留）。
std::string normalizeModelPath(const std::filesystem::path& path);

// 使用者給的路徑 → 外部模型的 id。相對路徑會先依現行工作目錄轉成絕對，
// 因為 id 之後要存進 config，工作目錄一換就找不到了。
std::string externalModelId(const std::filesystem::path& path);

// entryPath 若落在 modelsDir 底下，回傳 scanModels 用的那種相對 id；否則 nullopt。
//
// 存在的理由：使用者在檢視器裡最順手的瀏覽目標就是 models 目錄。不先還原成
// 相對 id 的話，「設定為桌寵」會用絕對路徑把同一隻再收一次 —— 清單上出現兩隻
// 名字一模一樣的模型（設定頁只顯示 name，看不出差別），而且 config.model.current
// 記的是絕對路徑，每次重開都會再長回來。
//
// **Windows 上比對不分大小寫**：NTFS 本來就不分，而 QFileDialog 與 QStandardPaths
// 給的磁碟機大小寫不保證一致，精確比對會讓自家目錄裡的模型被當成外部模型。
// 其他平台的檔案系統真的分大小寫，所以維持精確比對。
std::optional<std::string> modelsDirRelativeId(const std::filesystem::path& entryPath, const std::filesystem::path& modelsDir);

// ── 命令列 ──────────────────────────────────────────────

// 指定這次啟動要用的模型；Viewer 的「設定為桌寵」按鈕送的就是這個。
inline constexpr const char* kSetModelFlag = "--set-model";

// 取 --set-model 後面那一個參數。沒有旗標、旗標後面沒東西、
// 或後面接的是另一個旗標（"--" 開頭）都回 nullopt ——
// 把 "--hidden" 當成路徑拿去載入只會得到一句莫名其妙的錯誤。
std::optional<std::string> setModelArg(const std::vector<std::string>& args);

// ── 單一實例的訊息 ──────────────────────────────────────
//
// 第二個實例（或 Viewer）透過 QLocalSocket 送一行給既有實例，既有實例回一行。
// 格式：<verb> <payload>\n。收端要讀到換行、或讀到對方斷線為止 ——
// 舊版送的是不帶換行的 "show"，那條路要繼續認得。

// 桌寵那條 QLocalSocket 的名字。Live2D Viewer 要用同一個名字才連得上，
// 所以放在這裡讓兩個執行檔共用（原本只是 main.cpp 裡的一個字面值）。
inline constexpr const char* kInstanceServerName = "live2d_mate-single-instance";

enum class InstanceRequestKind { Unknown, Show, SetModel };

struct InstanceRequest {
  InstanceRequestKind kind = InstanceRequestKind::Unknown;
  // 只有 SetModel 有值
  std::string path;
};

std::string encodeShowRequest();
std::string encodeSetModelRequest(const std::string& path);
InstanceRequest parseInstanceRequest(const std::string& line);

struct InstanceReply {
  bool ok = false;
  std::string message;
};

// 失敗訊息裡的換行會換成空白 —— 一則回覆就是一行，切成兩行的話收端會讀不完整。
std::string encodeInstanceReply(bool ok, const std::string& message);
InstanceReply parseInstanceReply(const std::string& line);

}  // namespace l2m
