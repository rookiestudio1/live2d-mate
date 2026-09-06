// MCP 伺服器的 host 邏輯（core/mcp_host.h）：區網位址的挑選與顯示。
// 網路介面以一維的 NetworkAddress 清單表示（每筆自帶 iface 名稱）。
#include <QtTest>

#include <string>
#include <vector>

#include "core/mcp_host.h"

using namespace l2m;

namespace {

// 仿 os.networkInterfaces() 的輸出：一張實體網卡、一個 loopback、一個 IPv6
std::vector<NetworkAddress> fixtureInterfaces() {
  return {
    {"127.0.0.1", true, true, "Loopback Pseudo-Interface 1"},
    {"::1", false, true, "Loopback Pseudo-Interface 1"},
    {"192.168.1.5", true, false, "Wi-Fi"},
    {"fe80::1", false, false, "Wi-Fi"},
    {"10.0.0.8", true, false, "Ethernet"},
  };
}

}  // namespace

class TestMcpHost : public QObject {
  Q_OBJECT

private slots:
  // === isLoopbackHost ===

  // 只有本機連得到
  void loopbackHosts() {
    for (const char* host : {"127.0.0.1", "localhost", "LOCALHOST", "::1", "[::1]", "127.0.0.53", " 127.0.0.1 "}) {
      QVERIFY2(isLoopbackHost(host), host);
    }
  }

  // 不算 loopback
  void nonLoopbackHosts() {
    for (const char* host : {"0.0.0.0", "::", "192.168.1.5", "10.0.0.8", ""}) {
      QVERIFY2(!isLoopbackHost(host), host);
    }
  }

  // === requiresToken ===

  // 只有 loopback 可以不設 token
  void tokenOnlyRequiredOffLoopback() {
    QVERIFY(!requiresToken("127.0.0.1"));
    QVERIFY(!requiresToken("::1"));
    QVERIFY(requiresToken("0.0.0.0"));
    QVERIFY(requiresToken("192.168.1.5"));
  }

  // === validateMcpBinding ===

  // 綁 loopback 時沒 token 也放行
  void loopbackNeedsNoToken() { QVERIFY(!validateMcpBinding("127.0.0.1", std::nullopt).has_value()); }

  // 綁對外位址且沒 token 時擋下來（只有空白的 token 等於沒有）
  void exposedWithoutTokenIsRefused() {
    const auto a = validateMcpBinding("0.0.0.0", std::nullopt);
    QVERIFY(a.has_value());
    QVERIFY(a->find("token") != std::string::npos);

    QVERIFY(validateMcpBinding("192.168.1.5", std::string("")).has_value());
    QVERIFY(validateMcpBinding("192.168.1.5", std::string("   ")).has_value());
  }

  // 綁對外位址但有 token 時放行
  void exposedWithTokenIsAllowed() { QVERIFY(!validateMcpBinding("0.0.0.0", std::string("s3cret")).has_value()); }

  // === listHostOptions ===

  // 永遠把 loopback 排第一、所有介面排第二
  void loopbackAndAllComeFirst() {
    const auto options = listHostOptions(fixtureInterfaces());
    QCOMPARE(options[0].value, std::string("127.0.0.1"));
    QVERIFY(options[0].kind == McpHostKind::Loopback);
    QCOMPARE(options[1].value, std::string("0.0.0.0"));
    QVERIFY(options[1].kind == McpHostKind::All);
  }

  // 列出實體網卡的 IPv4 並帶上網卡名稱（依位址排序）
  void listsPhysicalIpv4() {
    const auto options = listHostOptions(fixtureInterfaces());
    QCOMPARE(options.size(), size_t(4));
    QCOMPARE(options[2].value, std::string("10.0.0.8"));
    QCOMPARE(options[2].label, std::string("Ethernet"));
    QCOMPARE(options[3].value, std::string("192.168.1.5"));
    QCOMPARE(options[3].label, std::string("Wi-Fi"));
  }

  // 略過 internal 與 IPv6
  void skipsInternalAndIpv6() {
    for (const auto& option : listHostOptions(fixtureInterfaces())) {
      QVERIFY(option.value != "::1");
      QVERIFY(option.value != "fe80::1");
    }
  }

  // 沒有任何實體網卡時仍然給得出兩個選項
  void emptyInterfacesStillGiveTwo() {
    const auto options = listHostOptions({});
    QCOMPARE(options.size(), size_t(2));
    QVERIFY(options[0].kind == McpHostKind::Loopback);
    QVERIFY(options[1].kind == McpHostKind::All);
  }

  // === resolveAdvertisedHost ===

  // 綁 0.0.0.0 時挑一個真的連得到的區網位址
  void advertisesLanAddress() {
    QCOMPARE(resolveAdvertisedHost("0.0.0.0", fixtureInterfaces()), std::string("10.0.0.8"));
    QCOMPARE(resolveAdvertisedHost("::", fixtureInterfaces()), std::string("10.0.0.8"));
  }

  // 找不到區網位址時退回 127.0.0.1，總比給出連不到的 0.0.0.0 好
  void fallsBackToLoopback() { QCOMPARE(resolveAdvertisedHost("0.0.0.0", {}), std::string("127.0.0.1")); }

  // 綁具體位址時原樣回傳
  void concreteHostPassesThrough() {
    QCOMPARE(resolveAdvertisedHost("192.168.1.5", fixtureInterfaces()), std::string("192.168.1.5"));
    QCOMPARE(resolveAdvertisedHost("127.0.0.1", fixtureInterfaces()), std::string("127.0.0.1"));
  }

  // === mcpUrl / mcpOrigin ===

  // 組出連線網址與來源
  void buildsUrlAndOrigin() {
    QCOMPARE(mcpUrl("127.0.0.1", 3777), std::string("http://127.0.0.1:3777/mcp"));
    QCOMPARE(mcpOrigin("192.168.1.5", 3777), std::string("http://192.168.1.5:3777"));
  }

  // IPv6 要加方括號才是合法網址
  void ipv6NeedsBrackets() {
    QCOMPARE(mcpUrl("::1", 3777), std::string("http://[::1]:3777/mcp"));
    QCOMPARE(mcpOrigin("[::1]", 3777), std::string("http://[::1]:3777"));
  }

  // === generateToken ===

  // 夠長且只有網址安全字元
  void tokenIsUrlSafe() {
    const std::string token = generateToken();
    QVERIFY(token.size() >= 32);
    for (const char c : token) {
      const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
      QVERIFY2(safe, token.c_str());
    }
  }

  // 每次都不一樣
  void tokenIsRandom() { QVERIFY(generateToken() != generateToken()); }
};

QTEST_APPLESS_MAIN(TestMcpHost)
#include "test_mcp_host.moc"
