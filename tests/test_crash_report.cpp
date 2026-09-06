// 當機報告的位址格式化、檔名規則與保留策略。
//
// 這一支釘住的是「報告會不會靜靜指向錯的地方」：writeHex 少補一位零、
// formatFrame 減錯基底，產出的報告格式完全正確，只是每一行都指到別的函式 ——
// 那種錯誤在真的當機、而且有人拿去對 pdb 之前不會有任何人發現。
#include <QtTest>

#include <cstring>
#include <string>
#include <vector>

#include "core/crash_report.h"

using namespace l2m;

namespace {

// 把固定緩衝介面包成好讀的字串，測試裡才不必每次擺一個 char[]
std::string hex(unsigned long long value, int digits, size_t cap = 64) {
  std::vector<char> buf(cap, '\0');
  const size_t n = writeHex(buf.data(), cap, value, digits);
  return std::string(buf.data(), n);
}

std::string frame(unsigned long long addr, unsigned long long base, unsigned long long size, const char* name = "live2d_mate.exe") {
  char buf[128];
  const size_t n = formatFrame(buf, sizeof(buf), addr, base, size, name);
  return std::string(buf, n);
}

}  // namespace

class TestCrashReport : public QObject {
  Q_OBJECT

private slots:
  // 固定寬度補零，一律大寫（WinDbg 與 llvm-symbolizer 兩邊都吃）
  void hexPadsToWidth() {
    QCOMPARE(hex(0, 8), std::string("00000000"));
    QCOMPARE(hex(0x1a2b3c, 8), std::string("001A2B3C"));
    QCOMPARE(hex(0xffffffffffffffffull, 16), std::string("FFFFFFFFFFFFFFFF"));
  }

  // digits = 0 是最短表示
  void hexMinimalWhenNoWidth() {
    QCOMPARE(hex(0, 0), std::string("0"));
    QCOMPARE(hex(0xabc, 0), std::string("ABC"));
  }

  // **寬度不足時絕對不能截掉高位**：把 0x1234 印成 "34" 是會把人帶去
  // 完全錯誤的函式的那種錯誤，寧可讓欄位變寬
  void hexNeverTruncatesHighDigits() { QCOMPARE(hex(0x1234, 2), std::string("1234")); }

  // 緩衝放不下就一個位元組都不寫（handler 那邊沒有第二次機會）
  void hexRefusesShortBuffer() {
    char buf[4];
    std::memset(buf, '#', sizeof(buf));
    QCOMPARE(writeHex(buf, 4, 0x123456, 8), size_t(0));
    QCOMPARE(buf[0], '#');
  }

  // 落在模組區間內 -> module+RVA，RVA 補到 8 位
  void frameInsideImageUsesRva() { QCOMPARE(frame(0x7ff6a01a2b3cull, 0x7ff6a0000000ull, 0xabc000ull), std::string("live2d_mate.exe+0x001A2B3C")); }

  // 邊界：基底本身是 RVA 0（界內），base + size 已經是界外
  void frameBoundaries() {
    QCOMPARE(frame(0x7ff6a0000000ull, 0x7ff6a0000000ull, 0xabc000ull), std::string("live2d_mate.exe+0x00000000"));
    QCOMPARE(frame(0x7ff6a0abc000ull, 0x7ff6a0000000ull, 0xabc000ull), std::string("0x00007FF6A0ABC000"));
    QCOMPARE(frame(0x7ff69fffffffull, 0x7ff6a0000000ull, 0xabc000ull), std::string("0x00007FF69FFFFFFF"));
  }

  // size 0 代表「基底沒抓到」，一律當界外 —— 不能算出一個看起來很像真的 RVA
  void frameWithoutImageSizeIsAbsolute() { QCOMPARE(frame(0x7ff6a01a2b3cull, 0x7ff6a0000000ull, 0), std::string("0x00007FF6A01A2B3C")); }

