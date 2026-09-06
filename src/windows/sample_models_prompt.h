#pragma once

// 「一隻模型都沒有」的引導對話框。要不要開、開去哪個網址在 core/sample_models.h。
//
// 放在 windows/ 而不是 main.cpp 的匿名命名空間：main.cpp 的啟動順序本身就是
// 一份要逐行讀的合約，塞一段十幾行的 QMessageBox 進去只會讓那份合約更難讀。
//
// 兩個踩得到的點寫在 .cpp 裡，這裡先講結論：
//   ① **一定要排到事件迴圈起來之後才開**（呼叫端用 QTimer::singleShot(0)）。
//      exec() 會開一個巢狀事件迴圈，在 app.exec() 之前直接呼叫等於讓後面的
//      系統匣、MCP 伺服器全部停在那裡等使用者按鍵。
//   ② 對話框要跟著置頂。角色視窗是 always-on-top 的。

#include <filesystem>
#include <string>

namespace l2m {

// 回傳 true ＝ 使用者選了「去下載」（官方頁面與 models 目錄都已經開了）。
bool promptForSampleModels(const std::string& uiLocale, const std::filesystem::path& modelsDir);

}  // namespace l2m
