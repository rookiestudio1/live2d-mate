#include "app_controller.h"

#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QEasingCurve>
#include <QGuiApplication>
#include <QLocale>
#include <QPointer>
#include <QRandomGenerator>
#include <QScreen>
#include <QStyleHints>
#include <QTime>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

#include "../platform/autostart.h"
#include "../platform/user_idle.h"
#include "core/ambient_wind.h"
#include "core/autonomy_gates.h"
#include "core/day_period.h"
#include "core/idle_signal.h"
#include "core/persona_doc.h"
#include "core/screen_rescue.h"
#include "perform_runner.h"
#include "../windows/character_window.h"
#include "../windows/window_manager.h"
#include "core/bottom_align.h"
#include "core/config_schema.h"
#include "core/i18n.h"
#include "core/interaction_logic.h"
#include "core/json_doc.h"
#include "core/model_commands.h"
#include "core/config_patch.h"
#include "core/external_model.h"
#include "core/model_scanner.h"
#include "core/persona.h"
#include "core/persona_default.h"
#include "core/string_util.h"
#include "live2d/action_player.h"
#include "live2d/model_controller.h"

namespace l2m {

namespace {

// 游標輪詢：40ms、位移門檻 2px
constexpr int kCursorIntervalMs = 40;
constexpr int kCursorThresholdPx = 2;

// schedule 的延遲上限：6 小時。再長的排程只會在關機時默默消失。
constexpr double kMaxScheduleMs = 6 * 60 * 60 * 1000;

// 閒置表演不挑待機群組 —— 那個本來就會自己接
constexpr const char* kIdleGroup = "Idle";

// 在席偵測的心跳間隔
constexpr int kPresenceIntervalMs = 5000;
// 兩次歡迎詞之間的最短間隔：Resumed 與 CameBack 可能背靠背觸發
constexpr double kGreetCooldownMs = 60000;
// 啟動歡迎詞等淡入結束再開口（kFadeDurationMs 400 再留一點餘裕）
constexpr int kLaunchGreetDelayMs = 1200;
// 啟動歡迎詞最多為天氣多等這麼久，以及等待期間的輪詢間隔（見 greetOnLaunch）
constexpr int kLaunchGreetWeatherWaitMs = 10000;
// 兩次預警規劃之間至少隔這麼久。規劃沒產出台詞時**刻意不記鍵**（那一場才不會
// 被永遠跳過），所以沒有這道閘的話，LLM 一直失敗就會變成每 5 秒打一次 API
constexpr double kWeatherPlanRetryMs = 60000;
constexpr int kLaunchGreetPollMs = 250;
// 撫摸偵測的 hitTest 節流（游標輪詢 25 Hz，命中測試 ~10 Hz 就夠）
constexpr double kPetSampleIntervalMs = 100;
// 摸摸的表情反應維持多久
constexpr double kPetExpressionHoldMs = 8000;
// 熟悉度寫入 config 的節流：一次撫摸只記一筆，不把設定檔打成篩子
constexpr double kFamiliarityIntervalMs = 10000;
// 摸摸台詞的冷卻：連續撫摸只在第一下講話，一次摸摸不該噴一串台詞
constexpr double kPetLineCooldownMs = 90000;

double clampValue(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

// 把一段 JSON 文字當成值塞進物件（用來把已經序列化好的區塊合併進 state）
void addRawJson(yyjson_mut_doc* doc, yyjson_mut_val* obj, const char* key, const std::string& text) {
  auto parsed = jsonu::Doc::parse(text);
  if (!parsed || !parsed->root()) {
    yyjson_mut_obj_add_null(doc, obj, key);
    return;
  }
  yyjson_mut_obj_add_val(doc, obj, key, yyjson_val_mut_copy(doc, parsed->root()));
}

// 一段期間 +1／-1 的 RAII 計數守衛。
// 忘了減的症狀是「桌寵再也不表演，重開才好」，極難查 —— 所以不用裸計數器：
// 做成 RAII 並用 shared_ptr 交給 PerformRunner 的 done lambda 捕獲，
// 執行器跑完 deleteLater() 時自然釋放，任何早退路徑都蓋得到。
class CounterGuard {
public:
  explicit CounterGuard(int& counter) : counter_(counter) { ++counter_; }
  ~CounterGuard() { --counter_; }
  CounterGuard(const CounterGuard&) = delete;
  CounterGuard& operator=(const CounterGuard&) = delete;

private:
  int& counter_;
};

// 自主表演期間把閒置時鐘靜音（idleSuppress_）：桌寵自己做的動作不算「有人來了」
using IdleSuppress = CounterGuard;
// perform 進行中算「忙」（performActive_）：步驟之間的 wait 沒有任何活動訊號，
// 不擋住的話復原會在表演中途把前面幾步設好的表情抽掉。
// 兩個名字語意相反，刻意分開以免讀錯。
using PerformBusy = CounterGuard;

// 診斷用：L2M_IDLE_TURBO=<ms> 把閒置表演的進場與間隔都改成這個毫秒數
//（只影響記憶體、絕不寫 config），設了同時把每輪規劃的步驟 qDebug 出來。
// 與 L2M_SAY / L2M_PROFILE 同一套慣例。
double idleTurboMs() {
  static const double value = qEnvironmentVariable("L2M_IDLE_TURBO").toDouble();
  return value;
}

// 手勢時窗跟著 OS 走：單擊的遞延時間＝連點的判定時窗＝系統雙擊間隔
//（使用者在系統設定調過雙擊速度，桌寵的判定就該跟著變）。
GestureDetector::Options gestureOptionsFromOs() {
  GestureDetector::Options options;
  if (const auto* hints = QGuiApplication::styleHints()) {
    options.multiTapWindowMs = std::max(1, hints->mouseDoubleClickInterval());
  }
  return options;
}

std::string escapeJsonString(const std::string& raw) {
  std::string out;
  out.reserve(raw.size() + 8);
  for (const char c : raw) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
    }
  }
  return out;
}

}  // namespace

AppController::AppController(ConfigStore& config, CharacterWindow& window, WindowManager& windowManager, std::filesystem::path modelsDir, std::filesystem::path personasDir,
                             std::filesystem::path memoryDir, QObject* parent)
  : QObject(parent),
    config_(config),
    window_(window),
    windowManager_(windowManager),
    modelsDir_(std::move(modelsDir)),
    personasDir_(std::move(personasDir)),
    memoryDir_(std::move(memoryDir)),
    timers_(this),
    gesture_(gestureOptionsFromOs()) {
  swingClock_.start();
  runClock_.start();
  // 互動回呼接線：CharacterWindow 只收事件，決策都在這裡
  window_.onDragBy = [this](int dx, int dy) {
    // 使用者的拖曳優先：微移動的滑行立刻讓路，兩者搶位置只會互相拉扯
    stopGlide();
    // 拖曳搖晃吃「實際套用的位移」：貼螢幕邊被 clamp 住時視窗沒動，就不該甩頭髮
    const QPoint applied = windowManager_.moveBy(dx, dy);
    if (config_.get().interaction.dragSwing) {
      dragSwing_.addSample(applied.x(), applied.y(), double(swingClock_.nsecsElapsed()) / 1e6);
    }
  };
  window_.onWheelZoom = [this](int direction) { wheelZoom(direction); };
  // 使用者摸了角色也算「有活動」，閒置復原與閒置表演都要重新倒數
  window_.onActivity = [this] { touchIdle(); };
  window_.dragEnabled = [this] {
    const auto& cfg = config_.get();
    return cfg.interaction.dragMove && !cfg.interaction.lockPosition;
  };
  window_.wheelZoomEnabled = [this] { return config_.get().interaction.wheelZoom; };
  window_.clickThroughEnabled = [this] { return config_.get().interaction.clickThrough; };
  // 按下／放開餵手勢偵測（單擊、連點）；hover 的撫摸樣本由 pollCursor 餵。
  // 點擊動作也從這條出去（onGesture 收 Tap / MultiTap 才播）—— 單擊遞延判定
  // 需要「有沒有下一擊」的全貌，視窗事件層看不到。
  window_.onPointerEvent = [this](bool pressed, QPointF local) {
    const QRect bounds = windowManager_.bounds();
    PointerSample sample;
    sample.x = local.x();
    sample.y = local.y();
    // 位移判定要用螢幕座標：dragMove 期間視窗跟著游標跑，視窗內座標不動
    sample.screenX = local.x() + bounds.x();
    sample.screenY = local.y() + bounds.y();
    sample.nowMs = static_cast<double>(runClock_.elapsed());
    sample.pressed = pressed;
    sample.areas = hitAreasAt(local);
    sample.inside = !sample.areas.empty() || window_.isOpaqueAt(local);
    feedGesture(sample);
  };

  // 模型載入完成才有參數表可以綁
  connect(&window_, &CharacterWindow::modelLoaded, this, &AppController::onModelLoaded);
  // 腳底對齊等的是「新模型的遮罩落地」—— firstFrameRendered 時遮罩還在途中
  connect(&window_, &CharacterWindow::maskUpdated, this, &AppController::onMaskUpdated);

  IdleReset::Deps resetDeps;
  resetDeps.delayMs = [this] {
    const auto& idle = config_.get().idle;
    return idle.autoReset ? static_cast<double>(idle.resetMs) : 0.0;
  };
  resetDeps.isBusy = [this] { return isBusy(); };
  resetDeps.reset = [this] { resetToIdle(); };
  idleReset_ = std::make_unique<IdleReset>(timers_, std::move(resetDeps));

  // MCP 收工之後的短復原（core/idle.h 的 McpQuietReset，kMcpQuietResetMs = 5 秒）。
  // 與 idleReset_ 併存而不是取代：那條管的是「沒人理它」的兩分鐘，使用者互動也算；
  // 這條只在 AI 真的下過指令時倒數，觸發一次就熄火。收的是同一個 resetToIdle()，
  // 復原語意只有一份。
  mcpQuiet_ = std::make_unique<McpQuietReset>(timers_, McpQuietReset::Deps{[this] { return isBusy(); }, [this] { resetToIdle(); }});

  IdlePerformer::Deps performDeps;
  performDeps.idleMs = [this] {
    const auto& idle = config_.get().idle;
    if (!idle.perform) return 0.0;
    if (idleTurboMs() > 0) return idleTurboMs();
    return static_cast<double>(idle.performAfterMs);
  };
  performDeps.intervalMs = [this] {
    if (idleTurboMs() > 0) return idleTurboMs();
    return static_cast<double>(config_.get().idle.performIntervalMs);
  };
  performDeps.isBusy = [this] {
    if (isBusy()) return true;
    // 離座就跳過該輪但仍停在 Performing（不 stop）：使用者一回來立刻有反應
    return config_.get().autonomy.pauseWhenAway && presence_.state() == PresenceState::Away;
  };
  performDeps.perform = [this] { performIdle(); };
  idlePerformer_ = std::make_unique<IdlePerformer>(timers_, std::move(performDeps));

  // 在席偵測：5 秒心跳。牆上時鐘跳躍（休眠）與回座事件都在 samplePresence 裡判
  presenceTimer_.setInterval(kPresenceIntervalMs);
  connect(&presenceTimer_, &QTimer::timeout, this, &AppController::samplePresence);

  // 啟動的歡迎詞：等第一幀真的畫上螢幕（對著還沒出現的角色講話很怪），
  // 再等淡入結束，最後還要等天氣有結果（greetOnLaunch）。
  // firstFrameRendered 每次換模型都會發，只有第一次算啟動。
  connect(&window_, &CharacterWindow::firstFrameRendered, this, [this] {
    if (launchGreeted_) return;
    launchGreeted_ = true;
    QTimer::singleShot(kLaunchGreetDelayMs, this, [this] { greetOnLaunch(); });
  });

  // 螢幕熱插拔：等 OS 把工作區都排定了再檢查（拔螢幕的瞬間清單還在變動）
  const auto scheduleRescue = [this] { QTimer::singleShot(1000, this, [this] { rescueOffscreenWindow(); }); };
  connect(qGuiApp, &QGuiApplication::screenAdded, this, scheduleRescue);
  connect(qGuiApp, &QGuiApplication::screenRemoved, this, scheduleRescue);

  cursorTimer_.setInterval(kCursorIntervalMs);
  connect(&cursorTimer_, &QTimer::timeout, this, &AppController::pollCursor);

  fadeTimer_.setSingleShot(true);
  connect(&fadeTimer_, &QTimer::timeout, this, [this] {
    // 先取走再執行：動作本身可能又排一次淡出（例如載入完成後的收尾）
    std::function<void()> action;
    action.swap(pendingAction_);
    if (action) action();
  });
}

