#pragma once

// 純字串輔助：大小寫不敏感比對（僅 ASCII）、trim、UTF-8 字元數。
// 模型檔名的副檔名一律是 ASCII，Unicode 大小寫摺疊在這裡用不到。

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace l2m::strutil {

inline std::string toLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// 大小寫不敏感的結尾比對
inline bool endsWithInsensitive(const std::string& text, const std::string& suffix) {
  if (text.size() < suffix.size()) return false;
  return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

// 大小寫不敏感的完全比對
inline bool equalsInsensitive(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

// 大小寫不敏感的子字串比對（只摺疊 ASCII）
inline bool containsInsensitive(const std::string& text, const std::string& needle) { return toLowerAscii(text).find(toLowerAscii(needle)) != std::string::npos; }

// 大小寫不敏感的開頭比對
inline bool startsWithInsensitive(const std::string& text, const std::string& prefix) {
  if (text.size() < prefix.size()) return false;
  return std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

// 去掉頭尾空白（只認 ASCII 空白）
inline std::string trim(const std::string& s) {
  const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && isSpace(static_cast<unsigned char>(s[begin]))) ++begin;
  while (end > begin && isSpace(static_cast<unsigned char>(s[end - 1]))) --end;
  return s.substr(begin, end - begin);
}

// 剝掉包住整段文字的 markdown code fence（```json … ```）。
// LLM 的回覆明明指示了不要，小模型照加是常態 —— 行為大腦（llm_behavior_prompt）
// 與角色卡擴寫（persona_generate）都要先過這一步再解析。
// 沒有 fence 就原樣回傳（trim 後）。
inline std::string stripCodeFence(const std::string& raw) {
  std::string text = trim(raw);
  if (text.rfind("```", 0) != 0) return text;
  const size_t firstNewline = text.find('\n');
  if (firstNewline == std::string::npos) return std::string();
  text = text.substr(firstNewline + 1);
  const size_t closing = text.rfind("```");
  if (closing != std::string::npos) text = text.substr(0, closing);
  return trim(text);
}

// 是不是合法的 UTF-8 位元組序列。
//
// 用途是 zip 條目名稱的編碼判定（見 core/zip_archive.h）：ZIP 規格說沒設
// general purpose bit 11 的檔名是 CP437，但實務上一半的工具寫 UTF-8 卻不標記，
// 另一半（Windows 檔案總管）寫系統 ANSI 碼頁。分辨這兩者唯一可靠的辦法就是
// 「先驗驗看是不是合法 UTF-8」—— 非 ASCII 的 ANSI 位元組幾乎不可能剛好是合法 UTF-8。
//
// 過長編碼、代理對區間（U+D800..U+DFFF）與超過 U+10FFFF 一律判為不合法：
// 放行它們等於讓 yyjson 稍後才失敗，而且失敗的地方離現場很遠。
inline bool isValidUtf8(const std::string& text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  const auto* end = p + text.size();
  while (p < end) {
    const unsigned char lead = *p;
    if (lead < 0x80) {
      ++p;
      continue;
    }
    int extra = 0;
    unsigned int cp = 0;
    if ((lead & 0xE0) == 0xC0) {
      extra = 1;
      cp = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
      extra = 2;
      cp = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
      extra = 3;
      cp = lead & 0x07u;
    } else {
      return false;  // 續位元組當前導，或 0xF8..0xFF
    }
    if (p + extra >= end) return false;
    for (int i = 1; i <= extra; ++i) {
      const unsigned char cont = p[i];
      if ((cont & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cont & 0x3Fu);
    }
    if (extra == 1 && cp < 0x80) return false;
    if (extra == 2 && cp < 0x800) return false;
    if (extra == 3 && cp < 0x10000) return false;
    if (cp > 0x10FFFF) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    p += extra + 1;
  }
  return true;
}

// UTF-8 的字元數（碼位），不是位元組數。
//
// 角色描述的長度上限是「2000 個字」，一個中文字是 3 個位元組、emoji 是 4 個 ——
// 用 size() 檢查會讓中文使用者只能打三分之一。UI 的字數計數器與
// core/persona.h 的 personaTextIssue 一定要共用這一份，兩邊各算一次
// 必然會在 emoji 上對不上（QString::size() 數的是 UTF-16 單元，
// 一個 emoji 算 2）。
//
// 作法：只數「不是續位元組」的位元組（續位元組一律是 0b10xxxxxx）。
inline size_t utf8Length(const std::string& text) {
  size_t count = 0;
  for (const char ch : text) {
    if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++count;
  }
  return count;
}

// 截到最多 maxChars 個字元（碼位），**絕不切在多位元組序列中間**。
//
// 為 core/mcp_tool_specs.h 的 mcpInstructions 長度守衛而生：host 會把 instructions
// 截斷在固定的字元數，而它切的是「第 N 個字元」不是「第 N 個位元組」時我們無從干預 ——
// 唯一能做的是自己先切乾淨。用 size() 或 substr() 硬切的話，中文與 emoji 的角色描述
// 尾端會留下半個序列，那份 JSON 送出去就不是合法 UTF-8 了。
//
// 作法與 utf8Length 同源：只在「不是續位元組」的位置算一個字元，數滿就從那裡切。
inline std::string utf8Truncate(const std::string& text, size_t maxChars) {
  if (maxChars == 0) return {};
  size_t count = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
      if (count == maxChars) return text.substr(0, i);
      ++count;
    }
  }
  return text;
}

}  // namespace l2m::strutil