  // 模組名沒有就只能印絕對位址
  void frameWithoutNameIsAbsolute() { QCOMPARE(frame(0x7ff6a01a2b3cull, 0x7ff6a0000000ull, 0xabc000ull, nullptr), std::string("0x00007FF6A01A2B3C")); }

  // 緩衝不夠就回 0；呼叫端據此整行跳過，而不是寫出半行
  void frameRefusesShortBuffer() {
    char buf[8];
    QCOMPARE(formatFrame(buf, sizeof(buf), 0x7ff6a01a2b3cull, 0x7ff6a0000000ull, 0xabc000ull, "live2d_mate.exe"), size_t(0));
  }

  // 日期時間補零、pid 不補零
  void crashNameFormat() {
    QCOMPARE(crashFileName({2026, 8, 31, 14, 23, 45, 12345}), std::string("crash-20260831-142345-12345.log"));
    QCOMPARE(crashFileName({2026, 1, 2, 3, 4, 5, 7}), std::string("crash-20260102-030405-7.log"));
  }

  // 固定緩衝版與 std::string 版必須一字不差 —— 前者給 handler 用、
  // 後者給啟動掃描用，兩邊對不上就是「寫出來的檔案自己掃不到」
  void crashNameBufferMatchesString() {
    const CrashStamp stamp{2026, 12, 31, 23, 59, 59, 4294967295u};
    char buf[64];
    const size_t n = writeCrashFileName(buf, sizeof(buf), stamp);
    QCOMPARE(std::string(buf, n), crashFileName(stamp));
  }

  // 緩衝放不下就完全不寫
  void crashNameRefusesShortBuffer() {
    char buf[8];
    QCOMPARE(writeCrashFileName(buf, sizeof(buf), {2026, 8, 31, 14, 23, 45, 12345}), size_t(0));
  }

  // 組裝與解析要能往返
  void crashNameRoundTrips() {
    const CrashStamp cases[] = {
      {2026, 8, 31, 14, 23, 45, 12345},
      {2028, 2, 29, 0, 0, 0, 1},                // 閏日、午夜
      {2026, 12, 31, 23, 59, 59, 4294967295u},  // DWORD 上限的 pid
    };
    for (const CrashStamp& stamp : cases) {
      const auto parsed = parseCrashFileName(crashFileName(stamp));
      QVERIFY(parsed.has_value());
      QVERIFY(*parsed == stamp);
    }
  }

  // logs/ 裡的外來檔案一律認不得。**"live2d_mate-2026-08-31.log" 是重點**：
  // crash 檔跟一般 log 同一個資料夾，認錯就會把一般 log 拿去做保留 20 份的清理
  void crashNameRejectsForeignNames() {
    const char* rejected[] = {
      "readme.txt",
      "live2d_mate-2026-08-31.log",  // 一般 log，絕對不能認
      "live2d_mate-2026-08-31.2.log",
      "crash.log",                              // 沒有時間戳
      "crash-20260831-142345.log",              // 少了 pid
      "crash-2026-08-31-142345-1.log",          // 日期帶分隔線
      "crash-20260831-142345-12345.txt",        // 副檔名不對
      "crash-20261331-142345-1.log",            // 月份 13
      "crash-20260832-142345-1.log",            // 日 32
      "crash-20260800-142345-1.log",            // 日 0
      "crash-20260831-246000-1.log",            // 時 24
      "crash-20260831-146000-1.log",            // 分 60
      "crash-20260831-142360-1.log",            // 秒 60
      "crash-20260831-142345-.log",             // 空 pid
      "crash-20260831-142345-x.log",            // pid 不是數字
      "crash-20260831-142345-9999999999.log",   // 10 位但超出 DWORD 上限
      "crash-20260831-142345-99999999999.log",  // pid 位數超出 DWORD
      "crash--142345-1.log",
      ".log",
      "",
    };
    for (const char* name : rejected) {
      QVERIFY2(!parseCrashFileName(name).has_value(), name);
    }
  }

