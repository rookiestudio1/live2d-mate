#pragma once

// 應用層動作的唯一入口（系統匣、MCP、輔助視窗共用同一條路徑）。
// 隨里程碑逐步長出完整功能：
//   M3：模型切換、視窗操作、互動設定、全域游標輪詢
//   M4：參數覆寫、閒置行為　M5：TTS　M6：MCP
//
// 所有回傳 CommandResult 的方法，失敗時都會附上 hint（可用選項清單）——
// 這是給 AI 自我修正用的，系統匣的錯誤對話框也吃同一組字串。

#include <QElapsedTimer>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QTimer>

#include <filesystem>
#include <functional>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/annotations.h"
#include "core/command_result.h"
#include "core/config_store.h"
#include "core/drag_swing.h"
#include "core/fade_timing.h"
#include "core/idle.h"
#include "core/idle_director.h"
#include "core/model_types.h"
#include "core/persona.h"
#include "core/motion_builder.h"
#include "core/perform_step.h"
#include "core/gesture_detector.h"
#include "core/mood.h"
#include "core/presence_tracker.h"
#include "core/qt_timer_host.h"
#include "core/schedule_book.h"
#include "core/settings_layout.h"
#include "core/gaze_director.h"
#include "core/weather_alert.h"
#include "live2d/parameter_overlay.h"

class CharacterWindow;

namespace l2m {

class WindowManager;

class AppController : public QObject {
  Q_OBJECT

public:
  // 說話請求。engine/voice 省略時用設定裡的值。
  struct SpeakRequest {
    std::string text;
    std::optional<std::string> voice;
    std::optional<std::string> engine;
    std::optional<double> rate;
    // true 時等到播放結束才回呼
    bool wait = false;
    // 思考（MCP 的 think 工具）：思考泡泡＋帶殘響的聲音＋嘴巴不動
    bool thinking = false;
  };

  AppController(ConfigStore& config, CharacterWindow& window, WindowManager& windowManager, std::filesystem::path modelsDir, std::filesystem::path personasDir, std::filesystem::path memoryDir,
                QObject* parent = nullptr);
  ~AppController() override;

  // 啟動：搬移舊版命名檔、掃描模型、載入設定裡的目前模型、啟動游標輪詢與閒置計時
  void start();

  ConfigStore& config() { return config_; }
  const std::filesystem::path& modelsDir() const { return modelsDir_; }
  const std::filesystem::path& personasDir() const { return personasDir_; }
  // 長期記憶目錄（memory/<角色名>.md；core/persona_memory.h）。
  // LLM 行為大腦與角色分頁的「開啟記憶資料夾」共用這一個口
  const std::filesystem::path& memoryDir() const { return memoryDir_; }
  WindowManager& windowManager() { return windowManager_; }
  ParameterOverlay& overlay() { return overlay_; }

  // ── 模型 ──
  void rescan();
  const std::vector<ModelInfo>& models() const { return models_; }
  const ModelInfo* currentModel() const;
  CommandResult switchModel(const std::string& idOrName);
  // 收下一個 models 目錄以外的模型（Live2D Viewer 的「設定為桌寵」送過來的），
  // 加進清單之後立刻切過去。詳見 core/external_model.h 與 externalModels_。
  CommandResult adoptExternalModel(const std::filesystem::path& entryPath);
  // 最近一次模型載入失敗的原因，包成一般的 CommandResult 讓錯誤呈現只有一套。
  // 為什麼不從 switchModel() 的回傳值帶出來：那支在**淡出之前**就回覆了
  // （見它自己的註解），真正的載入要 400 ms 後才發生，失敗只能從
  // modelSwitchFinished(false) 之後回頭來問。
  CommandResult lastModelLoadResult() const;
  // 設定視窗的模型分頁在 modelSwitchFinished(false) 裡呼叫，宣告「這次失敗我來講」。
  // 沒人認領的那些（開機、MCP、系統匣發起的載入）才由 modelLoadFailedReporter 通知，
  // 免得使用者同時收到一個對話框和一個氣泡。
  void markModelLoadFailureReported() { modelLoadFailureReported_ = true; }
  // 開機時的第一次載入發生在 main() 建出系統匣**之前**，那時候沒有東西可以顯示訊息。
  // main() 掛上 reporter 之後呼叫這支把積著的那一次補送出去。
  void flushPendingModelLoadFailure();
  ModelAnnotations currentAnnotations() const;

