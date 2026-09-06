#pragma once

// Server-Sent Events 的**增量**解析器（cloud LLM 的串流回覆用）。
//
// 與 mcp_stdio.cpp 那段一次性的 data: 逐行拆解不同：這裡的輸入是
// QNetworkReply::readyRead 交來的任意位元組切片，一個事件可能被切在任何
// 位置（field 名稱中間、UTF-8 多位元組字元中間都有可能），所以必須是
// 有狀態的物件 —— feed() 吃切片、只吐「已經完整」的事件，殘尾留到下一次。
//
// 依 WHATWG 規格實作的子集（LLM 服務實際會送的部分）：
//  * 行結尾 CRLF / LF / CR 都認；事件以空行分隔。
//  * "data:" 多行時以 \n 串接；"event:" 記事件名；欄位值開頭的一個空格去掉。
//  * ":" 開頭是註解（OpenAI 相容服務常拿它當 keep-alive），整行忽略。
//  * "id:" / "retry:" 不需要，讀到直接忽略。
// [DONE] 哨兵不在這一層處理 —— 它是 OpenAI 的應用層約定，
// 由 core/llm_stream_accumulator.h 認。

#include <cstddef>
#include <string>
#include <vector>

namespace l2m {

struct SseEvent {
  // "event:" 欄位；沒有時為空（OpenAI 只送 data、Anthropic 每個事件都有名字）
  std::string event;
  // "data:" 欄位（多行以 \n 串接）
  std::string data;
};

class SseParser {
public:
  // 吃一片位元組，回傳其中**完整結束**（遇到空行）的事件。
  // data 只在呼叫期間有效，內容會被複製。
  std::vector<SseEvent> feed(const char* data, size_t size);

  // 串流正常結束時呼叫：規格上最後一個事件沒有空行也該被派發。
  // 回傳殘尾湊出來的最後一個事件（沒有就回空 vector）。
  std::vector<SseEvent> finish();

private:
  void consumeLine(const std::string& line, std::vector<SseEvent>& out);

  std::string pendingLine_;  // 還沒讀到行結尾的殘尾
  bool pendingCr_ = false;   // 上一片結尾是 CR，下一片開頭的 LF 要吸掉
  std::string eventName_;    // 累積中事件的 event 欄位
  std::string dataLines_;    // 累積中事件的 data（已含換行串接）
  bool hasData_ = false;     // data 欄位出現過（空字串的 data 也算事件）
};

}  // namespace l2m
