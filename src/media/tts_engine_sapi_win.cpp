#include "tts_engine_sapi.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/json_doc.h"
#include "tts_stream_util.h"

namespace l2m {

namespace {

constexpr int kPowerShellTimeoutMs = 30000;

const char* kListVoicesScript = R"PS(
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$s.GetInstalledVoices() | Where-Object { $_.Enabled } | ForEach-Object {
  [PSCustomObject]@{
    Name    = $_.VoiceInfo.Name
    Culture = $_.VoiceInfo.Culture.Name
    Gender  = $_.VoiceInfo.Gender.ToString()
  }
} | ConvertTo-Json -Compress -Depth 3
$s.Dispose()
)PS";

const char* kSynthScript = R"PS(
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
if ($env:L2D_VOICE) { try { $s.SelectVoice($env:L2D_VOICE) } catch { } }
$s.Rate = [int]$env:L2D_RATE
$s.SetOutputToWaveFile($env:L2D_OUT)
$text = Get-Content -LiteralPath $env:L2D_IN -Raw -Encoding UTF8
$s.Speak($text)
$s.Dispose()
)PS";

bool writeTextFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  return file.write(bytes) == bytes.size();
}

// 建好一個要跑腳本的 QProcess（還沒 start）。同步與非同步兩條路共用。
QProcess* makePowerShell(QObject* parent, const QString& scriptPath, const QProcessEnvironment& env) {
  auto* process = new QProcess(parent);
  process->setProcessEnvironment(env);
  process->setProgram(QStringLiteral("powershell.exe"));
  process->setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), scriptPath});
  process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
    // 不要閃出主控台視窗
    args->flags |= CREATE_NO_WINDOW;
  });
  return process;
}

// 行程結束後把錯誤整理成一句話；成功回空字串
QString powerShellError(QProcess* process) {
  const QString err = QString::fromUtf8(process->readAllStandardError()).trimmed();
  if (process->exitStatus() != QProcess::NormalExit || process->exitCode() != 0) {
    return err.isEmpty() ? QStringLiteral("powershell.exe exited with %1").arg(process->exitCode()) : err;
  }
  return {};
}

// 同步跑一支 PowerShell 腳本。
//
// **只剩語音清單在用。** 合成那一條已經改成非同步了：句段管線會讓一段長台詞
// 變成好幾次呼叫，同步版每一次都凍住畫面一兩秒，使用者會以為當掉。
// 語音清單則是「開設定頁時查一次」，而且結果永久快取，留著同步版比較單純。
// 逾時 30 秒兜底。
bool runPowerShell(const QString& scriptPath, const QProcessEnvironment& env, QString* stdOut, QString* error) {
  QProcess process;
  process.setProcessEnvironment(env);
  process.setProgram(QStringLiteral("powershell.exe"));
  process.setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), scriptPath});
  process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });

  process.start();
  if (!process.waitForStarted(5000)) {
    if (error) *error = QStringLiteral("powershell.exe could not be started");
    return false;
  }
  if (!process.waitForFinished(kPowerShellTimeoutMs)) {
    process.kill();
    process.waitForFinished(2000);
    if (error) *error = QStringLiteral("powershell.exe timed out");
    return false;
  }

  const QString err = powerShellError(&process);
  if (!err.isEmpty()) {
    if (error) *error = err;
    return false;
  }
  if (stdOut) *stdOut = QString::fromUtf8(process.readAllStandardOutput());
  return true;
}

}  // namespace

SapiEngine::SapiEngine(QObject* parent) : QObject(parent) {}

int SapiEngine::sapiRate(double rate) {
  const int mapped = static_cast<int>(std::lround((rate - 1) * 10));
  return std::clamp(mapped, -10, 10);
}

std::vector<VoiceInfo> SapiEngine::parseVoiceList(const std::string& json, const std::string& engineId) {
  std::vector<VoiceInfo> voices;
  auto doc = jsonu::Doc::parse(json);
  if (!doc || !doc->root()) return voices;

  // ConvertTo-Json 只有一個項目時輸出物件而不是陣列，兩種都要接
  const auto push = [&voices, &engineId](yyjson_val* item) {
    if (!item || !yyjson_is_obj(item)) return;
    VoiceInfo voice;
    voice.name = jsonu::getString(item, "Name");
    voice.id = voice.name;
    voice.locale = jsonu::getString(item, "Culture");
    voice.gender = jsonu::getString(item, "Gender");
    voice.engine = engineId;
    if (!voice.id.empty()) voices.push_back(std::move(voice));
  };

  if (yyjson_is_arr(doc->root())) {
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(doc->root(), &iter);
    yyjson_val* item = nullptr;
    while ((item = yyjson_arr_iter_next(&iter))) push(item);
  } else {
    push(doc->root());
  }
  return voices;
}

