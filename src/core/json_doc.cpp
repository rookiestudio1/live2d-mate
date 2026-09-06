#include "json_doc.h"

#include <fstream>
#include <sstream>

namespace l2m::jsonu {

std::optional<std::string> readFileUtf8(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) return std::nullopt;
  return buffer.str();
}

std::optional<Doc> Doc::parse(const std::string& text) {
  yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
  if (!doc) return std::nullopt;
  return Doc(doc);
}

std::optional<Doc> Doc::parseFile(const std::filesystem::path& path) {
  const auto text = readFileUtf8(path);
  if (!text) return std::nullopt;
  return parse(*text);
}

std::string MutDoc::write(bool pretty) const {
  if (!doc_) return "";
  char* raw = yyjson_mut_write(doc_, pretty ? YYJSON_WRITE_PRETTY_TWO_SPACES : 0, nullptr);
  std::string result = raw ? raw : "";
  if (raw) free(raw);
  return result;
}

}  // namespace l2m::jsonu
