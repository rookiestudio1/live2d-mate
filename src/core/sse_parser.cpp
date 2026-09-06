#include "sse_parser.h"

namespace l2m {

std::vector<SseEvent> SseParser::feed(const char* data, size_t size) {
  std::vector<SseEvent> out;
  for (size_t i = 0; i < size; ++i) {
    const char ch = data[i];
    // CRLF 拆在兩片之間：上一片以 CR 結尾已經派發過那一行，這裡吸掉 LF
    if (pendingCr_) {
      pendingCr_ = false;
      if (ch == '\n') continue;
    }
    if (ch == '\n') {
      consumeLine(pendingLine_, out);
      pendingLine_.clear();
    } else if (ch == '\r') {
      consumeLine(pendingLine_, out);
      pendingLine_.clear();
      pendingCr_ = true;
    } else {
      pendingLine_ += ch;
    }
  }
  return out;
}

std::vector<SseEvent> SseParser::finish() {
  std::vector<SseEvent> out;
  if (!pendingLine_.empty()) {
    consumeLine(pendingLine_, out);
    pendingLine_.clear();
  }
  pendingCr_ = false;
  // 最後一個事件沒有結尾空行也要派發（伺服器送完就關連線是常態）
  if (hasData_) {
    consumeLine(std::string(), out);
  }
  eventName_.clear();
  dataLines_.clear();
  hasData_ = false;
  return out;
}

void SseParser::consumeLine(const std::string& line, std::vector<SseEvent>& out) {
  // 空行＝事件結束，派發
  if (line.empty()) {
    if (hasData_) {
      out.push_back({eventName_, dataLines_});
    }
    eventName_.clear();
    dataLines_.clear();
    hasData_ = false;
    return;
  }
  // 註解行（keep-alive）
  if (line[0] == ':') return;

  // "field: value"；沒有冒號的行規格上是「值為空的欄位」，我們用不到，忽略
  const size_t colon = line.find(':');
  if (colon == std::string::npos) return;
  const std::string field = line.substr(0, colon);
  std::string value = line.substr(colon + 1);
  // 規格：值開頭的**一個**空格去掉（只去一個，"data:  x" 的第二個空格是內容）
  if (!value.empty() && value[0] == ' ') value.erase(0, 1);

  if (field == "event") {
    eventName_ = value;
  } else if (field == "data") {
    if (hasData_) dataLines_ += '\n';
    dataLines_ += value;
    hasData_ = true;
  }
  // id / retry：不需要，忽略
}

}  // namespace l2m