void SapiEngine::listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) {
  if (voicesCache_.has_value()) {
    done(*voicesCache_, "");
    return;
  }

  QTemporaryDir dir;
  if (!dir.isValid()) {
    done({}, "Could not create a temporary directory for SAPI");
    return;
  }

  const QString scriptPath = dir.filePath(QStringLiteral("list-voices.ps1"));
  if (!writeTextFile(scriptPath, QByteArray(kListVoicesScript))) {
    done({}, "Could not write the SAPI helper script");
    return;
  }

  QString out;
  QString error;
  if (!runPowerShell(scriptPath, QProcessEnvironment::systemEnvironment(), &out, &error)) {
    done({}, error.toStdString());
    return;
  }

  voicesCache_ = parseVoiceList(out.trimmed().toStdString(), id_);
  done(*voicesCache_, "");
}

void SapiEngine::isAvailable(std::function<void(bool)> done) {
  listVoices([done](std::vector<VoiceInfo> voices, std::string error) { done(error.empty() && !voices.empty()); });
}

std::unique_ptr<TtsRequestHandle> SapiEngine::synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) {
  auto cancelled = std::make_shared<bool>(false);

  const auto fail = [sink](std::string error) {
    if (sink.onError) sink.onError(std::move(error));
  };
  const auto failedHandle = [&cancelled] { return std::make_unique<FunctionTtsHandle>(cancelled); };

  // **暫存目錄的生命週期掛在回呼上，不是堆疊上。**
  // 舊版這裡是 `QTemporaryDir dir;`，同步版沒問題；改成非同步之後，
  // synthesize() 一返回目錄就被刪掉，PowerShell 正在寫的那個 wav 會憑空消失。
  auto dir = std::make_shared<QTemporaryDir>();
  if (!dir->isValid()) {
    fail("Could not create a temporary directory for SAPI");
    return failedHandle();
  }

  const QString scriptPath = dir->filePath(QStringLiteral("synth.ps1"));
  const QString inputPath = dir->filePath(QStringLiteral("input.txt"));
  const QString outputPath = dir->filePath(QStringLiteral("out.wav"));

  // 加上 BOM，讓 PowerShell 5.1 的 Get-Content -Encoding UTF8 正確處理中文
  QByteArray input("ï»¿", 3);
  input.append(QByteArray::fromStdString(text));

  if (!writeTextFile(scriptPath, QByteArray(kSynthScript)) || !writeTextFile(inputPath, input)) {
    fail("Could not write the SAPI helper files");
    return failedHandle();
  }

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert(QStringLiteral("L2D_IN"), inputPath);
  env.insert(QStringLiteral("L2D_OUT"), outputPath);
  env.insert(QStringLiteral("L2D_VOICE"), QString::fromStdString(options.voice.value_or(std::string())));
  env.insert(QStringLiteral("L2D_RATE"), QString::number(sapiRate(options.rate.value_or(1))));

  QProcess* process = makePowerShell(this, scriptPath, env);

  connect(process, &QProcess::finished, process, [process, dir, outputPath, sink, cancelled](int, QProcess::ExitStatus) {
    process->deleteLater();
    // 被取消：連結果都不必看，dir 的解構會把暫存檔一起帶走
    if (*cancelled) return;

    const QString error = powerShellError(process);
    if (!error.isEmpty()) {
      if (sink.onError) sink.onError(error.toStdString());
      return;
    }
    QFile wav(outputPath);
    if (!wav.open(QIODevice::ReadOnly)) {
      if (sink.onError) sink.onError("SAPI produced no audio file");
      return;
    }
    const QByteArray bytes = wav.readAll();
    if (bytes.isEmpty()) {
      if (sink.onError) sink.onError("SAPI produced an empty audio file");
      return;
    }
    emitWholeBody(sink, std::vector<char>(bytes.begin(), bytes.end()), "audio/wav");
  });

  connect(process, &QProcess::errorOccurred, process, [process, sink, cancelled](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) return;
    process->deleteLater();
    if (*cancelled) return;
    if (sink.onError) sink.onError("powershell.exe could not be started");
  });

  // 逾時兜底。行程活著就殺掉，finished 會接著跑並回報非零結束碼。
  QTimer::singleShot(kPowerShellTimeoutMs, process, [guard = QPointer<QProcess>(process)] {
    if (guard && guard->state() != QProcess::NotRunning) guard->kill();
  });

  process->start();

  // 取消：殺行程。finished 仍會發，但 cancelled 旗標會讓它閉嘴。
  return std::make_unique<FunctionTtsHandle>(cancelled, [guard = QPointer<QProcess>(process)] {
    if (guard && guard->state() != QProcess::NotRunning) guard->kill();
  });
}

}  // namespace l2m
