// Edge 線上語音的協定細節。
//
// 協定是自己實作的，所以簽章、SSML 模板、訊框切割都必須釘住 ——
// 這三件事錯了只會表現成「合成失敗」或「播出一段雜訊」，很難從現象追回來。
#include <QtTest>

#include <string>

#include "core/edge_tts_protocol.h"

using namespace l2m;

class TestEdgeSsml : public QObject {
  Q_OBJECT

private slots:
  // 簽章：時間對齊到 300 秒，所以同一個 5 分鐘窗內的值必須一致。
  // 基準時間刻意挑「已經落在窗起點」的 1699999800
  //（11644473600 本身是 300 的倍數，所以只要 unix 秒是 300 的倍數就對齊）。
  void secMsGecIsStableWithinWindow() {
    const std::string token = edge::kTrustedClientToken;
    constexpr int64_t kWindowStart = 1699999800;
    QCOMPARE((kWindowStart + 11644473600LL) % 300, 0LL);

    const std::string a = edge::secMsGec(kWindowStart, token);
    const std::string b = edge::secMsGec(kWindowStart + 299, token);
    const std::string c = edge::secMsGec(kWindowStart + 300, token);

    QCOMPARE(a, b);
    QVERIFY(a != c);
  }

  // 輸出是 64 個字元的大寫十六進位（SHA-256）
  void secMsGecShape() {
    const std::string gec = edge::secMsGec(1700000000, edge::kTrustedClientToken);
    QCOMPARE(gec.size(), size_t(64));
    for (const char ch : gec) {
      QVERIFY((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F'));
    }
  }

  // 手算向量：
  //   ticks        = 1699999800 + 11644473600 = 13344473400
  //   rounded      = 13344473400（已經是 300 的倍數）
  //   windowsTicks = 133444734000000000
  //   payload      = "133444734000000000" + trustedClientToken
  // 這條保證「加基準、對齊、乘 10^7」三步都沒寫反。
  void secMsGecMatchesHandComputedVector() {
    const int64_t ticks = 1699999800LL + 11644473600LL;
    QCOMPARE(ticks, 13344473400LL);
    QCOMPARE(ticks % 300, 0LL);

    const std::string payload = "133444734000000000" + std::string(edge::kTrustedClientToken);
    const QByteArray digest = QCryptographicHash::hash(QByteArray(payload.data(), static_cast<qsizetype>(payload.size())), QCryptographicHash::Sha256);
    const std::string expected = QString::fromLatin1(digest.toHex()).toUpper().toStdString();

    QCOMPARE(edge::secMsGec(1699999800, edge::kTrustedClientToken), expected);
  }

  // 連線 URL 帶齊四個查詢參數
  void synthUrlCarriesAllParams() {
    const std::string url = edge::buildSynthUrl("DEADBEEF", "abc123");
    QVERIFY(url.rfind(edge::kWssUrl, 0) == 0);
    QVERIFY(url.find("TrustedClientToken=") != std::string::npos);
    QVERIFY(url.find("Sec-MS-GEC=DEADBEEF") != std::string::npos);
    QVERIFY(url.find(std::string("Sec-MS-GEC-Version=") + edge::kSecMsGecVersion) != std::string::npos);
    QVERIFY(url.find("ConnectionId=abc123") != std::string::npos);
  }

  // speech.config 訊框：標頭與 JSON 之間是 CRLF CRLF，格式要出現在 JSON 裡
  void speechConfigFrameShape() {
    const std::string frame = edge::buildSpeechConfig(edge::kOutputFormat);
    QVERIFY(frame.rfind("Content-Type:application/json", 0) == 0);
    QVERIFY(frame.find("Path:speech.config\r\n\r\n") != std::string::npos);
    QVERIFY(frame.find(edge::kOutputFormat) != std::string::npos);
    QCOMPARE(edge::framePath(frame), std::string("speech.config"));
  }

  // SSML：語速用相對百分比，正負號都要帶
  void ssmlRatePercent() {
    const std::string faster = edge::buildSsml("zh-TW-HsiaoChenNeural", "zh-TW", "你好", 50);
    QVERIFY(faster.find("rate=\"+50%\"") != std::string::npos);

    const std::string slower = edge::buildSsml("zh-TW-HsiaoChenNeural", "zh-TW", "你好", -25);
    QVERIFY(slower.find("rate=\"-25%\"") != std::string::npos);

    const std::string normal = edge::buildSsml("zh-TW-HsiaoChenNeural", "zh-TW", "你好", 0);
    QVERIFY(normal.find("rate=\"+0%\"") != std::string::npos);
  }

  // SSML：語音名稱與語系都要落在正確的屬性上
  void ssmlCarriesVoiceAndLocale() {
    const std::string ssml = edge::buildSsml("ja-JP-NanamiNeural", "ja-JP", "こんにちは", 0);
    QVERIFY(ssml.find("xml:lang=\"ja-JP\"") != std::string::npos);
    QVERIFY(ssml.find("<voice name=\"ja-JP-NanamiNeural\">") != std::string::npos);
    QVERIFY(ssml.find("こんにちは") != std::string::npos);
  }

  // 文字裡的 XML 特殊字元要逸出，不然整段 SSML 會解析失敗
  void ssmlEscapesText() {
    const std::string ssml = edge::buildSsml("v", "en-US", "A & B <tag>", 0);
    QVERIFY(ssml.find("A &amp; B &lt;tag&gt;") != std::string::npos);
    QVERIFY(ssml.find("<tag>") == std::string::npos);
  }

  // 合成請求訊框帶著 X-RequestId 與 Path:ssml
  void ssmlRequestFrameShape() {
    const std::string frame = edge::buildSsmlRequest("cafe1234", "<speak/>");
    QVERIFY(frame.rfind("X-RequestId:cafe1234\r\n", 0) == 0);
    QVERIFY(frame.find("Path:ssml\r\n\r\n<speak/>") != std::string::npos);
    QCOMPARE(edge::framePath(frame), std::string("ssml"));
  }

  // 從語音名稱推地區
  void localeFromVoiceName() {
    QCOMPARE(edge::localeFromVoiceName("zh-TW-HsiaoChenNeural"), std::string("zh-TW"));
    QCOMPARE(edge::localeFromVoiceName("ja-JP-NanamiNeural"), std::string("ja-JP"));
    QCOMPARE(edge::localeFromVoiceName("nonsense"), std::string(""));
  }

  // 二進位訊框：音訊資料從 "Path:audio\r\n" 之後開始
  void findAudioPayloadLocatesData() {
    const std::string frame = std::string("X-RequestId:abc\r\nContent-Type:audio/mpeg\r\nPath:audio\r\nID3PAYLOAD");
    const size_t offset = edge::findAudioPayload(frame.data(), frame.size());
    QVERIFY(offset != std::string::npos);
    QCOMPARE(frame.substr(offset), std::string("ID3PAYLOAD"));
  }

  // 沒有標記時回 npos（那種訊框要整個丟掉，不能當成音訊接上去）
  void findAudioPayloadRejectsOtherFrames() {
    const std::string frame = "Path:turn.start\r\n\r\n{}";
    QCOMPARE(edge::findAudioPayload(frame.data(), frame.size()), std::string::npos);
    QCOMPARE(edge::findAudioPayload("", 0), std::string::npos);
  }

  // 文字訊框的 Path 判讀（turn.end 才代表整段結束）
  void framePathParsing() {
    QCOMPARE(edge::framePath("X-RequestId:a\r\nPath:turn.end\r\n\r\n"), std::string("turn.end"));
    QCOMPARE(edge::framePath("Path:turn.start\r\n\r\n"), std::string("turn.start"));
    QCOMPARE(edge::framePath("no headers"), std::string(""));
  }

  // 語音清單：去掉 Microsoft 前綴，偏好地區排前面
  void voiceListSortingAndNaming() {
    const std::string json = R"([
      {"ShortName":"en-US-AriaNeural","FriendlyName":"Microsoft Aria Online","Locale":"en-US","Gender":"Female"},
      {"ShortName":"zh-TW-HsiaoChenNeural","FriendlyName":"Microsoft HsiaoChen Online","Locale":"zh-TW","Gender":"Female"},
      {"ShortName":"pt-BR-FranciscaNeural","FriendlyName":"Microsoft Francisca Online","Locale":"pt-BR","Gender":"Female"}
    ])";

    const auto voices = edge::parseVoiceList(json, "edge");
    QCOMPARE(voices.size(), size_t(3));
    // zh-TW 是偏好清單的第一個，要排到最前面
    QCOMPARE(voices[0].id, std::string("zh-TW-HsiaoChenNeural"));
    QCOMPARE(voices[0].name, std::string("HsiaoChen Online"));
    QCOMPARE(voices[0].engine, std::string("edge"));
    QCOMPARE(voices[1].locale, std::string("en-US"));
    // 不在偏好清單裡的排最後
    QCOMPARE(voices[2].locale, std::string("pt-BR"));
  }
};

QTEST_APPLESS_MAIN(TestEdgeSsml)
#include "test_edge_ssml.moc"
