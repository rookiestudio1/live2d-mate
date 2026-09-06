#include "voice_list.h"

#include <algorithm>
#include <map>

namespace l2m {

namespace {

// UI 語系對應的語音地區。使用者切到韓文就該先看到韓文語音，不必自己往下捲。
const std::map<std::string, std::string>& uiVoiceLocale() {
  static const std::map<std::string, std::string> table{{"en", "en-US"}, {"ja", "ja-JP"}, {"ko", "ko-KR"}, {"zh-CN", "zh-CN"}, {"zh-TW", "zh-TW"}};
  return table;
}

// 內建引擎的顯示名稱走 i18n；未知引擎退回它自己回報的 name
const std::map<std::string, const char*>& engineLabelKeys() {
  static const std::map<std::string, const char*> keys{
    {"edge", "tts.engine.edge"},           {"sapi", "tts.engine.sapi"},         {"macsay", "tts.engine.macsay"},
    {"gptsovits", "tts.engine.gptsovits"}, {"voicebox", "tts.engine.voicebox"}, {"custom", "tts.engine.custom"},
  };
  return keys;
}

int voiceLocaleRank(const std::string& locale, const std::string& preferred) {
  if (!preferred.empty() && locale == preferred) return -1;
  const auto& locales = voiceDisplayLocales();
  for (int i = 0; i < static_cast<int>(locales.size()); ++i) {
    if (locale == locales[i]) return i;
  }
  return static_cast<int>(locales.size());
}

}  // namespace

const std::vector<std::string>& voiceDisplayLocales() {
  static const std::vector<std::string> locales{"zh-TW", "zh-HK", "zh-CN", "ja-JP", "ko-KR", "en-US"};
  return locales;
}

std::string preferredVoiceLocale(const std::string& uiLocale) {
  const auto it = uiVoiceLocale().find(uiLocale);
  return it != uiVoiceLocale().end() ? it->second : std::string();
}

std::vector<VoiceInfo> orderVoicesForDisplay(const std::vector<VoiceInfo>& voices, const std::string& uiLocale) {
  const std::string preferred = preferredVoiceLocale(uiLocale);
  const auto& locales = voiceDisplayLocales();

  std::vector<VoiceInfo> shown;
  for (const auto& voice : voices) {
    if (std::find(locales.begin(), locales.end(), voice.locale) != locales.end()) {
      shown.push_back(voice);
    }
  }

  if (shown.empty()) {
    // 一個都對不上（GPT-SoVITS 的自訂 preset 之類）就照原樣顯示前幾個，
    // 總比讓使用者面對一片空白好
    for (const auto& voice : voices) {
      if (static_cast<int>(shown.size()) >= kVoiceFallbackLimit) break;
      shown.push_back(voice);
    }
    return shown;
  }

  std::stable_sort(shown.begin(), shown.end(), [&preferred](const VoiceInfo& a, const VoiceInfo& b) { return voiceLocaleRank(a.locale, preferred) < voiceLocaleRank(b.locale, preferred); });
  return shown;
}

const char* engineLabelKey(const std::string& engineId) {
  const auto it = engineLabelKeys().find(engineId);
  return it != engineLabelKeys().end() ? it->second : nullptr;
}

}  // namespace l2m
