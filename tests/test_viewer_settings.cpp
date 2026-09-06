// Live2D Viewer 的視窗幾何記憶（core/viewer_settings.h）：
// viewer.json 的解析與寫出，以及「螢幕變了怎麼把視窗擺回看得見的地方」。
//
// 這裡驗的都是靜默失敗的那一類：尺寸 0 的視窗點不到、負座標被夾掉的話多螢幕
// 使用者每次都被拉回主螢幕、副螢幕拔掉之後視窗開在虛空裡 —— 三種都不會有錯誤訊息。
#include <QtTest>

#include <string>
#include <vector>

#include "core/viewer_settings.h"

using namespace l2m;

class TestViewerSettings : public QObject {
  Q_OBJECT

private slots:
  // 寫出去再讀回來要一模一樣（這個檔只有這一條合約）
  void roundTrip() {
    const WindowGeometry saved{120, -400, 1280, 800, true};
    const auto parsed = parseViewerWindow(serializeViewerWindow(saved));
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->x, 120);
    QCOMPARE(parsed->y, -400);
    QCOMPARE(parsed->width, 1280);
    QCOMPARE(parsed->height, 800);
    QCOMPARE(parsed->maximized, true);
  }

  // 負座標要原樣留著：主螢幕左邊那一台就是負的，夾成 0 等於每次都把視窗拉回主螢幕
  void negativeCoordinatesKept() {
    const auto parsed = parseViewerWindow(R"({"window":{"x":-1920,"y":-100,"width":800,"height":600}})");
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->x, -1920);
    QCOMPARE(parsed->y, -100);
  }

  // maximized 缺了就是 false —— 手改這個檔的人只會寫位置與大小
  void maximizedOptional() {
    const auto parsed = parseViewerWindow(R"({"window":{"x":0,"y":0,"width":800,"height":600}})");
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->maximized, false);
  }

  // 壞掉的檔案一律 nullopt（呼叫端用預設值開窗，不吵使用者）
  void brokenInputRejected() {
    QVERIFY(!parseViewerWindow("").has_value());
    QVERIFY(!parseViewerWindow("not json at all").has_value());
    QVERIFY(!parseViewerWindow("{}").has_value());                           // 沒有 window 區
    QVERIFY(!parseViewerWindow(R"({"window":42})").has_value());             // 型別不對
    QVERIFY(!parseViewerWindow(R"({"window":{"x":0,"y":0}})").has_value());  // 缺尺寸
    QVERIFY(!parseViewerWindow(R"({"window":{"x":"0","y":0,"width":800,"height":600}})").has_value());
  }

  // 尺寸不合理一律當成檔案壞掉：0 或負數的視窗根本點不到，關掉就再也開不回來
  void absurdSizeRejected() {
    QVERIFY(!parseViewerWindow(R"({"window":{"x":0,"y":0,"width":0,"height":600}})").has_value());
    QVERIFY(!parseViewerWindow(R"({"window":{"x":0,"y":0,"width":-800,"height":600}})").has_value());
    QVERIFY(!parseViewerWindow(R"({"window":{"x":0,"y":0,"width":800,"height":10}})").has_value());
    QVERIFY(!parseViewerWindow(R"({"window":{"x":0,"y":0,"width":999999,"height":600}})").has_value());
  }

  // ── fitToScreens ─────────────────

  // 沒有螢幕資訊時原樣回傳 —— 不知道螢幕在哪就不要自作聰明地搬動視窗
  void noScreensKeepsGeometry() {
    const WindowGeometry saved{100, 100, 800, 600, false};
    const WindowGeometry fitted = fitToScreens(saved, {});
    QCOMPARE(fitted.x, 100);
    QCOMPARE(fitted.y, 100);
    QCOMPARE(fitted.width, 800);
    QCOMPARE(fitted.height, 600);
  }

  // 看得到就一格都不動（含刻意擺在螢幕邊緣、跨在兩面螢幕中間那種擺法）
  void visibleWindowUntouched() {
    const std::vector<ScreenRect> screens{{0, 0, 1920, 1040}, {1920, 0, 1920, 1040}};
    const WindowGeometry saved{1800, 200, 800, 600, false};  // 跨在兩面之間
    const WindowGeometry fitted = fitToScreens(saved, screens);
    QCOMPARE(fitted.x, 1800);
    QCOMPARE(fitted.y, 200);
    QCOMPARE(fitted.width, 800);
    QCOMPARE(fitted.height, 600);
  }

  // 副螢幕拔掉：上次開在 x=2400 的視窗會落在虛空裡，要擺回主螢幕正中央
  void offScreenWindowRecentered() {
    const std::vector<ScreenRect> screens{{0, 0, 1920, 1040}};
    const WindowGeometry fitted = fitToScreens({2400, 300, 800, 600, false}, screens);
    QCOMPARE(fitted.width, 800);
    QCOMPARE(fitted.height, 600);
    QCOMPARE(fitted.x, (1920 - 800) / 2);
    QCOMPARE(fitted.y, (1040 - 600) / 2);
  }

  // 螢幕原點不是 (0,0) 也要回到那一面的正中央（主螢幕排在左邊的多螢幕排法）
  void recenterUsesScreenOrigin() {
    const std::vector<ScreenRect> screens{{-1920, -200, 1920, 1040}};
    const WindowGeometry fitted = fitToScreens({5000, 5000, 800, 600, false}, screens);
    QCOMPARE(fitted.x, -1920 + (1920 - 800) / 2);
    QCOMPARE(fitted.y, -200 + (1040 - 600) / 2);
  }

  // 4K 上記下來的大視窗換到 1080p：先夾成螢幕大小，右下角那半截才拉得回來
  void oversizedWindowClamped() {
    const std::vector<ScreenRect> screens{{0, 0, 1920, 1040}};
    const WindowGeometry fitted = fitToScreens({0, 0, 3000, 2000, false}, screens);
    QCOMPARE(fitted.width, 1920);
    QCOMPARE(fitted.height, 1040);
  }

  // 只露出一條縫（抓不到標題列）也算看不見，一樣要擺回中央
  void sliverVisibleStillRecentered() {
    const std::vector<ScreenRect> screens{{0, 0, 1920, 1040}};
    const WindowGeometry fitted = fitToScreens({1910, 500, 800, 600, false}, screens);
    QCOMPARE(fitted.x, (1920 - 800) / 2);
    QCOMPARE(fitted.y, (1040 - 600) / 2);
  }

  // 擺在哪一面螢幕上就留在哪一面 —— 交集最大的那面才是「原本待的地方」
  void staysOnItsOwnScreen() {
    const std::vector<ScreenRect> screens{{0, 0, 1920, 1040}, {1920, 0, 1920, 1040}};
    const WindowGeometry fitted = fitToScreens({2000, 100, 800, 600, false}, screens);
    QCOMPARE(fitted.x, 2000);
    QCOMPARE(fitted.y, 100);
  }
};

QTEST_APPLESS_MAIN(TestViewerSettings)
#include "test_viewer_settings.moc"
