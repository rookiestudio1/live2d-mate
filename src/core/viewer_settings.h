#pragma once

// Live2D Viewer 的視窗幾何記憶（位置、大小、是否最大化）。
//
// 設定檔是 **%APPDATA%/live2d_mate/viewer.json** —— 跟桌寵**同一個資料夾、
// 不同檔案**。刻意不寫進桌寵的 config.json：兩支是各自獨立的行程（Viewer 沒有
// single-instance，還可以同時開好幾個），而 ConfigStore 是「整份重寫」的，
// Viewer 在關閉時插一筆進去只會跟正在跑的桌寵互相覆蓋。反過來說，Viewer 也
// 不該有辦法動到桌寵的設定。
//
// **為什麼是自己的純函式而不是 QWidget::saveGeometry()**：後者是不透明的二進位
// blob，寫進 JSON 只能是一串 base64 —— 人眼看不出東西、也編不了測試，而本專案的
// 設定檔一律是「記事本改得動」的 JSON。代價是多螢幕與尺寸這些情境要自己想清楚，
// 於是就有了 fitToScreens()：「上次開在副螢幕，這次副螢幕沒接，視窗開在看不見的
// 地方」是這類功能最典型的 bug，而那條規則只有純函式化才驗得到
//（Qt/OS 的部分留在 src/viewer/：讀檔、寫檔、QScreen 換算）。

#include <optional>
#include <string>
#include <vector>

namespace l2m {

// 視窗矩形。座標是**螢幕座標系**，x/y 可以是負的 —— 主螢幕左邊那一台就是負座標，
// 夾成 0 的話多螢幕使用者的視窗每次都會被拉回主螢幕。
struct WindowGeometry {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  // 最大化的視窗記的是**還原後**的矩形（QWidget::normalGeometry），
  // 否則下次取消最大化會得到一個滿螢幕大小的「小」視窗。
  bool maximized = false;
};

// 尺寸的合理範圍。超出範圍的一律當成檔案壞掉而不是「使用者想要這樣」——
// 0 或負數的視窗根本點不到，關掉之後就再也開不回來。
inline constexpr int kMinWindowWidth = 320;
inline constexpr int kMinWindowHeight = 240;
inline constexpr int kMaxWindowSize = 20000;
// 座標的合理範圍（多螢幕排開來也不會有人到這個量級）
inline constexpr int kMaxWindowCoord = 100000;

// viewer.json 的內容 → 視窗幾何。JSON 壞掉、沒有 window 區、尺寸不合理一律回
// nullopt，而且**不算錯誤** —— 呼叫端就用內建預設值開一個視窗，不必報給使用者。
std::optional<WindowGeometry> parseViewerWindow(const std::string& json);

// 視窗幾何 → viewer.json 的內容（pretty，因為這個檔就是給人改的）。
// 整份重寫：這個檔目前只有一個寫入者，也只有 window 這一區。
std::string serializeViewerWindow(const WindowGeometry& window);

// 一面螢幕的可用範圍（QScreen::availableGeometry 換過來的）
struct ScreenRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// 把記下來的矩形擺回「看得見的地方」。screens 的第一個要是主螢幕
// （一面都碰不到時會退回它）。空清單就原樣回傳 —— 沒有螢幕資訊時不要自作聰明。
WindowGeometry fitToScreens(const WindowGeometry& saved, const std::vector<ScreenRect>& screens);

}  // namespace l2m
