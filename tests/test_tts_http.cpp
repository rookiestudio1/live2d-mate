// 各家 TTS 服務的請求組裝（core/tts_http.h）。
//
// 「請求主體怎麼組」抽成純函式，測試直接打那一層
// （14 個欄位、URL 編碼、profile 映射），不必為了測試在引擎裡開一個
// HTTP 用戶端的注入點。
//
// 引擎本身的行為（探測、fallback、錯誤訊息包裝）由 test_tts 與手動驗證覆蓋。
#include <QtTest>

#include <string>

#include "core/json_doc.h"
#include "core/tts_http.h"

using namespace l2m;

namespace {

GptSovitsPreset preset() {
  GptSovitsPreset p;
  p.id = "hana";
  p.name = "花";
  p.refAudioPath = "D:/refs/hana.wav";
  p.promptText = "你好，我是花。";
  p.promptLang = "zh";
  p.locale = "zh-TW";
  return p;
}

std::string jsonString(const jsonu::Doc& doc, const char* key) { return jsonu::getString(doc.root(), key); }

double jsonNumber(const jsonu::Doc& doc, const char* key) { return yyjson_get_num(jsonu::get(doc.root(), key)); }

bool jsonBool(const jsonu::Doc& doc, const char* key) { return yyjson_get_bool(jsonu::get(doc.root(), key)); }

bool jsonHas(const jsonu::Doc& doc, const char* key) { return jsonu::get(doc.root(), key) != nullptr; }

}  // namespace

class TestTtsHttp : public QObject {
  Q_OBJECT

private slots:
  // === http 共用工具 ===

  // normalizeBaseUrl 去掉尾端斜線與前後空白
  void normalizeBaseUrlTrims() {
    QCOMPARE(normalizeBaseUrl("http://127.0.0.1:9880/"), std::string("http://127.0.0.1:9880"));
    QCOMPARE(normalizeBaseUrl("http://127.0.0.1:9880///"), std::string("http://127.0.0.1:9880"));
    QCOMPARE(normalizeBaseUrl("  http://127.0.0.1:9880  "), std::string("http://127.0.0.1:9880"));
  }

  // localeToLangCode 取語言碼並轉小寫
  void localeToLangCodeLowercases() {
    QCOMPARE(localeToLangCode("zh-TW"), std::string("zh"));
    QCOMPARE(localeToLangCode("zh-CN"), std::string("zh"));
    QCOMPARE(localeToLangCode("ja"), std::string("ja"));
    QCOMPARE(localeToLangCode("en"), std::string("en"));
  }

  // langCodeToLocale 補成系統匣看得懂的地區字串
  void langCodeToLocaleExpands() {
    QCOMPARE(langCodeToLocale("zh"), std::string("zh-CN"));
    QCOMPARE(langCodeToLocale("en"), std::string("en-US"));
    QCOMPARE(langCodeToLocale("ja"), std::string("ja-JP"));
    QCOMPARE(langCodeToLocale("ko"), std::string("ko-KR"));
    QCOMPARE(langCodeToLocale("yue"), std::string("zh-HK"));
  }

  // 已含連字號或未知語言原樣回傳
  void langCodeToLocalePassesThrough() {
    QCOMPARE(langCodeToLocale("pt-BR"), std::string("pt-BR"));
    QCOMPARE(langCodeToLocale("sw"), std::string("sw"));
    QCOMPARE(langCodeToLocale(""), std::string(""));
  }

  // 後端回 JSON 錯誤時把 message 撈出來；純文字與空白也各有形狀
  void describeErrorResponseUnwrapsJson() {
    QCOMPARE(describeErrorResponse(400, R"({"message":"ref_audio_path is required"})"), std::string("HTTP 400 - ref_audio_path is required"));
    QCOMPARE(describeErrorResponse(422, R"({"detail":"validation failed"})"), std::string("HTTP 422 - validation failed"));
    QCOMPARE(describeErrorResponse(500, "  boom  "), std::string("HTTP 500 - boom"));
    QCOMPARE(describeErrorResponse(503, ""), std::string("HTTP 503"));
  }

