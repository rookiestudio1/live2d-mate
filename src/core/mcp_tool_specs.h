#pragma once

// MCP 工具的規格表與輸出格式（純資料與純函式）。
//
// 抽到 l2m_core 的理由：這裡的英文字串就是產品的「AI 介面」本身 ——
// 24 個工具的說明、JSON Schema、三段固定註記、以及「失敗一定附 hint」的
// 輸出格式。它們沒有任何 GUI 相依，卻是最不能悄悄壞掉的東西
//（例如原始字串被內容裡的 `)"` 提前截斷），所以要能單獨測。
//
// 工具的實作在 src/mcp/mcp_tools.cpp（那邊要碰 AppController）。

#include <string>
#include <vector>

namespace l2m {

struct ToolSpec {
  const char* name;
  const char* description;
  // JSON Schema（物件型別）的原始文字
  const char* inputSchema;
  // 唯讀工具不重置閒置計時 —— 不然 AI 一直輪詢 get_state 就永遠不會復原
  bool readOnly;
};

const std::vector<ToolSpec>& mcpToolSpecs();

// tools/list 的 result JSON。
// 有套用角色時，speak 與 perform 的 description 後面會多接一句指向那個角色
//（perform 的 steps 有 action:"speak" 帶 text，所以兩個都要）。
// 工具表本身是 static 的，角色名只在產生 JSON 這一層接上去 ——
// ToolSpec 的欄位都是 const char*，不為了這件事改結構。
//
// 每筆另外帶 `_meta: {"anthropic/alwaysLoad": true}`，要求 host 不要延後載入
// （理由寫在 .cpp 的產生處）。
std::string mcpToolsListJson(const std::string& personaName = {});

// AI 的發話節奏。四個開關互相獨立，十六種組合都要讀得通。
//
// 用 struct 而不是一串 bool 參數：相鄰的 bool 在呼叫端看不出誰是誰，
// 而這幾件事調錯的症狀（桌寵太吵／跑完不吭聲／工作途中一路沉默）
// 都不會編譯失敗。
//
// 四個欄位一一對應設定視窗 MCP 分頁的四個核取方塊，也就是 config 的
// mcp.talkative / notifyOnComplete / speakNoWait / announceSteps
//（對照表在 core/settings_layout.h 的 mcpToggles()）。
struct SpeechProtocol {
  // 每吐一個段落就唸該段摘要。關＝退回「一則回覆只講一次」的舊行為。
  //
  // 這個開關同時決定協定要不要提「省一趟往返」：**關著才提**（安靜模式本來就該少跑），
  // 開著時反過來明講「不准為了省往返或省一次工具呼叫而少講」。
  // 理由是 host 自己的 system prompt 幾乎都帶著「獨立的工具呼叫合併成同一則訊息、
  // 少跑 round trip」這一條，跟逐段發聲直接打架；協定裡只要再出現一次省成本的措辭，
  // AI 就會把它推廣成「講少一點比較好」，一場對話撐久了退化成只在頭尾各講一句。
  bool talkative = true;
  // 回應結束時再 perform 一次收尾
  bool notifyOnComplete = true;
  // speak 排進佇列就回覆，不等開口也不等播完。這件事**一定要告訴 AI** ——
  // 它拿到成功回覆之後若以為那句已經唸完，接下來的節奏會整個錯開
  //（最常見的是立刻送 stop_speaking 收尾，把還沒唸的整串砍掉）。
  bool speakNoWait = true;
  // 工作途中每換一個階段就報一句（讀程式碼／改檔案／編譯／跑測試／出錯改作法）。
  // 關＝退回「中間的工具鏈完全不旁白」的舊行為。
  //
  // 粒度是「每支工具呼叫」，沒有例外。原本停在「階段」是為了擋語音積壓
  //（speak 排隊播放，一個階段裡連發五六個讀檔常常只要一兩秒，逐一報告會讓
  // 工作早就做完、角色還在唸三分鐘前的動作），但「階段」的邊界是 AI 自己劃的，
  // 而當時尾巴那句「同一階段內連續的讀寫保持安靜」正好給了它把整段工作劃成
  // 一個階段的藉口 —— 實測是開頭 perform 一次之後全程安靜。所以 2026-09 改成
  // 沒有解釋空間的「每一支」，積壓改用長度節流（「每句只講幾個字」）。
  // 文字裡也一併告訴 AI「把 speak 跟那支工具呼叫放在同一則訊息送出」，
  // 但**那句話的「為什麼」跟著 talkative 走**：安靜時說「省一趟往返」（每次工具呼叫
  // 都要重送整份對話，另外開一則只為了講話就是多一趟完整往返），多話時改說
  // 「講在動手之前」—— 在多話模式下講成省成本，等於自己拆自己的台，理由見 talkative。
  bool announceSteps = true;
};

// host 會把 instructions 截斷在這麼多個**字元（碼位）**。
//
// 2026-09 實測 Claude Code：伺服器送出 2475 字元，AI 收到的那一份剛好斷在第 2048 個
// ——切在 "...one short line when you start a distinct phase - reading the" 之後，
// 協定的最後一條被腰斬成半句。這個數字不是 MCP 規格而是 host 的行為，所以它只該
// 出現在 mcpInstructions 的守衛裡，**不該回頭去綁 core/persona.h 的 kPersonaMaxChars**
// ——那個上限有自己的理由（之後要塞 Local LLM 的 8K context），而且降低輸入上限會讓
// 已經寫超過的描述「載入得了、存不回去」（personaDocIssue 只擋 writePersona）。
inline constexpr size_t kInstructionsBudget = 2048;

// initialize 時給模型的整體說明。**四段固定順序**：
//   ① 能力說明 → ② 互動協定 → ③ 角色描述本文 → ④ 角色適用範圍（外框）
// instructions 是唯一會被 host 直接放進 system prompt 的欄位，也就是唯一
//「AI 不必主動撈就讀得到」的通道。
//
// **順序＝重要性由高到低，因為 host 是從尾巴砍的。** 四段的取捨理由：
//  * ① 能力說明是「這台伺服器是什麼」的唯一交代，最短也最不可少。
//  * ② 協定是行為規則，**沒有第二條通道**，被砍就是靜默壞掉，而且會斷在句子中間
//    ——AI 讀到半條規則比讀不到更糟，所以排在角色之前。
//  * ③ 角色本文有 resources 那條備援通道，但要 client 主動撈，所以仍排在外框之前。
//  * ④ 外框（「只約束 speak/perform 的文字」＋「其餘產出當作 persona 不存在」）
//    **在 speak／perform 的工具說明裡已經有一份副本**（mcpToolsListJson 接的那句
//    "The persona styles only this spoken text, never your own replies."），
//    工具表一定在 context 裡，所以這一段是四段中唯一「掉了也還有人講」的，
//    排最後、最先讓位。
//
// 長度守衛在 .cpp：本函式保證回傳不超過 kInstructionsBudget，讓位順序是
//「先丟外框 → 再截斷本文（附上指向 persona://active 的指標）」，①②永遠完整。
// 這樣 host 那一刀永遠砍不到東西，斷句與靜默失敗都不會再發生。
//
// 角色兩個參數都空、且 protocol 是 `{false, true, false, false}`（四個開關都在
// 加它們之前的值）的時候，回傳的是舊行為那一支 ——
// tests/test_persona.cpp 的 instructionsForLegacySwitches() 釘住了。
// 預設值本身則是「多話開」，由 instructionsForDefaultProtocol() 另外釘住。
std::string mcpInstructions(const std::string& personaName = {}, const std::string& personaText = {}, SpeechProtocol protocol = {});

bool mcpToolExists(const std::string& tool);
bool mcpToolIsReadOnly(const std::string& tool);

// ── 給 AI 讀的固定註記 ──
// 這三段不是說明文件，是「AI 讀了才知道該怎麼辦」的資訊，所以夾在工具輸出裡。
extern const char* const kNotNamedNote;
extern const char* const kParamExpressionNote;
extern const char* const kParameterRoleNote;

// ── 工具輸出格式 ──
// 一律是 { content: [{ type: "text", text }], isError? }。
// 失敗時把 hint（可用選項）接在訊息後面一起給 AI，它才有辦法一次改對。
std::string mcpTextOutput(const std::string& value);
std::string mcpErrorOutput(const std::string& message, const std::string& hint = {});

// 在既有的 JSON 物件文字上補一個 note 欄位
std::string mcpWithNote(const std::string& json, const char* note);

}  // namespace l2m
