#include "tts_segment_pipeline.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QString>

#include <algorithm>
#include <utility>

namespace l2m {

namespace {

// 切句清單裡每一句預覽幾個字。夠認出「是哪一句」就好，不是要把全文再印一遍
constexpr int kSegmentPreviewChars = 24;

}  // namespace

// 一次合成的整段狀態。內層引擎的回呼都導到它，全部在 GUI 執行緒。
struct SegmentedTtsEngine::Job {
  // 每一句的緩衝。
  //
  // 名字是 parts 不是 slots：`slots` 是 Qt 的關鍵字巨集（qglobal.h 定義成空），
  // 這個檔案一旦 include 到任何 Qt 標頭，`std::vector<Part> slots;` 就會被
  // 展開成 `std::vector<Part> ;`，錯誤訊息完全看不出來是這個原因。
  //
  // 刻意先 resize 到定量，索引才穩定 ——
  // 內層引擎可能在 synthesize() 裡就同步回呼（sapi 就是），
  // 那會在我們還沒把 handle 存回去之前重新進到 pump()。
  struct Part {
    std::unique_ptr<TtsRequestHandle> handle;
    std::vector<char> audio;
    std::string mime;
    bool requested = false;
    bool done = false;
    // 這一句花了多久合成。缺料（環形緩衝來不及補）幾乎都是因為某一段的
    // 合成時間超過前一段的播放時間，所以每一段都印出來。
    QElapsedTimer clock;
    qint64 elapsedMs = 0;
    bool failed = false;
    std::string error;
  };

  std::vector<std::string> segments;
  std::vector<Part> parts;
  SpeakOptions options;
  TtsStreamSink sink;

  // 已經送給 sink 的段數
  size_t emitted = 0;
  bool openSent = false;
  bool cancelled = false;
  bool finished = false;
  // 重入護欄：同步回呼會讓 pump() 從自己內部再被呼叫一次
  bool pumping = false;
  bool again = false;

  // 段落的對帳行。使用者回報「漏句／沒講完」時，這一行就是結論：
  // 切 7 句但只交出 3 句 ＝ 中途被砍；切 7 交出 7 卻少聽到一句 ＝ 問題在下游的播放器。
  std::string summary() const {
    size_t requestedCount = 0;
    for (const Part& part : parts) {
      if (part.requested) ++requestedCount;
    }
    return "切 " + std::to_string(segments.size()) + " 句／送出請求 " + std::to_string(requestedCount) + " 句／交給播放器 " + std::to_string(emitted) + " 句";
  }

  void cancelAll() {
    for (Part& part : parts) {
      if (part.handle) part.handle->cancel();
    }
  }
};

SegmentedTtsEngine::SegmentedTtsEngine(std::unique_ptr<TtsEngine> inner, SegmentOptions options, size_t prefetch) : inner_(std::move(inner)), options_(options), prefetch_(prefetch) {}

SegmentedTtsEngine::~SegmentedTtsEngine() = default;

const std::string& SegmentedTtsEngine::id() const { return inner_->id(); }
const std::string& SegmentedTtsEngine::name() const { return inner_->name(); }

void SegmentedTtsEngine::isAvailable(std::function<void(bool)> done) { inner_->isAvailable(std::move(done)); }

void SegmentedTtsEngine::listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) { inner_->listVoices(std::move(done)); }

std::unique_ptr<TtsRequestHandle> SegmentedTtsEngine::synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) {
  std::vector<std::string> segments = splitIntoSpeechSegments(text, options_);

  // 切句結果整份印出來。使用者回報「漏句」時，這一份與氣泡上的全文一比對，
  // 立刻分得出是「切句就少了」還是「切對了但沒唸」—— 前者要查 text_segments，
  // 後者要往下游查。每句只印前 kSegmentPreviewChars 個字，夠認出是哪一句就好
  {
    QString listing;
    for (size_t i = 0; i < segments.size(); ++i) {
      const QString piece = QString::fromStdString(segments[i]);
      listing += QStringLiteral("\n  %1/%2（%3 字）：%4").arg(i + 1).arg(segments.size()).arg(piece.size()).arg(piece.left(kSegmentPreviewChars));
    }
    qDebug().noquote() << QStringLiteral("[tts] 切成 %1 句%2").arg(segments.size()).arg(listing);
  }

  // 切不出兩段以上就不必套管線：多一層只會多一次搬運，行為也該與沒包時一樣
  if (segments.size() <= 1) return inner_->synthesize(text, options, std::move(sink));

  auto job = std::make_shared<Job>();
  job->segments = std::move(segments);
  job->parts.resize(job->segments.size());
  job->options = options;
  job->sink = std::move(sink);

  // 用 FunctionTtsHandle 而不是自訂類別：Job 是私有的巢狀型別，
  // 成員函式裡的 lambda 碰得到，檔案層級的類別碰不到。
  auto handle = std::make_unique<FunctionTtsHandle>(std::make_shared<bool>(false), [job] {
    if (job->cancelled) return;
    job->cancelled = true;
    // 外部取消（stop_speaking／設定頁的停止鈕／關閉程式）。剩下的句子從此不會唸，
    // 而且不會有任何錯誤 —— 沒有這一行的話，被取消與正常唸完在日誌上長得一樣
    qWarning().noquote() << QStringLiteral("[tts] 段落被取消：%1").arg(QString::fromStdString(job->summary()));
    job->cancelAll();
  });
  pump(job);
  return handle;
}