  // ── 角色描述 ──
  // 檔案本身的讀寫在 core/persona.h；這裡只管「哪一份在用」與變更通知。
  void rescanPersonas();
  const std::vector<PersonaInfo>& personas() const { return personas_; }
  // config.persona.current；指到的檔案已經不在了就回空字串
  std::string activePersonaName() const;
  // 套用一份角色描述。name 給空字串＝取消套用。
  CommandResult usePersona(const std::string& name);
  // 現讀一份要推給 MCP 的快照（掃描 ＋ 逐份讀檔）
  PersonaSnapshot personaSnapshot() const;
  // 描述檔的內容被改過了（存檔／刪除），要重新推快照
  void notifyPersonaEdited();
  // 初次啟動（personas/ 目錄本來就不存在時）種一份預設秘書角色並直接套用。
  // 內容是給使用者看的 template；寫檔失敗就安靜放棄 —— 種不進去是小事，
  // 不值得擋住啟動。main() 在 start() 之前呼叫。
  void seedDefaultPersona();

  // index < 0 代表群組內隨機；priority 省略時用 Force（明確點名的動作不該被待機擋下）
  CommandResult playMotion(const std::string& group, int index = -1, std::optional<int> priority = std::nullopt);

  // nullopt 代表重設表情；holdMs 帶了就在時間到之後自己回復
  CommandResult setExpression(const std::optional<std::string>& name, std::optional<double> holdMs = std::nullopt);

  // ── 參數 ──
  const std::vector<ParameterInfo>& listParameters() const;
  // cdi3 的名稱與角色 ＋ Cubism 執行期的現值，合成一份 JSON 給 AI
  CommandResult parameterReport();
  CommandResult setParameters(const std::vector<SetParameterRequest>& requests);
  CommandResult resetParameters(const std::optional<std::vector<std::string>>& ids);
  CommandResult animate(const std::vector<Keyframe>& keyframes, const BuildMotionOptions& options);

  // ── 視線 ──
  // x/y 為 -1..1 的相對值；兩個都是 nullopt 代表回到正面並解除釘選
  CommandResult lookAt(std::optional<double> x, std::optional<double> y);

  // ── 視窗 ──
  CommandResult setScale(double scale);
  CommandResult setOpacity(double opacity);
  CommandResult moveToPreset(const std::string& preset);
  // preset 優先；否則用 x/y（0~1 視為工作區比例，大於 1 視為絕對螢幕座標）
  CommandResult moveTo(std::optional<double> x, std::optional<double> y, const std::optional<std::string>& preset);
  // 補間滑行到工作區比例位置（自主表演的微移動用；MCP 的 move 永遠瞬移）。
  // 滑行途中使用者開始拖曳會被中止（拖曳優先），照樣回成功。
  void glideTo(double xRatio, double yRatio, double durationMs, std::function<void(CommandResult)> done);
  CommandResult setVisible(bool visible);
  bool isVisible() const;
  CommandResult setAlwaysOnTop(bool enabled);
  void setOpenAtLogin(bool enabled);

  // ── 說話（實作由 M5 的 SpeechController 注入）──
  void speak(const SpeakRequest& request, std::function<void(CommandResult)> done);
  // 嘀咕：只出氣泡、不經過 TTS。自主台詞的 bubble 模式用（MCP 不走這裡）。
  void mutter(const std::string& text, std::function<void(CommandResult)> done);
  CommandResult stopSpeaking();
  // list_voices 工具的 JSON（引擎為 nullopt 代表全部）
  void listVoicesJson(const std::optional<std::string>& engine, std::function<void(std::string)> done);

  // ── 命名 ──
  // 開啟設定視窗的「模型」分頁（命名編輯器就在那一頁的右半）
  CommandResult openNaming();
  // 命名列的「試看」：用 motionKey 直接指定群組或某一個索引
  CommandResult previewMotionKey(const std::string& key);
  // 記錄某個動作／表情的意義，直接寫進模型資料夾裡的命名檔
  CommandResult setMeaning(AnnotationKind kind, const std::string& key, const std::string& meaning);
  // 「使用預設名稱」：把還沒命名的動作／表情用它們本身的名稱補齊
  //（規則在 core/annotations.h 的 fillDefaultNames）。寫檔一次、emit 一次，
  // 不走逐鍵 setMeaning —— 幾十個鍵就是幾十次磁碟寫入與幾十輪 modelsChanged
  CommandResult applyDefaultNames();