AppController::~AppController() {
  // 先拆掉閒置計時器再讓 timers_ 解構，免得回呼打到已經半毀的自己
  idleReset_.reset();
  idlePerformer_.reset();
  mcpQuiet_.reset();
  for (const int id : schedule_.ids()) timers_.clearTimeout(id);
  clearExpressionHold();
}

void AppController::start() {
  rescan();
  rescanPersonas();

  // 舊版把命名集中放在 userData，搬進各模型資料夾後要重掃才讀得到
  std::vector<std::string> ids;
  ids.reserve(models_.size());
  for (const auto& m : models_) ids.push_back(m.id);
  const int migrated = migrateLegacyAnnotations(modelsDir_.parent_path() / "annotations.json", modelsDir_, ids);
  if (migrated > 0) {
    qInfo() << "[annotations] 已搬移舊版命名檔:" << migrated;
    rescan();
  }

  // 設定裡的目前模型優先；沒有（或已被刪除）就取掃描結果的第一個
  const auto& cfg = config_.get();
  const ModelInfo* target = nullptr;
  if (cfg.model.current.has_value()) target = resolveModel(models_, *cfg.model.current);
  if (!target && !models_.empty()) target = &models_.front();

  if (target) {
    switchModel(target->id);
  } else {
    qWarning() << "[models]" << QString::fromStdString(modelListHint());
  }

  cursorTimer_.start();
  presenceTimer_.start();
  // 上次關機後螢幕配置可能變了（拔了外接螢幕），啟動先撈一次
  rescueOffscreenWindow();
  touchIdle();
}

// ── 模型 ────────────────────────────────────────────────

void AppController::rescan() {
  models_ = scanModels(modelsDir_);

  // 關掉 app 的當下正在用的外部模型只留在 config 裡，開機第一次掃描要把它補回來。
  // 不補的話下面的 stillThere 會判它不在而清掉 currentModelId_，start() 就掉回
  // 第一隻內建模型 —— 「從檢視器設過的桌寵」重開之後會自己變回別人。
  const std::optional<std::string>& saved = config_.get().model.current;
  if (saved.has_value() && isExternalModelId(*saved)) rememberExternalModel(*saved);

  // 一律接在掃描結果**尾端**：排前面的話 resolveModel 的「名稱包含」比對會先
  // 命中外部模型，models 目錄裡同名的那隻反而叫不動（同 builtin_actions 的理由）
  models_.insert(models_.end(), externalModels_.begin(), externalModels_.end());

  const bool stillThere = std::any_of(models_.begin(), models_.end(), [this](const ModelInfo& m) { return m.id == currentModelId_; });
  if (!stillThere) currentModelId_.clear();
  emit modelsChanged();
}

bool AppController::rememberExternalModel(const std::string& id) {
  const auto known = std::find_if(externalModels_.begin(), externalModels_.end(), [&id](const ModelInfo& m) { return m.id == id; });
  if (known != externalModels_.end()) return true;
  // 失敗的那一個也要記住：rescan() 掛在 list_models 上，AI 想叫幾次就叫幾次，
  // 而讀不到的那條路徑每次都會重跑一輪 openModelAssets ＋ 解析並印一行警告。
  // 只記一個就夠 —— 會走到這裡的來源只有 config.model.current，同時只有一條。
  if (id == failedExternalId_) return false;

  std::optional<ModelInfo> info = describeModel(std::filesystem::u8path(id));
  if (!info) {
    qWarning() << "[models] 外部模型讀不到:" << QString::fromStdString(id);
    failedExternalId_ = id;
    return false;
  }
  // describeModel 的 id 是傳進去的路徑原樣；統一成正規化後的形式，
  // 同一個檔案用兩種寫法進來才不會在清單上變成兩隻
  info->id = id;
  externalModels_.push_back(std::move(*info));
  return true;
}

CommandResult AppController::adoptExternalModel(const std::filesystem::path& entryPath) {
  // models 目錄底下的模型**不是**外部模型。檢視器裡最順手的瀏覽目標就是那個目錄，
  // 不先還原成相對 id 的話，同一隻會用絕對路徑再收一次而在清單上變成兩隻同名的
  //（設定頁只顯示 name，看不出差別），而且 config.model.current 記的是絕對路徑，
  // 每次重開都會再長回來。判定規則與大小寫處理見 core/external_model.h。
  if (const std::optional<std::string> inside = modelsDirRelativeId(entryPath, modelsDir_)) {
    rescan();
    return switchModel(*inside);
  }

  const std::string id = externalModelId(entryPath);
  // 重試一條先前失敗過的路徑：清掉記錄，讓它真的再讀一次
  //（檔案可能是隨身碟重新插上、或使用者剛把它放回去）
  if (id == failedExternalId_) failedExternalId_.clear();
  if (!rememberExternalModel(id)) {
    return CommandResult::failure("Cannot read model \"" + id + "\"", "Pick a *.model3.json, or a *.zip that contains one.");
  }
  rescan();
  return switchModel(id);
}

// ── 角色描述 ──────────────────────────────────────────
//
// 這裡只有「哪一份在用」與變更通知；名稱規則、長度上限、檔案讀寫都在
// core/persona.h（那些是純規則，要能單獨測）。

void AppController::rescanPersonas() {
  auto found = scanPersonas(personasDir_);
  // 清單真的變了才發訊號（比照 ConfigStore 的「序列化結果有變才 emit」）。
  // 角色分頁的 refresh() 會自己叫這個函式，好讓使用者把 .md 丟進資料夾之後
  // 一開設定就看得到；無條件 emit 的話就是
  // refresh → personaChanged → refreshIfActive → refresh … 繞不完。
  const bool same = found.size() == personas_.size() && std::equal(found.begin(), found.end(), personas_.begin(), [](const PersonaInfo& a, const PersonaInfo& b) { return a.name == b.name; });
  personas_ = std::move(found);
  if (!same) emit personaChanged();
}

std::string AppController::activePersonaName() const {
  const auto& current = config_.get().persona.current;
  if (!current || current->empty()) return {};
  // 使用者可能直接在檔案總管裡把檔案刪掉了；設定裡的值不代表它還在
  const bool stillThere = std::any_of(personas_.begin(), personas_.end(), [&current](const PersonaInfo& p) { return p.name == *current; });
  return stillThere ? *current : std::string();
}

CommandResult AppController::usePersona(const std::string& name) {
  if (name.empty()) {
    config_.patch(nullPatch("persona", "current"));
    emit personaChanged();
    return CommandResult::success();
  }

  const bool exists = std::any_of(personas_.begin(), personas_.end(), [&name](const PersonaInfo& p) { return p.name == name; });
  if (!exists) {
    return CommandResult::failure("No persona named \"" + name + "\"", personaListHint(personas_, personasDir_.u8string()));
  }

  config_.patch(stringPatch("persona", "current", name));
  emit personaChanged();
  return CommandResult::success();
}

PersonaSnapshot AppController::personaSnapshot() const { return loadPersonaSnapshot(personasDir_, activePersonaName()); }

void AppController::notifyPersonaEdited() { emit personaChanged(); }

void AppController::seedDefaultPersona() {
  // 「初次啟動」的判定是**掃不到任何角色檔**，而不是「personas/ 目錄不存在」——
  // 目錄早在每次啟動的 create_directories 就被建出來了（歷來版本皆然），
  // 拿目錄存在與否當標記的話，既有安裝永遠種不進去。
  // 已知代價：使用者把 .md 全刪光後，下次啟動會把 Default 種回來
  //（想不套用角色請按角色分頁的「不使用角色」，或直接改掉 Default.md 的內容）。
  rescanPersonas();
  if (!personas_.empty()) return;

  const std::string text = defaultPersonaMarkdown(uiLocale());
  if (text.empty()) return;  // qrc 掉了；寧可不種也不要種一個空角色
  if (!writePersona(personasDir_, kDefaultPersonaName, text).ok) return;
  // usePersona 會對掃描結果驗名稱，先把剛寫的那一份掃進來
  rescanPersonas();
  usePersona(kDefaultPersonaName);  // 寫 config.persona.current 並 emit personaChanged
  qInfo() << "[persona] 初次啟動，已種入預設角色" << kDefaultPersonaName;
}

const ModelInfo* AppController::currentModel() const {
  for (const auto& m : models_) {
    if (m.id == currentModelId_) return &m;
  }
  return nullptr;
}

