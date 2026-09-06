#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QIcon>
#include <QLocalSocket>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QSurfaceFormat>

#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

#include "../media/audio_player.h"
#include "../media/http_json.h"
#include "../media/tts_engine_edge.h"
#include "../media/tts_engine_gptsovits.h"
#include "../media/tts_engine_voicebox.h"
#include "../media/tts_engine_custom.h"
#include "../media/weather_service.h"
#include "../windows/bubble_window.h"
#include "../llm/llm_behavior_planner.h"
#include "../llm/llm_manager.h"
#include "../mcp/mcp_http_server.h"
#include "../mcp/mcp_stdio.h"
#include "../mcp/mcp_tools.h"
#include "../windows/character_window.h"
#include "../windows/settings/settings_window.h"
#include "splash_process.h"
#include "../windows/tray.h"
#include "../platform/autostart.h"
#include "../platform/crash_handler.h"
#include "../windows/window_manager.h"
#include "app_controller.h"
#include "logging.h"
#include "core/config_patch.h"
#include "core/config_store.h"
#include "core/crash_report.h"
#include "core/external_model.h"
#include "core/idle_director.h"
#include "core/json_doc.h"
#include "core/model_scanner.h"
#include "core/persona.h"
#include "core/persona_doc.h"
#include "core/persona_memory.h"
#include "core/i18n.h"
#include "core/tts_manager.h"
#include "core/weather_alert.h"
#include "core/tts_segment_pipeline.h"
#include "single_instance.h"
#include "speech_controller.h"

#ifdef Q_OS_WIN
#include "../media/tts_engine_sapi.h"
#endif

namespace fs = std::filesystem;

namespace {

// i18n 訊息表目錄：開發環境用 repo 的 i18n/，部署用執行檔旁的 i18n/
fs::path resolveI18nDir() {
#ifdef L2M_DEV_I18N_DIR
  const fs::path dev = fs::u8path(L2M_DEV_I18N_DIR);
  std::error_code ec;
  if (fs::exists(dev / "en.json", ec)) return dev;
#endif
  return fs::u8path(QCoreApplication::applicationDirPath().toStdString()) / "i18n";
}

fs::path appDataDir() { return fs::u8path(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toStdString()); }

// 只從設定檔挖出「要不要停用硬體加速」這一個旗標。
//
// 為什麼不直接建 ConfigStore：那個決定必須趕在 Qt 建立 GL 之前下，
// 而 ConfigStore 是 QObject（內含防抖用的 QTimer），在 QApplication 之前
// 建構的話計時器不會觸發，設定就只剩結束時才寫得出去。
// 這裡只做一次唯讀解析，不碰任何 Qt 物件。
bool readDisableHardwareAcceleration(const fs::path& configPath) {
  auto doc = l2m::jsonu::Doc::parseFile(configPath);
  if (!doc || !doc->root()) return false;
  yyjson_val* app = l2m::jsonu::get(doc->root(), "app");
  yyjson_val* value = l2m::jsonu::get(app, "disableHardwareAcceleration");
  return value && yyjson_is_bool(value) && yyjson_get_bool(value);
}

}  // namespace

