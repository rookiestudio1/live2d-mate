#pragma once

// 角色卡 AI 擴寫：**分階段的對話式生成**（LLM 第二期）。
//
// 第一版是「一次請求產出整份 markdown 角色卡」，實測小模型的標題五花八門
//（## 開頭、粗體、翻譯成中文…），正規化追著修永遠追不完。改成同一場對話
// 分七個階段各下一次指示（描述 → 台詞 → 歡迎詞 → 時段問候 → 久坐提醒 →
// 摸摸反應 → 天氣預警），每一階段只要求輸出**純內容**（描述是散文、其餘一行一句）——
// 回覆直接對應到角色卡的一個區塊，標題解析整個問題不存在了。
//
// 「同一場對話」：API 是無狀態的，session 靠把前面階段的問答附回每次請求
// 模擬（llm_types.h 的 messages 陣列本來就是這個用途）。後面的階段看得到
// 前面的產出，語氣才會一致；Ollama 的前綴快取讓重送歷史幾乎不花時間。
//
// 這是純邏輯的狀態機（不碰網路），非同步搬運在 persona_page.cpp：
// nextMessages() 給出這一階段要送的訊息，accept() 收回覆（清 fence、
// 剝列表符號、丟超長行、驗長度），done() 之後 result() 組回 PersonaDoc。
// 產出仍然只進編輯器預覽，**存檔永遠是使用者按的那一下**。

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llm_types.h"
#include "persona_doc.h"

namespace l2m {

struct PersonaGenerateInput {
  // 現有內容（全空＝從零生成）。reserved 區原樣帶進 result。
  PersonaDoc current;
  // 使用者給的一句方向（可空：就現有內容補全）。
  // **語言也在這裡講** —— session 不再自己下語言指示，理由見 nextMessages()。
  std::string brief;
};

// 生成階段，順序即為對話順序。七個各對應角色分頁的一個輸入框。
enum class PersonaGenStage { Description, Lines, Welcome, Greetings, Breaks, Petted, Weather };

class PersonaGenerateSession {
public:
  explicit PersonaGenerateSession(PersonaGenerateInput input);

  bool done() const;
  PersonaGenStage currentStage() const;
  // 進度顯示用：已完成幾個 / 總共幾個
  int stageIndex() const;
  int stageCount() const;

  // 這一階段要送的完整訊息（system ＋ 已完成階段的問答 ＋ 這一階段的指示）。
  // done() 之後不可再呼叫。
  std::vector<LlmMessage> nextMessages() const;

  // 收下這一階段的回覆。回空字串＝收下並前進到下一階段；
  // 非空＝這一階段不合格（描述空白／超長），階段**不前進**，
  // 字串就是給使用者看的錯誤（英文）。
  // 台詞階段的整理是寬容的：剝 "- "／"* "／"1. " 這類列表符號
  //（指示說了不要，小模型照加是常態）、超長行直接丟掉。
  std::string accept(const std::string& responseText);

  // done() 之後：組好的角色卡（含 input.current 的 reserved 區）
  PersonaDoc result() const;

private:
  PersonaGenerateInput input_;
  PersonaDoc doc_;
  size_t stage_ = 0;
  // 已完成階段的問答（user 指示＋assistant 回覆），附回每次請求模擬同一場對話
  std::vector<LlmMessage> history_;
};

}  // namespace l2m
