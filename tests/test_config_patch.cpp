// 設定 patch 的 JSON 組裝（core/config_patch.h）：patch 是手拼的字串，
// 逃逸與數字格式這兩個坑都在這裡釘住。
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/config_patch.h"
#include "core/config_schema.h"
#include "core/config_store.h"

using namespace l2m;
namespace fs = std::filesystem;

class TestConfigPatch : public QObject {
  Q_OBJECT

private:
  // patch 真的餵得進 ConfigStore（解析得動、驗證過得了）
  static bool acceptedByStore(const std::string& patchJson) {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    try {
      store.patch(patchJson);
    } catch (const std::exception&) {
      return false;
    }
    return true;
  }

private slots:
  // === JSON scalar 編碼 ===

  // 字串值逃逸雙引號與反斜線 —— MCP 的 host 與 token 是使用者自由輸入的
  void escapesQuotesAndBackslashes() {
    QCOMPARE(jsonString("a\"b"), std::string("\"a\\\"b\""));
    QCOMPARE(jsonString("C:\\path"), std::string("\"C:\\\\path\""));
  }

  // 控制字元走跳脫序列，不會直接塞進 JSON 字串裡
  void escapesControlCharacters() {
    QCOMPARE(jsonString("a\nb\tc"), std::string("\"a\\nb\\tc\""));
    QCOMPARE(jsonString(std::string("x\x01y")), std::string("\"x\\u0001y\""));
  }

  // 非 ASCII 原樣保留（UTF-8 位元組不動）
  void keepsUtf8Verbatim() { QCOMPARE(jsonString("待機"), std::string("\"待機\"")); }

  // 整數值不印小數點，與 config_schema 的 putNum 一致
  void integersHaveNoDecimalPoint() {
    QCOMPARE(jsonNumber(1), std::string("1"));
    QCOMPARE(jsonNumber(3777), std::string("3777"));
    QCOMPARE(jsonNumber(0), std::string("0"));
  }

  // 小數用最短的往返表示，不是 std::to_string 的六位小數
  void fractionsUseShortestRoundTrip() {
    QCOMPARE(jsonNumber(0.75), std::string("0.75"));
    QCOMPARE(jsonNumber(1.25), std::string("1.25"));
    QCOMPARE(jsonNumber(0.1), std::string("0.1"));
  }

  // === patch 組裝 ===

  void singleFieldShapes() {
    QCOMPARE(boolPatch("tts", "showBubble", true), std::string("{\"tts\":{\"showBubble\":true}}"));
    QCOMPARE(numberPatch("model", "scale", 1.5), std::string("{\"model\":{\"scale\":1.5}}"));
    QCOMPARE(stringPatch("app", "locale", "zh-TW"), std::string("{\"app\":{\"locale\":\"zh-TW\"}}"));
    QCOMPARE(nullPatch("tts", "voice"), std::string("{\"tts\":{\"voice\":null}}"));
  }

  void multipleFieldsKeepInsertionOrder() {
    const std::string json = ConfigPatch("idle").boolean("autoReset", false).number("resetMs", 30000).json();
    QCOMPARE(json, std::string("{\"idle\":{\"autoReset\":false,\"resetMs\":30000}}"));
  }

  // 換引擎一定要同時把 voice 清成 null：語音 id 是引擎專屬的
  void engineSwitchAlwaysClearsVoice() { QCOMPARE(ttsEnginePatch("voicebox"), std::string("{\"tts\":{\"engine\":\"voicebox\",\"voice\":null}}")); }

  // tts.custom 一定是整包送。ConfigStore::patch 的內層是整個覆蓋，
  // 少送一個欄位就會被 parseConfig 補回預設值而不是保留原值。
  void ttsCustomPatchSendsEveryField() {
    CustomTtsConfig custom;
    custom.url = "https://api.example.com/tts";
    custom.method = "post-json";
    custom.params = R"({"text":"${TEXT}"})";
    custom.headers = "Authorization: Bearer sk-1";
    custom.timeoutMs = 30000;
    QCOMPARE(ttsCustomPatch(custom), std::string("{\"tts\":{\"custom\":{"
                                                 "\"url\":\"https://api.example.com/tts\","
                                                 "\"method\":\"post-json\","
                                                 "\"params\":\"{\\\"text\\\":\\\"${TEXT}\\\"}\","
                                                 "\"headers\":\"Authorization: Bearer sk-1\","
                                                 "\"timeoutMs\":30000}}}"));
  }