std::string AppController::modelListHint() const { return l2m::modelListHint(models_, modelsDir_.u8string()); }

std::optional<CommandResult> AppController::requireModel() const {
  if (currentModel()) return std::nullopt;
  return CommandResult::failure("No model is loaded", modelListHint());
}

std::filesystem::path AppController::entryPathOf(const ModelInfo& model) const {
  // 外部模型的 id 本身就是完整路徑（見 core/external_model.h）。
  // 明寫這個分支而不是靠 operator/ 「右邊是絕對路徑就取代左邊」的隱性行為 ——
  // 那條規則只在有 root name 的平台成立，讀的人也看不出這裡支援外部模型。
  if (isExternalModelId(model.id)) return std::filesystem::u8path(model.id);
  return modelsDir_ / std::filesystem::u8path(model.id);
}

CommandResult AppController::switchModel(const std::string& idOrName) {
  // 舊模型的表情計時器對新模型沒有意義，而且會在新模型上亂清一次
  clearExpressionHold();

  const ModelInfo* model = resolveModel(models_, idOrName);
  if (!model) {
    return CommandResult::failure("Model \"" + idOrName + "\" not found", modelListHint());
  }

  // 腳底對齊：載入是同步的（一進 requestModelLoad 舊模型就被 reset），
  // 舊模型的視覺底部必須現在量。量不到（啟動第一次載入、遮罩未就緒）就跳過。
  // 淡出不影響這一步 —— draw() 的 alpha 只作用在畫面上，命中遮罩吃的是預設的
  // 1.0f（見 live2d/model_controller.h），淡到全透明時仍然量得到。
  if (const auto bottom = window_.modelBottomNormalized()) {
    const QRect b = windowManager_.bounds();
    pendingBottomAlignY_ = b.y() + bottomOffsetPx(*bottom, b.height());
    // 全透明重試額度：模型可能淡入中（能等多久隨遮罩節流而變，見 kMaskRetries）
    alignRetries_ = kMaskRetries;
  }

  // 設定與清單立刻更新，不等淡出 —— 設定視窗的選取狀態延遲 400ms 才跟上
  // 會讓使用者以為按鈕沒反應
  currentModelId_ = model->id;
  config_.patch("{\"model\":{\"current\":\"" + escapeJsonString(model->id) + "\"}}");
  emit modelsChanged();

  const QString entry = QString::fromStdString(entryPathOf(*model).u8string());
  // 淡出必須整整跑完才能進這裡：載入是同步的，一進去 GUI 執行緒就凍住數秒，
  // 動畫與載入完全不能重疊（理由見 app/splash_process.h）。
  fadeOutThen([this, entry] {
    // 跟著淡出一起延後 —— 留在前面的話，淡出中的那 400ms 舊模型還在畫，
    // AI 設過的參數覆寫會當場彈回原值
    overlay_.detach();
    // 要在 requestModelLoad 之前 —— GL 已就緒時那一行是就地同步載入，
    // 一進去 GUI 執行緒就凍住了，之後再叫什麼都畫不出來
    if (onModelLoadStarted) onModelLoadStarted();
    window_.requestModelLoad(entry);
  });

  return CommandResult::success("{\"model\":\"" + escapeJsonString(model->name) + "\"}");
}

void AppController::fadeOutThen(std::function<void()> run) {
  // 前一個待處理的動作被這一個取代（連按兩次切換模型只會載入最後那一個）
  fadeTimer_.stop();
  std::function<void()> superseded;
  superseded.swap(pendingAction_);
  // 唯一不能被默默丟掉的是待處理的隱藏：isVisible() 已經對外回報 false，
  // 丟掉的話視窗會一直留在畫面上，狀態與畫面永久對不起來。立刻補做，
  // 底下的判斷就會看到「本來就看不見」而直接執行 run。
  if (pendingVisible_ && !*pendingVisible_ && superseded) superseded();
  pendingVisible_.reset();

  // 本來就看不見：沒有畫面可以淡，也不該讓使用者白等 400 ms
  if (!windowManager_.isVisible() || !window_.modelController()) {
    if (run) run();
    return;
  }

  window_.beginFade(false);
  pendingAction_ = std::move(run);
  fadeTimer_.start(static_cast<int>(kFadeDurationMs));
}

CommandResult AppController::lastModelLoadResult() const {
  const std::string& error = window_.lastLoadError();
  // 空字串代表載入根本沒開始（找不到入口檔之類），給一句不會誤導的通則
  if (error.empty()) return CommandResult::failure("The model could not be loaded.", modelListHint());
  return CommandResult::failure(error, window_.lastLoadHint());
}

void AppController::reportModelLoadFailure(const CommandResult& result) {
  // 開機的第一次載入排在 main() 建出系統匣之前（見 app/main.cpp 的物件順序），
  // 那時候還沒有任何東西可以顯示訊息。先積著，等 main() 掛上 reporter 再補送。
  if (!modelLoadFailedReporter) {
    pendingModelLoadFailure_ = result;
    return;
  }
  modelLoadFailedReporter(result);
}

void AppController::flushPendingModelLoadFailure() {
  if (!pendingModelLoadFailure_ || !modelLoadFailedReporter) return;
  const CommandResult result = *pendingModelLoadFailure_;
  pendingModelLoadFailure_.reset();  // 只補送一次
  modelLoadFailedReporter(result);
}

ModelAnnotations AppController::currentAnnotations() const {
  const ModelInfo* model = currentModel();
  return model ? model->annotations : ModelAnnotations{};
}

void AppController::onModelLoaded(bool ok) {
  auto* controller = window_.modelController();
  if (!ok || !controller) {
    // 載入失敗後不再 paint、遮罩不會更新，把待處理的對齊收掉才不會殘留到下一次
    pendingBottomAlignY_.reset();
    pendingExtentMeasure_ = false;
    // 載入失敗之後 controller_ 是 null、paintGL 直接折返，遮罩永遠不會再更新 ——
    // 不補這一句的話氣泡會一直貼著**上一隻模型**的頭頂高度
    if (onModelExtent) onModelExtent(0.0, 1.0);
    overlay_.detach();
    // 誰來告訴使用者：設定視窗的模型分頁若正在等自己按下的那一次載入，會在這個 emit
    // 裡認領（markModelLoadFailureReported）並自己彈對話框；沒人認領時才由系統匣通知。
    // 開機那一次一定走後者 —— 分頁是延遲建立的，那時候它還不存在。
    modelLoadFailureReported_ = false;
    emit modelSwitchFinished(false);
    if (!modelLoadFailureReported_) reportModelLoadFailure(lastModelLoadResult());
    return;
  }

  overlay_.attach(controller);
  overlay_.lipSyncActive = lipSyncActive;
  // 掛點只需要接一次，但重接是冪等的，模型切換後照樣有效
  controller->earlyParameterHook = [this](Csm::CubismModel* model) { overlay_.applyEarly(model); };
  controller->lateParameterHook = [this](Csm::CubismModel* model) { overlay_.applyLate(model); };
  controller->mouthOpenProvider = mouthOpenSource;
  // 拖曳搖晃：換模型從乾淨狀態開始；開關關閉時回預設 Output（inactive → 風力歸零）
  dragSwing_.reset();
  controller->dragSwingProvider = [this]() -> DragSwing::Output {
    if (!config_.get().interaction.dragSwing) return {};
    return dragSwing_.sample(double(swingClock_.nsecsElapsed()) / 1e6);
  };
  // 環境風：設定即時生效（每幀讀 config），時基與拖曳搖晃共用 swingClock_。
  // 模型沒有 physics3.json 時 update() 那側的 _physics 是 null，風自然無效。
  controller->ambientWindProvider = [this]() -> WindVector {
    const auto& wind = config_.get().wind;
    if (!wind.enabled) return {};
    return ambientWind({}, true, wind.direction == "left" ? WindDirection::Left : WindDirection::Right, wind.strength, double(swingClock_.nsecsElapsed()) / 1e6);
  };
  // 氣泡錨點：新模型的遮罩落地時量一次視覺上下緣（onMaskUpdated）。
  // 這裡還量不到 —— 遮罩比 modelLoaded 晚 1~2 幀，這一刻讀到的還是舊模型那份。
  pendingExtentMeasure_ = true;
  extentRetries_ = kMaskRetries;
  emit stateChanged();
  emit modelSwitchFinished(true);
}

void AppController::onMaskUpdated() {
  // 平時的每次遮罩更新都從這裡直接離開 —— 兩件事都是模型載入後的 one-shot
  if (!pendingBottomAlignY_ && !pendingExtentMeasure_) return;

  // 兩件事讀的是同一份 pixels_，掃兩遍必然得到同一個答案 —— 量一次分給兩邊。
  // 快速連切時這裡量到的可能還是前一個模型的遮罩（invalidate 只作廢在途回讀，
  // 不清 pixels_）—— 退化成「對齊更早那個模型的底部」，即連鎖對齊，可接受。
  const auto bottom = window_.modelBottomNormalized();

  // 氣泡錨點：量到一次模型的視覺上下緣就熄火。**刻意不每次遮罩更新都重量** ——
  // 遮罩最快每幀就更新一次，跟著重擺的話舉手、甩頭都會讓氣泡上下跳。
  if (pendingExtentMeasure_) {
    const auto top = window_.modelTopNormalized();
    if (top && bottom) {
      pendingExtentMeasure_ = false;
      if (onModelExtent) onModelExtent(*top, *bottom);
    } else if (--extentRetries_ <= 0) {
      // 全透明：模型可能還在淡入，限次重試；量不到就放棄並把錨點交回整個視窗，
      // 否則氣泡會一直貼著**上一隻模型**的頭頂高度
      pendingExtentMeasure_ = false;
      if (onModelExtent) onModelExtent(0.0, 1.0);
    }
  }

  if (!pendingBottomAlignY_) return;

  if (!bottom) {
    // 全透明：模型可能還在淡入，限次重試；量不到就放棄，絕不無限等
    if (--alignRetries_ <= 0) pendingBottomAlignY_.reset();
    return;
  }

  const QRect b = windowManager_.bounds();
  const int newY = windowYForBottom(*pendingBottomAlignY_, bottomOffsetPx(*bottom, b.height()));
  pendingBottomAlignY_.reset();
  // moveTo 自帶 clamp 與 config 寫回（preset 模式也因此轉成明確 x/y）。
  // 貼近工作區邊緣被 clamp 拉回時腳底會略偏 —— 沿用所有移動路徑的既有行為。
  if (newY != b.y()) windowManager_.moveTo(b.x(), newY);
}

