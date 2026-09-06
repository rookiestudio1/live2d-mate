#pragma once

// 氣泡要用哪一個字型家族。
//
// 沒有瀏覽器那種「交給 `system-ui` 就好」的捷徑，得自己挑，而且挑法直接決定畫面會不會卡住。
//
// 為什麼不用 QFont::setFamilies() 給候選清單：
// 那是最直覺的寫法，也是這支程式原本的寫法，但只要清單裡不只一個家族，
// Qt 就會為它建 fallback chain，在 Windows 上等於跑一輪 DirectWrite 字型後援解析。
// 實測（這台機器裝了 919 個字型家族，Qt 6.8.3）第一次 QFontMetrics::boundingRect()：
//
//   setFamilies({"Microsoft JhengHei UI", "Microsoft JhengHei", <系統 UI 字型>})  1398 ms
//   setFamilies({"Microsoft JhengHei UI", "Microsoft JhengHei"})                  1292 ms
//   setFamilies({"NoSuchFamily12345"})                                            1320 ms
//   setFamily("Microsoft JhengHei UI")（單一、且真的裝了）                            7.6 ms
//   完全不動 QGuiApplication::font()                                                10 ms
//
// 而 QFontDatabase::families() 本身只要 5.2 ms。所以正確的做法是
// **先問資料庫誰真的在，挑到之後用單數的 setFamily()**，不要把選擇權丟給 DirectWrite。
//
// 這件事看得見：氣泡第一次 show 的時候，那一下度量整個卡在 GUI 執行緒上，
// 使用者看到的是「按下測試語音 → 動畫凍住一秒多 → 氣泡和聲音一起冒出來」
// （氣泡的繪製要等 slot 返回事件迴圈，所以必然是先凍結再一起出現）。
//
// 放在 l2m_core 而不是 windows/bubble_window.cpp：後者編進執行檔，
// 只連 l2m_core 的 QTest 連不到它。查詢「這台機器裝了什麼」是 Qt 的事
// （QFontDatabase 在 Qt6::Gui），所以那一半留在呼叫端，
// 這裡只收「已安裝清單」當參數 —— 規則因此純粹、可測。

#include <string>
#include <vector>

namespace l2m {

// 依語系回傳偏好的字型家族候選，順序即偏好。
// 不含系統 UI 字型保底 —— 那個名字要由呼叫端（拿得到 QGuiApplication）補在最後。
std::vector<std::string> bubbleFontCandidates(const std::string& locale);

// 從候選中挑第一個出現在 installed 裡的家族（不分大小寫）。
// 都沒有就回空字串，呼叫端應該保留 Qt 的預設字型，
// 而不是硬塞一個不存在的家族 —— 硬塞的代價就是上面那 1.3 秒。
std::string pickInstalledFamily(const std::vector<std::string>& candidates, const std::vector<std::string>& installed);

}  // namespace l2m