  // 參數樣板裡的雙引號、反斜線與換行都要活著到 ConfigStore 那一頭
  void ttsCustomPatchEscapesParams() {
    CustomTtsConfig custom;
    custom.url = "https://api.example.com/tts";
    custom.method = "post-json";
    custom.params = "{\"text\":\"${TEXT}\",\"path\":\"C:\\\\voices\"}\nsecond line";
    QVERIFY(acceptedByStore(ttsCustomPatch(custom)));
  }

  // 設定視窗語音分頁的「套用」按鈕就是靠 customTtsFieldsEqual 決定亮不亮。
  // 漏比一個欄位＝使用者明明改了卻按不下去，所以每個 UI 欄位都要逐一釘住。
  void customTtsFieldsEqualCatchesEveryUiField() {
    CustomTtsConfig base;
    base.url = "https://api.example.com/tts";
    base.method = "post-json";
    base.params = R"({"text":"${TEXT}"})";
    base.headers = "Authorization: Bearer sk-1";
    QVERIFY(customTtsFieldsEqual(base, base));

    CustomTtsConfig other = base;
    other.url = "https://api.example.com/tts2";
    QVERIFY(!customTtsFieldsEqual(base, other));

    other = base;
    other.method = "get";
    QVERIFY(!customTtsFieldsEqual(base, other));

    other = base;
    other.params = R"({"text":"${TEXT}","voice":"alloy"})";
    QVERIFY(!customTtsFieldsEqual(base, other));

    other = base;
    other.headers = "Authorization: Bearer sk-2";
    QVERIFY(!customTtsFieldsEqual(base, other));
  }

  // timeoutMs 沒有 UI，使用者改不到 —— 算進比較的話「套用」按鈕會被一個
  // 永遠碰不到的欄位卡在亮著。但它仍然要被 patch 送出去（不然會掉回預設值），
  // 這一條就是在釘住「比較看的欄位 ⊊ patch 送的欄位」。
  void customTtsFieldsEqualIgnoresTimeout() {
    CustomTtsConfig base;
    base.url = "https://api.example.com/tts";
    CustomTtsConfig other = base;
    other.timeoutMs = base.timeoutMs + 1000;
    QVERIFY(customTtsFieldsEqual(base, other));
    QVERIFY(ttsCustomPatch(base) != ttsCustomPatch(other));
  }

  // 設定視窗語音分頁的「套用」按鈕靠 ttsLocalServerFieldsEqual 決定亮不亮。
  // 兩個位址各有一個輸入框，漏比一個＝那一欄改了卻按不下去。
  void ttsLocalServerFieldsEqualCatchesBothUrls() {
    TtsConfig base;
    QVERIFY(ttsLocalServerFieldsEqual(base, base));

    TtsConfig other = base;
    other.gptsovits.baseUrl = "http://192.168.1.10:9880";
    QVERIFY(!ttsLocalServerFieldsEqual(base, other));

    other = base;
    other.voicebox.baseUrl = "http://192.168.1.10:17493";
    QVERIFY(!ttsLocalServerFieldsEqual(base, other));
  }

  // 沒有 UI 的欄位不算進比較（取捨同 customTtsFieldsEqual），但仍然要被送出去
  void ttsLocalServerFieldsEqualIgnoresFieldsWithoutUi() {
    TtsConfig base;
    TtsConfig other = base;
    other.gptsovits.timeoutMs = base.gptsovits.timeoutMs + 1000;
    other.voicebox.timeoutMs = base.voicebox.timeoutMs + 1000;
    QVERIFY(ttsLocalServerFieldsEqual(base, other));
    QVERIFY(ttsLocalServerPatch(base) != ttsLocalServerPatch(other));
  }

