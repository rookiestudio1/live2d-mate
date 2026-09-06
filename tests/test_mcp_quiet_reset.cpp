// MCP 收工之後的短復原（core/idle.h 的 McpQuietReset）。
// 與兩分鐘的 IdleReset 不同，這條只在 AI 真的下過指令時才倒數。
//
// 這裡釘死的是三件會出錯的事：
//   ① 沒有 MCP 下過指令時完全不倒數（不然使用者自己玩桌寵也會被清）
//   ② 說話中不觸發，而且要等「講完」才重新從頭數滿 5 秒
//   ③ 觸發一次就熄火，等下一個 MCP 指令才會再倒數
#include <QtTest>

#include "core/idle.h"
#include "fake_timers.h"

using namespace l2m;

namespace {

struct Harness {
  FakeTimers timers;
  int resetCalls = 0;
  bool busy = false;
  McpQuietReset quiet;

  Harness()
    : quiet(timers, McpQuietReset::Deps{
                      [this] { return busy; },
                      [this] { resetCalls++; },
                    }) {}
};

}  // namespace

class TestMcpQuietReset : public QObject {
  Q_OBJECT

private slots:
  // 沒有任何 MCP 指令時完全不倒數 —— 使用者自己點角色、講話，都不該被 5 秒清掉
  void idleUntilFirstMcpCommand() {
    Harness h;
    h.quiet.onActivity();
    QCOMPARE(h.quiet.driving(), false);
    QCOMPARE(h.quiet.pending(), false);
    h.timers.advance(kMcpQuietResetMs * 10);
    QCOMPARE(h.resetCalls, 0);
  }

  // 一個 MCP 指令之後，安靜滿 5 秒才復原
  void firesAfterQuietWindow() {
    Harness h;
    h.quiet.onCommand();
    QCOMPARE(h.quiet.driving(), true);
    h.timers.advance(kMcpQuietResetMs - 1);
    QCOMPARE(h.resetCalls, 0);
    h.timers.advance(1);
    QCOMPARE(h.resetCalls, 1);
  }

  // 期間又來一個 MCP 指令就重新給滿，不會提早清掉上一步設好的表情
  void newCommandRestartsWindow() {
    Harness h;
    h.quiet.onCommand();
    h.timers.advance(kMcpQuietResetMs - 1);
    h.quiet.onCommand();
    h.timers.advance(kMcpQuietResetMs - 1);
    QCOMPARE(h.resetCalls, 0);
    h.timers.advance(1);
    QCOMPARE(h.resetCalls, 1);
  }

  // ★ MCP 下達 speak 時：說話期間絕不觸發，而且倒數要從「講完」重新數滿 5 秒，
  // 不是把說話前剩下的殘值接著用。onActivity() 對應 AppController::touchIdle()，
  // SpeechController::finished（佇列真的清空才發）就接在那上面。
  void waitsForSpeechThenCountsFromScratch() {
    Harness h;
    h.busy = true;
    h.quiet.onCommand();
    h.timers.advance(kMcpQuietResetMs * 3);
    QCOMPARE(h.resetCalls, 0);

    h.busy = false;
    h.quiet.onActivity();
    h.timers.advance(kMcpQuietResetMs - 1);
    QCOMPARE(h.resetCalls, 0);
    h.timers.advance(1);
    QCOMPARE(h.resetCalls, 1);
  }

  // 復原一次就熄火：之後的活動（使用者互動、自主表演說話）不會再排下一輪，
  // 要等下一個 MCP 指令
  void stopsUntilNextCommand() {
    Harness h;
    h.quiet.onCommand();
    h.timers.advance(kMcpQuietResetMs);
    QCOMPARE(h.resetCalls, 1);
    QCOMPARE(h.quiet.driving(), false);

    h.quiet.onActivity();
    h.timers.advance(kMcpQuietResetMs * 3);
    QCOMPARE(h.resetCalls, 1);

    h.quiet.onCommand();
    h.timers.advance(kMcpQuietResetMs);
    QCOMPARE(h.resetCalls, 2);
  }
};

QTEST_APPLESS_MAIN(TestMcpQuietReset)
#include "test_mcp_quiet_reset.moc"