CommandResult AppController::playMotion(const std::string& group, int index, std::optional<int> priority) {
  if (auto missing = requireModel()) return *missing;
  touchIdle();

  ResolvedMotion resolved;
  const std::optional<int> wanted = index < 0 ? std::nullopt : std::optional<int>(index);
  const CommandResult check = resolveMotion(*currentModel(), group, wanted, &resolved);
  if (!check) return check;

  if (!window_.modelController()) return CommandResult::failure("The character window is not ready yet");

  const int level = priority.value_or(ModelController::PriorityForce);
  const bool played = startMotionByName(resolved.group, resolved.index.value_or(-1), level);
  emit stateChanged();
  if (!played) {
    return CommandResult::failure("Motion \"" + resolved.group + "\" could not be played", "Another motion with a higher priority is running; retry, or pass priority 3 to force it.");
  }
  return CommandResult::success();
}

bool AppController::startMotionByName(const std::string& group, int index, int priority) {
  auto* controller = window_.modelController();
  if (!controller) return false;

  // 內建動作與模型自己的動作的分岔在 live2d/action_player.h（Viewer 走同一份）。
  // 還沒掃到模型時只有「模型自己的」那條路可走。
  const ModelInfo* model = currentModel();
  if (!model) return controller->startMotion(group, index, priority);
  return playMotionByName(*controller, *model, group, index, priority);
}

CommandResult AppController::setExpression(const std::optional<std::string>& name, std::optional<double> holdMs) {
  if (auto missing = requireModel()) return *missing;

  const ModelInfo& model = *currentModel();
  std::optional<std::string> resolved;
  if (name.has_value()) {
    std::string match;
    const CommandResult check = resolveExpression(model, *name, &match);
    if (!check) return check;
    resolved = match;
  }

  auto* controller = window_.modelController();
  if (!controller) return CommandResult::failure("The character window is not ready yet");

  // 內建表情、虛擬表情、.exp3.json 三條路的分岔與收尾在 live2d/action_player.h
  // （Viewer 走同一份）。回 false 只有一種可能：.exp3.json 裡沒有這個表情。
  if (!applyExpressionByName(*controller, overlay_, model, resolved)) {
    return CommandResult::failure("Model \"" + model.name + "\" has no expression \"" + *resolved + "\"", expressionHint(model));
  }

  // 舊的 hold 一定要清掉，不然前一個表情的計時器會把新表情也一起收走
  clearExpressionHold();
  touchIdle();
  if (resolved.has_value() && holdMs.has_value() && *holdMs > 0) {
    expressionHoldId_ = timers_.setTimeout(
      [this] {
        expressionHoldId_ = -1;
        setExpression(std::nullopt);
      },
      *holdMs);
  }

  emit stateChanged();
  return CommandResult::success();
}

void AppController::clearExpressionHold() {
  if (expressionHoldId_ == -1) return;
  timers_.clearTimeout(expressionHoldId_);
  expressionHoldId_ = -1;
}

// ── 參數 ────────────────────────────────────────────────

const std::vector<ParameterInfo>& AppController::listParameters() const {
  static const std::vector<ParameterInfo> empty;
  const ModelInfo* model = currentModel();
  return model ? model->parameters : empty;
}

CommandResult AppController::parameterReport() {
  if (auto missing = requireModel()) return *missing;

  auto* controller = window_.modelController();
  const bool supported = controller && controller->hasParameterTable();
  const std::vector<ParameterSnapshot> snapshots = supported ? controller->parameterSnapshots() : std::vector<ParameterSnapshot>{};

  return CommandResult::success(buildParameterReportJson(*currentModel(), supported, snapshots, overlay_.activeIds()));
}

CommandResult AppController::setParameters(const std::vector<SetParameterRequest>& requests) {
  touchIdle();
  if (auto missing = requireModel()) return *missing;

  const CommandResult check = validateSetParameters(*currentModel(), requests);
  if (!check) return check;

  if (!overlay_.available()) {
    return CommandResult::failure("The loaded model exposes no writable parameter table", "This model has no parameter table. Use play_motion or set_expression instead.");
  }

  const auto outcome = overlay_.set(requests);
  if (!outcome.unknown.empty()) {
    std::string joined;
    for (size_t i = 0; i < outcome.unknown.size(); ++i) {
      if (i > 0) joined += ", ";
      joined += outcome.unknown[i];
    }
    return CommandResult::failure("Unknown parameter(s): " + joined, parameterHint(*currentModel(), outcome.unknown));
  }

  emit stateChanged();
  return CommandResult::success();
}

CommandResult AppController::resetParameters(const std::optional<std::vector<std::string>>& ids) {
  if (auto missing = requireModel()) return *missing;
  overlay_.release(ids);
  emit stateChanged();
  return CommandResult::success();
}

CommandResult AppController::animate(const std::vector<Keyframe>& keyframes, const BuildMotionOptions& options) {
  touchIdle();
  if (auto missing = requireModel()) return *missing;

  std::vector<std::map<std::string, double>> frameParams;
  frameParams.reserve(keyframes.size());
  for (const auto& frame : keyframes) {
    std::map<std::string, double> one;
    for (const auto& entry : frame.params) one[entry.first] = entry.second;
    frameParams.push_back(std::move(one));
  }
  const CommandResult check = validateAnimateParams(*currentModel(), uniqueKeyframeParams(frameParams));
  if (!check) return check;

  Motion3 motion;
  try {
    motion = buildMotion3(keyframes, options);
  } catch (const std::exception& err) {
    return CommandResult::failure(err.what(), "Each keyframe is { at_ms, params }; give at least two at different times.");
  }

  auto* controller = window_.modelController();
  if (!controller) return CommandResult::failure("The character window is not ready yet");
  if (!controller->playSynthesizedMotion(motion, options.loop, options.fadeInMs, options.fadeOutMs)) {
    return CommandResult::failure("Failed to play the synthesized motion");
  }

  emit stateChanged();
  return CommandResult::success();
}

// ── 視線 ────────────────────────────────────────────────

CommandResult AppController::lookAt(std::optional<double> x, std::optional<double> y) {
  const QRect bounds = windowManager_.bounds();
  if (!x.has_value() || !y.has_value()) {
    lookAtPinned_ = false;
    window_.setFocusFromLocal(QPointF(bounds.width() / 2.0, bounds.height() / 2.0));
    return CommandResult::success();
  }

  touchIdle();
  lookAtPinned_ = true;
  const double localX = (clampValue(*x, -1, 1) + 1) / 2 * bounds.width();
  const double localY = (clampValue(*y, -1, 1) + 1) / 2 * bounds.height();
  window_.setFocusFromLocal(QPointF(localX, localY));
  return CommandResult::success();
}

// ── 視窗 ────────────────────────────────────────────────

CommandResult AppController::setScale(double scale) {
  if (!std::isfinite(scale)) return CommandResult::failure("scale must be a number");
  const double value = clampValue(scale, kMinScale, kMaxScale);
  windowManager_.applyScale(value);
  emit stateChanged();
  return CommandResult::success("{\"scale\":" + std::to_string(value) + "}");
}

CommandResult AppController::setOpacity(double opacity) {
  if (!std::isfinite(opacity)) return CommandResult::failure("opacity must be a number");
  const double value = clampValue(opacity, 0.1, 1);
  config_.patch("{\"model\":{\"opacity\":" + std::to_string(value) + "}}");
  windowManager_.setOpacity(value);
  emit stateChanged();
  return CommandResult::success("{\"opacity\":" + std::to_string(value) + "}");
}

CommandResult AppController::moveToPreset(const std::string& preset) { return moveTo(std::nullopt, std::nullopt, preset); }

CommandResult AppController::moveTo(std::optional<double> x, std::optional<double> y, const std::optional<std::string>& preset) {
  const auto& presets = positionPresets();
  const auto presetHint = [&presets] {
    std::string joined;
    for (size_t i = 0; i < presets.size(); ++i) {
      if (i > 0) joined += ", ";
      joined += presets[i];
    }
    return "Available presets: " + joined;
  };

  QPoint position;
  if (preset.has_value()) {
    if (std::find(presets.begin(), presets.end(), *preset) == presets.end()) {
      return CommandResult::failure("Unknown position preset \"" + *preset + "\"", presetHint());
    }
    position = windowManager_.moveToPreset(*preset);
  } else if (!x.has_value() || !y.has_value()) {
    return CommandResult::failure("Provide both x and y, or use preset instead", presetHint());
  } else if (!std::isfinite(*x) || !std::isfinite(*y)) {
    return CommandResult::failure("x and y must be numbers");
  } else {
    // 0~1 視為工作區比例，大於 1 視為絕對螢幕座標 —— AI 不必知道使用者的解析度
    const bool ratioMode = *x >= 0 && *x <= 1 && *y >= 0 && *y <= 1;
    position = ratioMode ? windowManager_.moveToRatio(*x, *y) : windowManager_.moveTo(static_cast<int>(*x), static_cast<int>(*y));
  }

  emit stateChanged();
  return CommandResult::success("{\"x\":" + std::to_string(position.x()) + ",\"y\":" + std::to_string(position.y()) + "}");
}

CommandResult AppController::setVisible(bool visible) {
  if (visible) {
    // 淡出到一半又要顯示：撤掉待處理的隱藏，從當下的透明度接回去（不會跳）。
    // pendingVisible_ 有值就保證待處理動作真的是隱藏，不會誤殺別人的動作。
    if (pendingVisible_ && !*pendingVisible_) {
      fadeTimer_.stop();
      pendingAction_ = nullptr;
      pendingVisible_.reset();
    }
    windowManager_.setVisible(true);
    window_.beginFade(true);
    emit stateChanged();
    return CommandResult::success();
  }

  fadeOutThen([this] {
    pendingVisible_.reset();
    windowManager_.setVisible(false);
    emit stateChanged();
  });
  // fadeOutThen 可能已經就地跑完（本來就看不見），那時沒有「淡出中」可言
  if (fadeTimer_.isActive()) pendingVisible_ = false;
  emit stateChanged();
  return CommandResult::success();
}

bool AppController::isVisible() const {
  // 淡出中回報「意圖」而不是視窗當下的狀態：AI 連著呼叫 set_visible(false)
  // 與 get_state 時，不該在那 400 ms 內讀到 visible: true。
  if (pendingVisible_) return *pendingVisible_;
  return windowManager_.isVisible();
}

