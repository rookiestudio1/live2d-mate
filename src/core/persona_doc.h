#pragma once

// 角色 .md 的分區結構。整份檔案仍然是純文字、記事本可改，
// 只是多了三個英文標題把「描述」「台詞」「歡迎詞」分開：
//
//   # Character Description   ← 唯一送給 AI 的一區
//   # Dialogue List           ← 閒置時的隨機台詞，一行一句（純本機使用）
//   # Welcome Text            ← 啟動／回座／喚醒時的歡迎詞，一行一句（純本機使用）
//   # Greeting by Time        ← 時段問候（core/day_period.h）
//   # Break Reminder          ← 久坐提醒（autonomy.breakReminder）
//   # Petted                  ← 被摸摸的反應（occasion="petted"）
//   # Weather Alert           ← 壞天氣預警（core/weather_alert.h）
//
// 區塊標題是解析用的英文標記，設定視窗的「角色」分頁把各區拆進輸入框，
// 畫面上完全看不到這些標題。
//
// 向後相容規則（這條最重要，寫錯會毀掉現有角色卡）：
//  * **一個保留字標題都沒有 → 整份當成 description**，其餘兩區空。
//    今天的檔案、以及從別處抓來的自由格式角色卡，行為與從前一模一樣，
//    送給 AI 的內容一個位元組都不會變。
//  * 有保留字標題才啟用分區切割。第一個保留字標題之前的文字併進 description；
//    **不在保留字清單裡的 `#` 標題留在它所屬那一區的內文裡**，不獨立抽出 ——
//    角色卡裡寫「# 背景設定」是很正常的事，抽出來會讓描述被腰斬。
//  * 保留字清單裡、但這一版還沒有對應欄位的區塊（目前是空集合，七個都有了）
//    原樣保留、寫回時放在最後 —— 使用者用新版寫了新的一區、又用舊版存了一次，
//    那一區才不會被吃掉。
//
// 長度上限分開算：kPersonaMaxChars（persona.h，2000）只管 description ——
// 它的理由（進 host 的 system prompt、之後要塞 Local LLM 的 8K context）
// 完全不受台詞影響。台詞與歡迎詞只在本機用（氣泡與 TTS），上限寬鬆。

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace l2m {

// 台詞區（Dialogue List 或 Welcome Text 各自）的總字元數上限。
// 純粹防手滑把整本小說貼進去；單位與 kPersonaMaxChars 相同（碼位）。
inline constexpr int kPersonaLinesMaxChars = 4000;
// 單行台詞的上限：再長氣泡撐不下，TTS 也會被 splitIntoSpeechSegments 切開
inline constexpr int kPersonaLineMaxChars = 200;

struct PersonaDoc {
  std::string description;           // # Character Description（唯一送給 AI 的一區）
  std::vector<std::string> lines;    // # Dialogue List，一行一句，空行略過
  std::vector<std::string> welcome;  // # Welcome Text，一行一句
  // # Greeting by Time，一行一句；行首可帶 "morning:" 等時段前綴
  //（解析在 core/day_period.h，這裡只負責分區）
  std::vector<std::string> greetings;
  // # Break Reminder，一行一句：久坐提醒的保底台詞池（autonomy.breakReminder）。
  // 沒寫這一區時規則版整輪安靜；LLM 行為大腦則照樣能現場生成。
  std::vector<std::string> breaks;
  // # Petted，一行一句：被摸摸時的反應台詞（occasion="petted"）
  std::vector<std::string> petted;
  // # Weather Alert，一行一句：壞天氣預警的保底台詞池（occasion="weatherAlert"）。
  // 行首可帶天氣前綴（"rain:" / "snow:" / "thunder:" / "hot:" / "cold:" / "wind:"），
  // 解析在 core/weather_alert.h 的 weatherLinesFor，規則與 # Greeting by Time 一致；
  // 這裡只負責分區。
  std::vector<std::string> weatherAlerts;
  // 在保留字清單裡、但還沒有對應欄位的未來區塊（目前是空集合，機制留著：
  // 之後加新保留字時，舊版讀到新版寫的檔才不會把那一區吃掉）。
  // first 是標題的標準寫法，second 是原始內文。
  // **不是**用來收所有認不得的 # 標題：那些一律留在所屬區塊的內文裡。
  std::vector<std::pair<std::string, std::string>> reserved;
};

// 解析。永不失敗 —— 任何輸入都有一個合理的解讀（最差就是整份當 description）。
PersonaDoc parsePersonaDoc(const std::string& raw);

// 序列化。lines / welcome / reserved 全空時直接回 description 原文
//（自由格式的角色卡存回去不會被硬加上標題）；否則依固定順序輸出各區。
std::string serializePersonaDoc(const PersonaDoc& doc);

// 台詞區的驗證（總長與單行長度）。sectionName 用英文區塊名，
// 錯誤字串是使用者與 AI 共用的介面文字（比照 persona.h 的慣例）。
std::optional<std::string> personaLinesIssue(const std::vector<std::string>& lines, const char* sectionName);

// 整份文件的驗證：description 走 personaTextIssue、三個台詞區走 personaLinesIssue。
// writePersona 靠它擋住存檔 —— 上限規則只有這一份，UI 與磁碟層共用。
std::optional<std::string> personaDocIssue(const PersonaDoc& doc);

}  // namespace l2m