  // === GPT-SoVITS ===

  // 把設定檔的 preset 映成語音清單
  void gptSovitsVoicesFromPresets() {
    GptSovitsConfig config;
    config.presets.push_back(preset());
    GptSovitsPreset yuki = preset();
    yuki.id = "yuki";
    yuki.name = "雪";
    yuki.locale = "ja-JP";
    config.presets.push_back(yuki);

    const auto voices = gptSovitsVoices(config, "gptsovits");
    QCOMPARE(voices.size(), size_t(2));
    QCOMPARE(voices[0].id, std::string("hana"));
    QCOMPARE(voices[0].name, std::string("花"));
    QCOMPARE(voices[0].locale, std::string("zh-TW"));
    QCOMPARE(voices[0].engine, std::string("gptsovits"));
    QCOMPARE(voices[1].locale, std::string("ja-JP"));
  }

  // 合成時把 preset 與語速填進 /tts 的 body（14 個欄位）
  void gptSovitsBodyCarriesPresetAndRate() {
    GptSovitsConfig config;
    config.textLang = "auto";
    const std::string body = buildGptSovitsBody(config, preset(), "早安", 1.25);

    auto doc = jsonu::Doc::parse(body);
    QVERIFY(doc.has_value());
    QCOMPARE(jsonString(*doc, "text"), std::string("早安"));
    QCOMPARE(jsonString(*doc, "text_lang"), std::string("auto"));
    QCOMPARE(jsonString(*doc, "ref_audio_path"), std::string("D:/refs/hana.wav"));
    QCOMPARE(jsonString(*doc, "prompt_text"), std::string("你好，我是花。"));
    QCOMPARE(jsonString(*doc, "prompt_lang"), std::string("zh"));
    QCOMPARE(jsonString(*doc, "media_type"), std::string("wav"));
    QCOMPARE(jsonBool(*doc, "streaming_mode"), false);
    QCOMPARE(jsonBool(*doc, "parallel_infer"), true);
    QCOMPARE(jsonNumber(*doc, "speed_factor"), 1.25);
    QCOMPARE(jsonNumber(*doc, "top_k"), 15.0);
    QCOMPARE(jsonNumber(*doc, "top_p"), 1.0);
    QCOMPARE(jsonNumber(*doc, "temperature"), 1.0);
    QCOMPARE(jsonString(*doc, "text_split_method"), std::string("cut5"));
    QCOMPARE(jsonNumber(*doc, "batch_size"), 1.0);
  }

  // 切權重的 URL 要做 URL 編碼
  void gptSovitsWeightUrlEncodes() {
    QCOMPARE(gptSovitsWeightUrl("http://127.0.0.1:9880", "set_gpt_weights", "D:/w/g.ckpt"), std::string("http://127.0.0.1:9880/set_gpt_weights?weights_path=D%3A%2Fw%2Fg.ckpt"));
    QCOMPARE(gptSovitsWeightUrl("http://127.0.0.1:9880", "set_sovits_weights", "D:/w/s.pth"), std::string("http://127.0.0.1:9880/set_sovits_weights?weights_path=D%3A%2Fw%2Fs.pth"));
  }

  // mediaType 對應 mime
  void gptSovitsMimeMapping() {
    QCOMPARE(gptSovitsMime("wav"), std::string("audio/wav"));
    QCOMPARE(gptSovitsMime("ogg"), std::string("audio/ogg"));
    QCOMPARE(gptSovitsMime("aac"), std::string("audio/aac"));
  }

  // === Voicebox ===