  // ── 進階 ──
  // 依序執行一連串動作；任何一步失敗就停下並回報是第幾步。非同步，完成時回呼。
  void perform(const std::vector<PerformStep>& steps, std::function<void(CommandResult)> done);
  // 延遲執行一個動作，立即回傳。app 結束時未觸發的排程會被清掉。
  CommandResult schedule(double delayMs, std::function<void()> run);

  // 目前的完整狀態（get_state 工具用）
  std::string getStateJson() const;

  // ── 閒置 ──
  // 有活動：重新計時。AI 的每一個非唯讀指令與滑鼠點擊都會經過這裡。
  void touchIdle();
  // 設定改了秒數或開關時立刻生效
  void refreshIdle();
  // 把 AI 留下的狀態收乾淨（表情、參數覆寫、循環動作、被釘住的視線）
  void resetToIdle();
  // 有 MCP 工具被呼叫（McpTools::invoke 的唯一進場點，24 個工具都經過）。
  // 開啟／重新給滿 kMcpQuietResetMs 的短復原倒數 —— AI 收工之後別讓表情
  // 掛在臉上到下一次互動。細節與「為什麼要等說完」見 core/idle.h。
  void notifyMcpCommand();

  // 設定變更後把執行期狀態套一遍（穿透、透明度、置頂等）
  void applyConfig();

  // ── 淡出 ──
  // 角色淡出 kFadeDurationMs 之後才執行 run。本來就看不見（隱藏中、或還沒有
  // 任何模型）時就地執行，不白等 400 ms。
  //
  // 時序一律靠計時器而不是「淡完了」的訊號：視窗隱藏或模型還沒載好時根本沒有
  // frame 送出，等訊號會永遠等不到、run 整個掉。動畫與時序各走各的，
  // 兩邊吃同一個 core/fade_timing.h 的常數。
  //
  // one-shot 且互相取代：淡出中再呼叫一次會撤掉前一個 run 並重新計時
  //（連按兩次切換模型時只有最後那一個真的載入）。
  void fadeOutThen(std::function<void()> run);

  // 目前的 UI 語系（config 的 auto 會依系統語系解析）
  std::string uiLocale() const;

  // 「找不到模型」時附的提示（系統匣與 MCP 共用）
  std::string modelListHint() const;
  // reporter 還沒掛上（開機）就先記著，等 flushPendingModelLoadFailure() 補送
  void reportModelLoadFailure(const CommandResult& result);

  // ── 由 main 注入的外部能力 ──
  // 說話：未接上時 speak() 會回明確的錯誤而不是假裝成功
  std::function<void(const SpeakRequest&, std::function<void(CommandResult)>)> speakHandler;
  // 嘀咕（只出氣泡）：SpeechController::mutter，與說話共用同一條佇列
  std::function<void(const std::string&, std::function<void(CommandResult)>)> mutterHandler;
  std::function<CommandResult()> stopSpeakingHandler;
  std::function<void(const std::optional<std::string>&, std::function<void(std::string)>)> voicesJsonProvider;
  // 正在說話（閒置判斷用）
  std::function<bool()> speakingProbe;
  // 開啟設定視窗並切到指定分頁（系統匣與 MCP 工具共用同一個入口）
  std::function<void(SettingsTab)> settingsOpener;
  // get_state 的擴充區塊（回傳 JSON 物件文字）：M5 的 TTS 與 M6 的 MCP 各自注入；
  // llm 由 LlmManager::stateJson 注入（同一個模式）
  std::function<std::string()> ttsStateJson;
  std::function<std::string()> mcpStateJson;
  std::function<std::string()> llmStateJson;
  // 口型：每幀 0..1 的嘴巴開合（由 AudioPlayer 的音量包絡提供）。
  // 模型每次載入都要重接一次，所以放在這裡由 onModelLoaded 統一套用。
  std::function<float()> mouthOpenSource;
  // 說話中：覆寫層要跳過口型參數，硬蓋上去會讓嘴巴一動也不動
  std::function<bool()> lipSyncActive;