int main(int argc, char* argv[]) {
  // 日誌一定要排在最前面，而且要在 --mcp-stdio 分流「之前」：
  // spdlog 出廠的 default logger 寫的是 **stdout**，而橋接模式的 stdout
  // 就是 MCP 協定本身的傳輸通道 —— 印一個位元組進去 JSON-RPC 訊框就髒了。
  // 排在這裡讓那件事由結構保證，而不是靠「記得不要在橋接模式裡寫日誌」。
  // 同時這也是唯一能攔到下面那幾行「QApplication 之前的 qWarning」的位置。
  //
  // 宣告成第一個 local ＝ 最後一個解構，日誌活得比所有子系統的 teardown 都久。
  // 這一階段只有 console，檔案 sink 要等搶到 single-instance 鎖之後才補上。
  const l2m::LoggingGuard logging;

  // 當機報告：安裝點必須在 --mcp-stdio 分流「之前」，三種行程才都受保護。
  // 代價是那時 QApplication 還沒建、setApplicationName 還沒跑，QStandardPaths
  // 查不到路徑 —— 所以目錄由 appDataDirFromEnv() 手刻讀 %APPDATA% 算出來。
  // 這跟下面 readDisableHardwareAcceleration() 手刻 yyjson 讀一次是同一種
  // 刻意的重複，理由見 platform/crash_handler.h。
  const char* crashMode = "main";
  if (argc >= 2 && std::strcmp(argv[1], "--mcp-stdio") == 0) {
    crashMode = "mcp-stdio";
  } else if (argc >= 2 && std::strcmp(argv[1], l2m::kSplashFlag) == 0) {
    crashMode = "splash";
  }
  l2m::platform::installCrashHandler(l2m::platform::appDataDirFromEnv() / "logs", crashMode);

  // stdio 橋接模式：只是一支管線轉發器，不建立 QApplication、不開視窗、
  // 不搶單一實例鎖（每個 client 會各開一個）。必須排在所有 Qt 初始化之前。
  if (argc >= 2 && std::strcmp(argv[1], "--mcp-stdio") == 0) {
    return l2m::runMcpStdioBridge(argc, argv);
  }

  // 啟動畫面子行程：同樣是同一個執行檔裡完全不同的程式 ——
  // 不建 ConfigStore、不碰 GL、不搶 single-instance 鎖。
  // 存在的理由見 splash_process.h（主行程的 GUI 執行緒會被模型載入卡住數秒）。
  if (argc >= 2 && std::strcmp(argv[1], l2m::kSplashFlag) == 0) {
    return l2m::runSplashProcess(argc, argv);
  }

  // 透明 OpenGL 視窗必須有 alpha buffer，且必須在 QApplication 建立「之前」設定。
  // Cubism 的 GL renderer 是 ES2 風格（client-side vertex array），
  // core profile 完全禁止這種寫法，必須用 compatibility context
  // （Windows 驅動給 4.x compat；macOS 沒有 compat 就退回 2.1 legacy，同官方範例）。
  QSurfaceFormat fmt;
  fmt.setAlphaBufferSize(8);
  fmt.setRenderableType(QSurfaceFormat::OpenGL);
  fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
  fmt.setSwapInterval(1);  // vsync
  QSurfaceFormat::setDefaultFormat(fmt);

  // 應用程式名稱要在任何 QStandardPaths 查詢之前設定：
  // AppDataLocation 是 %APPDATA%/<applicationName>，名字沒設就會少一層目錄，
  // 整個 app 會去讀寫錯的設定檔。
  QCoreApplication::setApplicationName(QStringLiteral("live2d_mate"));

  // 設定與模型目錄都在 %APPDATA%/live2d_mate 底下
  const fs::path appData = appDataDir();
  const fs::path configPath = appData / "config.json";
  const fs::path modelsDir = appData / "models";
  const fs::path personasDir = appData / "personas";
  const fs::path memoryDir = appData / "memory";

  // 模型目錄要在這裡就建出來，不能等到有東西要寫才建。
  // ConfigStore 只會建 config.json 的上層目錄（config_store.cpp），
  // models/ 在此之前沒有任何人建 —— 而系統匣的「開啟模型資料夾」是
  // QDesktopServices::openUrl(file://…)，路徑不存在時它只是回傳 false，
  // 什麼都不會發生也沒有錯誤訊息（實測 macOS 上就是這樣沒反應）。
  // 目錄早已存在的機器上看不出來，全新環境才會踩到。
  std::error_code modelsDirEc;
  fs::create_directories(modelsDir, modelsDirEc);
  if (modelsDirEc) {
    qWarning() << "[models] 建立模型目錄失敗:" << QString::fromStdString(modelsDir.u8string()) << QString::fromStdString(modelsDirEc.message());
  }

  // 角色描述目錄同理：設定視窗的「角色」分頁有「開啟資料夾」的入口，
  // 而使用者一份都還沒建的時候那個目錄根本不存在。
  std::error_code personasDirEc;
  fs::create_directories(personasDir, personasDirEc);
  if (personasDirEc) {
    qWarning() << "[persona] 建立角色描述目錄失敗:" << QString::fromStdString(personasDir.u8string()) << QString::fromStdString(personasDirEc.message());
  }

  // 長期記憶目錄同理（memory/<角色名>.md；core/persona_memory.h）
  std::error_code memoryDirEc;
  fs::create_directories(memoryDir, memoryDirEc);
  if (memoryDirEc) {
    qWarning() << "[llm] 建立記憶目錄失敗:" << QString::fromStdString(memoryDir.u8string()) << QString::fromStdString(memoryDirEc.message());
  }

  // 硬體加速的關閉必須在 Qt 建立 GL 之前決定，之後才設完全沒有作用
  //（M0-M3 的版本漏了這一點）。這裡只唯讀地挖那一個旗標，
  // ConfigStore 本體要等 QApplication 之後才建（它內含 QTimer）。
  if (readDisableHardwareAcceleration(configPath)) {
    QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
  }

  QApplication app(argc, argv);
  app.setApplicationDisplayName(QStringLiteral("Live2D Mate"));
#ifdef L2M_APP_VERSION
  app.setApplicationVersion(QStringLiteral(L2M_APP_VERSION));
#endif
  // 桌寵沒有「最後一個視窗」的概念，關窗不等於結束（結束只從系統匣走）
  app.setQuitOnLastWindowClosed(false);

  // 全 app 的視窗圖示（標題列左上角、工作列按鈕、Alt-Tab）。
  // 設在這裡而不是各個視窗自己設：QWindow 沒有自己的 icon 時就會沿用這一份，
  // 所以設定視窗、氣泡、將來新增的視窗全部一次到位。
  //
  // 用 .ico 而不是 icon.png，是因為它一個檔案裡就帶了 9 種尺寸，
  // 而其中 16²／32² 刻意換成系統匣那組簡化線稿（1024² 的插畫縮到 16² 只會糊掉）。
  // 對應關係與產生方式見 resources/make_app_ico.py。
  //
  // 讀 .ico 要靠 Qt 的 qico imageformats plugin，部署時漏掉那顆 dll 會整個讀不到，
  // 所以留一條退路到 PNG —— 尺寸不理想，總比沒有圖示好。
  QIcon appIcon(QStringLiteral(":/icons/app.ico"));
  if (appIcon.isNull()) {
    qWarning() << "[app] app.ico 讀取失敗（缺少 qico plugin？），退回 icon.png";
    appIcon = QIcon(QStringLiteral(":/icons/icon.png"));
  }
  QApplication::setWindowIcon(appIcon);

  // 命令列旗標的判定排在 single-instance 之前：--set-model 得跟著請求一起送給
  // 既有實例（見 single_instance.h），搶到鎖之後才解析就來不及了。
  // 也順便決定 splash 要不要顯示 —— 那件事得先知道是不是 --hidden 啟動。
  std::vector<std::string> argList;
  for (const QString& arg : app.arguments()) argList.push_back(arg.toStdString());
  const bool startHidden = l2m::platform::shouldStartHidden(argList);
  const std::optional<std::string> setModelPath = l2m::setModelArg(argList);

  // 單一實例：第二個實例啟動時，通知既有實例顯示角色（或換模型）後直接退出
  SingleInstance instance(QString::fromLatin1(l2m::kInstanceServerName));
  if (!instance.tryAcquire(setModelPath ? QString::fromStdString(*setModelPath) : QString())) {
    // 換模型是有成敗的，把它變成離開碼給命令列的呼叫者。
    //（Live2D Viewer 走的**不是**這條：桌寵在跑時它自己連 socket 讀回覆，
    //  沒在跑時 QProcess::startDetached 根本不看離開碼。這條只在
    //  「手動下 --set-model」與啟動競態時會走到。）
    if (!instance.reply().ok) {
      qWarning() << "[models] 既有實例回報失敗:" << QString::fromStdString(instance.reply().message);
      return 1;
    }
    return 0;
  }

  // 日誌第二階段：補上檔案 sink，並回放開機到這裡為止暫存的訊息。
  // **只有搶到鎖的主行程開檔案**：橋接（每個 client 一個）、splash 子行程、
  // 以及剛剛被擋下來的第二實例都不開 —— 多行程同時持有同一個 log 檔時，
  // 輪替的改名會失敗（理由詳見 logging.h）。
  l2m::attachLogFile(appData / "logs");

  l2m::i18n::setMessagesDir(resolveI18nDir());

  // ── 啟動 Splash 子行程 ──
  // 為什麼是另一個行程而不是這個行程裡的視窗：模型載入會把這裡的 GUI
  // 執行緒整個卡住數秒（光是 CreateRenderer 的 shader 編譯就 2.3 秒），
  // 同一個行程裡不管怎麼推畫面，那段動畫都一定是凍的。詳見 splash_process.h。
  // 這裡只負責把它生出來，之後靠一條 local socket 的「斷線」通知它收尾。
  const bool showSplash = !startHidden && !qEnvironmentVariableIsSet("L2M_NO_SPLASH");
  const QString splashServer = l2m::splashServerName();
  QLocalSocket splashLink;
  // 執行期換模型時另外開的膠囊提示（見下方 onModelLoadStarted）。
  // 只記名字不掛連線：那個子行程是「連上即收尾」，不像啟動畫面要一路掛著。
  QString compactServer;
  int compactSeq = 0;
  bool splashSpawned = false;
  if (showSplash) {
    splashSpawned = QProcess::startDetached(QCoreApplication::applicationFilePath(), {QString::fromLatin1(l2m::kSplashFlag), splashServer});
    if (!splashSpawned) qWarning() << "[splash] 子行程啟動失敗，這次啟動沒有 splash";
  }

  // ConfigStore 是 QObject（內含防抖用的 QTimer），一定要在 QApplication 之後建
  l2m::ConfigStore config(configPath);

  CharacterWindow window;
  l2m::WindowManager windowManager(window, config);
  l2m::BubbleWindow bubble;
  l2m::AppController controller(config, window, windowManager, modelsDir, personasDir, memoryDir);

  // ── 語音 ──
  l2m::HttpJson http;
  std::vector<std::unique_ptr<l2m::TtsEngine>> engines;
  // 陣列順序即為 fallback 順序：線上品質優先，本機服務次之，系統內建離線語音墊底
  engines.push_back(std::make_unique<l2m::EdgeTtsEngine>(http));
  engines.push_back(std::make_unique<l2m::GptSovitsEngine>(http, [&config] { return config.get().tts.gptsovits; }));
  engines.push_back(std::make_unique<l2m::VoiceboxEngine>(http, [&config] { return config.get().tts.voicebox; }, [&controller] { return controller.uiLocale(); }));
  // 自訂端點排在這裡而不是第一個：TtsManager::defaultEngineId() 取的就是第一個，
  // 放前面會把既有使用者的預設引擎從 edge 換掉。沒填 URL 時 isAvailable 回 false，
  // 所以「沒設定過」的人完全感覺不到它的存在。
  engines.push_back(std::make_unique<l2m::CustomTtsEngine>(http, [&config] { return config.get().tts.custom; }));
#ifdef Q_OS_WIN
  engines.push_back(std::make_unique<l2m::SapiEngine>());
#endif

  // 不會分塊回傳的引擎全部包上句段管線：文字切句、逐句合成、邊播邊合成下一句。
  // 這一層是「開口延遲」最大的來源之一 —— 實測本機 GPT-SoVITS 唸 70 字要
  // 10 秒才出聲，切句之後第一句約 2 秒。streams() 為 true 的引擎（真正邊收邊送的）
  // 不必包，包了反而多一層搬運。
  for (auto& engine : engines) {
    if (engine->streams()) continue;
    engine = std::make_unique<l2m::SegmentedTtsEngine>(std::move(engine));
  }

  l2m::TtsManager tts(std::move(engines));
  l2m::AudioPlayer player;
  l2m::SpeechController speech(config, tts, player, bubble);

  // ── 接線 ──
  controller.speakHandler = [&speech](const l2m::AppController::SpeakRequest& request, std::function<void(l2m::CommandResult)> done) {
    l2m::SpeechController::Request job;
    job.text = request.text;
    job.voice = request.voice;
    job.engine = request.engine;
    job.rate = request.rate;
    job.wait = request.wait;
    job.thinking = request.thinking;
    job.onSpeechStart = request.onSpeechStart;
    speech.speak(job, std::move(done));
  };
  // 嘀咕（自主台詞的 bubble 模式）：走 SpeechController 的同一條佇列，
  // 與真的說話互斥，氣泡不會互相蓋台
  controller.mutterHandler = [&speech](const std::string& text, std::function<void(l2m::CommandResult)> done) { speech.mutter(text, std::move(done)); };
  controller.stopSpeakingHandler = [&speech] { return speech.stopSpeaking(); };
  controller.voicesJsonProvider = [&speech](const std::optional<std::string>& engine, std::function<void(std::string)> done) { speech.voicesJson(engine, std::move(done)); };
  controller.speakingProbe = [&speech] { return speech.speaking(); };
  controller.ttsStateJson = [&speech] { return speech.stateJson(); };
  // 接的是 speaking() 而不是「環裡此刻有沒有音訊」：串流缺料時嘴巴要柔和收合、
  // 而且要繼續擋住 AI 的 set_parameters（ParameterOverlay::skipForLipSync），
  // 不能因為一次網路抖動就把嘴巴的控制權交出去。
  controller.mouthOpenSource = [&player] { return player.speaking() ? player.mouthOpen() : 0.0f; };
  controller.lipSyncActive = [&player] { return player.speaking(); };

  // 說完一句就重新開始閒置倒數 —— 一句長台詞講完，閒置時間不該已經先去掉一半。
  // 自主表演的 speak 步驟不會被這一條打掉閒置時鐘：finished 是在 speak 的 done
  // 回呼之後**同一個呼叫堆疊**裡發的（completeCurrent → answer → runNext →
  // emit finished），那時 PerformRunner 只是排了 deleteLater 還沒真的解構，
  // idleSuppress_ 仍然持有（守衛由 done_ lambda 抓著，解構才釋放）。
  // 若未來把 finished 改成 queued 發送，這個保證就破了 —— 要改先看這裡。
  QObject::connect(&speech, &l2m::SpeechController::finished, &controller, [&controller] { controller.touchIdle(); });

  // ── 天氣 ──
  // 服務只讀設定、不寫 config（media/weather_service.h 的第一點），
  // 所以自動定位的結果要在這裡寫回去；寫回會發 changed → applyConfig()，
  // 服務下一步就抓得到預報了。
  l2m::WeatherService weather(http, [&config] { return config.get().weather; });
  QObject::connect(&weather, &l2m::WeatherService::locationResolved, &app, [&config](double latitude, double longitude, const QString& name) {
    try {
      config.patch(l2m::weatherLocationPatch(latitude, longitude, name.toStdString()));
    } catch (const std::exception& error) {
      qWarning() << "[weather] 寫回定位結果失敗:" << error.what();
    }
  });
  // 設定一改就重排輪詢（開關、地點、間隔都在裡面）
  QObject::connect(&config, &l2m::ConfigStore::changed, &weather, [&weather] { weather.applyConfig(); });
  weather.applyConfig();
  // 預警判斷每 5 秒的在席心跳現讀一次；服務那邊 15 分鐘才真的抓一次網路
  controller.weatherSnapshot = [&weather] { return weather.snapshot(); };
  // 啟動歡迎詞要等這個為真才講，好讓 LLM 有天氣可以拿來打招呼
  //（等待上限在 AppController::greetOnLaunch）。
  // **失敗也算有結果** —— 抓不到天氣不該讓桌寵連招呼都不打。
  controller.weatherReady = [&weather, &config] {
    if (!config.get().weather.enabled) return true;
    return weather.snapshot().valid || !weather.lastError().empty();
  };

  // ── 自主行為 ──
  // 規劃器有兩個腦：LLM 版（src/llm/llm_behavior_planner.h，設定齊全且
  // driveIdle 開啟時接手）與本機規則版（core/idle_director.h，永遠的保底）。
  // AppController 那側只認 behaviorPlanner 這一個掛鉤，換腦它一個字都不用改。
  QElapsedTimer directorClock;
  directorClock.start();
  l2m::IdleDirector director({
    [] { return QRandomGenerator::global()->generateDouble(); },
    [&directorClock] { return static_cast<double>(directorClock.elapsed()); },
  });

  l2m::LlmManager llm(http, [&config] { return config.get().llm; });
  controller.llmStateJson = [&llm] { return llm.stateJson(); };

  l2m::LlmBehaviorPlanner::Deps llmPlannerDeps;
  llmPlannerDeps.llm = &llm;
  llmPlannerDeps.getConfig = [&config] { return config.get().llm; };
  llmPlannerDeps.nowMs = [&directorClock] { return static_cast<double>(directorClock.elapsed()); };
  // prompt 只送 # Character Description（persona_doc.h 的鐵律），每次現讀 ——
  // 內建 LLM 沒有 MCP「要重連才生效」的限制，換角色下一輪就吃到新的
  llmPlannerDeps.personaDescription = [&controller] { return l2m::parsePersonaDoc(controller.personaSnapshot().activeText()).description; };
  // 長期記憶同樣現讀：memory/<角色>.md，readPersona 換個目錄就是記憶檔
  llmPlannerDeps.memoryText = [&controller] {
    const std::string active = controller.activePersonaName();
    if (active.empty()) return std::string();
    const auto raw = l2m::readPersona(controller.memoryDir(), active);
    return raw ? l2m::clampPersonaMemory(*raw) : std::string();
  };
  llmPlannerDeps.hourNow = [] { return QTime::currentTime().hour(); };
  // 天氣每一輪都帶（有資料的話）：使用者要的「當背景知識」就是這個
  llmPlannerDeps.weatherLine = [&weather, &config] { return l2m::weatherContextLine(weather.snapshot(), QDateTime::currentSecsSinceEpoch(), config.get().weather.unit == "f"); };
  l2m::LlmBehaviorPlanner llmPlanner(std::move(llmPlannerDeps));

  controller.behaviorPlanner = [&director, &llmPlanner](const l2m::IdleWorld& world, const l2m::IdlePlanOptions& options, std::function<void(std::vector<l2m::PerformStep>)> done) {
    // 規則版以值捕捉 world/options：LLM 失敗的 fallback 是在網路往返之後才跑的
    auto fallback = [&director, world, options] { return director.plan(world, options); };
    if (llmPlanner.plan(world, options, fallback, done)) return;
    if (done) done(fallback());
  };
  // 換模型後把冷卻與不重複的記憶歸零（群組名是模型專屬的）
  QObject::connect(&controller, &l2m::AppController::modelSwitchFinished, &app, [&director](bool) { director.reset(); });

  // 氣泡跟著角色走
  QObject::connect(&windowManager, &l2m::WindowManager::boundsChanged, &bubble, [&bubble](const QRect& bounds) { bubble.follow(bounds); });
  QObject::connect(&windowManager, &l2m::WindowManager::visibleChanged, &bubble, [&bubble](bool visible) { bubble.setCharacterVisible(visible); });

  // ── 接上 splash 子行程，並在這裡就接好收尾訊號 ──
  // 一定要在 setVisible 之前：QWindow::setVisible(true) 在 Windows 上會同步送
  // WM_PAINT 給視窗過程，所以下面那行當場就跑完 initializeGL()、把 glReady_ 設成
  // true（此時 pendingModelEntry_ 還是空的，什麼都沒載）。等到 controller.start()
  // 呼叫 requestModelLoad 時，走的已經是 glReady_ == true 的「就地同步載入」分支
  // —— 模型其實是在 start() 裡載完並 emit modelLoaded 的，根本沒等到 app.exec()。
  // connect 放在 start() 之後就是接在 emit 之後，整組落空（實測過）。
  if (showSplash) {
    // 連上就一直掛著：這條連線不傳資料，「斷線」本身就是收尾訊號。
    // 主行程若崩潰，OS 會替我們關掉它，splash 不會變成孤兒視窗。
    // 重試窗給到 1.5 秒：冷啟動的子行程實測要 ~691 ms 才 listen 就緒（見 80e9052），
    // 連不上的代價是 splash 要賴到自己的 10 秒逾時才收。spawn 失敗就不用白等了。
    for (int attempt = 0; splashSpawned && attempt < 60; ++attempt) {
      splashLink.connectToServer(splashServer);
      if (splashLink.waitForConnected(25)) break;
      splashLink.abort();
    }
    if (splashLink.state() != QLocalSocket::ConnectedState) qWarning() << "[splash] 連不上子行程，它會自己逾時收掉";

    // 執行期換模型：主行程會被同步載入卡住 2-5 秒，角色在那段時間是消失的
    //（loadPendingModel 開頭就先 reset 掉舊的 controller_）。同樣交給子行程畫，
    // 只是換成貼在角色身上的小卡片，而不是螢幕正中央那張大 banner。
    controller.onModelLoadStarted = [&] {
      // 啟動階段的大 splash 還在，這段已經被它蓋住了
      if (splashLink.state() == QLocalSocket::ConnectedState) return;
      if (!compactServer.isEmpty()) return;  // 上一次的還沒收掉

      const QRect at = windowManager.bounds();
      const QString name = splashServer + QStringLiteral("-c%1").arg(++compactSeq);
      if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), {QString::fromLatin1(l2m::kSplashFlag), name, QStringLiteral("--compact"), QString::number(at.x()), QString::number(at.y()),
                                                                             QString::number(at.width()), QString::number(at.height())})) {
        return;
      }
      // spawn 完就回去載入，不在這裡等它握手。
      // 曾經在這裡跑一個 400 ms 的連線重試迴圈，結果兩頭皆空：子行程實測要
      // 450 ms 以上才 listen 得起來，連不上（膠囊只能等自己的 3 秒逾時才消失），
      // 而那 400 ms 又是白白加在切換時間上的。改成載入完成後再連上通知一次 ——
      // 那時它一定早就就緒了。
      compactServer = name;
    };

    // 等的是「第一幀真的畫上螢幕」而不是 modelLoaded —— 後者只代表 GL 資源
    // 準備好，那時畫面還是空的，等待畫面一走會露出一瞬間的空白。
    // 這兩個訊號每次切模型都會發，但斷過的 socket 再斷是 no-op。
    auto dismiss = [&splashLink, &compactServer] {
      // 啟動畫面：一路掛著的連線，斷開就是訊號
      splashLink.disconnectFromServer();
      // 換模型的膠囊：連上去就是訊號，連完立刻收
      if (compactServer.isEmpty()) return;
      QLocalSocket notify;
      notify.connectToServer(compactServer);
      // 小模型載得比子行程起得還快，這裡要等它一下才連得上。
      // 阻塞的是「載入已經結束」之後的這一小段，不影響切換本身。
      notify.waitForConnected(500);
      notify.disconnectFromServer();
      compactServer.clear();
    };
    QObject::connect(&window, &CharacterWindow::firstFrameRendered, &app, dismiss);
    // 載入失敗就不會有第一幀了，這條是唯一的退路
    QObject::connect(&window, &CharacterWindow::modelLoaded, &app, [dismiss](bool ok) {
      if (!ok) dismiss();
    });
  }

  // 角色視窗要先顯示，第一次 expose 才會觸發 initializeGL。
  // 它在模型載入完成前是整片透明的，所以蓋在 splash 上也看不見。
  if (!startHidden) windowManager.setVisible(true);

  bubble.setLocale(controller.uiLocale());
  bubble.setAlwaysOnTop(config.get().app.alwaysOnTop);
  bubble.setOffsetY(config.get().tts.bubbleOffsetY);
  bubble.setShadow(l2m::bubbleShadowFromId(config.get().tts.bubbleShadow));
  // 氣泡錨點：模型載好、遮罩落地時量一次視覺上下緣，氣泡貼的就是頭頂而不是視窗頂端
  controller.onModelExtent = [&bubble](double top, double bottom) { bubble.setModelExtent(top, bottom); };
  bubble.follow(windowManager.bounds());

  // 初次啟動（一份角色描述都沒有）：種入預設秘書角色。
  // 判斷與略過都在 seedDefaultPersona 裡，這裡無條件呼叫。
  controller.seedDefaultPersona();

  // --set-model：這一次啟動就用這隻外部模型。刻意寫進 config 而不是另外開一條
  // 載入路徑 —— start() 的 rescan() 本來就會把 model.current 的絕對路徑補回模型
  // 清單（見 AppController::rescan），於是成功、以及「檔案不見了就退回第一隻」
  // 兩條路都跟平常的啟動一模一樣。
  //
  // **先驗過才寫**：路徑打錯時無條件覆蓋的話，start() 會找不到而退回第一隻，
  // switchModel 再把那一隻寫回 config —— 使用者原本選的模型就因為一個錯字
  // 被安靜地換掉了。讀不到就整段跳過，維持原本的設定。
  if (setModelPath) {
    const fs::path entry = fs::u8path(*setModelPath);
    if (const std::optional<std::string> inside = l2m::modelsDirRelativeId(entry, modelsDir)) {
      config.patch(l2m::stringPatch("model", "current", *inside));
    } else if (l2m::describeModel(entry)) {
      config.patch(l2m::stringPatch("model", "current", l2m::externalModelId(entry)));
    } else {
      qWarning() << "[models] --set-model 讀不到這個模型，維持原本的設定:" << QString::fromStdString(*setModelPath);
    }
  }

  controller.start();
  controller.applyConfig();

  // start() 之後沒有目前模型 → switchModel 沒被呼叫過 → pendingModelEntry_ 是空的
  // → loadPendingModel() 直接 return、modelLoaded 永遠不會來。這裡補一刀斷線，
  // splash 才不用等到自己的逾時。（models 目錄是空的、或設定指到已刪除的模型
  // 而掃描結果也空）有模型時上面的 modelLoaded 早就斷過了，再斷一次是 no-op。
  if (showSplash && !controller.currentModel()) splashLink.disconnectFromServer();

  // 走 controller 而不是 windowManager，被叫醒的角色才是淡入的（見 AppController::setVisible）
  QObject::connect(&instance, &SingleInstance::secondInstanceLaunched, &controller, [&controller] { controller.setVisible(true); });

  // 第二個實例帶了 --set-model（Live2D Viewer 的「設定為桌寵」走的就是這條）。
  // 用 std::function 而不是訊號，是因為成敗要原樣回給送出的那一端。
  instance.onSetModel = [&controller](const QString& path) {
    controller.setVisible(true);
    return controller.adoptExternalModel(fs::u8path(path.toStdString()));
  };

  // 設定變更時把跟著設定跑的執行期狀態補上（氣泡的置頂、語系與間距微調）。
  // AppController::applyConfig() 不碰氣泡，這條 changed 才是氣泡真正生效的路徑。
  QObject::connect(&config, &l2m::ConfigStore::changed, &bubble, [&bubble, &controller](const l2m::AppConfig& cfg) {
    bubble.setAlwaysOnTop(cfg.app.alwaysOnTop);
    bubble.setLocale(controller.uiLocale());
    bubble.setOffsetY(cfg.tts.bubbleOffsetY);
    bubble.setShadow(l2m::bubbleShadowFromId(cfg.tts.bubbleShadow));
    if (!cfg.tts.showBubble) bubble.hideBubble();
  });

  // ── MCP ──
  l2m::McpTools mcpTools(controller);
  l2m::McpHttpServer mcpServer(mcpTools, QApplication::applicationVersion());

  controller.mcpStateJson = [&config, &mcpServer] {
    const l2m::AppConfig& cfg = config.get();
    const l2m::McpHttpServer::Status state = mcpServer.status();
    std::string json = "{\"enabled\":";
    json += cfg.mcp.enabled ? "true" : "false";
    json += ",\"host\":\"" + cfg.mcp.host + "\"";
    json += ",\"port\":" + std::to_string(cfg.mcp.port);
    json += ",\"url\":" + (state.running ? "\"" + state.url + "\"" : std::string("null"));
    json += "}";
    return json;
  };

  // 角色描述要走三條通道送到 AI 面前（initialize 的 instructions、
  // speak/perform 的工具說明、resources）。三條都由 handleRpc 在 httplib 的
  // worker 執行緒上組，所以內容不能現讀磁碟 —— GUI 執行緒推一份快照過去，
  // worker 只讀複本。
  const auto pushPersona = [&controller, &mcpServer] { mcpServer.setPersona(controller.personaSnapshot()); };
  QObject::connect(&controller, &l2m::AppController::personaChanged, &app, pushPersona);
  pushPersona();

  // 發話節奏的四個開關（設定 → MCP）只影響 initialize 的 instructions 文字，
  // 但那段文字同樣是在 worker 執行緒上組的，所以也走「GUI 執行緒推快照」。
  // 沒有這一段的話四個核取方塊會全部是死的 —— config.json 寫得進去，
  // AI 拿到的卻永遠是 SpeechProtocol 的預設值。
  const auto pushSpeechProtocol = [&mcpServer](const l2m::AppConfig& cfg) {
    mcpServer.setSpeechProtocol(l2m::SpeechProtocol{cfg.mcp.talkative, cfg.mcp.notifyOnComplete, cfg.mcp.speakNoWait, cfg.mcp.announceSteps});
  };
  QObject::connect(&config, &l2m::ConfigStore::changed, &app, pushSpeechProtocol);
  pushSpeechProtocol(config.get());

  if (config.get().mcp.enabled) {
    const l2m::AppConfig& cfg = config.get();
    mcpServer.start(cfg.mcp.host, cfg.mcp.port, cfg.mcp.token);
  }

  // ── 設定視窗 ──
  // 一定要建在 mcpServer 之後：它持有 McpHttpServer&，而解構是建構的反序，
  // 這樣視窗才會先於伺服器解構（MCP 分頁掛著 statusChanged）。
  l2m::VoiceDeps voiceDeps;
  voiceDeps.listEngines = [&speech](std::function<void(std::vector<l2m::TtsEngineInfo>)> done) { speech.listEngines(std::move(done)); };
  voiceDeps.listVoices = [&speech, &config](std::function<void(std::vector<l2m::VoiceInfo>)> done) {
    // 只列目前引擎的語音；跨引擎混在一起下拉會長到爆
    speech.listVoices(config.get().tts.engine, std::move(done));
  };
  voiceDeps.resetVoiceCache = [&tts] { tts.resetCache(); };

  // 一般分頁調整氣泡間距時借氣泡視窗顯示預覽 —— 那個設定不看畫面根本調不了。
  // 與 voiceDeps 同理：BubbleWindow 與 SpeechController 都不是 AppController 的
  // 能力，由這裡注入，設定分頁才不必反過來認識它們。
  l2m::BubbleDeps bubbleDeps;
  bubbleDeps.showPreview = [&bubble](const QString& text) { bubble.showText(text); };
  bubbleDeps.hidePreview = [&bubble] { bubble.hideBubble(); };
  bubbleDeps.speaking = [&speech] { return speech.speaking(); };

  // LLM 分頁的「測試連線」吃表單上的設定（還沒按套用），所以臨時建一個
  // 引擎用那份設定發請求；引擎只在呼叫當下用到，回呼不抓它，栈上建完就走。
  l2m::LlmDeps llmDeps;
  llmDeps.listModels = [&http](const l2m::LlmConfig& config, std::function<void(std::vector<std::string>, std::string)> done) {
    l2m::HttpLlmEngine probe(http, [config] { return config; });
    probe.listModels(std::move(done));
  };
  // 角色卡 AI 擴寫：走 LlmManager 的存檔設定（不像 listModels 吃表單 ——
  // 擴寫是「用你設好的 LLM 幫忙寫」，不是連線測試）。enabled 這裡要擋：
  // 引擎只驗欄位齊不齊，開關語意是呼叫端的事。
  llmDeps.chatBuffered = [&llm, &config](std::vector<l2m::LlmMessage> messages, l2m::LlmChatOptions options, std::function<void(std::string, std::string)> done) {
    if (!config.get().llm.enabled) {
      if (done) done("", "LLM is disabled. Enable it in Settings > LLM first.");
      return std::unique_ptr<l2m::LlmRequestHandle>(std::make_unique<l2m::FunctionLlmHandle>(std::make_shared<bool>(false)));
    }
    return llm.chatBuffered(messages, options, std::move(done));
  };

  // 天氣分頁區塊的能力。geocoding 的 language 用 UI 語系的主要語言 ——
  // 使用者用母語打「台北」「東京」也查得到
  l2m::WeatherDeps weatherDeps;
  weatherDeps.resolveCity = [&weather, &controller](const std::string& name, std::function<void(bool, double, double, std::string, std::string)> done) {
    const std::string locale = controller.uiLocale();
    weather.resolveCity(name, locale.substr(0, locale.find('-')), [done](bool ok, l2m::GeoLocation location, std::string error) {
      if (done) done(ok, location.latitude, location.longitude, location.name, error);
    });
  };
  weatherDeps.summary = [&weather, &config] { return l2m::weatherContextLine(weather.snapshot(), QDateTime::currentSecsSinceEpoch(), config.get().weather.unit == "f"); };
  weatherDeps.lastError = [&weather] { return weather.lastError(); };

  l2m::SettingsWindow settings(controller, mcpServer, std::move(voiceDeps), std::move(bubbleDeps), std::move(llmDeps), std::move(weatherDeps));
  // 天氣抓回來（或失敗）之後，設定視窗若正開著就把狀態列重畫
  QObject::connect(&weather, &l2m::WeatherService::snapshotChanged, &settings, [&settings] { settings.refreshVisible(); });
  QObject::connect(&weather, &l2m::WeatherService::statusChanged, &settings, [&settings] { settings.refreshVisible(); });
  // MCP 的 open_naming_editor 改成開設定視窗並停在模型分頁
  controller.settingsOpener = [&settings](l2m::SettingsTab tab) { settings.open(tab); };

  l2m::Tray::Deps trayDeps;
  trayDeps.openSettings = [&settings](l2m::SettingsTab tab) { settings.open(tab); };
  trayDeps.mcpEnabled = [&config] { return config.get().mcp.enabled; };
  trayDeps.mcpRunning = [&mcpServer] { return mcpServer.status().running; };
  trayDeps.mcpUrl = [&mcpServer] { return mcpServer.status().url; };
  trayDeps.quit = [&app, &config, &controller] {
    config.flush();
    // 角色淡出之後才真的結束。角色本來就看不見時 fadeOutThen 會就地執行，
    // 「離開」不會因此變慢。
    controller.fadeOutThen([&app] { app.quit(); });
  };

  l2m::Tray tray(controller, std::move(trayDeps));
  // 在設定視窗裡換了語言之後，系統匣的文字也要跟著換
  settings.trayRetranslate = [&tray] { tray.rebuild(); };

  // 模型載入失敗而設定視窗沒有認領時（開機、MCP、系統匣發起的載入）由系統匣氣泡講。
  // 最重要的情境是**開機**：config 指定的模型載不起來時，畫面上本來只有一片空白。
  controller.modelLoadFailedReporter = [&controller, &tray](const l2m::CommandResult& result) {
    QString body = QString::fromStdString(result.error);
    if (!result.hint.empty()) body += QStringLiteral("\n") + QString::fromStdString(result.hint);
    tray.notify(QString::fromStdString(l2m::i18n::translate(controller.uiLocale(), "model.loadFailed.notifyTitle")), body);
  };
  // 開機那一次發生在系統匣建出來之前，積著的補送出去
  controller.flushPendingModelLoadFailure();

  // 診斷用：設了 L2M_SAY 就在啟動 3 秒後講一句，用來驗證整條
  // 合成 → 播放 → 氣泡 → 口型 的鏈路（與 L2M_PROFILE 等環境變數同一套慣例）。
  const QString saySample = qEnvironmentVariable("L2M_SAY");
  if (!saySample.isEmpty()) {
    QTimer::singleShot(3000, &controller, [&controller, saySample] {
      l2m::AppController::SpeakRequest request;
      request.text = saySample.toStdString();
      request.wait = true;
      controller.speak(request, [](l2m::CommandResult result) {
        if (result.ok) {
          qInfo() << "[tts] L2M_SAY 播放完成";
        } else {
          qWarning() << "[tts] L2M_SAY 失敗:" << QString::fromStdString(result.error);
        }
      });
    });
  }

  // 上次異常結束的提示與 crash 檔清理。
  //
  // **刻意排在這裡而不是 handler 裡**：當機當下呼叫 UI 在堆疊已經爆掉
  //（0xC00000FD）的情況下很可能二次崩潰，所以提示交給下一次正常啟動的
  // 一般 Qt 程式碼路徑（見 platform/crash_handler.h）。
  {
    const fs::path logsDir = appData / "logs";
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(logsDir, ec)) {
      if (ec) break;
      if (!entry.is_regular_file(ec)) continue;
      names.push_back(entry.path().filename().u8string());
    }

    // **順序不能反**：先算出「最新那一份」，再刪過期的。倒過來的話，
    // 剛好累積到第 21 份時會把正要提示的那一份先刪掉 —— 使用者點下通知
    // 只會打開一個沒有那份報告的資料夾，而且完全看不出出過事
    const auto latest = l2m::latestCrashFile(names);
    for (const std::string& stale : l2m::expiredCrashFiles(names, l2m::kCrashKeepCount)) {
      fs::remove(logsDir / stale, ec);
    }

    // 記的是「提示過哪一份檔名」而不是布林旗標：同一份只跳一次，
    // 但下次再崩（檔名帶新的時間戳）就會再提示
    if (latest && *latest != config.get().crash.lastNotified) {
      qWarning() << "[crash] 偵測到上次異常結束:" << QString::fromStdString(*latest);
      // stringPatch 是 config_patch.h 既有的單欄位捷徑，逃脫由它內部的
      // jsonEscape 負責 —— 手拼 JSON 的話檔名裡的反斜線會讓整包 patch 解析失敗
      config.patch(l2m::stringPatch("crash", "lastNotified", *latest));
      const QString folder = QString::fromStdString(logsDir.u8string());
      tray.notify(QString::fromStdString(l2m::i18n::translate(controller.uiLocale(), "crash.notifyTitle")), QString::fromStdString(l2m::i18n::translate(controller.uiLocale(), "crash.notifyBody")),
                  [folder] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
    }
  }

  // 診斷用：設了 L2M_CRASH_TEST 就在啟動 3 秒後故意崩一次，驗證 crash handler
  //（與 L2M_SAY／L2M_PROFILE 同一套慣例，理由見 platform/crash_handler.h）
  if (!qEnvironmentVariable("L2M_CRASH_TEST").isEmpty()) {
    QTimer::singleShot(3000, [] { l2m::platform::triggerCrashTestFromEnv(); });
  }

  // MCP 狀態現在顯示在系統匣圖示的 tooltip，不再佔一列選單，
  // 所以只更新 tooltip 而不整份重建選單 —— 順帶讓這條 direct connection 上的
  // slot 變得極輕。設定視窗那一端由 McpPage 自己接。
  QObject::connect(&mcpServer, &l2m::McpHttpServer::statusChanged, &tray, &l2m::Tray::updateToolTip);

  QObject::connect(&app, &QCoreApplication::aboutToQuit, [&] {
    // 先停伺服器：join 工作執行緒並讓所有未決呼叫立刻失敗，
    // 不然 worker 會對著即將離開事件迴圈的 GUI 執行緒等滿 185 秒
    mcpServer.stop();
    speech.stopSpeaking();
    bubble.close();
    settings.close();
    config.flush();
  });

  const int result = app.exec();
  config.flush();
  return result;
}
