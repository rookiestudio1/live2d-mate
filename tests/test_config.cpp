// config.json 的讀寫：預設值、patch 合併與驗證、壞檔備份、序列化鍵順序
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "core/config_store.h"

using namespace l2m;
namespace fs = std::filesystem;

namespace {

void writeText(const fs::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
}

bool fileExists(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

// 設定物件的等值比對（序列化後比字串）
bool sameConfig(const AppConfig& a, const AppConfig& b) { return serializeConfig(a) == serializeConfig(b); }

}  // namespace

class TestConfig : public QObject {
  Q_OBJECT

private slots:
  // 檔案不存在時使用預設值
  void missingFileUsesDefaults() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    QVERIFY(sameConfig(store.get(), defaultConfig()));
  }

  // patch 只覆蓋指定欄位，同一區塊的其他欄位保留
  void patchOnlyTouchesGivenFields() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    store.patch(R"({"model":{"scale":1.5}})");
    QCOMPARE(store.get().model.scale, 1.5);
    QCOMPARE(store.get().model.opacity, defaultConfig().model.opacity);
    QCOMPARE(store.get().tts.engine, defaultConfig().tts.engine);
  }

  // patch 會發出 change 事件並寫入磁碟
  void patchEmitsAndWrites() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    QSignalSpy spy(&store, &ConfigStore::changed);

    store.patch(R"({"mcp":{"port":4000}})");
    store.flush();

    QCOMPARE(spy.count(), 1);
    const auto doc = jsonu::Doc::parseFile(file);
    QVERIFY(doc.has_value());
    QCOMPARE(yyjson_get_int(jsonu::get(jsonu::get(doc->root(), "mcp"), "port")), 4000);
  }

  // 值沒有實際改變時不觸發事件
  void noChangeNoEvent() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    QSignalSpy spy(&store, &ConfigStore::changed);
    store.patch(R"({"model":{"scale":1}})");
    QCOMPARE(spy.count(), 0);
  }

  // 不合法的值會被拒絕，設定維持原狀
  void invalidValueRejected() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    bool threw = false;
    try {
      store.patch(R"({"model":{"scale":99}})");
    } catch (const std::runtime_error& e) {
      threw = std::string(e.what()).find("Invalid config value") != std::string::npos;
    }
    QVERIFY(threw);
    QCOMPARE(store.get().model.scale, defaultConfig().model.scale);
  }

  // 壞掉的 JSON 會被備份並以預設值重建
  void brokenJsonBackedUp() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    const fs::path file = dir / "config.json";
    writeText(file, "{ not json");
    ConfigStore store(file);
    QVERIFY(sameConfig(store.get(), defaultConfig()));
    QVERIFY(fileExists(dir / "config.bak.json"));
  }

  // 欄位驗證失敗的設定檔同樣會被備份重建
  void invalidFieldsBackedUp() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    const fs::path file = dir / "config.json";
    writeText(file, R"({"mcp":{"port":"not-a-number"}})");
    ConfigStore store(file);
    QCOMPARE(store.get().mcp.port, defaultConfig().mcp.port);
    QVERIFY(fileExists(dir / "config.bak.json"));
  }

  // 讀得回上次寫入的內容
  void roundTripsThroughDisk() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    {
      ConfigStore first(file);
      first.patch(R"({"tts":{"voice":"zh-TW-HsiaoChenNeural"},"interaction":{"lookAt":false,"dragSwing":false,"speakFacingFront":false}})");
      first.flush();
    }

    ConfigStore second(file);
    QVERIFY(second.get().tts.voice.has_value());
    QCOMPARE(*second.get().tts.voice, std::string("zh-TW-HsiaoChenNeural"));
    QCOMPARE(second.get().interaction.lookAt, false);
    // 拖曳搖晃開關：預設 true，關掉要寫得進去也讀得回來
    QCOMPARE(defaultConfig().interaction.dragSwing, true);
    QCOMPARE(second.get().interaction.dragSwing, false);
    // 說話回正開關：同上
    QCOMPARE(defaultConfig().interaction.speakFacingFront, true);
    QCOMPARE(second.get().interaction.speakFacingFront, false);
  }

  // 氣泡間距微調：預設 0，負值寫得進去也讀得回來
  void bubbleOffsetRoundTrips() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    QCOMPARE(defaultConfig().tts.bubbleOffsetY, 0);
    {
      ConfigStore first(file);
      first.patch(R"({"tts":{"bubbleOffsetY":-40}})");
      QCOMPARE(first.get().tts.bubbleOffsetY, -40);
      first.flush();
    }
    ConfigStore second(file);
    QCOMPARE(second.get().tts.bubbleOffsetY, -40);
  }

  // 氣泡最晚延遲：預設 5000 ms，改得動也讀得回來
  void bubbleMaxDelayRoundTrips() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    QCOMPARE(defaultConfig().tts.bubbleMaxDelay, 5000.0);
    {
      ConfigStore first(file);
      first.patch(R"({"tts":{"bubbleMaxDelay":800}})");
      QCOMPARE(first.get().tts.bubbleMaxDelay, 800.0);
      first.flush();
    }
    ConfigStore second(file);
    QCOMPARE(second.get().tts.bubbleMaxDelay, 800.0);
  }

  // 0 是合法值：代表不等聲音，氣泡照舊在開始合成時就出現
  void bubbleMaxDelayZeroAccepted() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    store.patch(R"({"tts":{"bubbleMaxDelay":0}})");
    QCOMPARE(store.get().tts.bubbleMaxDelay, 0.0);
  }

  // 超出範圍的延遲被拒絕，設定維持原狀
  void bubbleMaxDelayOutOfRangeRejected() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    bool threw = false;
    try {
      store.patch(R"({"tts":{"bubbleMaxDelay":99999}})");
    } catch (const std::runtime_error& e) {
      threw = std::string(e.what()).find("Invalid config value") != std::string::npos;
    }
    QVERIFY(threw);
    QCOMPARE(store.get().tts.bubbleMaxDelay, defaultConfig().tts.bubbleMaxDelay);
  }
  // 氣泡陰影：預設硬邊，三檔都收得進去也讀得回來
  void bubbleShadowRoundTrips() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    QCOMPARE(defaultConfig().tts.bubbleShadow, std::string("hard"));
    {
      ConfigStore first(file);
      first.patch(R"({"tts":{"bubbleShadow":"soft"}})");
      QCOMPARE(first.get().tts.bubbleShadow, std::string("soft"));
      first.flush();
    }
    ConfigStore second(file);
    QCOMPARE(second.get().tts.bubbleShadow, std::string("soft"));
  }

  // 不在 bubbleShadowIds() 裡的值被拒絕，設定維持原狀
  void bubbleShadowUnknownRejected() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    bool threw = false;
    try {
      store.patch(R"({"tts":{"bubbleShadow":"glow"}})");
    } catch (const std::runtime_error& e) {
      threw = std::string(e.what()).find("Invalid config value") != std::string::npos;
    }
    QVERIFY(threw);
    QCOMPARE(store.get().tts.bubbleShadow, defaultConfig().tts.bubbleShadow);
  }

  // 音量上限放寬到 2（>1 為播放端軟體增益）：2 收得進去也讀得回來
  void volumeUpToTwoAccepted() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    QCOMPARE(defaultConfig().tts.volume, 1.0);
    {
      ConfigStore first(file);
      first.patch(R"({"tts":{"volume":2}})");
      QCOMPARE(first.get().tts.volume, 2.0);
      first.flush();
    }
    ConfigStore second(file);
    QCOMPARE(second.get().tts.volume, 2.0);
  }

  // 超出範圍的音量（>2 或負值）被拒絕，設定維持原狀
  void volumeOutOfRangeRejected() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    for (const char* patch : {R"({"tts":{"volume":2.5}})", R"({"tts":{"volume":-0.1}})"}) {
      bool threw = false;
      try {
        store.patch(patch);
      } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("Invalid config value") != std::string::npos;
      }
      QVERIFY(threw);
      QCOMPARE(store.get().tts.volume, defaultConfig().tts.volume);
    }
  }

  // autonomy：預設值與讀寫往返
  void autonomyRoundTrips() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    QCOMPARE(defaultConfig().autonomy.enabled, true);
    QCOMPARE(defaultConfig().autonomy.expression, true);
    QCOMPARE(defaultConfig().autonomy.speech, std::string("bubble"));
    QCOMPARE(defaultConfig().autonomy.greet, true);
    QCOMPARE(defaultConfig().autonomy.pauseWhenAway, true);
    QCOMPARE(defaultConfig().autonomy.awayAfterMs, 600000);
    QCOMPARE(defaultConfig().autonomy.familiarity, 0);
    // 隨機移動預設關（明確 opt-in，見 config_schema.h）
    QCOMPARE(defaultConfig().autonomy.move, false);
    {
      ConfigStore first(file);
      first.patch(R"({"autonomy":{"enabled":false,"expression":false,"speech":"voice",)"
                  R"("greet":false,"pauseWhenAway":false,"awayAfterMs":120000,"familiarity":42,)"
                  R"("move":true}})");
      first.flush();
    }
    ConfigStore second(file);
    QCOMPARE(second.get().autonomy.enabled, false);
    QCOMPARE(second.get().autonomy.expression, false);
    QCOMPARE(second.get().autonomy.speech, std::string("voice"));
    QCOMPARE(second.get().autonomy.greet, false);
    QCOMPARE(second.get().autonomy.pauseWhenAway, false);
    QCOMPARE(second.get().autonomy.awayAfterMs, 120000);
    QCOMPARE(second.get().autonomy.familiarity, 42);
    QCOMPARE(second.get().autonomy.move, true);
  }

  // autonomy.speech 只認三個 enum 值
  void autonomySpeechRejectsUnknownValues() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    bool threw = false;
    try {
      store.patch(R"({"autonomy":{"speech":"loud"}})");
    } catch (const std::runtime_error& e) {
      threw = std::string(e.what()).find("Invalid config value") != std::string::npos;
    }
    QVERIFY(threw);
    QCOMPARE(store.get().autonomy.speech, std::string("bubble"));
  }

  // wind：預設關、方向 right、強度 0.5；讀寫往返
  void windRoundTrips() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    QCOMPARE(defaultConfig().wind.enabled, false);
    QCOMPARE(defaultConfig().wind.direction, std::string("right"));
    QCOMPARE(defaultConfig().wind.strength, 0.5);
    {
      ConfigStore first(file);
      first.patch(R"({"wind":{"enabled":true,"direction":"left","strength":0.8}})");
      first.flush();
    }
    ConfigStore second(file);
    QCOMPARE(second.get().wind.enabled, true);
    QCOMPARE(second.get().wind.direction, std::string("left"));
    QCOMPARE(second.get().wind.strength, 0.8);
  }

  // wind 的不合法值被拒絕（方向不在 enum、強度超過 1）
  void windInvalidValuesRejected() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    for (const char* patch : {R"({"wind":{"direction":"up"}})", R"({"wind":{"strength":1.5}})"}) {
      bool threw = false;
      try {
        store.patch(patch);
      } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("Invalid config value") != std::string::npos;
      }
      QVERIFY2(threw, patch);
    }
    QCOMPARE(store.get().wind.direction, std::string("right"));
    QCOMPARE(store.get().wind.strength, 0.5);
  }

  // 序列化鍵順序：新 section 一律接在最後，persona → autonomy → wind → llm。
  // 前六個 section 的順序是既有 config.json 的合約，這四個出貨後也是合約。
  void newSectionsSerializeLast() {
    const std::string json = serializeConfig(defaultConfig());
    const size_t persona = json.find("\"persona\"");
    const size_t autonomy = json.find("\"autonomy\"");
    const size_t wind = json.find("\"wind\"");
    const size_t llm = json.find("\"llm\"");
    QVERIFY(persona != std::string::npos);
    QVERIFY(autonomy != std::string::npos);
    QVERIFY(wind != std::string::npos);
    QVERIFY(llm != std::string::npos);
    QVERIFY(persona < autonomy);
    QVERIFY(autonomy < wind);
    QVERIFY(wind < llm);
  }

  // llm：預設值、讀寫往返
  void llmRoundTrips() {
    QCOMPARE(defaultConfig().llm.enabled, false);
    QCOMPARE(defaultConfig().llm.provider, std::string("openai"));
    QCOMPARE(defaultConfig().llm.baseUrl, std::string(kDefaultLlmBaseUrl));
    QCOMPARE(defaultConfig().llm.anthropicModel, std::string("claude-opus-5"));
    QCOMPARE(defaultConfig().llm.temperature, 0.8);
    QCOMPARE(defaultConfig().llm.driveIdle, false);
    QCOMPARE(defaultConfig().llm.idleLlmCooldownMs, 300000);

    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    {
      ConfigStore first(file);
      first.patch(R"({"llm":{"enabled":true,"provider":"anthropic","apiKey":"sk-x","model":"llama3",)"
                  R"("anthropicApiKey":"sk-ant","temperature":1.2,"driveIdle":true,)"
                  R"("idleLlmCooldownMs":0}})");
      first.flush();
    }
    ConfigStore second(file);
    QCOMPARE(second.get().llm.enabled, true);
    QCOMPARE(second.get().llm.provider, std::string("anthropic"));
    QCOMPARE(second.get().llm.apiKey, std::string("sk-x"));
    QCOMPARE(second.get().llm.anthropicApiKey, std::string("sk-ant"));
    QCOMPARE(second.get().llm.temperature, 1.2);
    QCOMPARE(second.get().llm.driveIdle, true);
    QCOMPARE(second.get().llm.idleLlmCooldownMs, 0);
  }

  // llm 的不合法值被拒絕（provider 不在 enum、temperature 超出 0~2）
  void llmInvalidValuesRejected() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    for (const char* patch : {R"({"llm":{"provider":"gemini"}})", R"({"llm":{"temperature":3}})", R"({"llm":{"maxTokens":4}})"}) {
      bool threw = false;
      try {
        store.patch(patch);
      } catch (const std::runtime_error&) {
        threw = true;
      }
      QVERIFY2(threw, patch);
    }
    QCOMPARE(store.get().llm.provider, std::string("openai"));
  }

  // autonomy 的久坐提醒欄位：預設關、門檻範圍
  void breakReminderRoundTrips() {
    QCOMPARE(defaultConfig().autonomy.breakReminder, false);
    QCOMPARE(defaultConfig().autonomy.breakAfterMs, 3600000);

    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    store.patch(R"({"autonomy":{"breakReminder":true,"breakAfterMs":1800000}})");
    QCOMPARE(store.get().autonomy.breakReminder, true);
    QCOMPARE(store.get().autonomy.breakAfterMs, 1800000);

    bool threw = false;
    try {
      store.patch(R"({"autonomy":{"breakAfterMs":1000}})");
    } catch (const std::runtime_error&) {
      threw = true;
    }
    QVERIFY(threw);
  }

  // 超出範圍的間距被拒絕，設定維持原狀
  void bubbleOffsetOutOfRangeRejected() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    bool threw = false;
    try {
      store.patch(R"({"tts":{"bubbleOffsetY":9999}})");
    } catch (const std::runtime_error& e) {
      threw = std::string(e.what()).find("Invalid config value") != std::string::npos;
    }
    QVERIFY(threw);
    QCOMPARE(store.get().tts.bubbleOffsetY, defaultConfig().tts.bubbleOffsetY);
  }

  // crash section 的往返。預設是空字串＝「還沒提示過任何一份」
  void crashLastNotifiedRoundTrips() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    QVERIFY(defaultConfig().crash.lastNotified.empty());
    {
      ConfigStore first(file);
      first.patch(R"({"crash":{"lastNotified":"crash-20260831-142345-12345.log"}})");
      first.flush();
    }
    ConfigStore second(file);
    QCOMPARE(second.get().crash.lastNotified, std::string("crash-20260831-142345-12345.log"));
  }

  // crash 是最後一個 section：鍵順序是既有 config.json 的合約，
  // 插在中間會讓每次重寫都整份 diff
  void crashSectionIsLast() {
    const std::string json = serializeConfig(defaultConfig());
    const size_t llm = json.find("\"llm\"");
    const size_t crash = json.find("\"crash\"");
    QVERIFY(llm != std::string::npos);
    QVERIFY(crash != std::string::npos);
    QVERIFY(llm < crash);
  }
};

QTEST_GUILESS_MAIN(TestConfig)
#include "test_config.moc"
