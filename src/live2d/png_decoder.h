#pragma once

// 貼圖用的 PNG 解碼器：libspng + zlib-ng，直接解成 QImage::Format_RGBA8888。
//
// ── 為什麼不用 QImage::loadFromData ──
//
// Qt 的 PNG handler 是編進 QtGui 的 libpng，而 **qtbase 把 libpng 的 SIMD 明文
// 關掉了**（src/3rdparty/libpng/CMakeLists.txt 寫死 PNG_ARM_NEON_OPT=0，x86 那條
// 要 PNG_INTEL_SSE 才會開、Qt 沒定義），底下的 zlib 也是原版 madler 1.3.2。
// 但**真正的原因不是那個** —— 實測 PNG 解碼有九成的時間在 inflate，反濾波開不開
// SIMD 差不到 10 ms。所以這裡換掉的重點是 inflate（zlib-ng），libspng 只是
// 「能餵 zlib-ng 又比 QImage 少一次格式轉換」的載具。
//
// 實測 LiveroiD_A-Y01 的 8192×16384 貼圖（30 MB → 512 MiB，i5-13500）：
//   QImage::loadFromData + convertTo   671 ms
//   本函式 + convertTo                 309 ms   ← 2.2x
// 完整的四種組合對照與踩過的坑寫在 cmake/FetchZlibNg.cmake。
//
// **8 位元的 PNG 兩條路逐位元組完全一致**（拿上面那張 512 MiB 的貼圖比過，
// 536,870,912 個位元組零差異）。**16 位元的不會** —— spng 降到 8 位元是右移
// 截斷（`r >> 8`），Qt 是先解成 RGBA64 再由 `convertTo` 走 `div_257` 四捨五入，
// 每個通道最多差 1。畫面上看不出來，但別把「完全一致」當成整條路徑的保證。
//
// 順帶省掉一次格式轉換：Qt 的 handler 對 RGBA PNG 產出的是 ARGB32，還要再轉成
// 本專案要的 RGBA8888；spng 直接就是 RGBA8。
//
// ── 這支是「快路徑」，失敗一律回 null 讓呼叫端退回 QImage ──
//
// 刻意不自己報錯：呼叫端本來就有一條完整的失敗診斷鏈（core/texture_format.h
// 那句「為什麼一張都解不出來」），這裡橫插一腳只會多出兩套說法。
// 所以「不是 PNG」「尺寸超過上限」「spng 解不動」三種都只是回 null image。

#include <QByteArray>
#include <QImage>

namespace l2m {

// data：整份 PNG 的位元組（不是路徑 —— zip 模型的來源本來就只有 bytes）
// maxDecodedBytes：解碼後允許佔用的上限，超過就不解（擋解壓縮炸彈：檔頭是可以
//   說謊的，一個宣稱 60000×60000 的壞檔案會當場把記憶體吃光）
// 回傳 Format_RGBA8888 的影像；不是 PNG 或解不動時回 null image，呼叫端退回
//   QImage::loadFromData。
QImage decodePngRgba8(const QByteArray& data, quint64 maxDecodedBytes);

}  // namespace l2m
