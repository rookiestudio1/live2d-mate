#pragma once

// PNG 檔頭的判定與解析 —— 「這份 bytes 是不是 PNG、多大、解出來要幾個位元組」。
//
// 貼圖解碼從 QImage 換成 libspng 之後（見 live2d/png_decoder.h：**PNG 解碼的
// 時間九成在 inflate**，換掉 zlib 才是 2.2 倍的來源，理由與實測數字寫在
// cmake/FetchZlibNg.cmake），「這一張要不要走 spng」變成每張貼圖都要判一次的
// 分岔。判錯的兩個方向都是靜默失敗：把 JPEG 餵給 spng 只是白跑一趟還好，
// 但把 PNG 誤判成不是 PNG，就會整批悄悄退回舊路徑 —— 畫面完全正常、
// 一句警告都沒有，只是速度回到從前，沒有人會發現。
// 而 IHDR 是大端序的手工解析，位移錯一格就是尺寸整個亂掉。
// `l2m_live2d` 連不進測試（見 CMakeLists 的 l2m_add_test），所以這一段放 core。
//
// 「解碼後要幾個位元組」也收在這裡：8192×16384 × 4 已經是 5 億多，用 32 位元
// 算會溢位，而溢位之後那個小數字會讓上限檢查一路放行，真正炸掉的地方在後面的
// 配置，看起來像是「解碼器壞了」。

#include <cstddef>
#include <cstdint>
#include <optional>

namespace l2m {

// IHDR 的內容。欄位名沿用 PNG 規格的用詞，好對照 spec 查。
struct PngHeader {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bitDepth = 0;   // 1 / 2 / 4 / 8 / 16
  uint8_t colorType = 0;  // 0 灰階, 2 RGB, 3 調色盤, 4 灰階+alpha, 6 RGBA
  uint8_t interlace = 0;  // 0 無, 1 Adam7
};

// 只看前 8 個位元組的 PNG 簽章。資料不足一律回 false。
bool isPngSignature(const void* data, size_t size);

// 解析簽章 + IHDR。不是 PNG、長度不足、或 IHDR 的欄位不合規格時回 nullopt
//（後者交給呼叫端退回通用解碼器去報錯，這裡不負責產生錯誤訊息）。
std::optional<PngHeader> readPngHeader(const void* data, size_t size);

// 解成 8 位元 RGBA 要幾個位元組（width × height × 4）。
// 尺寸為 0 或乘起來會溢位 uint64 時回 0 —— 呼叫端一律把 0 當成「不要碰」。
uint64_t pngRgba8Bytes(const PngHeader& header);

}  // namespace l2m
