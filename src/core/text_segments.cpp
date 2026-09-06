#include "text_segments.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include "string_util.h"

namespace l2m {

namespace {

// 解碼後的一個碼位：值、在原字串裡的位元組位移、位元組長度。
// 先整份解一次再對索引操作，比在切點判斷裡到處算 UTF-8 長度好懂太多。
struct CodePoint {
  uint32_t value = 0;
  size_t offset = 0;
  size_t length = 1;
};

std::vector<CodePoint> decodeUtf8(const std::string& text) {
  std::vector<CodePoint> points;
  points.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    size_t length = 1;
    uint32_t value = lead;
    if ((lead & 0x80) == 0) {
      length = 1;
      value = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      length = 2;
      value = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      value = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      value = lead & 0x07u;
    } else {
      // 壞掉的位元組：當成單一碼位帶過去，切句不該把輸入吃掉
      points.push_back({lead, i, 1});
      ++i;
      continue;
    }
    if (i + length > text.size()) {
      points.push_back({lead, i, 1});
      ++i;
      continue;
    }
    for (size_t k = 1; k < length; ++k) {
      value = (value << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
    }
    points.push_back({value, i, length});
    i += length;
  }
  return points;
}

bool isAsciiDigit(uint32_t cp) { return cp >= '0' && cp <= '9'; }
bool isAsciiSpace(uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v'; }

// 全形句末標點。ASCII 的 . ! ? ; 另外判（見 isHardTerminator）
bool isWideTerminator(uint32_t cp) {
  return cp == 0x3002 ||  // 。
         cp == 0xFF01 ||  // ！
         cp == 0xFF1F ||  // ？
         cp == 0xFF1B ||  // ；
         cp == 0x2026;    // …
}

// 軟切點：只有在單句超過 maxChars 時才會用到
bool isSoftBreak(uint32_t cp) {
  return cp == 0xFF0C ||  // ，
         cp == 0x3001 ||  // 、
         cp == 0xFF1A ||  // ：
         cp == ',' || cp == ':' || isAsciiSpace(cp);
}

// 收尾標點要跟著前一段走，不然引號會單獨落到下一段開頭
bool isClosing(uint32_t cp) {
  return cp == 0x300D ||  // 」
         cp == 0x300F ||  // 』
         cp == 0xFF09 ||  // ）
         cp == 0x3011 ||  // 】
         cp == 0x300B ||  // 》
         cp == 0x201D ||  // ”
         cp == 0x2019 ||  // ’
         cp == ')' || cp == ']' || cp == '}' || cp == '"' || cp == '\'';
}

// 英文縮寫：後面的句點不是句末。清單刻意短 —— 誤判成「不是句末」只會讓
// 那一段長一點，誤判成「是句末」則會在句子中間硬斷，後者難聽得多。
const std::array<const char*, 8>& abbreviations() {
  static const std::array<const char*, 8> kList{"mr.", "mrs.", "ms.", "dr.", "e.g.", "i.e.", "vs.", "no."};
  return kList;
}

// index 落在某個縮寫的結尾句點上嗎
bool endsAbbreviation(const std::string& text, const std::vector<CodePoint>& points, size_t index) {
  const size_t end = points[index].offset + points[index].length;
  for (const char* abbreviation : abbreviations()) {
    const std::string word(abbreviation);
    if (end < word.size()) continue;
    if (strutil::equalsInsensitive(text.substr(end - word.size(), word.size()), word)) return true;
  }
  return false;
}

// ASCII 句點只有在「後面是空白或結尾」且「前後都不是數字」時才算句末，
// 否則 3.14 與 v1.2 會被切成兩半。
bool isHardTerminator(const std::string& text, const std::vector<CodePoint>& points, size_t index) {
  const uint32_t cp = points[index].value;
  if (isWideTerminator(cp)) return true;
  if (cp == '!' || cp == '?' || cp == ';') return true;
  if (cp != '.') return false;

  const bool prevIsDigit = index > 0 && isAsciiDigit(points[index - 1].value);
  const bool nextIsDigit = index + 1 < points.size() && isAsciiDigit(points[index + 1].value);
  if (prevIsDigit && nextIsDigit) return false;
  if (index + 1 < points.size() && !isAsciiSpace(points[index + 1].value)) return false;
  return !endsAbbreviation(text, points, index);
}

// URL 不能在中間切。偵測到協定或 www. 就一路標到下一個空白為止。
std::vector<bool> markUrls(const std::string& text, const std::vector<CodePoint>& points) {
  std::vector<bool> locked(points.size(), false);
  static const std::array<const char*, 3> kPrefixes{"http://", "https://", "www."};
  for (size_t i = 0; i < points.size(); ++i) {
    bool hit = false;
    for (const char* prefix : kPrefixes) {
      const std::string needle(prefix);
      if (points[i].offset + needle.size() > text.size()) continue;
      if (strutil::equalsInsensitive(text.substr(points[i].offset, needle.size()), needle)) {
        hit = true;
        break;
      }
    }
    if (!hit) continue;
    size_t j = i;
    while (j < points.size() && !isAsciiSpace(points[j].value)) {
      locked[j] = true;
      ++j;
    }
    i = j;
  }
  return locked;
}

// [begin, end) 的碼位還原成字串，並去掉頭尾空白
std::string sliceTrimmed(const std::string& text, const std::vector<CodePoint>& points, size_t begin, size_t end) {
  if (begin >= end) return {};
  const size_t from = points[begin].offset;
  const size_t to = points[end - 1].offset + points[end - 1].length;
  return strutil::trim(text.substr(from, to - from));
}

// 一段的碼位範圍
struct Range {
  size_t begin = 0;
  size_t end = 0;
  size_t size() const { return end - begin; }
};

// 第一刀：只在句末切
std::vector<Range> splitAtTerminators(const std::string& text, const std::vector<CodePoint>& points, const std::vector<bool>& locked) {
  std::vector<Range> ranges;
  size_t begin = 0;
  for (size_t i = 0; i < points.size(); ++i) {
    if (locked[i] || !isHardTerminator(text, points, i)) continue;
    // 連續的句末標點與收尾引號一起吃掉（「什麼？！」的兩個標點不該分家）
    size_t end = i + 1;
    while (end < points.size() && (isHardTerminator(text, points, end) || isClosing(points[end].value))) {
      ++end;
    }
    ranges.push_back({begin, end});
    begin = end;
    i = end - 1;
  }
  if (begin < points.size()) ranges.push_back({begin, points.size()});
  return ranges;
}

// 第二刀：超過上限的段落在軟切點再切。取「不超過上限的最後一個軟切點」，
// 一個都沒有就硬切在上限處。
void splitLongRange(const std::vector<CodePoint>& points, const std::vector<bool>& locked, Range range, size_t maxChars, std::vector<Range>& out) {
  while (range.size() > maxChars) {
    const size_t limit = range.begin + maxChars;
    size_t cut = 0;
    for (size_t i = range.begin; i < limit; ++i) {
      if (!locked[i] && isSoftBreak(points[i].value)) cut = i + 1;
    }
    if (cut <= range.begin) {
      // 沒有軟切點（例如一長串沒有標點的英文）：硬切在上限處。
      // 但如果那一刀落在鎖住的區間裡（URL 中間），就往**後**推到它結束為止 ——
      // 這一段會超過上限，可是把網址唸成兩半更糟，而且往回找只會退到段首。
      cut = limit;
      while (cut < range.end && locked[cut] && locked[cut - 1]) ++cut;
    }
    out.push_back({range.begin, cut});
    range.begin = cut;
  }
  if (range.size() > 0) out.push_back(range);
}

}  // namespace

std::vector<std::string> splitIntoSpeechSegments(const std::string& text, const SegmentOptions& options) {
  const std::string trimmed = strutil::trim(text);
  if (trimmed.empty()) return {};

  const size_t maxChars = options.maxChars > 0 ? options.maxChars : 1;
  const std::vector<CodePoint> points = decodeUtf8(trimmed);
  const std::vector<bool> locked = markUrls(trimmed, points);

  std::vector<Range> ranges;
  for (const Range& range : splitAtTerminators(trimmed, points, locked)) {
    splitLongRange(points, locked, range, maxChars, ranges);
  }

  // 第三刀：太短的段要併掉。合成請求本身有固定成本（本機推論尤其），
  // 為了「好。」單獨發一次很不划算 —— 而且它 0.5 秒就播完，接著就是乾等下一段。
  std::vector<std::string> segments;
  // 併不進前一段的短段先留著，等下一段來了往**後**併。
  // 開頭的「哼！」尤其重要：它沒有前一段可併，單獨成段就會讓角色開口
  // 講兩個字然後靜音好幾秒，比晚一點開口還難聽。
  std::string carry;
  for (const Range& range : ranges) {
    std::string piece = sliceTrimmed(trimmed, points, range.begin, range.end);
    if (piece.empty()) continue;
    if (!carry.empty()) {
      piece = carry + piece;
      carry.clear();
    }
    if (strutil::utf8Length(piece) >= options.minChars) {
      segments.push_back(std::move(piece));
      continue;
    }
    if (!segments.empty() && strutil::utf8Length(segments.back()) + strutil::utf8Length(piece) <= maxChars) {
      segments.back() += piece;
      continue;
    }
    carry = std::move(piece);
  }
  if (!carry.empty()) {
    if (!segments.empty()) {
      segments.back() += carry;
    } else {
      segments.push_back(std::move(carry));
    }
  }

  // 全部被空白吃掉的極端情況（例如只有標點）：至少回原文，不要回空清單
  if (segments.empty()) segments.push_back(trimmed);
  return segments;
}

}  // namespace l2m