  // 即將開始載入模型（同步、會把 GUI 執行緒卡住數秒）。
  // main() 用它在執行期換模型時叫出等待用的小卡片；啟動時的第一次載入
  // 由啟動畫面本身覆蓋，那邊會自己跳過。
  std::function<void()> onModelLoadStarted;

  // 新模型的視覺上下緣量好了（normalized 0..1，0=視窗頂）。main() 接到氣泡視窗，
  // 讓氣泡貼的是模型的頭頂而不是視窗頂端（見 core/bubble_placement.h 的
  // bubbleAnchorRect）。量不到時會補一次 (0, 1)，也就是退回以整個視窗為錨點。
  std::function<void(double topNormalized, double bottomNormalized)> onModelExtent;

  // 模型載入失敗、而且設定視窗沒有認領時，由誰告訴使用者（main() 接到系統匣氣泡）。
  // 這條路存在的理由是**開機**：config 指定的模型載不起來時，設定視窗的分頁是延遲
  // 建立的、那時根本還不存在，沒有這個掛鉤的話使用者只會看到一片空白加一行 log。
  std::function<void(const CommandResult&)> modelLoadFailedReporter;

  // 自主行為的規劃器。未注入（或 autonomy.enabled 關閉）時退回舊行為
  //（隨機播一個非 Idle 動作）。
  // **這就是未來換上 LLM 大腦時唯一要換的東西**：本機規則版（IdleDirector）
  // 同步呼叫 done，LLM 版隔一段網路往返再呼叫，這一側一個字都不用改。
  std::function<void(const IdleWorld&, const IdlePlanOptions&, std::function<void(std::vector<PerformStep>)>)> behaviorPlanner;

  // 現在的天氣（media/weather_service.h 的 snapshot）。未注入或 invalid ＝
  // 沒有天氣資料，預警整段不跑。**每 5 秒的在席心跳現讀** —— 服務那邊
  // 15 分鐘才更新一次，讀貴的東西不在這裡。
  std::function<WeatherSnapshot()> weatherSnapshot;