CommandResult AppController::setAlwaysOnTop(bool enabled) {
  config_.patch(std::string("{\"app\":{\"alwaysOnTop\":") + (enabled ? "true" : "false") + "}}");
  windowManager_.setAlwaysOnTop(enabled);
  emit stateChanged();
  return CommandResult::success();
}

void AppController::setOpenAtLogin(bool enabled) {
  config_.patch(std::string("{\"app\":{\"openAtLogin\":") + (enabled ? "true" : "false") + "}}");
  platform::setOpenAtLogin(enabled);
  emit stateChanged();
}

// ── 說話 ────────────────────────────────────────────────

void AppController::speak(const SpeakRequest& request, std::function<void(CommandResult)> done) {
  touchIdle();
  if (!speakHandler) {
    if (done) {
      done(CommandResult::failure("Speech is not available", "No TTS engine is wired up in this build."));
    }
    return;
  }
  speakHandler(request, std::move(done));
}

void AppController::mutter(const std::string& text, std::function<void(CommandResult)> done) {
  // 刻意不 touchIdle：嘀咕是桌寵自己的行為，不是「有人來了」
  //（呼叫端只有自主表演，且已在 idleSuppress_ 底下 —— 這裡是雙重保險）
  if (!mutterHandler) {
    if (done) done(CommandResult::failure("Muttering is not available"));
    return;
  }
  mutterHandler(text, std::move(done));
}

void AppController::listVoicesJson(const std::optional<std::string>& engine, std::function<void(std::string)> done) {
  if (!voicesJsonProvider) {
    done("{\"count\":0,\"voices\":[]}");
    return;
  }
  voicesJsonProvider(engine, std::move(done));
}

CommandResult AppController::stopSpeaking() {
  if (!stopSpeakingHandler) return CommandResult::failure("Speech is not available");
  return stopSpeakingHandler();
}

// ── 命名 ────────────────────────────────────────────────

CommandResult AppController::openNaming() {
  if (!settingsOpener) return CommandResult::failure("The settings window is not ready yet");
  settingsOpener(SettingsTab::Model);
  return CommandResult::success();
}

CommandResult AppController::previewMotionKey(const std::string& key) {
  const MotionKeyParts parts = parseMotionKey(key);
  return playMotion(parts.group, parts.index, ModelController::PriorityForce);
}

CommandResult AppController::setMeaning(AnnotationKind kind, const std::string& key, const std::string& meaning) {
  const ModelInfo* model = currentModel();
  if (!model) return CommandResult::failure("No model is loaded", modelListHint());

  // models_ 是唯一的真實來源，改在原地才會反映到系統匣與 MCP 清單
  auto it = std::find_if(models_.begin(), models_.end(), [model](const ModelInfo& m) { return m.id == model->id; });
  if (it == models_.end()) return CommandResult::failure("No model is loaded", modelListHint());

  const ModelAnnotations previous = it->annotations;
  it->annotations = applyMeaning(previous, kind, key, meaning);

  const std::filesystem::path entry = entryPathOf(*it);
  try {
    writeAnnotations(entry, it->annotations);
  } catch (const std::exception& err) {
    it->annotations = previous;
    // 指出真正要寫的那個檔，而不是「模型資料夾」—— zip 模型的命名檔是寫在
    // zip **旁邊**的 sidecar（見 core/annotations.h），講模型資料夾會誤導
    return CommandResult::failure(std::string("Failed to write annotations: ") + err.what(), "Make sure this path is writable: " + annotationsPathFor(entry).u8string());
  }

  emit modelsChanged();
  return CommandResult::success();
}

CommandResult AppController::applyDefaultNames() {
  const ModelInfo* model = currentModel();
  if (!model) return CommandResult::failure("No model is loaded", modelListHint());

  // models_ 是唯一的真實來源，改在原地才會反映到系統匣與 MCP 清單
  auto it = std::find_if(models_.begin(), models_.end(), [model](const ModelInfo& m) { return m.id == model->id; });
  if (it == models_.end()) return CommandResult::failure("No model is loaded", modelListHint());

  const ModelAnnotations previous = it->annotations;
  const ModelAnnotations next = fillDefaultNames(*it, previous);
  // 全部都已命名時什麼都不必寫，也不必吵醒兩個 view 重建
  if (next == previous) return CommandResult::success();
  it->annotations = next;

  const std::filesystem::path entry = entryPathOf(*it);
  try {
    writeAnnotations(entry, it->annotations);
  } catch (const std::exception& err) {
    it->annotations = previous;
    // 與 setMeaning 同一條：指出真正要寫的那個檔（zip 模型是 sidecar）
    return CommandResult::failure(std::string("Failed to write annotations: ") + err.what(), "Make sure this path is writable: " + annotationsPathFor(entry).u8string());
  }

  emit modelsChanged();
  return CommandResult::success();
}

// ── 進階 ────────────────────────────────────────────────

void AppController::perform(const std::vector<PerformStep>& steps, std::function<void(CommandResult)> done) {
  touchIdle();
  // 整段表演期間算「忙」（理由見 isBusy()）。守衛交給 done lambda 捕獲，
  // 執行器 deleteLater() 時自然釋放 —— 中途失敗早退也蓋得到。
  auto busy = std::make_shared<PerformBusy>(performActive_);
  // 執行器自己 deleteLater()，這裡不必持有；掛在 this 底下確保 app 結束時一起收掉
  auto* runner = new PerformRunner(
    *this, steps,
    [busy, done = std::move(done)](CommandResult result) {
      if (done) done(std::move(result));
    },
    this);
  runner->start();
}

CommandResult AppController::schedule(double delayMs, std::function<void()> run) {
  const double delay = clampValue(delayMs, 0, kMaxScheduleMs);
  // id 要在 lambda 建好之後才拿得到，用共享格子把它遞進去
  // （最短延遲 0 也是排到下一輪事件迴圈，填值一定早於觸發）
  auto slot = std::make_shared<int>(-1);
  const int id = timers_.setTimeout(
    [this, run = std::move(run), slot] {
      // 先從清單移除再執行，執行中排新的排程才不會被連坐清掉
      schedule_.remove(*slot);
      if (run) run();
    },
    delay);
  *slot = id;
  schedule_.add(id, static_cast<double>(runClock_.elapsed()) + delay);
  return CommandResult::success("{\"delayMs\":" + std::to_string(static_cast<long long>(delay)) + "}");
}

std::string AppController::getStateJson() const {
  const AppConfig& cfg = config_.get();
  const QRect bounds = windowManager_.bounds();

  jsonu::MutDoc doc;
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* root = yyjson_mut_obj(d);
  doc.setRoot(root);

  const ModelInfo* model = currentModel();
  if (model) {
    yyjson_mut_obj_add_strcpy(d, root, "model", model->name.c_str());
  } else {
    yyjson_mut_obj_add_null(d, root, "model");
  }

  yyjson_mut_val* window = yyjson_mut_obj(d);
  yyjson_mut_obj_add_int(d, window, "x", bounds.x());
  yyjson_mut_obj_add_int(d, window, "y", bounds.y());
  yyjson_mut_obj_add_int(d, window, "width", bounds.width());
  yyjson_mut_obj_add_int(d, window, "height", bounds.height());
  yyjson_mut_obj_add_bool(d, window, "visible", windowManager_.isVisible());
  yyjson_mut_obj_add_bool(d, window, "alwaysOnTop", cfg.app.alwaysOnTop);
  yyjson_mut_obj_add_bool(d, window, "clickThrough", cfg.interaction.clickThrough);
  yyjson_mut_obj_add_val(d, root, "window", window);

  yyjson_mut_val* display = yyjson_mut_obj(d);
  if (QScreen* screen = QGuiApplication::primaryScreen()) {
    yyjson_mut_obj_add_int(d, display, "width", screen->availableGeometry().width());
    yyjson_mut_obj_add_int(d, display, "height", screen->availableGeometry().height());
  }
  yyjson_mut_obj_add_val(d, root, "display", display);

  addRawJson(d, root, "config", serializeConfig(cfg, false));
  addRawJson(d, root, "models", modelListJson(models_, currentModelId_));
  if (ttsStateJson) addRawJson(d, root, "tts", ttsStateJson());
  if (mcpStateJson) addRawJson(d, root, "mcp", mcpStateJson());
  if (llmStateJson) addRawJson(d, root, "llm", llmStateJson());

  return doc.write(true);
}

// ── 閒置 ────────────────────────────────────────────────

bool AppController::isBusy() const {
  if (speakingProbe && speakingProbe()) return true;
  // 進行中的 perform 也算忙。步驟之間的 wait（與 move 的滑行）不會經過任何
  // 活動進場點，5 秒的 MCP 復原會就這樣在表演中途把第一步的表情抽掉。
  if (performActive_ > 0) return true;
  // 只有「馬上要發生」的排程算忙。以前是 !scheduled_.empty() —— AI 排一個
  // 6 小時的 schedule，閒置復原與閒置表演就雙雙停擺整整 6 小時。
  return schedule_.busyWithin(static_cast<double>(runClock_.elapsed()), kScheduleBusyHorizonMs);
}

void AppController::touchIdle() {
  // 自主表演期間，桌寵自己做的動作不算「有人來了」（守衛的理由見標頭註解）
  if (idleSuppress_ > 0) return;
  lastTouchMs_ = static_cast<double>(runClock_.elapsed());
  if (idleReset_) idleReset_->touch();
  if (idlePerformer_) idlePerformer_->touch();
  // MCP 驅動中才有作用。**「TTS 講完才開始數 5 秒」就是靠這一行** ——
  // main.cpp 把 SpeechController::finished（佇列真的清空才發）接到 touchIdle()，
  // 所以最後一句播完的那一刻倒數重新給滿，而不是沿用送出指令時剩下的殘值。
  if (mcpQuiet_) mcpQuiet_->onActivity();
}

void AppController::notifyMcpCommand() {
  // 唯讀工具（get_state／list_*）也算：AI 還在呼叫工具就代表它還在這一輪對話裡。
  // 刻意不順手 touchIdle() —— 那會改到既有兩分鐘閒置與閒置表演的語意。
  if (mcpQuiet_) mcpQuiet_->onCommand();
}

void AppController::refreshIdle() {
  if (idleReset_ && idleReset_->pending()) idleReset_->touch();
  if (idlePerformer_) idlePerformer_->refresh();
}

