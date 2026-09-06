#include "sample_models.h"

#include <map>

namespace l2m {

namespace {

// 官網實際存在的語系版本。理由與實測見標頭 —— 少一個 zh-CHT、
// 日文又是無前綴的根路徑，所以只能一條一條列。
const std::map<std::string, std::string>& sampleUrls() {
  static const std::map<std::string, std::string> urls = {
    {"ja", "https://www.live2d.com/learn/sample/"},
    {"ko", "https://www.live2d.com/ko/learn/sample/"},
    {"zh-CN", "https://www.live2d.com/zh-CHS/learn/sample/"},
  };
  return urls;
}

constexpr const char* kEnglishSampleUrl = "https://www.live2d.com/en/learn/sample/";

}  // namespace

std::string sampleModelsUrl(const std::string& uiLocale) {
  const auto it = sampleUrls().find(uiLocale);
  return it == sampleUrls().end() ? kEnglishSampleUrl : it->second;
}

bool shouldOfferSampleModels(bool anyModelInstalled, bool alreadyPrompted, bool startedHidden) { return !anyModelInstalled && !alreadyPrompted && !startedHidden; }

}  // namespace l2m
