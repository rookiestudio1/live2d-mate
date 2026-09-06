#include "logging.h"

// spdlog 排在 Qt 之前：它在 Windows 上會拉 windows.h（自帶 WIN32_LEAN_AND_MEAN），
// 跟 httplib 同樣的理由 —— 順序反過來時 Qt 的標頭會先污染巨集空間
#include <spdlog/spdlog.h>
#include <spdlog/details/os.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "core/log_category.h"
#include "core/log_rotation.h"
#include "../platform/console.h"

// 沒設 L2M_LOG_LEVEL 時的預設等級，由 CMakeLists.txt 依建置型別給值
//（Debug 給 "debug"、其餘給 "info"，理由寫在那裡）。單獨編這個 TU 時退回 "debug"：
// 少一個編譯定義就讓整份日誌變安靜，是最難查的那種失敗
#ifndef L2M_DEFAULT_LOG_LEVEL
#define L2M_DEFAULT_LOG_LEVEL "debug"
#endif

namespace fs = std::filesystem;

namespace l2m {
namespace {

// 單一 log 檔的大小上限。L2M_LOG_MAX_BYTES 可覆寫 ——
// 正常日誌量要跑幾十分鐘才滿 1 MB，那是驗證輪替行為唯一可行的手段
constexpr size_t kDefaultMaxBytes = 1024 * 1024;

// 保留天數：今天與前兩天
constexpr int kKeepDays = 3;

// 第一階段到第二階段之間暫存幾則訊息。開機早期（QApplication 建立、
// GL 初始化、模型掃描之前）的量遠低於這個數
constexpr size_t kPendingCapacity = 512;

constexpr const char* kConsolePattern = "[%H:%M:%S.%e] [%-11n] %^%l%$ %v";
constexpr const char* kFilePattern = "[%Y-%m-%d %H:%M:%S.%e] [%-11n] [%l] [%t] %v";

std::shared_ptr<spdlog::sinks::dist_sink_mt> g_sinks;
std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_pending;
QtMessageHandler g_previousHandler = nullptr;
spdlog::level::level_enum g_level = spdlog::level::debug;
std::mutex g_loggerMutex;

spdlog::level::level_enum defaultLevel() {
  const spdlog::level::level_enum level = spdlog::level::from_str(L2M_DEFAULT_LOG_LEVEL);
  // 與 levelFromEnv 同一個理由：from_str 認不得時回 off，而 off 是「整份靜靜消失」。
  // 這裡認不得代表 CMake 那邊打錯字，退回 debug 讓人當場看得出來
  if (level == spdlog::level::off && std::strcmp(L2M_DEFAULT_LOG_LEVEL, "off") != 0) {
    return spdlog::level::debug;
  }
  return level;
}

spdlog::level::level_enum levelFromEnv() {
  const QByteArray raw = qgetenv("L2M_LOG_LEVEL").trimmed().toLower();
  if (raw.isEmpty()) return defaultLevel();
  const std::string text = raw.toStdString();
  const spdlog::level::level_enum level = spdlog::level::from_str(text);
  // from_str 認不得時回 off —— 那會讓使用者打錯一個字就整份日誌靜靜消失。
  // 只有真的寫了 off 才當成關閉
  if (level == spdlog::level::off && text != "off") return defaultLevel();
  return level;
}

size_t maxBytesFromEnv() {
  bool ok = false;
  const qulonglong value = qgetenv("L2M_LOG_MAX_BYTES").toULongLong(&ok);
  if (!ok || value == 0) return kDefaultMaxBytes;
  return static_cast<size_t>(value);
}

LogDate localDateOf(const spdlog::details::log_msg& msg) {
  const std::time_t stamp = std::chrono::system_clock::to_time_t(msg.time);
  const std::tm parts = spdlog::details::os::localtime(stamp);
  return LogDate{parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday};
}

// 「單檔 1 MB」＋「只留 3 天」，spdlog 現成的兩個 sink 各只滿足一半：
// rotating_file_sink 只管大小、不認日期；daily_file_sink 有天數保留、卻沒有大小上限。
//
// 檔案 IO 走 std::ofstream + std::filesystem::path，**不用 spdlog 自己的 file_helper**：
// 沒定義 SPDLOG_WCHAR_FILENAMES 時 spdlog 的 filename_t 是 std::string，Windows 上
// 走 _fsopen 的窄字元版本、用系統 ANSI 碼頁（正體中文 Windows ＝ CP950）解路徑 ——
// 使用者名稱含中文的 %APPDATA% 會直接開不了檔。MSVC 的 basic_filebuf 吃
// const path& 時走的是 _wfsopen，順帶也是 _SH_DENYNO 共享模式。
// 這也跟 config_store.cpp 用 std::ofstream 的既有寫法一致。
class DailyRotatingFileSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
  DailyRotatingFileSink(fs::path dir, size_t maxBytes, int keepDays) : dir_(std::move(dir)), maxBytes_(maxBytes > 0 ? maxBytes : kDefaultMaxBytes), keepDays_(keepDays) {}

protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    const LogDate date = localDateOf(msg);
    if (!file_.is_open() || date != date_) openForDate(date);
    if (!file_.is_open()) return;

    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);