void AppController::resetToIdle() {
  if (!currentModel()) return;
  clearExpressionHold();

  // 這裡刻意走私有路徑，不繞公開的 setExpression / resetParameters / lookAt ——
  // 那些會再把「有活動」記一筆，變成每隔一個閒置週期就空轉復原一次的無窮迴圈。
  auto* controller = window_.modelController();
  if (controller) {
    controller->clearExpression();
    // stopLoopingAiMotion 只在「真的在循環」時才收尾；其他來源留下的參數殘值
    // 也要在閒置復原時一併清掉，所以沒循環的那條自己走一次同一個漏斗。
    // **不能直接呼叫 restoreBaseline()** —— 那會把模型丟回作者的編輯狀態
    //（實測碧藍航線 40 隻裡有 13 隻在那個狀態下所有肢體變體一起開著），
    // 而且此時沒有任何動作會接手蓋掉它：症狀就是「放著兩分鐘，桌寵自己長出第四隻手」。
    if (!controller->stopLoopingAiMotion()) controller->returnToIdle();
  }
  // 參數開關型的「虛擬表情」不必特別處理，覆寫層一起放掉就好
  overlay_.release(std::nullopt);

  lookAtPinned_ = false;
  const QRect bounds = windowManager_.bounds();
  window_.setFocusFromLocal(QPointF(bounds.width() / 2.0, bounds.height() / 2.0));

  emit stateChanged();
}

void AppController::playRandomMotion(int priority, bool allowIdleFallback) {
  const ModelInfo* model = currentModel();
  auto* controller = window_.modelController();
  if (!controller) return;

  // 掃描結果裡找不到目前這隻，但畫面上的模型還載著（外部模型的檔案被刪、
  // config 的 model.current 指到掃不到的 id…）—— 那時仍然要有東西可播，
  // 不然畫面上明明有角色、點下去卻完全沒反應。直接問 ModelController
  // 拿 model3.json 宣告的群組（不含內建合成動作，所以走 startMotion 就好）。
  if (!model) {
    std::vector<std::string> names;
    for (const auto& name : controller->motionGroups()) {
      if (!allowIdleFallback && strutil::equalsInsensitive(name, kIdleGroup)) continue;
      names.push_back(name);
    }
    if (names.empty()) return;
    controller->startMotion(names[static_cast<size_t>(QRandomGenerator::global()->bounded(static_cast<int>(names.size())))], -1, priority);
    emit stateChanged();
    return;
  }

  std::vector<const MotionGroupInfo*> groups;
  for (const auto& group : model->motions) {
    if (group.count <= 0) continue;
    if (strutil::equalsInsensitive(group.name, kIdleGroup)) continue;
    groups.push_back(&group);
  }
  // 很多 VTube Studio 出身的模型把所有動作都歸在 Idle 底下（model3.json 的
  // 執行期補全也會把 VTS 的 IdleAnimation 歸到這裡），排掉之後一個都不剩。
  // 閒置表演遇到這種模型就該安靜（待機本來就在播 Idle），但使用者點了角色
  // 卻毫無反應是另一回事 —— 那種情況連 Idle 一起挑，至少動作會重新起跑。
  if (groups.empty() && allowIdleFallback) {
    for (const auto& group : model->motions) {
      if (group.count > 0) groups.push_back(&group);
    }
  }
  if (groups.empty()) return;

  const int pick = QRandomGenerator::global()->bounded(static_cast<int>(groups.size()));
  startMotionByName(groups[static_cast<size_t>(pick)]->name, -1, priority);
  emit stateChanged();
}

void AppController::performIdle() {
  // 規劃器沒接上、或使用者把自主行為關掉：退回舊行為（隨機播一個非 Idle 動作）。
  // 用 Normal 而不是 Force：使用者或 AI 正在進行的動作優先，表演只是填空檔。
  if (!behaviorPlanner || !config_.get().autonomy.enabled) {
    playRandomMotion(ModelController::PriorityNormal, /*allowIdleFallback=*/false);
    return;
  }

  IdlePlanOptions options;
  // 閒置分級吃「app 閒置與 OS 真閒置取小」：你在旁邊狂打程式碼三小時沒碰角色，
  // 不代表你發呆三小時（core/idle_signal.h）
  const double now = static_cast<double>(runClock_.elapsed());
  options.level = idleLevelFor(effectiveIdleMs(now - lastTouchMs_, platform::userIdleMs()), static_cast<double>(config_.get().idle.performAfterMs));
  options.occasion = "idle";
  options.cheerful = mood_.cheerful(now);
  options.familiarityLevel = familiarityLevel(config_.get().autonomy.familiarity);

  // 規劃器可能是非同步的（未來的 LLM 版隔一段網路往返才回呼），
  // 用 QPointer 護衛 —— 回呼落地時 this 可能已經不在了
  QPointer<AppController> self(this);
  behaviorPlanner(buildIdleWorld("idle"), options, [self](std::vector<PerformStep> steps) {
    if (self) self->runAutonomous(std::move(steps));
  });
}

IdleWorld AppController::buildIdleWorld(const std::string& occasion, std::optional<WeatherAlertKind> weatherKind) const {
  IdleWorld world;
  const ModelInfo* model = currentModel();
  if (model) {
    for (const auto& group : model->motions) {
      if (group.count > 0) world.motions.push_back(group);
    }
    // autonomy.expression 關閉時直接不給表情清單 —— director 的
    // 「沒有具名表情就不碰表情」規則自然把 expression 步驟關掉
    if (config_.get().autonomy.expression) world.expressions = model->expressions;
    world.annotations = model->annotations;
  }

  // 台詞：套用中角色的 .md 對應區塊（core/persona_doc.h）。
  // 唯一來源就是角色檔 —— 按了「不使用角色」或把台詞區清空，
  // 閒置表演就只做動作與表情，安靜才是對的，不該有通用台詞冒出來。
  const auto& cfg = config_.get();
  const std::string active = activePersonaName();
  if (!active.empty() && cfg.autonomy.speech != "off") {
    if (const auto raw = readPersona(personasDir_, active)) {
      const PersonaDoc doc = parsePersonaDoc(*raw);
      if (occasion == "welcome") {
        // 歡迎池 ＝ # Welcome Text ＋ 此刻時段適用的 # Greeting by Time
        world.lines = doc.welcome;
        const auto timed = greetingsForPeriod(doc.greetings, dayPeriodFor(QTime::currentTime().hour()));
        world.lines.insert(world.lines.end(), timed.begin(), timed.end());
      } else if (occasion == "breakReminder") {
        world.lines = doc.breaks;
      } else if (occasion == "petted") {
        world.lines = doc.petted;
      } else if (occasion == "weatherAlert") {
        // 前綴過濾與 # Greeting by Time 同一套規則（core/weather_alert.h）
        world.lines = weatherKind ? weatherLinesFor(doc.weatherAlerts, *weatherKind) : doc.weatherAlerts;
      } else {
        world.lines = doc.lines;
      }
    }
  }
  const bool handlerReady = cfg.autonomy.speech == "bubble" ? static_cast<bool>(mutterHandler) : static_cast<bool>(speakHandler);
  world.canSpeak = cfg.autonomy.speech != "off" && handlerReady && !(speakingProbe && speakingProbe());

  // 微移動：三個開關都成立才散步（core/autonomy_gates.h）；比例位置給 director 當基準。
  // 這只是「要不要叫規劃器產生 move」，真正放行是 runAutonomous 執行前再判一次
  if (autonomousMoveAllowed(cfg)) {
    const QRect bounds = windowManager_.bounds();
    QScreen* screen = QGuiApplication::screenAt(bounds.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
      const QRect area = screen->availableGeometry();
      const int spanX = area.width() - bounds.width();
      const int spanY = area.height() - bounds.height();
      if (spanX > 0 && spanY > 0) {
        world.canMove = true;
        world.windowXRatio = std::clamp(double(bounds.x() - area.x()) / spanX, 0.0, 1.0);
        world.windowYRatio = std::clamp(double(bounds.y() - area.y()) / spanY, 0.0, 1.0);
      }
    }
  }
  return world;
}

void AppController::greet(const char* reason) {
  const auto& cfg = config_.get();
  if (!cfg.autonomy.enabled || !cfg.autonomy.greet || !behaviorPlanner) return;

  const double now = static_cast<double>(runClock_.elapsed());
  if (lastGreetMs_ >= 0 && now - lastGreetMs_ < kGreetCooldownMs) return;

  IdleWorld world = buildIdleWorld("welcome");
  // 只擋「說不了話」，不再要求歡迎池非空：LLM 版沒寫 # Welcome Text 也能
  // 依角色設定現寫一句；規則版拿到空池會回空步驟，安靜如舊
  if (!world.canSpeak) return;
  lastGreetMs_ = now;
  qDebug() << "[idle] 歡迎詞觸發:" << reason;

  IdlePlanOptions options;
  options.occasion = "welcome";
  options.cheerful = mood_.cheerful(now);
  options.familiarityLevel = familiarityLevel(cfg.autonomy.familiarity);
  QPointer<AppController> self(this);
  behaviorPlanner(world, options, [self](std::vector<PerformStep> steps) {
    if (self) self->runAutonomous(std::move(steps));
  });
}

void AppController::greetOnLaunch() {
  // 掛鉤沒接、或天氣沒啟用時 weatherReady 永遠為真 —— 這一段等於不存在
  const bool ready = !weatherReady || weatherReady();
  if (!ready && launchGreetWaitedMs_ < kLaunchGreetWeatherWaitMs) {
    launchGreetWaitedMs_ += kLaunchGreetPollMs;
    QTimer::singleShot(kLaunchGreetPollMs, this, [this] { greetOnLaunch(); });
    return;
  }
  // 上限到了還沒結果：照講。歡迎詞的重點是它有發生，天氣只是加分
  if (!ready) qDebug() << "[weather] 等不到天氣結果（" << kLaunchGreetWeatherWaitMs << "ms），歡迎詞照講";
  greet("啟動");
}

void AppController::samplePresence() {
  // awayAfterMs / breakAfterMs 熱更新：每次心跳帶最新設定值，其餘門檻用預設
  PresenceTracker::Options options;
  options.awayAfterMs = static_cast<double>(config_.get().autonomy.awayAfterMs);
  options.longSessionMs = static_cast<double>(config_.get().autonomy.breakAfterMs);
  presence_.setOptions(options);

  const PresenceEvent event = presence_.sample(static_cast<double>(runClock_.elapsed()), static_cast<double>(QDateTime::currentMSecsSinceEpoch()), platform::userIdleMs());
  switch (event.kind) {
    case PresenceEvent::Kind::CameBack:
      greet("離座回來");
      break;
    case PresenceEvent::Kind::Resumed:
      greet("休眠喚醒");
      break;
    case PresenceEvent::Kind::LongSession:
      remindBreak();
      break;
    default:
      break;
  }

  // 預警跟在席事件同一個心跳上跑：**每次都重算**，不排隊也不記狀態。
  // 離座期間產生的預警於是自然變成「回座時若還沒開始就照樣講、
  // 已經開始就自己消失」，不需要任何過期邏輯（見 checkWeatherAlert）
  checkWeatherAlert();
}