  // 字典序最大的那一份就是最新的
  void latestPicksNewest() {
    const std::vector<std::string> names = {
      "crash-20260830-090000-1.log",
      "crash-20260831-142345-12345.log",
      "crash-20260831-010000-2.log",
      "live2d_mate-2026-08-31.log",  // 一般 log，不參與
      "readme.txt",
    };
    QVERIFY(latestCrashFile(names).has_value());
    QCOMPARE(*latestCrashFile(names), std::string("crash-20260831-142345-12345.log"));
    QVERIFY(!latestCrashFile({}).has_value());
    QVERIFY(!latestCrashFile({"readme.txt"}).has_value());
  }

  // 不到保留份數就一個都不刪
  void expiredKeepsEverythingUnderLimit() {
    std::vector<std::string> names;
    for (int i = 1; i <= 20; ++i) names.push_back(crashFileName({2026, 8, 1, 0, 0, 0, (unsigned long)i}));
    QVERIFY(expiredCrashFiles(names, 20).empty());
  }

  // 超過就刪最舊的那些，且回傳保持輸入順序
  void expiredDropsOldestBeyondLimit() {
    const std::vector<std::string> names = {
      "crash-20260801-000000-1.log",  // 最舊，要刪
      "crash-20260831-000000-3.log",
      "crash-20260815-000000-2.log",  // 次舊，要刪
      "crash-20260901-000000-4.log",
    };
    const auto expired = expiredCrashFiles(names, 2);
    QCOMPARE(expired.size(), size_t(2));
    QCOMPARE(expired[0], std::string("crash-20260801-000000-1.log"));
    QCOMPARE(expired[1], std::string("crash-20260815-000000-2.log"));
  }

  // 一般 log 與外來檔案永遠不進刪除清單 —— 兩種檔案住同一個資料夾，
  // 認錯就是拿 crash 的保留規則去砍一般 log
  void expiredIgnoresForeignFiles() {
    const std::vector<std::string> names = {
      "live2d_mate-2026-08-31.log", "live2d_mate-2026-08-30.log", "readme.txt", "crash-20260801-000000-1.log", "crash-20260831-000000-2.log",
    };
    const auto expired = expiredCrashFiles(names, 1);
    QCOMPARE(expired.size(), size_t(1));
    QCOMPARE(expired[0], std::string("crash-20260801-000000-1.log"));
  }

  // keepCount 非正數是設定錯誤，寧可一個都不刪也不要把目錄清空
  void expiredRefusesNonPositiveKeepCount() {
    const std::vector<std::string> names = {"crash-20260801-000000-1.log"};
    QVERIFY(expiredCrashFiles(names, 0).empty());
    QVERIFY(expiredCrashFiles(names, -1).empty());
  }

  // 三個歷史現場的錯誤碼都要認得（見設計文件 §1）
  void reasonTextCoversHistoricalCodes() {
    QCOMPARE(std::string(crashReasonText(0xC0000005)), std::string("ACCESS_VIOLATION"));
    QCOMPARE(std::string(crashReasonText(0xC0000409)), std::string("STACK_BUFFER_OVERRUN"));
    QCOMPARE(std::string(crashReasonText(0xC00000FD)), std::string("STACK_OVERFLOW"));
    QCOMPARE(std::string(crashReasonText(0xE06D7363)), std::string("CPP_EXCEPTION"));
  }

  // 認不得的碼要有退路 —— 報告裡的 0x 數字本身仍然有用，
  // 不能因為查不到名字就整行不印
  void reasonTextFallsBackToUnknown() {
    QCOMPARE(std::string(crashReasonText(0x12345678)), std::string("UNKNOWN"));
    QCOMPARE(std::string(crashReasonText(0)), std::string("UNKNOWN"));
  }
};

QTEST_APPLESS_MAIN(TestCrashReport)
#include "test_crash_report.moc"