void SegmentedTtsEngine::pump(const std::shared_ptr<Job>& job) {
  if (job->pumping) {
    job->again = true;
    return;
  }
  job->pumping = true;

  do {
    job->again = false;
    if (job->cancelled || job->finished) break;

    // ① 把已經備妥的段依序送出去。順序是硬性的 —— 第 N+1 句可能比第 N 句
    //    先合成完，但唸出來的順序不能亂。
    while (job->emitted < job->parts.size()) {
      Job::Part& part = job->parts[job->emitted];
      if (!part.done) break;

      if (part.failed) {
        // 「沒講完就停了」最主要的出口：這一句失敗，後面所有句子連請求都不會送出。
        // 對帳行要在 cancelAll() 之前算，之後 requested 的意義就變了
        qWarning().noquote() << QStringLiteral("[tts] 段落中止於第 %1/%2 句：%3｜%4")
                                  .arg(job->emitted + 1)
                                  .arg(job->parts.size())
                                  .arg(QString::fromStdString(part.error.empty() ? "synthesis failed" : part.error))
                                  .arg(QString::fromStdString(job->summary()));
        job->finished = true;
        job->cancelAll();
        if (job->sink.onError) {
          job->sink.onError(part.error.empty() ? "synthesis failed" : part.error);
        }
        break;
      }

      if (!job->openSent) {
        job->openSent = true;
        if (job->sink.onOpen) job->sink.onOpen(part.mime);
      }
      const size_t bytes = part.audio.size();
      if (job->sink.onChunk && !part.audio.empty()) {
        job->sink.onChunk(part.audio.data(), part.audio.size());
      } else if (bytes == 0) {
        // 合成回報成功卻一個位元組都沒有：onChunk 被跳過、onSegmentEnd 照發，
        // 於是這一句無聲消失而其餘照跑 —— 正是「漏中間某一句」的其中一個形狀
        qWarning().nospace() << "[tts] 句段 " << (job->emitted + 1) << "/" << job->parts.size() << " 合成成功卻沒有音訊位元組，這一句會靜靜消失";
      }
      // 這一段的位元組完了，但整句還沒完 —— 播放器據此換下一個解碼器
      if (job->sink.onSegmentEnd) job->sink.onSegmentEnd();
      qDebug().nospace() << "[tts] 句段 " << (job->emitted + 1) << "/" << job->parts.size() << " 交給播放器（" << bytes << " bytes）";
      part.audio.clear();
      part.audio.shrink_to_fit();
      ++job->emitted;
    }
    if (job->cancelled || job->finished) break;

    if (job->emitted >= job->parts.size()) {
      job->finished = true;
      qDebug().noquote() << QStringLiteral("[tts] 段落完整交出：%1").arg(QString::fromStdString(job->summary()));
      if (job->sink.onDone) job->sink.onDone();
      break;
    }

    // ② 補請求：正在播的那一句，加上 kSegmentPrefetch 句的預取
    const size_t wanted = std::min(job->emitted + 1 + prefetch_, job->parts.size());
    for (size_t i = job->emitted; i < wanted; ++i) {
      if (job->parts[i].requested) continue;
      job->parts[i].requested = true;
      job->parts[i].clock.start();
      // 「從沒被請求」與「請求了但沒回來」是完全不同的故障，只有這一行分得出來
      qDebug().nospace() << "[tts] 句段 " << (i + 1) << "/" << job->parts.size() << " 送出請求（" << job->segments[i].size() << " bytes 文字）";

      TtsStreamSink inner;
      inner.onOpen = [job, i](std::string mime) { job->parts[i].mime = std::move(mime); };
      inner.onChunk = [job, i](const char* data, size_t size) {
        std::vector<char>& audio = job->parts[i].audio;
        audio.insert(audio.end(), data, data + size);
      };
      inner.onDone = [this, job, i] {
        if (job->cancelled || job->parts[i].done) return;
        job->parts[i].done = true;
        job->parts[i].elapsedMs = job->parts[i].clock.elapsed();
        qDebug().nospace() << "[tts] 句段 " << (i + 1) << "/" << job->parts.size() << " 備妥（合成 " << job->parts[i].elapsedMs << " ms，" << job->parts[i].audio.size() << " bytes，"
                           << job->parts[i].mime.c_str() << "）";
        pump(job);
      };
      inner.onError = [this, job, i](std::string error) {
        if (job->cancelled || job->parts[i].done) return;
        job->parts[i].done = true;
        job->parts[i].failed = true;
        // 連原文一起印：錯誤字串常常只說「HTTP 500」，光看那個不知道是哪一句踩到
        qWarning().noquote() << QStringLiteral("[tts] 句段 %1/%2 合成失敗（%3 ms）：%4｜原文：%5")
                                  .arg(i + 1)
                                  .arg(job->parts.size())
                                  .arg(job->parts[i].clock.elapsed())
                                  .arg(QString::fromStdString(error))
                                  .arg(QString::fromStdString(job->segments[i]));
        job->parts[i].error = std::move(error);
        pump(job);
      };

      auto handle = inner_->synthesize(job->segments[i], job->options, std::move(inner));
      if (job->cancelled || job->finished) break;
      if (job->parts[i].done) {
        // 內層是同步的（sapi 就是）：這一句已經好了。**先跳出去把它送出播放器**，
        // 不要在同一輪裡接著合成預取的下一句 —— 那等於白等一整段才開口。
        // again 已經被重入的 pump() 設起來了，外層迴圈會再跑一次 emit。
        break;
      }
      job->parts[i].handle = std::move(handle);
    }
  } while (job->again);

  job->pumping = false;
}

}  // namespace l2m
