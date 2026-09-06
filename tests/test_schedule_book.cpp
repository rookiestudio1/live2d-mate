// 排程帳本：isBusy 的「忙」只涵蓋馬上要發生的事。
// 修的 bug：AI 排一個 6 小時的 MCP schedule，閒置復原與閒置表演停擺整整 6 小時。
#include <QtTest>

#include "core/schedule_book.h"

using namespace l2m;

class TestScheduleBook : public QObject {
  Q_OBJECT

private slots:
  // 6 小時後才到期的排程不算忙（就是那個凍結 bug 的直接斷言）
  void farFutureScheduleIsNotBusy() {
    ScheduleBook book;
    book.add(1, 6.0 * 60 * 60 * 1000);
    QCOMPARE(book.busyWithin(0, kScheduleBusyHorizonMs), false);
    QCOMPARE(book.empty(), false);
  }

  // 30 秒後就要跑的算忙
  void imminentScheduleIsBusy() {
    ScheduleBook book;
    book.add(1, 30000);
    QCOMPARE(book.busyWithin(0, kScheduleBusyHorizonMs), true);
  }

  // 已過期還沒跑的也算忙：它就在下一輪事件迴圈
  void overdueScheduleIsBusy() {
    ScheduleBook book;
    book.add(1, 1000);
    QCOMPARE(book.busyWithin(5000, kScheduleBusyHorizonMs), true);
  }

  // 時間逼近到視野內時由不忙變忙
  void becomesBusyAsTimeApproaches() {
    ScheduleBook book;
    book.add(1, 100000);
    QCOMPARE(book.busyWithin(0, kScheduleBusyHorizonMs), false);
    QCOMPARE(book.busyWithin(50000, kScheduleBusyHorizonMs), true);
  }

  void removeEmptiesTheBook() {
    ScheduleBook book;
    book.add(7, 1000);
    book.remove(7);
    QCOMPARE(book.empty(), true);
    QCOMPARE(book.busyWithin(0, kScheduleBusyHorizonMs), false);
  }

  // 同一 id 重複 add 不留兩筆（覆蓋到期時刻）
  void duplicateAddKeepsOneEntry() {
    ScheduleBook book;
    book.add(3, 1000);
    book.add(3, 999999);
    QCOMPARE(book.ids().size(), size_t(1));
    QCOMPARE(book.busyWithin(0, kScheduleBusyHorizonMs), false);
  }

  // ids() 給解構時逐一 clearTimeout 用
  void idsListsEverything() {
    ScheduleBook book;
    book.add(1, 100);
    book.add(2, 200);
    book.add(3, 300);
    const std::vector<int> expected{1, 2, 3};
    QCOMPARE(book.ids(), expected);
    book.remove(2);
    const std::vector<int> after{1, 3};
    QCOMPARE(book.ids(), after);
  }
};

QTEST_GUILESS_MAIN(TestScheduleBook)
#include "test_schedule_book.moc"