  // 把 /profiles 映成語音清單並補齊地區
  void voiceboxProfilesBecomeVoices() {
    const std::string json = R"([{"id":"p1","name":"Morgan","language":"en"},{"id":"p2","name":"ハナ","language":"ja"}])";
    const auto voices = parseVoiceboxProfiles(json, "voicebox");

    QCOMPARE(voices.size(), size_t(2));
    QCOMPARE(voices[0].id, std::string("p1"));
    QCOMPARE(voices[0].name, std::string("Morgan"));
    QCOMPARE(voices[0].locale, std::string("en-US"));
    QCOMPARE(voices[0].engine, std::string("voicebox"));
    QCOMPARE(voices[1].name, std::string("ハナ"));
    QCOMPARE(voices[1].locale, std::string("ja-JP"));
  }

  // 合成的 body 帶上 profile 與語言，且沒有語速欄位
  void voiceboxBodyHasNoRate() {
    VoiceboxConfig config;
    const std::string body = buildVoiceboxBody(config, "p2", "こんにちは", "ja");

    auto doc = jsonu::Doc::parse(body);
    QVERIFY(doc.has_value());
    QCOMPARE(jsonString(*doc, "profile_id"), std::string("p2"));
    QCOMPARE(jsonString(*doc, "text"), std::string("こんにちは"));
    QCOMPARE(jsonString(*doc, "language"), std::string("ja"));
    // Voicebox 的 GenerationRequest 沒有語速欄位，送了只會被忽略或報錯
    QVERIFY(!jsonHas(*doc, "rate"));
    QVERIFY(!jsonHas(*doc, "engine"));
    QVERIFY(!jsonHas(*doc, "model_size"));
  }

  // 設定裡有 engine / modelSize 時一併送出
  void voiceboxBodyIncludesEngineAndModelSize() {
    VoiceboxConfig config;
    config.engine = "kokoro";
    config.modelSize = "0.6B";
    const std::string body = buildVoiceboxBody(config, "p1", "hi", "en");

    auto doc = jsonu::Doc::parse(body);
    QVERIFY(doc.has_value());
    QCOMPARE(jsonString(*doc, "engine"), std::string("kokoro"));
    QCOMPARE(jsonString(*doc, "model_size"), std::string("0.6B"));
  }

  // 壞掉的 profiles 回應不會炸，只是拿到空清單
  void voiceboxProfilesTolerateGarbage() {
    QVERIFY(parseVoiceboxProfiles("not json", "voicebox").empty());
    QVERIFY(parseVoiceboxProfiles("{}", "voicebox").empty());
    QVERIFY(parseVoiceboxProfiles(R"([{"name":"no id"}])", "voicebox").empty());
  }

  // === 自訂 HTTP 語音端點 ===

  // config 的字串轉成列舉；未知值退回 Get 而不是炸掉
  void customMethodParsing() {
    QVERIFY(customTtsMethod("get") == CustomTtsMethod::Get);
    QVERIFY(customTtsMethod("post") == CustomTtsMethod::PostForm);
    QVERIFY(customTtsMethod("post-json") == CustomTtsMethod::PostJson);
    QVERIFY(customTtsMethod("") == CustomTtsMethod::Get);
    QVERIFY(customTtsMethod("PATCH") == CustomTtsMethod::Get);
  }

  // Percent 模式走 encodeURIComponent 那一套（中文、空白、& 與 = 都要編掉）
  void expandsTextPercentEncoded() {
    QCOMPARE(expandTextPlaceholder("text=${TEXT}", "a b", TemplateEncoding::Percent), std::string("text=a%20b"));
    QCOMPARE(expandTextPlaceholder("text=${TEXT}", "a&b=c", TemplateEncoding::Percent), std::string("text=a%26b%3Dc"));
    QCOMPARE(expandTextPlaceholder("q=${TEXT}", "\u4f60\u597d", TemplateEncoding::Percent), std::string("q=%E4%BD%A0%E5%A5%BD"));
  }

