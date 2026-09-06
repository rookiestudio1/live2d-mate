#pragma once

// 角色描述（persona）：名稱規則、長度上限、磁碟上的多份描述檔。
//
// 一份角色描述就是 %APPDATA%/live2d_mate/personas/<名稱>.md 這一個純文字檔，
// 整個檔案就是描述本文，檔名去掉 .md 就是顯示名稱（也就是 id）。用 .md 而不是
// JSON 的理由：這是一整段散文，塞進 JSON 字串會讓每個換行都變成 \n，
// 使用者再也沒辦法拿記事本改，也沒辦法把別處抓來的角色卡直接丟進資料夾。
//
// 為什麼放在 l2m_core：
//  * 名稱規則與長度上限是產品規則，不是視窗的細節。personaNameIssue 回傳的
//    那句英文會直接變成 CommandResult::hint（core/command_result.h 的
//    「失敗一定附 hint」），是使用者與 AI 共用的介面文字。
//  * 掃描順序、只認 .md、跳過隱藏檔這些規則要能用 fixture 目錄直接測，
//    和 core/model_scanner.h 是同一個切法。
//
// 設定視窗的分頁在 src/windows/settings/persona_page.*；
// 送到 MCP 的三條通道在 core/mcp_resources.h 與 core/mcp_tool_specs.h。

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "command_result.h"

namespace l2m {

// 描述文字的上限，單位是**字元（碼位）**不是位元組 —— 見 strutil::utf8Length。
// 2000 個中文字約 1500~2000 tokens。這段文字每次都在 host 的 system prompt 裡，
// 而且之後接 Local LLM 聊天時要塞進 7B/8B 模型常見的 8K context，
// 再長下去模型對細節的遵守度反而會掉。
inline constexpr int kPersonaMaxChars = 2000;

// 名稱的字元數上限（同樣是碼位）。檔名要能在 Windows 上建得起來。
inline constexpr int kPersonaMaxNameChars = 64;

inline constexpr const char* kPersonaSuffix = ".md";

struct PersonaInfo {
  // 顯示名稱 ＝ 檔名去掉 .md ＝ id。刻意不做 id/name 兩套。
  std::string name;
};

struct PersonaEntry {
  std::string name;
  std::string text;
};

// 要送到 AI 面前的那一份。McpHttpServer 在 GUI 執行緒收下、
// 在 httplib 的 worker 執行緒讀複本，所以這個結構必須是可整份複製的純資料。
struct PersonaSnapshot {
  // 空字串 ＝ 目前沒有套用任何角色
  std::string activeName;
  // 全部角色，**含全文**。resources/read 要能讀到沒套用的那幾份，
  // 只帶名稱的話 AI 讀到的會是一個空殼。一份上限 2000 字，
  // 幾份也不過幾 KB，不值得為了省這點記憶體讓 worker 執行緒去碰磁碟。
  std::vector<PersonaEntry> personas;

  // 套用中那一份的全文；沒有套用、或檔案已經被刪掉時回空字串
  std::string activeText() const;
};

// ── 驗證 ──
// 都是「沒問題回 nullopt，有問題回一句英文」。那句英文會變成使用者看到的
// 錯誤訊息與 AI 讀到的 hint，所以要講清楚「不行的是什麼」而不只是「不行」。

// 名稱能不能當檔名。不做 sanitize 轉換而是直接擋下來 —— 一轉換就會出現
// 「顯示名 ≠ 檔名」，馬上多一組對應表要維護，而且使用者看不懂自己的角色
// 為什麼被改名。
std::optional<std::string> personaNameIssue(const std::string& name);

// 描述文字（目前只有長度）
std::optional<std::string> personaTextIssue(const std::string& text);

// 「找不到角色」時附的提示
std::string personaListHint(const std::vector<PersonaInfo>& personas, const std::string& personasDir);

// ── 磁碟 ──

// 掃描角色描述目錄，依名稱排序。目錄不存在時回空 vector（不是錯誤：
// 使用者還沒建過任何角色是正常狀態）。
std::vector<PersonaInfo> scanPersonas(const std::filesystem::path& dir);

// 讀一份描述；檔案不存在或讀不到回 nullopt
std::optional<std::string> readPersona(const std::filesystem::path& dir, const std::string& name);

// 寫一份描述（覆蓋）。名稱或長度不合法時不會碰磁碟。
CommandResult writePersona(const std::filesystem::path& dir, const std::string& name, const std::string& text);

CommandResult deletePersona(const std::filesystem::path& dir, const std::string& name);

// 掃描 ＋ 逐份讀出來，組成要推給 MCP 的快照。
// activeName 指到已經不存在的檔案時，快照的 activeName 會被清成空字串
//（使用者可能在檔案總管裡把檔案刪掉了）。
PersonaSnapshot loadPersonaSnapshot(const std::filesystem::path& dir, const std::string& activeName);

}  // namespace l2m
