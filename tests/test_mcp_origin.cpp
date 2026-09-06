// MCP 伺服器的 Origin 檢查（isAllowedOrigin）
//
// Origin 檢查是防 DNS rebinding 的那道門：瀏覽器的跨站請求一定帶 Origin，
// 而 Claude Code 這類非瀏覽器的 MCP client 不會帶。綁到區網之後這道門
// 不能整個放掉，所以這裡把每種組合都釘住。
//
// isAllowedOrigin 放在 core/mcp_host，測試才不必先起一台 HTTP 伺服器。
#include <QtTest>

#include <optional>
#include <string>

#include "core/mcp_host.h"

using namespace l2m;

class TestMcpOrigin : public QObject {
  Q_OBJECT

private slots:
  // 沒有 Origin 一律放行 —— 非瀏覽器的 MCP client 不會送這個標頭
  void missingOriginIsAllowed() {
    QVERIFY(isAllowedOrigin(std::nullopt, "127.0.0.1"));
    QVERIFY(isAllowedOrigin(std::nullopt, "0.0.0.0"));
    QVERIFY(isAllowedOrigin(std::string(""), "192.168.1.5"));
  }

  // 本機來源永遠放行
  void loopbackOriginsAllowed() {
    for (const char* origin : {"http://127.0.0.1:5173", "http://localhost:3000", "https://localhost", "http://[::1]:8080"}) {
      QVERIFY2(isAllowedOrigin(std::string(origin), "127.0.0.1"), origin);
    }
  }

  // 綁本機時，任何非本機來源都擋掉
  void nonLoopbackOriginRefusedOnLoopback() {
    QVERIFY(!isAllowedOrigin(std::string("http://evil.example.com"), "127.0.0.1"));
    QVERIFY(!isAllowedOrigin(std::string("http://192.168.1.5:8080"), "127.0.0.1"));
  }

  // 綁區網位址時，從那個位址開的頁面才算自己人
  void sameLanOriginAllowed() {
    QVERIFY(isAllowedOrigin(std::string("http://192.168.1.5:8080"), "192.168.1.5"));
    QVERIFY(!isAllowedOrigin(std::string("http://192.168.1.9:8080"), "192.168.1.5"));
    QVERIFY(!isAllowedOrigin(std::string("http://evil.example.com"), "192.168.1.5"));
  }

  // 綁 0.0.0.0 時不會因此放行任意瀏覽器來源
  //（0.0.0.0 是「綁在哪」，不是任何頁面的來源，所以只剩本機來源過得了）
  void bindingAllInterfacesDoesNotOpenTheDoor() {
    QVERIFY(!isAllowedOrigin(std::string("http://192.168.1.5:8080"), "0.0.0.0"));
    QVERIFY(isAllowedOrigin(std::string("http://localhost:5173"), "0.0.0.0"));
  }

  // 解析不了的 Origin 當成不合法
  void malformedOriginRefused() { QVERIFY(!isAllowedOrigin(std::string("not-a-url"), "127.0.0.1")); }

  // 長呼叫的期限與併發名額：speak 與 perform，**兩種 wait 都算**。
  //
  // 這條看起來很想改成「wait=false 就不是長呼叫」，但那會壞掉：說話是佇列的，
  // wait=false 的回覆掛在「這一句開始出聲」，要等前面排隊的句子全部播完。
  // 這個斷言就是用來擋住那個改動的。
  void speakAndPerformAreLongCallsRegardlessOfWait() {
    QVERIFY(isLongToolCall("speak", true));
    QVERIFY(isLongToolCall("speak", false));
    QVERIFY(isLongToolCall("perform", true));
    QVERIFY(isLongToolCall("perform", false));
    QVERIFY(!isLongToolCall("get_state", false));
    QVERIFY(!isLongToolCall("play_motion", true));
  }
};

QTEST_APPLESS_MAIN(TestMcpOrigin)
#include "test_mcp_origin.moc"