  // JsonString 模式只逃逸不加引號 —— 引號是樣板自己寫的
  void expandsTextJsonEscaped() {
    QCOMPARE(expandTextPlaceholder(R"({"t":"${TEXT}"})", "say \"hi\"", TemplateEncoding::JsonString), std::string("{\"t\":\"say \\\"hi\\\"\"}"));
    QCOMPARE(expandTextPlaceholder("${TEXT}", "a\\b", TemplateEncoding::JsonString), std::string("a\\\\b"));
    QCOMPARE(expandTextPlaceholder("${TEXT}", "a\nb", TemplateEncoding::JsonString), std::string("a\\nb"));
    // 中文不被 \uXXXX 化，UTF-8 位元組原樣留著
    QCOMPARE(expandTextPlaceholder("${TEXT}", "\u4f60\u597d", TemplateEncoding::JsonString), std::string("\u4f60\u597d"));
  }

  // 出現幾次就換幾次；一次都沒有就原樣回傳
  void expandsEveryOccurrence() {
    QCOMPARE(expandTextPlaceholder("a=${TEXT}&b=${TEXT}", "x", TemplateEncoding::Percent), std::string("a=x&b=x"));
    QCOMPARE(expandTextPlaceholder("nothing", "x", TemplateEncoding::Percent), std::string("nothing"));
    QCOMPARE(expandTextPlaceholder("", "x", TemplateEncoding::Percent), std::string(""));
  }

  // 標頭一行一個；空行與 # 略過，值裡的冒號保留，缺冒號的行忽略
  void parsesHeaderLines() {
    const auto headers = parseHeaderLines(
      "Authorization: Bearer a:b\n"
      "\n"
      "# a comment: not a header\n"
      "  X-Api-Key :  k1  \n"
      "garbage line\n"
      ": no name\n");
    QCOMPARE(headers.size(), size_t(2));
    QCOMPARE(headers[0].first, std::string("Authorization"));
    QCOMPARE(headers[0].second, std::string("Bearer a:b"));
    QCOMPARE(headers[1].first, std::string("X-Api-Key"));
    QCOMPARE(headers[1].second, std::string("k1"));
    QVERIFY(parseHeaderLines("").empty());
  }

  // GET 把參數接成 query string；樣板自己已經帶 ? 時要改用 &
  void customGetBuildsQueryString() {
    CustomTtsConfig config;
    config.url = "https://api.example.com/tts";
    config.method = "get";
    config.params = "text=${TEXT}&speaker=1";
    CustomTtsRequest request = buildCustomTtsRequest(config, "hi there");
    QCOMPARE(request.error, std::string(""));
    QCOMPARE(request.url, std::string("https://api.example.com/tts?text=hi%20there&speaker=1"));
    QVERIFY(request.body.empty());
    QVERIFY(request.contentType.empty());

    config.url = "https://api.example.com/tts?key=abc";
    request = buildCustomTtsRequest(config, "hi");
    QCOMPARE(request.url, std::string("https://api.example.com/tts?key=abc&text=hi&speaker=1"));
  }

  // POST（表單）走 x-www-form-urlencoded，內容編碼與 GET 同一套
  void customPostFormBuildsBody() {
    CustomTtsConfig config;
    config.url = "https://api.example.com/tts";
    config.method = "post";
    config.params = "text=${TEXT}&speaker=1";
    const CustomTtsRequest request = buildCustomTtsRequest(config, "a b");
    QCOMPARE(request.url, std::string("https://api.example.com/tts"));
    QCOMPARE(request.body, std::string("text=a%20b&speaker=1"));
    QCOMPARE(request.contentType, std::string("application/x-www-form-urlencoded"));
  }

  // POST（JSON）走 JSON 逃逸，content-type 也要跟著換
  void customPostJsonBuildsBody() {
    CustomTtsConfig config;
    config.url = "https://api.example.com/tts";
    config.method = "post-json";
    config.params = R"({"text":"${TEXT}","voice":"alloy"})";
    const CustomTtsRequest request = buildCustomTtsRequest(config, "say \"hi\"");
    QCOMPARE(request.body, std::string("{\"text\":\"say \\\"hi\\\"\",\"voice\":\"alloy\"}"));
    QCOMPARE(request.contentType, std::string("application/json"));
    // 組出來的東西必須還是合法 JSON，不然端點只會回 400
    QVERIFY(jsonu::Doc::parse(request.body).has_value());
  }