void AppController::checkWeatherAlert() {
  const auto& cfg = config_.get();
  if (!cfg.weather.enabled || !cfg.weather.alertEnabled) return;
  if (!cfg.autonomy.enabled || !behaviorPlanner || !weatherSnapshot) return;
  // 離座時不講（跟閒置表演同一條規則）；正在忙就等下一次心跳，
  // 預警的時效是以小時計的，晚 5 秒無所謂
  if (cfg.autonomy.pauseWhenAway && presence_.state() == PresenceState::Away) return;
  if (isBusy()) return;
  // 上一輪的規劃還在飛：LLM 要等一趟網路往返，5 秒後的心跳不能再發一次
  if (weatherAlertPending_) return;
  const double nowMs = static_cast<double>(runClock_.elapsed());
  if (lastWeatherPlanMs_ >= 0 && nowMs - lastWeatherPlanMs_ < kWeatherPlanRetryMs) return;

  WeatherAlertOptions alertOptions;
  alertOptions.lookaheadMinutes = cfg.weather.lookaheadMinutes;
  alertOptions.rainProbability = cfg.weather.rainProbability;
  alertOptions.fahrenheit = cfg.weather.unit == "f";
  // config 的鍵只負責跨重啟，開機後第一次比對時併進記憶體的集合（insert 是冪等的）
  if (!cfg.weather.lastAlertKey.empty()) announcedWeatherKeys_.insert(cfg.weather.lastAlertKey);
  const auto alert = evaluateWeatherAlert(weatherSnapshot(), QDateTime::currentSecsSinceEpoch(), alertOptions, announcedWeatherKeys_);
  if (!alert) return;

  IdleWorld world = buildIdleWorld("weatherAlert", alert->kind);
  // 說不了話（speech 關掉、或正在講別的）就整輪不算數 —— 鍵是在下面的回呼裡
  // 才記的，所以這裡 return 不會把這一場天氣跳掉
  if (!world.canSpeak) return;
  // 規則版的台詞來源是 # Weather Alert 區，LLM 版沒有台詞也生得出來 ——
  // 兩邊都不會出聲的那一輪就整個不算數，等使用者補了台詞（或開了 LLM）還來得及
  const bool llmDrives = cfg.llm.enabled && cfg.llm.driveIdle;
  if (world.lines.empty() && !llmDrives) return;
  qDebug() << "[weather] 預警觸發:" << QString::fromStdString(alert->summary);

  IdlePlanOptions options;
  options.occasion = "weatherAlert";
  options.cheerful = mood_.cheerful(nowMs);
  options.familiarityLevel = familiarityLevel(cfg.autonomy.familiarity);
  options.weatherAlert = alert->summary;

  // 重入防護在送出之前就要設好：規則版是**同步**回呼的（done 在這一行裡就跑完），
  // 設在後面等於永遠設不到
  weatherAlertPending_ = true;
  lastWeatherPlanMs_ = nowMs;
  QPointer<AppController> self(this);
  const std::string key = alert->key;
  behaviorPlanner(world, options, [self, key](std::vector<PerformStep> steps) {
    if (!self) return;
    self->weatherAlertPending_ = false;
    // **真的規劃出東西了才記鍵。** LLM 失敗會退回規則版，而規則版在
    // # Weather Alert 區是空的時候整輪安靜 —— 那時記下去等於把這一場天氣
    // 永遠跳過。什麼都沒產出就當這一輪沒發生，等 kWeatherPlanRetryMs 之後再試。
    if (steps.empty()) return;
    self->announcedWeatherKeys_.insert(key);
    try {
      self->config_.patch(stringPatch("weather", "lastAlertKey", key));
    } catch (const std::exception& error) {
      qWarning() << "[weather] 寫入 lastAlertKey 失敗:" << error.what();
    }
    self->runAutonomous(std::move(steps));
  });
}

void AppController::remindBreak() {
  const auto& cfg = config_.get();
  if (!cfg.autonomy.enabled || !cfg.autonomy.breakReminder || !behaviorPlanner) return;

  IdleWorld world = buildIdleWorld("breakReminder");
  // 規則版的台詞來源是 # Break Reminder 區；LLM 版沒有台詞也生得出來，
  // 但「說不了話」（speech off 或正在講話）兩邊都免了
  if (!world.canSpeak) return;
  qDebug() << "[idle] 久坐提醒觸發";

  IdlePlanOptions options;
  options.occasion = "breakReminder";
  options.cheerful = mood_.cheerful(static_cast<double>(runClock_.elapsed()));
  options.familiarityLevel = familiarityLevel(cfg.autonomy.familiarity);
  QPointer<AppController> self(this);
  behaviorPlanner(world, options, [self](std::vector<PerformStep> steps) {
    if (self) self->runAutonomous(std::move(steps));
  });
}

void AppController::reactPetted() {
  const auto& cfg = config_.get();
  if (!cfg.autonomy.enabled || !behaviorPlanner) return;
  const double now = static_cast<double>(runClock_.elapsed());
  if (lastPetLineMs_ >= 0 && now - lastPetLineMs_ < kPetLineCooldownMs) return;

  IdleWorld world = buildIdleWorld("petted");
  // 台詞池空著（角色沒寫 # Petted 區）就整輪安靜 —— LLM 版一律不接 petted
  //（等網路往返的回應很怪，見 llm_behavior_planner.h），這裡不用另外分流
  if (!world.canSpeak || world.lines.empty()) return;
  lastPetLineMs_ = now;

  IdlePlanOptions options;
  options.occasion = "petted";
  options.cheerful = mood_.cheerful(now);
  options.familiarityLevel = familiarityLevel(cfg.autonomy.familiarity);
  QPointer<AppController> self(this);
  behaviorPlanner(world, options, [self](std::vector<PerformStep> steps) {
    if (self) self->runAutonomous(std::move(steps));
  });
}

// ── 手勢 ────────────────────────────────────────────────

std::vector<std::string> AppController::hitAreasAt(const QPointF& local) const {
  auto* controller = window_.modelController();
  if (!controller) return {};
  const QRect bounds = windowManager_.bounds();
  if (bounds.width() <= 0 || bounds.height() <= 0) return {};
  float viewX = 0;
  float viewY = 0;
  controller->screenToView(local.x(), local.y(), bounds.width(), bounds.height(), &viewX, &viewY);
  return controller->hitTest(viewX, viewY);
}

void AppController::feedGesture(const PointerSample& sample) { onGesture(gesture_.feed(sample)); }

void AppController::onGesture(const Gesture& gesture) {
  const double now = static_cast<double>(runClock_.elapsed());
  switch (gesture.kind) {
    case GestureKind::Pet: {
      // 被摸了：開心一陣子、熟悉度 +1、套個表情回應（只用有寫命名意義的，
      // 與閒置表演同一條產品規則 —— 亂套虛擬表情會讓角色看起來壞掉）
      qDebug() << "[idle] 撫摸: 部位" << static_cast<int>(gesture.part);
      mood_.nudge(now);
      bumpFamiliarity();
      touchIdle();  // 摸它就是「有人來了」
      const ModelAnnotations annotations = currentAnnotations();
      std::vector<std::string> named;
      const ModelInfo* model = currentModel();
      if (model) {
        for (const auto& name : model->expressions) {
          const auto it = annotations.expressions.find(name);
          if (it != annotations.expressions.end() && !it->second.empty()) {
            named.push_back(name);
          }
        }
      }
      if (!named.empty()) {
        const int pick = QRandomGenerator::global()->bounded(static_cast<int>(named.size()));
        setExpression(named[static_cast<size_t>(pick)], kPetExpressionHoldMs);
      }
      // 台詞回應（# Petted 區；有冷卻，連續撫摸只講第一下）
      reactPetted();
      break;
    }
    case GestureKind::MultiTap:
      // 連點：被逗了。單擊改成遞延判定之後，連點期間不再有零散的點擊動作
      // 在播，所以這裡除了情緒與熟悉度，也要播一次點擊動作當視覺回應
      qDebug() << "[idle] 連點" << gesture.taps << "下";
      mood_.nudge(now);
      bumpFamiliarity();
      playTapMotion(gesture.areas, QPointF(gesture.x, gesture.y));
      break;
    case GestureKind::Tap:
      // 單擊確定成立（超過 OS 雙擊間隔沒有下一擊）才播點擊動作；
      // 時窗內的兩下併成一次反應（taps=2），不會疊兩個動作
      playTapMotion(gesture.areas, QPointF(gesture.x, gesture.y));
      break;
    case GestureKind::LongPress:
      // LongPress 先不掛行為（按住常常是拖曳的前奏，掛反應會誤觸）
      break;
    case GestureKind::None:
      break;
  }
}

void AppController::playTapMotion(const std::vector<std::string>& areas, const QPointF& local) {
  // 點擊的視覺回應（原本在 CharacterWindow::mouseReleaseEvent，搬來這裡
  // 才能吃遞延判定後的 Tap）：判部位 → 挑那一段動作播放。
  //
  // pickTapMotion 認得 touch_head／tap_body 這類命名（群組名與**動作檔名**
  // 兩邊都比），但模型不見得有做觸摸動作 —— VTube Studio 出身的常常整份都是
  // 自訂群組名（haoqi、keshui…），一個都對不上。那時退回隨機動作，
  // 點擊才一定有回應。
  if (!config_.get().interaction.tapMotion) return;
  auto* controller = window_.modelController();
  if (!controller) return;

  // 挑得出來就播那一段；**挑不出來、或那一段播不起來，一律退回隨機動作** ——
  // 點了完全沒反應比播錯一段還糟。播不起來是真的會發生：preloadMotions 會跳過
  // Meta 壞掉或讀不到的 motion3.json，那個 index 在 motions_ 裡根本不存在。
  const ModelInfo* model = currentModel();
  const QRect bounds = windowManager_.bounds();
  std::optional<TapMotionPick> pick;
  if (model) {
    const BodyPart part = controller->tapBodyPart(areas, local.x(), local.y(), bounds.width(), bounds.height());
    pick = pickTapMotion(areas, part, model->motions);
  }
  if (pick && startMotionByName(pick->group, pick->index, ModelController::PriorityForce)) {
    emit stateChanged();
    return;
  }
  playRandomMotion(ModelController::PriorityForce, /*allowIdleFallback=*/true);
}