  // 這一輪的天氣抓取「有結果了嗎」——**成功與失敗都算有結果**。
  // 天氣沒啟用時永遠回 true（未注入時同）。只有啟動歡迎詞在等它，
  // 理由與等待上限見 greetOnLaunch()。
  std::function<bool()> weatherReady;

signals:
  // 模型清單或目前模型改變（系統匣靠它重建）
  void modelsChanged();
  // 其他狀態改變（勾選、可見性等）
  void stateChanged();
  // 一次模型切換真的結束了（ok=false 代表載入失敗）。
  // 設定視窗的模型分頁靠它收掉「載入中」與 WaitCursor —— switchModel() 現在
  // 會先淡出才載入，接它的回傳值等於在淡出都還沒開始時就解除。
  void modelSwitchFinished(bool ok);
  // 角色清單、套用中的角色、或某份描述的內容改變。
  // main() 接這個訊號把新的快照推給 McpHttpServer，設定分頁接它重畫。
  void personaChanged();

private:
  void pollCursor();
  void wheelZoom(int direction);
  void onModelLoaded(bool ok);
  // 新模型的命中遮罩落地後做一次腳底對齊（pendingBottomAlignY_ 為 one-shot 旗標）
  void onMaskUpdated();
  void clearExpressionHold();
  bool isBusy() const;
  void performIdle();
  // 把當下的模型能力與角色台詞組成 IdleWorld（純資料）餵給 behaviorPlanner。
  // occasion 決定台詞池：idle → # Dialogue List、welcome → # Welcome Text、
  // breakReminder → # Break Reminder、petted → # Petted、
  // weatherAlert → # Weather Alert（再依 weatherKind 過濾行首前綴）。
  IdleWorld buildIdleWorld(const std::string& occasion, std::optional<WeatherAlertKind> weatherKind = std::nullopt) const;
  // 把規劃出來的步驟交給 PerformRunner（bestEffort、PriorityNormal、閒置時鐘靜音）
  void runAutonomous(std::vector<PerformStep> steps);
  // 歡迎詞（三個觸發共用）：啟動、離座回來、休眠喚醒。reason 只進 log。
  void greet(const char* reason);
  // 啟動的那一次歡迎詞。第一幀畫上去之後不是直接講，而是**等天氣有結果**
  //（weatherReady），這樣 LLM 才有天氣可以拿來打招呼；沒啟用天氣時
  // weatherReady 永遠為真，行為與從前一模一樣。
  //
  // **等待有上限**（kLaunchGreetWeatherWaitMs）：IP 定位與抓預報是兩趟往返、
  // 各自的逾時加起來最壞 25 秒，為了天氣讓桌寵開機後悶那麼久不打招呼，
  // 比沒帶到天氣更糟。時間到就照講。
  void greetOnLaunch();
  // 久坐提醒（PresenceTracker 的 LongSession 事件）：occasion="breakReminder"，
  // 台詞來自 # Break Reminder 區或 LLM 生成
  void remindBreak();
  // 摸摸的台詞回應（表情回應在 onGesture 就地處理）：occasion="petted"
  void reactPetted();
  // 5 秒心跳：餵 PresenceTracker，接住回座／喚醒事件
  void samplePresence();
  // 壞天氣預警。**每次心跳都重算一次**（core/weather_alert.h 是純函式，很便宜）：
  // 不排隊、不記狀態，所以「離座期間產生的預警」自然變成「回座時若還沒開始
  // 就照樣講、已經開始就自動消失」。唯一的狀態是 config 的 weather.lastAlertKey。
  void checkWeatherAlert();
  // 把一筆指標樣本（視窗事件或游標輪詢）餵給手勢偵測並處理結果
  void feedGesture(const PointerSample& sample);
  void onGesture(const Gesture& gesture);
  // 視窗內座標的 hitTest（手勢要知道摸的是哪個部位）
  std::vector<std::string> hitAreasAt(const QPointF& local) const;
  // 熟悉度 +1（寫 config，10 秒節流 —— 撫摸連發不該把設定檔打成篩子）
  void bumpFamiliarity();
  // 螢幕熱插拔後把留在死座標空間的視窗撈回可見處（core/screen_rescue.h）
  void rescueOffscreenWindow();
  // 使用者開始拖曳時中止進行中的滑行（兩者搶視窗位置只會互相拉扯）
  void stopGlide();
  // 隨機挑一個非 Idle 群組來播。閒置表演與「點擊但沒有對應群組」共用同一套挑選，
  // 差別只在優先權：使用者點的要蓋掉正在播的，閒置表演只是填空檔。
  void playRandomMotion(int priority, bool allowIdleFallback);
  // 依群組名播一段動作。core/builtin_actions.h 合成出來的內建動作在 model3.json
  // 裡不存在，走 startMotion() 會被 GetMotionCount 當場回絕，所以在這裡分流成
  // buildMotion3 + playSynthesizedMotion（跟 MCP 的 animate 同一條路）。
  // **每一個播動作的進場點都要走這支** —— 直接呼叫 startMotion() 的話，
  // 隨機挑到內建群組就是靜默沒反應。
  bool startMotionByName(const std::string& group, int index, int priority);
  // 點擊的視覺回應（Tap / MultiTap 從 onGesture 進來）。
  // areas＝按下當下的命中區（99% 的模型是空的），local＝按下的視窗內座標，
  // 兩者一起交給 core/interaction_logic.h 的 pickTapMotion 挑那一段。
  void playTapMotion(const std::vector<std::string>& areas, const QPointF& local);
  // 模型未載入時的統一錯誤
  std::optional<CommandResult> requireModel() const;
  std::filesystem::path entryPathOf(const ModelInfo& model) const;
  // 把一個外部模型記進 externalModels_（已經記過就什麼都不做）。
  // 檔案不見了、或不是 Cubism 4 模型時回 false，讓 rescan() 自然把它從清單漏掉。
  bool rememberExternalModel(const std::string& id);

  ConfigStore& config_;
  CharacterWindow& window_;
  WindowManager& windowManager_;
  std::filesystem::path modelsDir_;
  std::filesystem::path personasDir_;
  std::filesystem::path memoryDir_;