  // URL 裡的 ${TEXT} 一律百分比編碼，**即使方法是 post-json**
  void customUrlAlwaysPercentEncoded() {
    CustomTtsConfig config;
    config.url = "https://api.example.com/say/${TEXT}";
    config.method = "post-json";
    config.params = R"({"speaker":1})";
    const CustomTtsRequest request = buildCustomTtsRequest(config, "a b");
    QCOMPARE(request.url, std::string("https://api.example.com/say/a%20b"));
    QCOMPARE(request.body, std::string(R"({"speaker":1})"));
  }

  // 沒填 URL 就不該送出去，而且錯誤要講得出「去哪裡填」
  void customMissingUrlIsAnError() {
    CustomTtsConfig config;
    config.params = "text=${TEXT}";
    const CustomTtsRequest request = buildCustomTtsRequest(config, "hi");
    QVERIFY(!request.error.empty());
    QVERIFY(request.error.find("URL") != std::string::npos);
    QVERIFY(request.error.find("Settings") != std::string::npos);
  }

  // 少了 ${TEXT} 的話端點永遠只收到空字串且不會報錯，一定要擋在送出前
  void customMissingTextTokenIsAnError() {
    CustomTtsConfig config;
    config.url = "https://api.example.com/tts";
    config.params = "speaker=1";
    const CustomTtsRequest request = buildCustomTtsRequest(config, "hi");
    QVERIFY(!request.error.empty());
    // 訊息裡要一字不差地出現這個標記，使用者才知道要打什麼
    QVERIFY(request.error.find("${TEXT}") != std::string::npos);

    // 只寫在 URL 裡也算數
    config.url = "https://api.example.com/say/${TEXT}";
    QVERIFY(buildCustomTtsRequest(config, "hi").error.empty());
  }

  // 空的參數不會多長出一個 ? 出來
  void customGetWithoutParams() {
    CustomTtsConfig config;
    config.url = "https://api.example.com/say/${TEXT}";
    config.method = "get";
    const CustomTtsRequest request = buildCustomTtsRequest(config, "hi");
    QCOMPARE(request.url, std::string("https://api.example.com/say/hi"));
  }

  // 標頭會一起帶進 CustomTtsRequest
  void customCarriesHeaders() {
    CustomTtsConfig config;
    config.url = "https://api.example.com/tts";
    config.params = "text=${TEXT}";
    config.headers = "Authorization: Bearer sk-1";
    const CustomTtsRequest request = buildCustomTtsRequest(config, "hi");
    QCOMPARE(request.headers.size(), size_t(1));
    QCOMPARE(request.headers[0].first, std::string("Authorization"));
  }

  // miniaudio 解不了的回應要在播放前就擋掉，不能讓它靜默失敗成「有氣泡沒聲音」
  void customAudioErrorRejectsUnplayable() {
    QVERIFY(!customTtsAudioError("audio/wav", 0).empty());
    QVERIFY(!customTtsAudioError("application/json", 20).empty());
    QVERIFY(!customTtsAudioError("text/plain; charset=utf-8", 20).empty());
    QVERIFY(!customTtsAudioError("audio/ogg", 20).empty());
    QVERIFY(!customTtsAudioError("audio/aac", 20).empty());
    // 這三種是 miniaudio 真的解得了的
    QVERIFY(customTtsAudioError("audio/wav", 20).empty());
    QVERIFY(customTtsAudioError("audio/mpeg", 20).empty());
    QVERIFY(customTtsAudioError("audio/flac", 20).empty());
    // 沒給 content-type 也放行，交給 miniaudio 自己嗅探
    QVERIFY(customTtsAudioError("", 20).empty());
  }
};

QTEST_APPLESS_MAIN(TestTtsHttp)
#include "test_tts_http.moc"
