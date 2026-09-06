// 連線位址的 Happy Eyeballs 排序（Qt 的網路層是逐一嘗試，這個問題才會存在，
// 見 core/happy_eyeballs.h 的說明）
#include <QtTest>

#include <vector>

#include "core/happy_eyeballs.h"

using namespace l2m;

namespace {

QHostAddress a(const char* literal) { return QHostAddress(QString::fromLatin1(literal)); }

std::vector<QString> literals(const std::vector<QHostAddress>& addresses) {
  std::vector<QString> out;
  for (const auto& address : addresses) out.push_back(address.toString());
  return out;
}

}  // namespace

class TestHappyEyeballs : public QObject {
  Q_OBJECT

private slots:
  // 家族交錯，第一個位址的家族先行（speech.platform.bing.com 的實際情況：
  // DNS 回 2 個 AAAA + 2 個 A，v6 排前面）
  void interleavesFamiliesV6First() {
    const auto ordered = net::orderForProbe({a("2620:1ec:33:1::10"), a("2620:1ec:33::10"), a("150.171.27.10"), a("150.171.28.10")});
    QCOMPARE(literals(ordered), (std::vector<QString>{"2620:1ec:33:1::10", "150.171.27.10", "2620:1ec:33::10", "150.171.28.10"}));
  }

  // v4 排前面時 v4 先行
  void interleavesFamiliesV4First() {
    const auto ordered = net::orderForProbe({a("150.171.27.10"), a("2620:1ec:33::10"), a("150.171.28.10")});
    QCOMPARE(literals(ordered), (std::vector<QString>{"150.171.27.10", "2620:1ec:33::10", "150.171.28.10"}));
  }

  // 單一家族時維持原順序
  void keepsOrderWithinSingleFamily() {
    const auto ordered = net::orderForProbe({a("150.171.27.10"), a("150.171.28.10")});
    QCOMPARE(literals(ordered), (std::vector<QString>{"150.171.27.10", "150.171.28.10"}));
  }

  // 數量不對稱時多的家族尾端照原順序補完
  void drainsLongerFamilyTail() {
    const auto ordered = net::orderForProbe({a("2620:1ec:33::10"), a("2620:1ec:33:1::10"), a("2620:1ec:33:2::10"), a("150.171.27.10")});
    QCOMPARE(literals(ordered), (std::vector<QString>{"2620:1ec:33::10", "150.171.27.10", "2620:1ec:33:1::10", "2620:1ec:33:2::10"}));
  }

  // v4-mapped v6（::ffff:a.b.c.d）實際走 IPv4，要歸到 v4 家族
  void treatsV4MappedAsV4() {
    const auto ordered = net::orderForProbe({a("::ffff:150.171.27.10"), a("2620:1ec:33::10")});
    QCOMPARE(ordered.size(), size_t(2));
    bool ok = false;
    ordered.front().toIPv4Address(&ok);
    QVERIFY(ok);  // 第一個位址的家族（v4）先行
    QCOMPARE(ordered[1].toString(), QString("2620:1ec:33::10"));
  }

  void emptyStaysEmpty() { QVERIFY(net::orderForProbe({}).empty()); }
};

QTEST_APPLESS_MAIN(TestHappyEyeballs)
#include "test_happy_eyeballs.moc"