  std::vector<ModelInfo> models_;
  // 本次執行期間收下的外部模型（id 是絕對路徑）。rescan() 每次把它們接在
  // 掃描結果**尾端** —— 排前面的話 resolveModel 的「名稱包含」比對會先命中
  // 外部模型，models 目錄裡同名的那隻反而叫不動。
  //
  // 刻意**不寫進 config**：關掉 app 就沒了，符合「從檢視器試用一隻模型」的定位。
  // 唯一活得過重開的是「關掉時正在用的那一隻」—— 它存在 config 的 model.current，
  // 由 rescan() 開頭那段補回這個清單裡（不補的話桌寵重開會掉回第一隻內建模型，
  // 整個功能就不成立了）。
  std::vector<ModelInfo> externalModels_;
  // 上一條讀不到的外部路徑。rescan() 掛在 list_models 上（AI 想叫幾次就叫幾次），
  // 沒記的話每一次都會對著同一條死路徑重跑解析並印警告。
  std::string failedExternalId_;
  std::vector<PersonaInfo> personas_;
  std::string currentModelId_;

  ParameterOverlay overlay_;

  // 拖曳搖晃：速度濾波是 core 純邏輯，時間由 swingClock_ 注入（建構子 start）
  DragSwing dragSwing_;
  QElapsedTimer swingClock_;

  // 閒置：計時抽象在 core/idle.h，這裡提供 QTimer 版的計時器與注入的行為
  QtTimerHost timers_;
  std::unique_ptr<IdleReset> idleReset_;
  std::unique_ptr<IdlePerformer> idlePerformer_;
  // MCP 收工後的 5 秒短復原。與 idleReset_ 併存：那條是「沒人理它」的兩分鐘，
  // 這條只在 AI 下過指令時倒數，兩條收的都是 resetToIdle()
  std::unique_ptr<McpQuietReset> mcpQuiet_;

  // > 0 時 isBusy() 為真：進行中的 perform。步驟之間的 wait 不會經過任何活動
  // 進場點，不擋住的話 5 秒復原會在表演中途把前面設好的表情抽掉。
  // 加減一律走 PerformBusy（app_controller.cpp 的 RAII 守衛）。
  int performActive_ = 0;

  // set_expression 帶了 hold_ms 時的一次性計時器（-1 代表沒有）
  int expressionHoldId_ = -1;
  // schedule 排出去、還沒觸發的計時器與到期時刻。isBusy() 只把「馬上要發生」
  // 的排程算成忙 —— 修掉「AI 排一個 6 小時排程，閒置系統凍結 6 小時」的 bug。
  ScheduleBook schedule_;
  // schedule_ 到期時刻與 lastTouchMs_ 的時基（建構子 start，永不重啟）
  QElapsedTimer runClock_;
  // 最後一次真人／AI 活動的時刻（runClock_ 時基），閒置分級用
  double lastTouchMs_ = 0;
  // 在席偵測：5 秒心跳餵 OS 閒置訊號，離座時暫停表演、回座／喚醒時打招呼
  PresenceTracker presence_;
  QTimer presenceTimer_;
  // 手勢（摸摸、連點）與心情餘韻；熟悉度存在 config（只增不減）
  GestureDetector gesture_;
  MoodState mood_;
  double lastFamiliarityMs_ = -1;
  // 游標輪詢裡的 hitTest 節流（撫摸偵測 ~10 Hz 就夠，25 Hz 全做太貴）
  double lastPetSampleMs_ = -1;
  // 進行中的微移動滑行（QVariantAnimation，自持 DeleteWhenStopped）
  QPointer<QObject> activeGlide_;
  // 啟動的那一次歡迎詞只講一遍（firstFrameRendered 每次換模型都會發）
  bool launchGreeted_ = false;
  // 啟動歡迎詞為了等天氣已經等了多久（見 greetOnLaunch 的上限）
  int launchGreetWaitedMs_ = 0;
  // 這次執行期間**真的講出去過**的天氣預警鍵。只留最後一個會讓兩件都還沒
  // 發生的壞天氣互相把對方的記錄擠掉而無限乒乓（core/weather_alert.h 的規則 4）。
  // config 的 weather.lastAlertKey 只負責跨重啟，開機時併進這個集合。
  std::set<std::string> announcedWeatherKeys_;
  // 預警的規劃還在飛（LLM 版要等網路往返）。沒有這道閘的話 5 秒後的下一次
  // 心跳會對同一場天氣再發一次請求
  bool weatherAlertPending_ = false;
  // 上一次送出預警規劃的時刻（runClock_ 時基；-1 ＝ 還沒送過）。
  // 規劃沒產出東西時不記鍵（見 checkWeatherAlert），所以要另外節流，
  // 否則 LLM 一直失敗會變成每 5 秒打一次 API
  double lastWeatherPlanMs_ = -1;
  // 距上次歡迎詞的冷卻（runClock_ 時基；-1 = 還沒講過）。
  // Resumed 與 CameBack 可能背靠背觸發，不冷卻會連講兩句。
  double lastGreetMs_ = -1;
  // 摸摸台詞的冷卻（runClock_ 時基）：連續撫摸只在第一下講話，
  // 不然一次摸摸會噴出一串台詞
  double lastPetLineMs_ = -1;

