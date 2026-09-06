#include "edge_tts_protocol.h"

#include <QCryptographicHash>
#include <QString>

#include <algorithm>
#include <cstring>
#include <regex>

#include "json_doc.h"
#include "string_util.h"

namespace l2m::edge {

namespace {

// XML 字元逸出：AI 給的文字可能含 & < >，直接塞進 SSML 會讓整段解析失敗
std::string escapeXml(const std::string& raw) {
  std::string out;
  out.reserve(raw.size() + 16);
  for (const char c : raw) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&apos;";
        break;
      default:
        out += c;
    }
  }
  return out;
}

int localeRank(const std::string& locale) {
  const auto& preferred = preferredLocales();
  const auto it = std::find(preferred.begin(), preferred.end(), locale);
  return it == preferred.end() ? static_cast<int>(preferred.size()) : static_cast<int>(it - preferred.begin());
}

}  // namespace

const std::vector<std::string>& preferredLocales() {
  static const std::vector<std::string> locales{"zh-TW", "zh-HK", "zh-CN", "ja-JP", "en-US"};
  return locales;
}

std::string secMsGec(int64_t unixSeconds, const std::string& trustedClientToken) {
  // Unix epoch → Windows FILETIME epoch（1601-01-01）之間差 11644473600 秒
  int64_t ticks = unixSeconds + 11644473600LL;
  ticks -= ticks % 300;
  const int64_t windowsTicks = ticks * 10000000LL;

  const std::string payload = std::to_string(windowsTicks) + trustedClientToken;
  const QByteArray digest = QCryptographicHash::hash(QByteArray(payload.data(), static_cast<qsizetype>(payload.size())), QCryptographicHash::Sha256);

  return QString::fromLatin1(digest.toHex()).toUpper().toStdString();
}

std::string buildSynthUrl(const std::string& gec, const std::string& connectionId) {
  return std::string(kWssUrl) + "?TrustedClientToken=" + kTrustedClientToken + "&Sec-MS-GEC=" + gec + "&Sec-MS-GEC-Version=" + kSecMsGecVersion + "&ConnectionId=" + connectionId;
}

std::string buildSpeechConfig(const std::string& outputFormat) {
  return std::string("Content-Type:application/json; charset=utf-8\r\nPath:speech.config") + kHeaderDelimiter +
         "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{"
         "\"sentenceBoundaryEnabled\":\"false\",\"wordBoundaryEnabled\":\"false\"},"
         "\"outputFormat\":\"" +
         outputFormat + "\"}}}}";
}

std::string buildSsml(const std::string& voice, const std::string& locale, const std::string& text, int ratePercent) {
  const std::string sign = ratePercent >= 0 ? "+" : "";
  return "<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" "
         "xmlns:mstts=\"https://www.w3.org/2001/mstts\" xml:lang=\"" +
         locale + "\"><voice name=\"" + voice + "\"><prosody pitch=\"+0Hz\" rate=\"" + sign + std::to_string(ratePercent) + "%\" volume=\"+0%\">" + escapeXml(text) + "</prosody></voice></speak>";
}

std::string buildSsmlRequest(const std::string& requestId, const std::string& ssml) {
  return "X-RequestId:" + requestId + "\r\nContent-Type:application/ssml+xml\r\nPath:ssml" + kHeaderDelimiter + ssml;
}

std::string localeFromVoiceName(const std::string& voice) {
  static const std::regex pattern(R"([A-Za-z]{2}-[A-Za-z]{2})");
  std::smatch match;
  if (std::regex_search(voice, match, pattern)) return match.str();
  return {};
}

size_t findAudioPayload(const char* data, size_t size) {
  const size_t markerLength = std::strlen(kAudioDelimiter);
  if (size < markerLength) return std::string::npos;

  for (size_t i = 0; i + markerLength <= size; ++i) {
    if (std::memcmp(data + i, kAudioDelimiter, markerLength) == 0) return i + markerLength;
  }
  return std::string::npos;
}

std::string framePath(const std::string& frame) {
  const std::string marker = "Path:";
  const auto start = frame.find(marker);
  if (start == std::string::npos) return {};
  const auto from = start + marker.size();
  const auto end = frame.find("\r\n", from);
  return frame.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

std::vector<VoiceInfo> parseVoiceList(const std::string& json, const std::string& engineId) {
  std::vector<VoiceInfo> voices;
  auto doc = jsonu::Doc::parse(json);
  if (!doc || !doc->root() || !yyjson_is_arr(doc->root())) return voices;

  yyjson_arr_iter iter;
  yyjson_arr_iter_init(doc->root(), &iter);
  yyjson_val* item = nullptr;
  while ((item = yyjson_arr_iter_next(&iter))) {
    if (!yyjson_is_obj(item)) continue;

    VoiceInfo voice;
    voice.id = jsonu::getString(item, "ShortName");
    if (voice.id.empty()) continue;

    std::string friendly = jsonu::getString(item, "FriendlyName");
    const std::string prefix = "Microsoft ";
    if (friendly.rfind(prefix, 0) == 0) friendly = friendly.substr(prefix.size());
    voice.name = friendly.empty() ? voice.id : friendly;

    voice.locale = jsonu::getString(item, "Locale");
    voice.gender = jsonu::getString(item, "Gender");
    voice.engine = engineId;
    voices.push_back(std::move(voice));
  }

  // 偏好地區在前，其餘依地區與名稱排序
  std::stable_sort(voices.begin(), voices.end(), [](const VoiceInfo& a, const VoiceInfo& b) {
    const int ra = localeRank(a.locale);
    const int rb = localeRank(b.locale);
    if (ra != rb) return ra < rb;
    if (a.locale != b.locale) return a.locale < b.locale;
    return a.name < b.name;
  });
  return voices;
}

}  // namespace l2m::edge