    // 寫下去會超過就先換檔，所以檔案不會超出上限。一則訊息從不切成兩半 ——
    // 唯一的例外是單則訊息本身就大於上限（LLM 的 prompt dump 就有可能），
    // size_ > 0 的護欄讓空檔案無論如何先收下一則，否則會無限換檔
    if (size_ > 0 && size_ + formatted.size() > maxBytes_) {
      file_.close();
      ++index_;
      openCurrent();
      if (!file_.is_open()) return;
    }

    file_.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
    size_ += formatted.size();
    // 每則都 flush：這個 app 有 0xC0000409 直接 abort 的前科，
    // 而崩潰前那幾行正是最需要留下來的。本專案的日誌量負擔得起
    file_.flush();
  }

  void flush_() override {
    if (file_.is_open()) file_.flush();
  }

private:
  std::vector<std::string> listNames() const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
      if (ec) break;
      if (!entry.is_regular_file(ec)) continue;
      names.push_back(entry.path().filename().u8string());
    }
    return names;
  }

  void openForDate(const LogDate& date) {
    file_.close();
    std::error_code ec;
    fs::create_directories(dir_, ec);

    const std::vector<std::string> names = listNames();
    // 過期檔案在「開檔」這一刻清 —— 開機時會走一次，跨午夜換檔時再走一次。
    // 不另外掛計時器：桌寵可能連開好幾天不關，但那種情況跨午夜一定會經過這裡
    for (const std::string& expired : expiredLogFiles(names, date, keepDays_)) {
      fs::remove(dir_ / expired, ec);
    }

    date_ = date;
    index_ = latestLogIndex(names, date);
    if (index_ < 0) index_ = 0;
    openCurrent();
    // 接續當天最後一個序號；那個檔已經滿了就開下一號
    if (file_.is_open() && size_ >= maxBytes_) {
      file_.close();
      ++index_;
      openCurrent();
    }
  }

  void openCurrent() {
    const fs::path path = dir_ / logFileName(date_, index_);
    file_.open(path, std::ios::binary | std::ios::app);
    std::error_code ec;
    const auto existing = fs::file_size(path, ec);
    size_ = ec ? 0 : static_cast<size_t>(existing);
  }

  fs::path dir_;
  size_t maxBytes_;
  int keepDays_;
  std::ofstream file_;
  LogDate date_{};
  int index_ = 0;
  size_t size_ = 0;
};

std::shared_ptr<spdlog::logger> loggerFor(const std::string& name) {
  if (auto existing = spdlog::get(name)) return existing;
  // registry 自己雖然有鎖，但「查不到就註冊」這兩步之間會有競態
  //（MCP 的 worker 執行緒與 GUI 執行緒可能同時第一次用到同一個分類），
  // register_logger 撞名會丟 spdlog_ex，所以整段要是原子的
  std::lock_guard<std::mutex> lock(g_loggerMutex);
  if (auto existing = spdlog::get(name)) return existing;
  auto logger = std::make_shared<spdlog::logger>(name, g_sinks);
  logger->set_level(g_level);
  spdlog::register_logger(logger);
  return logger;
}