  // MCP 四個欄位一次送，token 為 nullopt 時寫 null
  void mcpPatchShape() {
    QCOMPARE(mcpPatch(true, "127.0.0.1", 3777, std::nullopt), std::string("{\"mcp\":{\"enabled\":true,\"host\":\"127.0.0.1\","
                                                                          "\"port\":3777,\"token\":null}}"));
    QCOMPARE(mcpPatch(false, "0.0.0.0", 8080, std::optional<std::string>("s3cret")), std::string("{\"mcp\":{\"enabled\":false,\"host\":\"0.0.0.0\","
                                                                                                 "\"port\":8080,\"token\":\"s3cret\"}}"));
  }

  // llm 整包送、餵得進 ConfigStore、讀回一致（含沒有 UI 的 timeoutMs / maxTokens）
  void llmPatchRoundTrips() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    LlmConfig llm;
    llm.enabled = true;
    llm.provider = "anthropic";
    llm.baseUrl = "http://127.0.0.1:1234/v1";
    llm.apiKey = "sk-\"quote";  // 逃逸規則與 mcp host 同一套
    llm.model = "llama3";
    llm.anthropicApiKey = "sk-ant";
    llm.anthropicModel = "claude-opus-5";
    llm.temperature = 1.1;
    llm.driveIdle = true;
    store.patch(llmPatch(llm));

