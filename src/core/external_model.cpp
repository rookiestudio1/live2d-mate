#include "external_model.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace l2m {
namespace {

// 訊息與回覆的四個 verb。全部小寫、不含空白，剖析時只比對到第一個空白為止。
constexpr const char* kShowVerb = "show";
constexpr const char* kSetModelVerb = "setmodel";
constexpr const char* kOkVerb = "ok";
constexpr const char* kErrorVerb = "error";

// 去掉一行結尾的 CR/LF。收端有三種可能：讀到換行才切、對方斷線才切、
// 以及舊版那種完全不帶換行的 "show"，三種都要落到同一個字串。
std::string trimLineEnd(const std::string& line) {
  std::size_t end = line.size();
  while (end > 0 && (line[end - 1] == '\n' || line[end - 1] == '\r')) --end;
  return line.substr(0, end);
}

// 一行訊息切成 verb 與其餘。刻意只切第一個空白：Windows 的路徑可以有空白
//（"C:/Program Files/..."），切成 token 陣列會把路徑拆碎。
void splitVerb(const std::string& line, std::string* verb, std::string* rest) {
  const std::string body = trimLineEnd(line);
  const std::size_t space = body.find(' ');
  *verb = body.substr(0, space);
  *rest = space == std::string::npos ? std::string() : body.substr(space + 1);
}

// 一則訊息就是一行，所以酬載裡的換行一律換成空白。
// 少了這一步，收端會在中途以為訊息結束而收到半截內容。
std::string flattenLine(const std::string& text) {
  std::string one = text;
  for (char& ch : one) {
    if (ch == '\n' || ch == '\r') ch = ' ';
  }
  return one;
}

// 路徑前綴比對。Windows 上不分大小寫（理由見標頭的 modelsDirRelativeId），
// 其他平台精確比對。tolower 在預設的 "C" locale 底下對 0x80 以上的位元組是恆等，
// 所以 UTF-8 的中日文路徑仍然是逐位元組精確比對，只有 ASCII 會被折疊 —— 正是想要的。
bool pathPrefixEquals(const std::string& path, const std::string& prefix) {
  if (path.size() < prefix.size()) return false;
#ifdef _WIN32
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(path[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
  }
  return true;
#else
  return path.compare(0, prefix.size(), prefix) == 0;
#endif
}

}  // namespace

bool isExternalModelId(const std::string& id) {
  if (id.empty()) return false;
  // POSIX 的絕對路徑，以及 UNC 的 "//server/share"
  if (id.front() == '/' || id.front() == '\\') return true;
  // Windows 磁碟機，"X:/" 或 "X:\"。"E:models" 那種磁碟機相對路徑刻意不算 ——
  // 它本來就是相對的，接在 modelsDir_ 後面反而是對的行為。
  return id.size() >= 3 && std::isalpha(static_cast<unsigned char>(id[0])) != 0 && id[1] == ':' && (id[2] == '/' || id[2] == '\\');
}

std::string normalizeModelPath(const std::filesystem::path& path) {
  std::string text = path.lexically_normal().u8string();
  std::replace(text.begin(), text.end(), '\\', '/');
  // 尾端斜線去掉，但根目錄那一條要留：拔掉之後 "E:/" 會變成 "E:"，
  // 那是磁碟機相對路徑，意思完全不一樣。
  while (text.size() > 1 && text.back() == '/' && !(text.size() == 3 && text[1] == ':')) text.pop_back();
  return text;
}

std::string externalModelId(const std::filesystem::path& path) {
  if (path.is_absolute()) return normalizeModelPath(path);
  // id 會存進 config，工作目錄一換就找不到了，所以相對路徑先補成絕對。
  // 補不成（例如工作目錄已被刪除）就照原樣正規化，讓上層的 describeModel 去失敗。
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  return normalizeModelPath(ec ? path : absolute);
}

std::optional<std::string> modelsDirRelativeId(const std::filesystem::path& entryPath, const std::filesystem::path& modelsDir) {
  const std::string entry = externalModelId(entryPath);
  std::string base = externalModelId(modelsDir);
  if (entry.empty() || base.empty()) return std::nullopt;

  // 尾端補一條斜線再比 —— 少了它，"…/models2/Foo" 會被當成 "…/models" 底下的東西。
  // 補完之後「目錄自己」也自然落在 size 判斷外面（它不是模型）。
  if (base.back() != '/') base.push_back('/');
  if (entry.size() <= base.size() || !pathPrefixEquals(entry, base)) return std::nullopt;
  return entry.substr(base.size());
}

std::optional<std::string> setModelArg(const std::vector<std::string>& args) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] != kSetModelFlag) continue;
    const std::string& value = args[i + 1];
    // 後面接的是另一個旗標就當成沒給 —— 把 "--hidden" 拿去載入
    // 只會得到一句看不懂的錯誤，還不如當作使用者打錯了
    if (value.empty() || value.rfind("--", 0) == 0) return std::nullopt;
    return value;
  }
  return std::nullopt;
}

std::string encodeShowRequest() { return std::string(kShowVerb) + "\n"; }

std::string encodeSetModelRequest(const std::string& path) { return std::string(kSetModelVerb) + " " + flattenLine(path) + "\n"; }

InstanceRequest parseInstanceRequest(const std::string& line) {
  std::string verb;
  std::string rest;
  splitVerb(line, &verb, &rest);

  InstanceRequest request;
  if (verb == kShowVerb) {
    request.kind = InstanceRequestKind::Show;
  } else if (verb == kSetModelVerb && !rest.empty()) {
    // 路徑是空的就維持 Unknown：拿空字串去 describeModel 只會多繞一圈才失敗
    request.kind = InstanceRequestKind::SetModel;
    request.path = rest;
  }
  return request;
}

std::string encodeInstanceReply(bool ok, const std::string& message) {
  if (ok) return std::string(kOkVerb) + "\n";
  if (message.empty()) return std::string(kErrorVerb) + "\n";
  return std::string(kErrorVerb) + " " + flattenLine(message) + "\n";
}

InstanceReply parseInstanceReply(const std::string& line) {
  std::string verb;
  std::string rest;
  splitVerb(line, &verb, &rest);

  InstanceReply reply;
  reply.ok = verb == kOkVerb;
  if (!reply.ok) reply.message = rest;
  return reply;
}

}  // namespace l2m
