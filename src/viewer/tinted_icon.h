#pragma once

// 工具列圖示的染色：把單色 svg 依目前的調色盤畫成「按鈕文字色」。
//
// 為什麼需要這一層：src/viewer/icons/*.svg 是從 Material Symbols 直接下載的原檔，
// 顏色寫死在 fill 屬性裡（下載預設是 #1f1f1f，近黑）。深色主題下工具列的底色也是
// 深的，六顆按鈕於是整排看不見 —— 圖示還在、也還按得到，只是黑底黑圖。
//
// 三種做法裡選了「畫完再整片重塗」：
//   ① 淺／深色各備一份 svg —— 素材變兩倍，而且只認得「深色主題」這一種情況，
//      使用者換一套高對比或自訂佈景照樣不對。
//   ② 載入時把 svg 位元組裡的 fill="#1f1f1f" 字串換掉 —— 綁死那個色碼，之後從
//      Material Symbols 重新下載一張（網站上可以選顏色）就靜靜失效，沒有錯誤訊息。
//   ③ 本檔：先讓 QtSvg 照原樣畫成點陣，再用 CompositionMode_SourceIn 整片塗成調色盤
//      的顏色。**完全不看 svg 裡寫的是什麼**，所以換素材、換色碼、甚至整套換成別家的
//      圖示集都不必回來改。SourceIn 只保留目的地的 alpha，邊緣的反鋸齒原封不動。
//      限制是只適用單色圖示 —— 這九張都是。
//
// 顏色取 QApplication::palette() 的 QPalette::ButtonText（Qt 的 style 畫工具列按鈕
// 文字用的就是這個角色），停用狀態取同一角色的 Disabled 群組 ——「設定為桌寵」在沒
// 載入模型時是灰的，交給 QIcon 自動生成那份灰的話，它是 style 依**原色**（近黑）推
// 出來的，深色主題下等於再暗一次。
//
// 用 QIconEngine 而不是預先把幾個尺寸畫好塞進 QIcon：畫的時機是 style 真的要畫的
// 那一刻，於是尺寸與 devicePixelRatio 都是當下正確的值（兩台不同縮放的螢幕之間拖
// 視窗時尤其明顯），也不必為了「可能會被要求哪些尺寸」先猜一份清單。
//
// **調色盤變了就整組重新指派一次 QIcon**（ViewerWindow::changeEvent → updateToolBarState）。
// 引擎每次畫都重讀調色盤，理論上主題一換、下一次 repaint 就會是新顏色。之所以還是
// 重新指派：Qt 內建的 QPixmapIconEngine／QIconLoaderEngine 會把畫好的 pixmap 存進
// QPixmapCache、鍵是 QIcon::cacheKey()，而同一個 QIcon 物件的鍵從頭到尾不會變。那條
// 快取寫在**內建引擎裡面**，自訂引擎照理走不到 —— 但那是 Qt 的實作細節、跨版本沒有
// 保證，而換一個新的 QIcon 的成本只是重解一張 600 位元組的 svg。便宜的保險，留著。

#include <QIcon>
#include <QString>

// 回傳一個「顏色跟著調色盤走」的圖示。path 是 qrc 路徑（:/viewer/icons/foo.svg）。
QIcon tintedIcon(const QString& path);