spdlog::level::level_enum toSpdLevel(QtMsgType type) {
  switch (type) {
    case QtDebugMsg:
      return spdlog::level::debug;
    case QtInfoMsg:
      return spdlog::level::info;
    case QtWarningMsg:
      return spdlog::level::warn;
    case QtCriticalMsg:
      return spdlog::level::err;
    case QtFatalMsg:
      return spdlog::level::critical;
  }
  return spdlog::level::info;
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
  const LogCategory split = splitLogCategory(message.toStdString());

  std::string name = split.name;
  if (name.empty()) {
    // Qt 自己的訊息帶 category（qt.qpa.window 之類）；我們的訊息一律是 default
    const bool named = context.category && std::strcmp(context.category, "default") != 0;
    name = named ? context.category : "app";
  }

  // file/line 只有定義 QT_MESSAGELOGCONTEXT 時才有值（見 CMakeLists.txt）
  spdlog::source_loc where{};
  if (context.file) {
    where = spdlog::source_loc{context.file, context.line, context.function ? context.function : ""};
  }

  // 刻意走 string_view 那個多載而不是格式化多載：訊息本文裡的左大括號
  //（JSON 片段、prompt dump）不能被當成 fmt 的替換欄位
  const spdlog::string_view_t body(split.body.data(), split.body.size());
  loggerFor(name)->log(where, toSpdLevel(type), body);

  // qFatal 不必在這裡 abort：Qt 的 QMessageLogger::fatal 會在 handler 回來之後
  // 自己走 qt_message_fatal。本專案目前零 qFatal，但這一點不能靠「反正沒人用」
}

}  // namespace

LoggingGuard::LoggingGuard() {
  if (g_sinks) return;

  // **一定要在建任何 sink 之前**：spdlog 的 wincolor_stderr_sink 在建構時
  // 就把 GetStdHandle(STD_ERROR_HANDLE) 抓起來存著
  platform::attachParentConsole();

  // 用 spdlog 預設的 stderr_color_mt（Windows 上是 wincolor sink）而不是 ansicolor：
  // wincolor 會用 GetConsoleMode 判斷對象是不是真的 console —— 是就上色，
  // 是 pipe 就寫純文字。剛好兩個場景都對：終端機有顏色，
  // Qt Creator 的 Application Output 不會出現一堆跳脫序列亂碼
  auto console = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  console->set_pattern(kConsolePattern);

  g_pending = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(kPendingCapacity);

  g_sinks = std::make_shared<spdlog::sinks::dist_sink_mt>();
  g_sinks->add_sink(console);
  g_sinks->add_sink(g_pending);

  g_level = levelFromEnv();
  spdlog::set_default_logger(loggerFor("app"));
  spdlog::set_level(g_level);

  g_previousHandler = qInstallMessageHandler(&qtMessageHandler);
}

LoggingGuard::~LoggingGuard() {
  // 先卸下 handler：static 解構期還可能有 Qt 內部訊息，那些要走回 Qt 預設輸出，
  // 不能再碰已經收攤的 sink。刻意不呼叫 spdlog::shutdown() ——
  // 它會把 default logger reset 成 nullptr，之後任何一行 spdlog::info() 就是空指標
  qInstallMessageHandler(g_previousHandler);
  spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) { logger->flush(); });
}

void attachLogFile(const fs::path& logsDir) {
  if (!g_sinks) return;

  auto file = std::make_shared<DailyRotatingFileSink>(logsDir, maxBytesFromEnv(), kKeepDays);
  file->set_pattern(kFilePattern);

  // 回放第一階段暫存的訊息。用 log_msg 原件而不是已格式化的字串，
  // 時間戳／等級／分類因此全部保留原值
  if (g_pending) {
    for (const auto& pending : g_pending->last_raw()) file->log(pending);
    g_sinks->remove_sink(g_pending);
    g_pending.reset();
  }

  g_sinks->add_sink(file);
}

}  // namespace l2m
