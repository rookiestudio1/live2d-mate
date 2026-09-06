#include "persona_memory.h"

#include "string_util.h"

namespace l2m {

std::string clampPersonaMemory(const std::string& raw) {
  const std::string text = strutil::trim(raw);
  if (strutil::utf8Length(text) <= static_cast<size_t>(kPersonaMemoryMaxChars)) return text;

  // 數到第 kPersonaMemoryMaxChars 個碼位的位元組位置（只在「不是續位元組」處計數，
  // 與 strutil::utf8Length 同一套判定），截斷永遠落在字元邊界
  size_t count = 0;
  size_t bytes = 0;
  for (; bytes < text.size(); ++bytes) {
    if ((static_cast<unsigned char>(text[bytes]) & 0xC0) != 0x80) {
      if (count == static_cast<size_t>(kPersonaMemoryMaxChars)) break;
      ++count;
    }
  }
  return strutil::trim(text.substr(0, bytes));
}

}  // namespace l2m
