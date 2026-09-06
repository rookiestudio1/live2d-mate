#pragma once

// 設定 schema：預設值、驗證、序列化（手寫驗證，不靠第三方 schema 函式庫）。

#include <yyjson.h>

#include <optional>
#include <string>
#include <vector>

#include "json_doc.h"

namespace l2m {

// 位置預設點：AI 與選單都用這組語意名稱，不必知道螢幕解析度
inline const std::vector<std::string>& positionPresets() {
  static const std::vector<std::string> presets{"bottom-right", "bottom-left", "top-right", "top-left", "center"};
  return presets;
}

// 支援的 UI 語系。順序即為系統匣「語言」選單的排列順序。
inline const std::vector<std::string>& supportedLocales() {
  static const std::vector<std::string> locales{"en", "ja", "ko", "zh-CN", "zh-TW"};
  return locales;
}

inline constexpr double kMinScale = 0.2;
inline constexpr double kMaxScale = 3;
// 氣泡與角色的間距微調範圍（px）。schema 與設定視窗的 QSpinBox 共用這一份，
// 兩邊的界限才不會各走各的。實際還會被 placeBubble() 依角色高度再夾一次。
inline constexpr int kMinBubbleOffsetY = -400;
inline constexpr int kMaxBubbleOffsetY = 400;
inline constexpr int kDefaultMcpPort = 3777;
// 預設只綁本機；要開給區網得自己在設定視窗改，而且必須設 token
inline constexpr const char* kDefaultMcpHost = "127.0.0.1";

// 兩個 TTS 服務的**預設**位址；使用者自己啟動服務，我們只當 HTTP 用戶端。
// 位址本身在設定 → 語音 → 伺服器位址改得到（不必是本機，填得下區網 IP 與埠）
inline constexpr const char* kDefaultGptSovitsUrl = "http://127.0.0.1:9880";
inline constexpr const char* kDefaultVoiceboxUrl = "http://127.0.0.1:17493";
// 本機 LLM 服務的預設位址（Ollama 的 OpenAI-compatible 端點）；同上，只當用戶端
inline constexpr const char* kDefaultLlmBaseUrl = "http://127.0.0.1:11434/v1";

// GPT-SoVITS 的音色預設。
// 它的 /tts 需要參考音檔＋提示文字才能決定音色，又沒有語音清單 API，
// 所以「有哪些聲音」只能由使用者在設定檔裡列出來。
// 注意 refAudioPath 與權重路徑都是 GPT-SoVITS 主機端的路徑，不是本 app 的。
struct GptSovitsPreset {
  std::string id;
  std::string name;
  std::string refAudioPath;
  std::string promptText;
  std::string promptLang = "zh";
  // 給系統匣分類用，要用 zh-TW / ja-JP 這種帶連字號的形式
  std::string locale = "zh-TW";
  std::optional<std::string> gptWeights;
  std::optional<std::string> sovitsWeights;
};

struct GptSovitsConfig {
  std::string baseUrl = kDefaultGptSovitsUrl;
  // 合成文字的語言；'auto' 交給 GPT-SoVITS 自己判斷
  std::string textLang = "auto";
  std::string mediaType = "wav";  // wav | ogg | aac
  // CPU 推論可能要好幾十秒，逾時留寬一點
  int timeoutMs = 120000;
  std::vector<GptSovitsPreset> presets;
  // 以下直接透傳給 /tts，預設值與 api_v2.py 相同
  int topK = 15;
  double topP = 1;
  double temperature = 1;
  std::string textSplitMethod = "cut5";
  int batchSize = 1;
};

// Voicebox 本機服務。語音清單由它的 /profiles 提供，不必手動列。
struct VoiceboxConfig {
  std::string baseUrl = kDefaultVoiceboxUrl;
  // nullopt 代表依 app 的 UI 語系推導
  std::optional<std::string> language;
  // nullopt 代表用 profile 自己的預設引擎（qwen / kokoro / chatterbox …）
  std::optional<std::string> engine;
  std::optional<std::string> modelSize;
  int timeoutMs = 120000;
};

// 自訂 HTTP 語音端點。使用者自己填 URL、方法與參數樣板，${TEXT} 會被換成要唸的句子。
//
// 刻意只支援「回應 body 直接就是音訊」的端點：AudioPlayer 用的 miniaudio 只解得了
// wav / mp3 / flac，包一層 JSON 或回 ogg/opus 都會在解碼那一步靜默失敗，
// 使用者只看得到「氣泡有出來但沒聲音」。所以引擎會在播放前就把這幾種擋掉並回明確錯誤。
//
// 引擎專屬設定裡，這一組的 UI 最完整（設定視窗 → 語音 → 自定義語音設定）；
// gptsovits 與 voicebox 只有 baseUrl 有輸入框（同一頁的「伺服器位址」），
// 其餘欄位（presets、取樣參數…）仍然只能手改 config.json。
struct CustomTtsConfig {
  // 空字串代表尚未設定，isAvailable 直接回 false（不會出現在可選的引擎裡）
  std::string url;
  std::string method = "get";  // get | post | post-json
  // 參數樣板，必須含 ${TEXT}。get/post 是 query/form 形式，post-json 是 JSON 主體。
  std::string params;
  // 自訂標頭，一行一個 "Name: Value"
  std::string headers;
  // 雲端 API 偶爾也會跑很久，逾時比照另外兩個引擎留寬一點
  int timeoutMs = 120000;
};

struct ModelConfig {
  // 目前模型的 id（models 目錄下的相對路徑），nullopt 代表尚未選擇
  std::optional<std::string> current;
  double scale = 1;
  double opacity = 1;
  // 視窗左上角座標；nullopt 代表使用 preset
  std::optional<double> x;
  std::optional<double> y;
  std::string preset = "bottom-right";
};

struct InteractionConfig {
  bool lookAt = true;            // 視線追蹤滑鼠
  bool clickThrough = true;      // 透明區點擊穿透
  bool dragMove = true;          // 拖曳模型移動視窗
  bool wheelZoom = true;         // 滾輪縮放
  bool lockPosition = false;     // 鎖定位置（鎖定後 dragMove 失效）
  bool tapMotion = true;         // 點擊模型觸發隨機動作
  bool dragSwing = true;         // 拖曳視窗時觸發物理搖晃（頭髮甩動）
  bool speakFacingFront = true;  // 說話期間頭與雙眼正視前方（暫停游標視線追蹤）
};

struct TtsConfig {
  std::string engine = "edge";
  std::optional<std::string> voice;
  double rate = 1;  // 語速倍率，1 為正常
  // 音量增益倍率 0~2，1 為原始音量（語音分頁滑桿的 50%）。>1 是播放端軟體增益：
  // miniaudio 在裝置層把樣本乘上去、超出 ±1 由內建限幅收掉（見 media/audio_player.cpp）。
  double volume = 1;
  bool showBubble = true;
  // 沒有語音時，氣泡至少顯示的毫秒數
  double bubbleMinDuration = 2500;
  // 氣泡與角色的間距微調（px，可為負）。負值把氣泡往角色身上拉近，正值推遠；
  // 上下鏡像，翻到角色下方時同一個負值改成往上靠近（見 core/bubble_placement.h）。
  // 放在 tts 底下純粹是因為 showBubble／bubbleMinDuration 已經在這裡，
  // 氣泡的設定集中在一處比較好找 —— 它跟語音合成本身無關。
  int bubbleOffsetY = 0;
  // 氣泡最晚延後多久出現（ms）。氣泡平常等到「第一個聲音真的出來」才顯示，
  // 那一刻才是使用者眼裡角色開口的瞬間；但合成是非同步的，本機推論慢的時候
  // （GPT-SoVITS 約 1.7 秒固定開銷 ＋ 0.155 秒/字）會有一段字與聲音都沒有的空窗，
  // 服務卡死時甚至要等到 timeoutMs 才會知道失敗。所以超過這個時間就先把字放出來，
  // 聲音晚點到照樣播。0 ＝ 不等，回到「開始合成就顯示」的舊行為。
  // 預設 5 秒是量出來的：本機 GPT-SoVITS 上 16 字要 3.5～4.3 秒才出第一個音，
  // 設 2 秒的話每一句都會走保險而不是跟著聲音，等於白改；5 秒蓋得住約 21 字，
  // 剛好是平常講話的長度，更長的句子與真正的卡死才會提前把字放出來。
  double bubbleMaxDelay = 5000;
  // 氣泡的投影樣式："off" / "hard" / "soft"（core/bubble_shape.h 的 bubbleShadowIds()
  // 就是允許值本身）。預設 hard ＝ 硬邊的位移剪影，跟氣泡本身「不透明白底＋黑框」的
  // 漫畫調性一致；柔邊給偏好一般 UI 浮起感的人。
  std::string bubbleShadow = "hard";
  GptSovitsConfig gptsovits;
  VoiceboxConfig voicebox;
  CustomTtsConfig custom;
};

struct McpConfig {
  bool enabled = true;
  // 監聽位址。預設只綁 127.0.0.1；改成 0.0.0.0 或區網 IP 會讓同網段的人
  // 也連得進來，那時 token 是必填的。
  std::string host = kDefaultMcpHost;
  int port = kDefaultMcpPort;
  // 選填；有值時要求 Authorization: Bearer <token>
  std::optional<std::string> token;
  // ── 發話節奏（設定 → MCP 的四個核取方塊）──
  // 這兩個欄位不影響伺服器怎麼跑，只影響 initialize 送出的 instructions 文字，
  // 所以改了**不必重啟伺服器**，但已連線的 AI 要重新連線才讀得到
  //（POST-only 的 httplib 沒有 SSE，推不了 notifications/*）。
  // 產生文字的地方是 core/mcp_tool_specs.h 的 SpeechProtocol。
  //
  // 多話：AI 每吐一個段落就唸該段摘要。關＝退回「一則回覆只講一次」的舊行為。
  // 預設開 —— 角色是使用者的主要輸出通道，安靜才是需要特地去選的那一邊。
  // 它同時決定協定裡要不要提「省一趟往返」——關著才提，開著時反過來明講
  //「不准為了省往返而少講」（host 自己的 system prompt 幾乎都要求合併工具呼叫，
  // 協定裡再提一次省成本，AI 會推廣成「講少一點比較好」）。細節見 SpeechProtocol。
  bool talkative = true;
  // 回應結束時再 perform 一次收尾。instructions 本來就有這一條，
  // 這個開關只是讓使用者關得掉。
  bool notifyOnComplete = true;
  // MCP 的 speak 工具「排進佇列就回覆」，不等開口、更不等播完。
  //
  // 只作用在 speak 這個工具上（src/mcp/mcp_tools.cpp），**perform 不受影響** ——
  // perform 的內部步驟預設 speakWait=true，那是它多步時序的依據，
  // 提前回覆會讓表演整個亂序。
  //
  // 代價：合成失敗、沒有音效裝置這類錯誤 AI 就收不到了（氣泡與 log 仍然有）。
  // 回覆文字因此刻意寫成 Queued 而不是 Spoke，不讓 AI 以為那句已經唸完。
  bool speakNoWait = true;
  // 工作途中每送出一支工具呼叫就報一句，沒有例外。
  // 關＝退回 instructions 原本的「中間的工具鏈完全不旁白」。
  //
  // 粒度是「每支工具呼叫」不是「階段」，理由寫在 core/mcp_tool_specs.h 的
  // SpeechProtocol::announceSteps：階段的邊界是 AI 自己劃的，實測會被劃成
  // 一整段而全程安靜。語音積壓改用「每句只講幾個字」節流。
  // 預設開，理由同 talkative —— 角色是使用者的主要輸出通道。
  bool announceSteps = true;
};

// 閒置三個毫秒數的驗證範圍。schema 與設定視窗「一般」分頁的 QSpinBox 共用這一份
//（spinbox 以秒顯示，除以 1000 再用），兩邊的界限才不會各走各的 ——
// 與 kMinBubbleOffsetY / kMaxBubbleOffsetY 同一個做法。
inline constexpr int kMinIdleResetMs = 10000;
inline constexpr int kMaxIdleResetMs = 3600000;
inline constexpr int kMinIdlePerformAfterMs = 10000;
inline constexpr int kMaxIdlePerformAfterMs = 3600000;
inline constexpr int kMinIdlePerformIntervalMs = 5000;
inline constexpr int kMaxIdlePerformIntervalMs = 3600000;

// 閒置自動復原。
// 動作會自己播完，但表情、沒帶 hold_ms 的參數覆寫、循環動作不會 ——
// AI 設完那一輪就結束了，不會回來收尾，所以由 app 自己清。
struct IdleConfig {
  bool autoReset = true;
  int resetMs = 120000;           // 閒置多久才復原
  bool perform = true;            // 閒置表演開關
  int performAfterMs = 180000;    // 閒置多久進入表演
  int performIntervalMs = 60000;  // 表演間隔
};

struct AppSectionConfig {
  bool openAtLogin = false;
  bool alwaysOnTop = true;
  // 透明視窗出現黑底時的逃生門，需重新啟動
  bool disableHardwareAcceleration = false;
  // UI 語系；'auto' 依系統語系決定
  std::string locale = "auto";
};

// 角色描述。描述本文不放在這裡 —— 它是 personas/<名稱>.md 那個檔案，
// 這裡只記「目前套用的是哪一份」。規則與檔案存取在 core/persona.h。
struct PersonaConfig {
  // personas/<name>.md 的檔名去掉副檔名；null ＝ 不套用任何角色
  std::optional<std::string> current;
};

// 自主行為（閒置表演的腦袋，見 core/idle_director.h）。
// 這裡只放開關；台詞本身在角色 .md 檔裡，不進 config。
struct AutonomyConfig {
  bool enabled = true;     // 關掉＝退回舊行為（只播隨機動作）
  bool expression = true;  // 表演時可換表情（只挑有寫命名意義的）
  // 自主台詞（閒置嘀咕與歡迎詞）怎麼發聲。預設 bubble：桌寵在會議中自己開口
  // 是最快被解除安裝的方式，而且沒設定 TTS 引擎的使用者一樣看得到台詞。
  std::string speech = "bubble";  // off | bubble | voice
  bool greet = true;              // 啟動／休眠喚醒／離座回來時講 # Welcome Text
  bool pauseWhenAway = true;      // 離座就不表演（省電、省 GPU）
  int awayAfterMs = 600000;       // 幾毫秒沒有鍵盤滑鼠輸入算離座 [60000, 7200000]
  // 累計互動次數（摸摸、連點），**只增不減**。只用來解鎖（墊高說話機率），
  // 永遠不會鎖回去 —— 衰減式好感度的下場見 core/mood.h 檔頭。沒有 UI。
  int familiarity = 0;  // [0, 1000000000]
  // 久坐提醒：連續使用電腦超過 breakAfterMs 就講一句（PresenceTracker 的
  // LongSession 事件）。台詞來自角色 .md 的 # Break Reminder 區，或由 LLM 生成
  //（core/llm_behavior_prompt.h）—— 兩個來源都沒有時整輪安靜。
  // 預設關：沒寫台詞區的角色開了也只是沉默，明確 opt-in 才不會顯得壞掉。
  bool breakReminder = false;
  int breakAfterMs = 3600000;  // [600000, 14400000]
  // 隨機移動（閒置表演的微移動步驟，見 idle_director.cpp 的 move）。
  // 預設關：桌寵自己換位置對沒預期的使用者是「角色怎麼跑掉了」，
  // 明確 opt-in 才不會顯得壞掉（與 breakReminder 同一條理由）。
  bool move = false;
};

// 環境風。走 CubismPhysics 的 Wind 選項，與 interaction.dragSwing 是同一條管線
//（兩者相加，見 live2d/model_controller.cpp 第 6.5 步）。刻意不塞進 interaction ——
// 那一組是「使用者對角色做什麼」，環境風是環境本身，而且它需要非布林欄位。
// 預設關：不是每個模型都有 physics3.json（沒有的話風完全無效），而且風是持續
// 改變外觀 —— 更新之後所有人的角色突然開始飄不是好體驗。
struct WindConfig {
  bool enabled = false;
  std::string direction = "right";  // left | right
  double strength = 0.5;            // 0..1
};

// 內建 LLM（local 與 cloud 走同一套 OpenAI-compatible 協定，另支援 Anthropic 原生）。
//
// 欄位刻意全部攤平在同一層：ConfigStore::patch() 的合併只有兩層，
// llm.openai.apiKey 這種三層結構走不通（tts.custom 是整包覆蓋的前例，代價已知）。
// 兩個 provider 的欄位用前綴區分（anthropicApiKey…），切換 provider 不會弄丟另一邊。
//
// apiKey 明文存放：mcp.token 已有前例；OS 層加密會讓 config.json 失去「用記事本就改得動、
// 複製到別台機器就能用」的可攜性，而且防不了同一使用者帳號下的其他程式。設定頁有一行小字提醒。
struct LlmConfig {
  bool enabled = false;
  std::string provider = "openai";  // openai | anthropic
  // OpenAI-compatible：local（Ollama / LM Studio / llama.cpp server）與多數雲端共用
  // 這三欄，差別只是 baseUrl 指哪裡、apiKey 有沒有填。預設指向本機 Ollama。
  std::string baseUrl = kDefaultLlmBaseUrl;
  std::string apiKey;
  std::string model;
  // Anthropic Messages API 原生（system 獨立欄位、SSE 事件形狀不同，值得單獨一個引擎）
  std::string anthropicApiKey;
  std::string anthropicModel = "claude-opus-5";
  double temperature = 0.8;  // [0, 2]
  // 行為大腦的台詞都很短；聊天（未來階段）再視需要放大
  int maxTokens = 512;     // [16, 8192]
  int timeoutMs = 120000;  // [5000, 600000]；本機推論可能要跑很久，逾時比照 TTS 留寬
  // 自主行為大腦開關（獨立於 enabled：之後可以只開聊天不接管閒置）
  bool driveIdle = false;
  // 雲端計費節流：兩次 LLM 閒置規劃至少隔這麼久，中間輪次退回 IdleDirector。
  // local 使用者可調 0 讓 LLM 全量接管。[0, 3600000]
  int idleLlmCooldownMs = 300000;
};

// 當機報告的狀態。**只有一個欄位，而且刻意只記檔名不記時間**：
// 「這一份提示過了沒」是唯一需要跨啟動保存的東西，
// 檔名本身就帶時間戳（見 core/crash_report.h）
struct CrashConfig {
  std::string lastNotified;  // 最近一次提示過的 crash 檔名；空＝還沒提示過任何一份
};

// 天氣（Open-Meteo，免金鑰）。
//
// 欄位攤平在同一層的理由同 LlmConfig：ConfigStore::patch() 的合併只有兩層。
//
// **lastAlertKey 是執行期狀態，卻刻意存在 config 裡**：它是「哪一場壞天氣
// 已經講過了」，不跨啟動保存的話每次開機都會被同一場雨唸一次 ——
// 那正是這個功能最容易變得討人厭的地方。只記鍵不記時間，事件過去之後
// 鍵自然對不上，不需要任何清理邏輯（見 core/weather_alert.h）。
//
// 座標的 (0, 0) 代表「還沒定位」（weatherHasLocation）；那個點在幾內亞灣外海，
// 取捨與理由寫在 core/weather_http.h。
// 上下限與設定頁的 QSpinBox 共用（一般分頁的天氣群組），
// 兩邊各寫一份很快就會對不上 —— 比照 kMinIdleResetMs 那一組的慣例。
inline constexpr int kMinWeatherRainProbability = 10;
inline constexpr int kMaxWeatherRainProbability = 100;
inline constexpr int kMinWeatherLookaheadMinutes = 30;
inline constexpr int kMaxWeatherLookaheadMinutes = 720;

struct WeatherConfig {
  bool enabled = false;
  bool autoLocate = true;  // 沒有座標時用 IP 粗定位補一次
  double latitude = 0;     // [-90, 90]
  double longitude = 0;    // [-180, 180]
  std::string locationName;
  std::string unit = "c";       // c | f，只影響顯示
  int pollIntervalMs = 900000;  // [300000, 21600000]；Open-Meteo 的網格本來就是逐小時更新
  bool alertEnabled = true;     // 壞天氣主動提醒（關掉之後天氣只當 LLM 的背景知識）
  int rainProbability = 30;     // [10, 100] 降水機率門檻（預設值的校準過程見 core/weather_alert.h）
  int lookaheadMinutes = 120;   // [30, 720] 往前看多久
  std::string lastAlertKey;     // 最近一次真的講出去的預警 key（見上）
};

struct AppConfig {
  ModelConfig model;
  InteractionConfig interaction;
  TtsConfig tts;
  McpConfig mcp;
  IdleConfig idle;
  AppSectionConfig app;
  PersonaConfig persona;
  AutonomyConfig autonomy;
  WindConfig wind;
  LlmConfig llm;
  CrashConfig crash;
  WeatherConfig weather;
};

// 產生一份全預設值的設定
inline AppConfig defaultConfig() { return {}; }

// 驗證並解析設定 JSON；失敗時回 nullopt 並在 issues 裡放「路徑: 訊息」清單。
// 語意：缺欄位補預設、型別錯或超出範圍就失敗、未知欄位忽略。
std::optional<AppConfig> parseConfig(yyjson_val* root, std::vector<std::string>* issues = nullptr);

// 依 schema 欄位順序序列化。鍵順序是既有設定檔的合約：新欄位一律接在既有鍵的後面，
// 插在中間會讓既有 config.json 每次重寫都整份 diff。
//
// persona / autonomy / wind / llm / crash / weather 是後來加的 section，**刻意接在最後面**；
// 六者之間的順序（persona → autonomy → wind → llm → crash → weather）依實作先後排定，
// 出貨後即為合約，不能事後對調。
std::string serializeConfig(const AppConfig& config, bool pretty = false);

}  // namespace l2m
