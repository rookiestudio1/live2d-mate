#pragma once

// 長期記憶：%APPDATA%/live2d_mate/memory/<角色名>.md，一個角色一份純文字檔。
//
// 沿用 persona 的整套哲學（persona.h 檔頭）：純文字、記事本改得動、
// 看得到刪得掉 —— 記憶是隱私敏感的東西，藏進資料庫等於使用者管不到。
// 檔名規則、讀取都直接重用 persona 的函式（readPersona 吃「目錄＋名稱」，
// 換個目錄就是記憶檔），刻意不再發明一套。
//
// 第一期（行為大腦）的記憶是**使用者手寫**的：在檔案裡記下想讓角色知道的事
//（「老闆常熬夜」「最近在趕專案 X」），整份塞進行為大腦的 system prompt。
// 聊天階段（第三期後）才會有 LLM 自己寫記憶的迴路 —— 到時寫入端也落在
// 同一個檔案，使用者照樣改得動。

#include <string>

namespace l2m {

// 注入 prompt 的上限（碼位，與 kPersonaMaxChars 同單位同理由：persona 2000 ＋
// 記憶 2000，兩段合計仍塞得進 7B/8B 模型常見的 8K context）。
inline constexpr int kPersonaMemoryMaxChars = 2000;

// 夾到上限：trim 之後**保留開頭**、超出的尾巴丟掉 —— 檔案開頭放最重要的事
// 是使用者唯一需要知道的規則（比「保留最新」好教：檔案是手寫的，沒有時序）。
// 截斷落在 UTF-8 字元邊界上，不會產生半個中文字的亂碼。
std::string clampPersonaMemory(const std::string& raw);

}  // namespace l2m
