#include "texture_format.h"

#include <algorithm>
#include <cctype>

namespace l2m {
namespace {

std::string toLower(std::string text) {
  for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

// 取副檔名（小寫、不含點）。沒有點、或最後一個點落在目錄分隔符前面
//（例如 "tex.d/atlas"）都當成「沒有副檔名」—— 那種檔名 QImage 一樣是靠內容
// 嗅探，拿目錄名去比對支援清單只會產生胡說八道的建議。
std::string extensionOf(const std::string& path) {
  const std::string::size_type dot = path.find_last_of('.');
  if (dot == std::string::npos) return {};
  const std::string::size_type slash = path.find_last_of("/\\");
  if (slash != std::string::npos && dot < slash) return {};
  return toLower(path.substr(dot + 1));
}

std::string join(const std::vector<std::string>& items, bool quoted) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) out += ", ";
    if (quoted) out += '"';
    out += items[i];
    if (quoted) out += '"';
  }
  return out;
}

}  // namespace

TextureFormatIssue inspectTextureFailure(const std::vector<std::string>& failedFiles, const std::vector<std::string>& supportedFormats, int totalTextures) {
  TextureFormatIssue issue;
  const int failed = static_cast<int>(failedFiles.size());
  // 沒失敗就沒話說。空的 error 是呼叫端的判定依據（見 ModelController::load()），
  // 所以這裡一定要留空而不是給一句「0 張失敗」。
  if (failed <= 0 || totalTextures <= 0) return issue;

  std::vector<std::string> supported;
  supported.reserve(supportedFormats.size());
  for (const std::string& format : supportedFormats) supported.push_back(toLower(format));

  for (const std::string& file : failedFiles) {
    const std::string ext = extensionOf(file);
    if (ext.empty()) continue;
    if (std::find(supported.begin(), supported.end(), ext) != supported.end()) continue;
    // 同一種格式只講一次：三張 webp 說三遍 "webp" 只是把建議洗掉
    if (std::find(issue.unsupported.begin(), issue.unsupported.end(), ext) != issue.unsupported.end()) continue;
    issue.unsupported.push_back(ext);
  }

  // 句子刻意用中性的 "failed to load" 而不是 "decode"：沒產生貼圖的原因也可能是
  // glTexImage2D 配置不出來（虛擬機 GPU 對 8192² 常常過不了，見 uploadTexture），
  // 那種情況說「解碼失敗」會把使用者指向一個完全沒問題的檔案。
  if (failed >= totalTextures) {
    issue.error = (totalTextures == 1) ? "The texture failed to load" : ("All " + std::to_string(totalTextures) + " textures failed to load");
  } else {
    issue.error = std::to_string(failed) + " of " + std::to_string(totalTextures) + " textures failed to load";
  }

  if (issue.unsupported.empty()) {
    issue.error += ".";
    // 不能只說「截斷或損毀」：解碼被尺寸上限擋下來的檔案完全沒問題，那樣講會
    // 把使用者送去修一個好好的檔案（實測 8192×16384 的圖集就是這樣被誤導的）。
    issue.hint = "The files may be truncated, corrupt, or too large to decode. Check FileReferences.Textures in the model3.json.";
    return issue;
  }

  const bool one = issue.unsupported.size() == 1;
  issue.error += std::string(": the image format") + (one ? " " : "s ") + join(issue.unsupported, true) + (one ? " is" : " are") + " not supported by this build.";
  // 兩條路都是使用者做得到的：轉檔不必動程式，裝外掛不必動模型
  issue.hint = "Convert the model textures to PNG, or install the Qt Image Formats plug-in that handles " + join(issue.unsupported, true) + ". This build can read: " + join(supported, false) + ".";
  return issue;
}

}  // namespace l2m