    const LlmConfig& stored = store.get().llm;
    QCOMPARE(stored.enabled, true);
    QCOMPARE(stored.provider, std::string("anthropic"));
    QCOMPARE(stored.baseUrl, llm.baseUrl);
    QCOMPARE(stored.apiKey, llm.apiKey);
    QCOMPARE(stored.anthropicApiKey, llm.anthropicApiKey);
    QCOMPARE(stored.temperature, 1.1);
    QCOMPARE(stored.driveIdle, true);
    QCOMPARE(stored.timeoutMs, llm.timeoutMs);
  }

  // LLM 分頁的「套用」按鈕靠 llmFieldsEqual 決定亮不亮：
  // 每個連線欄位都要抓得到差異；即時套用的欄位刻意不比
  void llmFieldsEqualCoversConnectionFields() {
    LlmConfig base;
    QVERIFY(llmFieldsEqual(base, base));
    for (const auto mutate :
         std::vector<std::function<void(LlmConfig&)>>{[](LlmConfig& c) { c.provider = "anthropic"; }, [](LlmConfig& c) { c.baseUrl = "http://x/v1"; }, [](LlmConfig& c) { c.apiKey = "k"; },
                                                      [](LlmConfig& c) { c.model = "m"; }, [](LlmConfig& c) { c.anthropicApiKey = "k2"; }, [](LlmConfig& c) { c.anthropicModel = "m2"; }}) {
      LlmConfig other = base;
      mutate(other);
      QVERIFY(!llmFieldsEqual(base, other));
    }
    // 即時套用的欄位不歸套用按鈕管
    LlmConfig other = base;
    other.enabled = !base.enabled;
    other.temperature = base.temperature + 0.5;
    other.driveIdle = !base.driveIdle;
    QVERIFY(llmFieldsEqual(base, other));
  }

  // === 與 ConfigStore 的相容性 ===

  // 產出的 patch 真的餵得進 ConfigStore
  void patchesSurviveConfigValidation() {
    QVERIFY(acceptedByStore(numberPatch("model", "scale", 1.25)));
    QVERIFY(acceptedByStore(ttsEnginePatch("edge")));
    QVERIFY(acceptedByStore(mcpPatch(true, "127.0.0.1", 3777, std::optional<std::string>("token"))));
    QVERIFY(acceptedByStore(boolPatch("interaction", "clickThrough", false)));
    QVERIFY(acceptedByStore(numberPatch("tts", "rate", 0.75)));
    // 負值不會被 jsonNumber 印壞，也過得了 schema 的範圍檢查
    QVERIFY(acceptedByStore(numberPatch("tts", "bubbleOffsetY", -10)));
    QVERIFY(acceptedByStore(ttsCustomPatch(CustomTtsConfig{})));
    QVERIFY(acceptedByStore(ttsLocalServerPatch(defaultConfig().tts)));
  }

  // 整包送進 ConfigStore 之後讀回來要一模一樣（含沒有 UI 的 timeoutMs）
  void ttsCustomPatchRoundTrips() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    CustomTtsConfig custom;
    custom.url = "https://api.example.com/tts";
    custom.method = "post";
    custom.params = "text=${TEXT}&speaker=1";
    custom.headers = "X-Api-Key: k1";
    custom.timeoutMs = 45000;
    store.patch(ttsCustomPatch(custom));

    const CustomTtsConfig& stored = store.get().tts.custom;
    QCOMPARE(stored.url, custom.url);
    QCOMPARE(stored.method, custom.method);
    QCOMPARE(stored.params, custom.params);
    QCOMPARE(stored.headers, custom.headers);
    QCOMPARE(stored.timeoutMs, custom.timeoutMs);
    // 隔壁的 gptsovits 沒有被這一包 patch 掃到
    QCOMPARE(store.get().tts.gptsovits.baseUrl, defaultConfig().tts.gptsovits.baseUrl);
  }

  // 位址是整包 patch 的其中一欄，其餘欄位必須原封不動地活下來。
  // 最致命的是 presets：漏送的話 parseConfig 會補回預設值（空陣列），
  // 使用者只是改個位址就把自己手寫的音色預設整組弄丟了。
  void ttsLocalServerPatchKeepsEverythingElse() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);

    // 先種一份帶 preset 的設定（模擬使用者手改過的 config.json）
    GptSovitsPreset preset;
    preset.id = "sakura";
    preset.name = "櫻";
    preset.refAudioPath = "C:\\voices\\sakura.wav";
    preset.promptText = "こんにちは";
    preset.promptLang = "ja";
    preset.locale = "ja-JP";
    preset.gptWeights = "C:\\models\\sakura.ckpt";
    TtsConfig seeded = defaultConfig().tts;
    seeded.gptsovits.presets.push_back(preset);
    seeded.gptsovits.batchSize = 4;
    seeded.voicebox.engine = std::optional<std::string>("kokoro");
    store.patch(ttsLocalServerPatch(seeded));

    // 使用者只改了兩個位址
    TtsConfig edited = store.get().tts;
    edited.gptsovits.baseUrl = "http://192.168.1.10:9880";
    edited.voicebox.baseUrl = "http://192.168.1.10:17493";
    store.patch(ttsLocalServerPatch(edited));

    const TtsConfig& stored = store.get().tts;
    QCOMPARE(stored.gptsovits.baseUrl, std::string("http://192.168.1.10:9880"));
    QCOMPARE(stored.voicebox.baseUrl, std::string("http://192.168.1.10:17493"));
    QCOMPARE(stored.gptsovits.presets.size(), size_t(1));
    QCOMPARE(stored.gptsovits.presets[0].id, preset.id);
    QCOMPARE(stored.gptsovits.presets[0].name, preset.name);
    QCOMPARE(stored.gptsovits.presets[0].refAudioPath, preset.refAudioPath);
    QCOMPARE(stored.gptsovits.presets[0].promptText, preset.promptText);
    QCOMPARE(stored.gptsovits.presets[0].promptLang, preset.promptLang);
    QCOMPARE(stored.gptsovits.presets[0].locale, preset.locale);
    QCOMPARE(stored.gptsovits.presets[0].gptWeights, preset.gptWeights);
    QCOMPARE(stored.gptsovits.presets[0].sovitsWeights, std::optional<std::string>());
    QCOMPARE(stored.gptsovits.batchSize, 4);
    QCOMPARE(stored.voicebox.engine, std::optional<std::string>("kokoro"));
    // 隔壁的 custom 沒有被這一包 patch 掃到
    QCOMPARE(stored.custom.url, defaultConfig().tts.custom.url);
  }

  // 使用者在 host 欄位打了一個雙引號：舊的就地拼字串會產生無效 JSON，
  // ConfigStore 丟出的例外訊息跟使用者做的事完全對不上。
  // 逃逸之後就只是一個「值很怪但形狀合法」的 patch。
  void hostWithQuoteStaysValidJson() { QVERIFY(acceptedByStore(mcpPatch(true, "127.0.0.1\"evil", 3777, std::nullopt))); }

  // 反斜線在 Windows 路徑很常見（GPT-SoVITS 的參考音檔路徑）
  void backslashInValueSurvives() { QVERIFY(acceptedByStore(stringPatch("mcp", "host", "C:\\weird\\host"))); }
};

QTEST_GUILESS_MAIN(TestConfigPatch)
#include "test_config_patch.moc"