void AppController::bumpFamiliarity() {
  const double now = static_cast<double>(runClock_.elapsed());
  if (lastFamiliarityMs_ >= 0 && now - lastFamiliarityMs_ < kFamiliarityIntervalMs) return;
  lastFamiliarityMs_ = now;
  const int next = config_.get().autonomy.familiarity + 1;
  config_.patch(numberPatch("autonomy", "familiarity", next));
}

// ── 微移動與螢幕救援 ──────────────────────────────────

void AppController::glideTo(double xRatio, double yRatio, double durationMs, std::function<void(CommandResult)> done) {
  stopGlide();
  const QRect bounds = windowManager_.bounds();
  QScreen* screen = QGuiApplication::screenAt(bounds.center());
  if (!screen) screen = QGuiApplication::primaryScreen();
  if (!screen) {
    if (done) done(CommandResult::failure("No screen available"));
    return;
  }
  const QRect area = screen->availableGeometry();
  const QPoint target(area.x() + static_cast<int>(std::round(clampValue(xRatio, 0, 1) * std::max(0, area.width() - bounds.width()))),
                      area.y() + static_cast<int>(std::round(clampValue(yRatio, 0, 1) * std::max(0, area.height() - bounds.height()))));

  auto* animation = new QVariantAnimation(this);
  activeGlide_ = animation;
  animation->setStartValue(bounds.topLeft());
  animation->setEndValue(target);
  animation->setDuration(static_cast<int>(clampValue(durationMs, 100, 10000)));
  animation->setEasingCurve(QEasingCurve::InOutSine);
  connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    const QPoint p = value.toPoint();
    windowManager_.moveTo(p.x(), p.y());
  });
  // 中止（stopGlide）也會發 finished —— 微移動被拖曳打斷不算失敗，照樣回成功
  connect(animation, &QVariantAnimation::finished, this, [done = std::move(done)] {
    if (done) done(CommandResult::success());
  });
  animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void AppController::stopGlide() {
  if (!activeGlide_) return;
  // stop() 會發 finished（done 回呼在那裡收尾），DeleteWhenStopped 自行釋放
  static_cast<QVariantAnimation*>(activeGlide_.data())->stop();
  activeGlide_.clear();
}

void AppController::rescueOffscreenWindow() {
  std::vector<ScreenRect> screens;
  for (const QScreen* screen : QGuiApplication::screens()) {
    const QRect area = screen->availableGeometry();
    screens.push_back({area.x(), area.y(), area.width(), area.height()});
  }
  const QRect bounds = windowManager_.bounds();
  const auto rescued = rescuePosition({bounds.x(), bounds.y(), bounds.width(), bounds.height()}, screens);
  if (!rescued) return;
  qDebug() << "[idle] 視窗落在可見範圍外，救回:" << rescued->first << rescued->second;
  windowManager_.moveTo(rescued->first, rescued->second);
}

void AppController::runAutonomous(std::vector<PerformStep> steps) {
  if (steps.empty()) return;  // 「這輪什麼都不做」是規劃器的合法輸出

  // 移動的閘門要在**執行前**再判一次，不能只信規劃當下算出來的 IdleWorld::canMove：
  // LLM 那條路從送出到回覆隔了一整趟網路往返（暫時性失敗還會再等 15 秒重試），
  // 這中間使用者完全來得及把「隨機移動」關掉或把位置鎖起來。這裡是所有規劃器
  //（規則版／LLM 版）共用的唯一漏斗，規劃端的 canMove 只是省得模型白花 token
  // 產生注定被丟掉的步驟。
  // 這一關只擋到「表演開始」為止 —— 濾完就不再看設定，而一段夾著 wait 的表演
  // 可以跑上好幾分鐘。開始之後的改動由下面 setMoveGate() 那一層負責。
  if (!autonomousMoveAllowed(config_.get())) {
    steps.erase(std::remove_if(steps.begin(), steps.end(), [](const PerformStep& step) { return step.action == "move"; }), steps.end());
    if (steps.empty()) return;  // 整輪只剩下被擋掉的移動：什麼都不做
  }

  // bubble 模式：speak 步驟改走 mutter（只出氣泡）。在這裡改而不是在 director
  // 裡 —— director 不認識 config，而且 LLM 版規劃器吐的也是同一份 schema，
  // 發聲方式永遠是執行端的決定
  if (config_.get().autonomy.speech == "bubble") {
    for (auto& step : steps) {
      if (step.action == "speak") step.bubbleOnly = true;
    }
  }

  if (idleTurboMs() > 0) {
    QString summary;
    for (const auto& step : steps) {
      if (!summary.isEmpty()) summary += QStringLiteral(", ");
      summary += QString::fromStdString(step.action);
      if (!step.group.empty()) summary += QStringLiteral("(") + QString::fromStdString(step.group) + QStringLiteral(")");
      if (!step.name.empty()) summary += QStringLiteral("(") + QString::fromStdString(step.name) + QStringLiteral(")");
      if (!step.text.empty()) summary += QStringLiteral("(") + QString::fromStdString(step.text) + QStringLiteral(")");
    }
    qDebug() << "[idle] 自主表演:" << summary;
  }

  // 閒置時鐘靜音：guard 由 done lambda 持有，PerformRunner 跑完（或任何一條
  // 早退路徑）deleteLater() 時自然釋放。忘了減的下場見 IdleSuppress 的註解。
  auto guard = std::make_shared<IdleSuppress>(idleSuppress_);
  auto* runner = new PerformRunner(*this, std::move(steps), [guard](CommandResult) { /* 結果不必回報給任何人 */ }, this);
  // 失敗步驟跳過（表情名剛好不存在不該讓整段表演消失）；
  // 動作用 Normal —— 使用者或 AI 正在進行的動作優先，表演只是填空檔
  runner->setBestEffort(true);
  runner->setDefaultPriority(ModelController::PriorityNormal);
  // 第三層：表演進行中每一步再現讀一次（理由見 PerformRunner::setMoveGate）
  runner->setMoveGate([this] { return autonomousMoveAllowed(config_.get()); });
  runner->start();
}

// ── 設定與輸入 ──────────────────────────────────────────

void AppController::applyConfig() {
  const auto& cfg = config_.get();
  windowManager_.setOpacity(cfg.model.opacity);
  windowManager_.setAlwaysOnTop(cfg.app.alwaysOnTop);
  // 點擊穿透（形狀視窗）由 CharacterWindow 每幀依 clickThroughEnabled 回呼套用/清除
  refreshIdle();
  emit stateChanged();
}

std::string AppController::uiLocale() const {
  const std::string setting = config_.get().app.locale;
  if (setting != "auto") return setting;
  return i18n::matchLocale(QLocale::system().name().toStdString());
}

void AppController::wheelZoom(int direction) {
  const double next = clampValue(config_.get().model.scale + direction * 0.1, kMinScale, kMaxScale);
  windowManager_.applyScale(next);
}

void AppController::pollCursor() {
  const QPoint pos = QCursor::pos();
  const double now = static_cast<double>(runClock_.elapsed());
  // 每一輪都推進手勢的時間（長按成立、撫摸逾時、遞延單擊收割）。
  // 只在游標靜止時 tick 的話，點完就把游標甩開會讓單擊拖到游標停下才成立。
  onGesture(gesture_.tick(now));

  // 視線（core/gaze_director.h）：權重的推進必須排在下面「游標沒動就 return」
  // 之前 —— 游標靜止時死區以下整段不執行，回正的動畫排在後面就永遠不會推進
  // （舊版「說完話恢復追蹤」踩過的就是這個坑）。
  const bool moved = (pos - lastCursor_).manhattanLength() >= kCursorThresholdPx;
  if (windowManager_.isVisible()) {
    const auto& cfg = config_.get();
    const QRect bounds = windowManager_.bounds();
    GazeInput gazeIn;
    gazeIn.nowMs = now;
    gazeIn.enabled = cfg.interaction.lookAt;
    gazeIn.pinned = lookAtPinned_;
    gazeIn.suppressed = cfg.interaction.speakFacingFront && speakingProbe && speakingProbe();
    gazeIn.cursorMoved = moved;
    const GazeOutput gaze = gaze_.tick(gazeIn);
    if (gaze.write) {
      // 視窗外也要追，角色才會轉頭看游標，所以 local 不夾在視窗內。
      // 權重 0 ＝ 視窗中心 ＝ 視線貢獻歸零（第 5 步是 AddParameterValue），
      // 呼吸與動作保留；與 look_at(reset) 走同一條路。
      const QPointF center(bounds.width() / 2.0, bounds.height() / 2.0);
      const QPointF local(pos.x() - bounds.x(), pos.y() - bounds.y());
      window_.setFocusFromLocal(center + (local - center) * gaze.weight);
    }
  } else {
    // 重新顯示時從正面開始，而不是接續幾分鐘前的權重
    gaze_.reset();
  }

  if (!moved) return;
  lastCursor_ = pos;

  if (!windowManager_.isVisible()) return;
  const QRect bounds = windowManager_.bounds();
  const QPointF local(pos.x() - bounds.x(), pos.y() - bounds.y());

  // 形狀視窗只在游標靠近時套用（遠離時畫面完整無裁切）
  const bool inside = bounds.contains(pos);
  window_.setRegionGate(inside);

  // 撫摸偵測：放開狀態的 hover 來回滑動（core/gesture_detector.h 檔頭）。
  // hitTest 降到 ~10 Hz、只在視窗內做；穿透開著時形狀視窗吃不到 hover 事件，
  // 這條輪詢照樣感覺得到。注意 hitTest 在透明度 < 1 時可能回空 —— 那時
  // isOpaqueAt 的遮罩仍在，inside 的判定不受影響，只是分不出部位。
  if (inside && now - lastPetSampleMs_ >= kPetSampleIntervalMs) {
    lastPetSampleMs_ = now;
    PointerSample sample;
    sample.x = local.x();
    sample.y = local.y();
    sample.screenX = pos.x();
    sample.screenY = pos.y();
    sample.nowMs = now;
    sample.pressed = QGuiApplication::mouseButtons() != Qt::NoButton;
    sample.areas = hitAreasAt(local);
    sample.inside = !sample.areas.empty() || window_.isOpaqueAt(local);
    feedGesture(sample);
  }
}

}  // namespace l2m
