#pragma once

// 貼圖格式的診斷：「這隻模型的貼圖為什麼一張都解不出來」。
//
// 解碼走的是 QImage（live2d/model_controller.cpp 的 DecodeTask），能解哪些格式
// 完全取決於這個 Qt 建置裝了哪些 imageformats 外掛 —— PNG/BMP 是編進 QtGui 的
// 內建 handler，JPEG/GIF/ICO 是隨 Qt 附的外掛，而 **WebP 屬於另外要勾的
// Qt Image Formats 模組**。
//（**PNG 現在走的是 live2d/png_decoder.h 的 libspng 快路徑**，只有它失敗時才退回
// QImage。對這支診斷沒有影響：PNG handler 永遠編在 QtGui 裡，所以呼叫端注入的
// 支援清單一定含 png，而「png 解不出來」的結論仍然是「檔案本身壞了」。）少了它，webp 貼圖的模型會走完一條完全靜默的失敗鏈：
// bytes 讀得到 → QImage 解出 null image → 不 glGenTextures、不 BindTexture →
// Cubism renderer 對 texture id 為 0 的 drawable 直接跳過繪製
// （CubismRenderer_OpenGLES2 的 `if (_textures[...] == 0) return;`）→ 整隻模型
// 透明，而 load() 照樣回報「模型載入完成」。實測 adaerbote_3（685 個 drawable、
// 兩張 4096² webp）的症狀就是一片空白，畫面上一個字的提示都沒有。
//
// 所以這裡把「為什麼失敗」算成一句話。做成純函式而不是就地寫在 ModelController
// 裡：副檔名比對的邊界（大小寫、jpg/jpeg、沒有副檔名、重複格式）錯一格就是給出
// 誤導的建議，而 `l2m_live2d` 連不進測試。支援清單由呼叫端注入（QImageReader
// 在 Qt Gui，core 不碰 GUI），順便讓測試不必真的去問這台機器裝了什麼外掛。

#include <string>
#include <vector>

namespace l2m {

// 貼圖載入失敗的說明。error 與 hint 是**英文**，直接給 CommandResult 與 log 用
//（面向使用者／AI 的字串一律英文，見 core/model_commands.h）。
struct TextureFormatIssue {
  // 這個建置解不動的副檔名，去重、小寫、依 model3.json 的出現順序，例如 {"webp"}。
  // 空的代表「格式都認得，是別的原因失敗」（檔案截斷、損毀、路徑對不上）。
  std::vector<std::string> unsupported;
  std::string error;
  std::string hint;
};

// failedFiles：**實際沒產生貼圖的那幾個**檔名（model3.json 的 FileReferences.Textures
//   原樣，可能帶目錄）。刻意不是「全部宣告過的貼圖」—— 那樣會指錯人：模型裡有一張
//   副檔名寫成 .tga 但其實是 PNG 的圖（QImage 靠內容嗅探照樣解得出來），另一張 png
//   截斷了，收全部的話訊息會叫使用者去裝一個根本用不到的 tga 外掛。
// supportedFormats：這個建置解得動的副檔名（例如 QImageReader::supportedImageFormats()）
// totalTextures：model3.json 宣告的總張數，只用來決定句型是「全滅」還是「N 張中的 M 張」
// 沒有失敗（failedFiles 空）或 totalTextures <= 0 時回一個 error 為空的結果。
TextureFormatIssue inspectTextureFailure(const std::vector<std::string>& failedFiles, const std::vector<std::string>& supportedFormats, int totalTextures);

}  // namespace l2m