  // > 0 時 touchIdle() 直接返回：自主表演期間，桌寵自己做的動作不算「有人來了」。
  // 不加這個守衛，表演的第一步就會把閒置時鐘打掉（playMotion 等 7 個進場點都會
  // touchIdle），「每 intervalMs 一輪」退化成「每 performAfterMs 一輪」，
  // IdleReset 也被順手重置 —— 與 resetToIdle() 註解裡刻意避開的是同一種
  // 自我觸發迴圈。加減一律走 RAII（見 app_controller.cpp 的 IdleSuppress）。
  int idleSuppress_ = 0;

  // 視線被 MCP 釘住時，游標輪詢不可以再蓋掉它
  bool lookAtPinned_ = false;

  // 視線的追蹤權重（core/gaze_director.h）：pollCursor 每輪 tick 一次，
  // 拿回來的權重在「正前方」與「游標」之間插值。滑鼠靜止後回正、說話期間
  // 正視前方、lookAtPinned_ 的釘選，全部匯進這一個權重 —— 各自寫 focus
  // 會在 25 Hz 上逐幀互相覆寫。
  GazeDirector gaze_;

  // 腳底對齊：切換模型時記下舊模型視覺底部的螢幕 Y（存絕對座標 —— 即使等待
  // 遮罩的 1~2 幀間使用者拖了視窗，語意仍是「對齊切換前的底部」）。
  // nullopt 代表沒有待處理的對齊；啟動時第一次載入量不到舊模型，自然跳過。
  std::optional<int> pendingBottomAlignY_;
  bool modelLoadFailureReported_ = false;
  std::optional<CommandResult> pendingModelLoadFailure_;
  // 新模型遮罩全透明（淡入中）時的重試額度，用完就放棄對齊
  int alignRetries_ = 0;

  // 氣泡錨點：新模型載入後，等遮罩落地量一次視覺上下緣（one-shot）。
  // 與腳底對齊共用 onMaskUpdated 的入口，但兩者的觸發時機不同 ——
  // 對齊只在「切換」模型時要做（要有舊模型的底部可對），量測則每次載入都要。
  bool pendingExtentMeasure_ = false;
  // 新模型遮罩全透明（淡入中）時的重試額度，用完就放棄量測
  int extentRetries_ = 0;
  // 兩個額度共用同一個數：「遮罩全透明時最多再等幾輪」對兩件事是同一個意思，
  // 分成兩個數字只會改一邊忘一邊。**能等多久隨遮罩節流而變** —— 游標遠離時
  // 200ms 一更（約 2 秒），游標在視窗上時每幀都更（約 0.17 秒）。
  static constexpr int kMaskRetries = 10;

  // 淡出計時器（single-shot）與它到期時要跑的動作。
  // 三個來源共用同一支：切換模型、隱藏角色、離開程式。
  QTimer fadeTimer_;
  std::function<void()> pendingAction_;
  // 淡出中「意圖上的可見性」。只有待處理動作真的是隱藏時才有值 ——
  // 否則 AI 連著呼叫 set_visible(false) 與 get_state，會在那 400 ms 內
  // 讀到 visible: true。
  std::optional<bool> pendingVisible_;

  // 全域游標輪詢：40ms、位移門檻 2px。視窗外也要追（視線跟隨），
  // 所以不能靠視窗滑鼠事件；穿透開關的判斷也掛在這裡。
  QTimer cursorTimer_;
  QPoint lastCursor_;
};

}  // namespace l2m
