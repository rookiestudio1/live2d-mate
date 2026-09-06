#pragma once

// 把一段要唸的文字切成「一次合成一句」的段落。
//
// 這是句段管線的核心：不會分塊回傳的引擎（sapi、以及任何一次吐完整段的
// HTTP 端點）沒辦法做位元組串流，但可以「先合成第一句就開始播，同時去合成
// 第二句」。實測差距很大 —— 本機 GPT-SoVITS 唸一段 70 字的話要 10 秒才開口，
// 切成句子之後第一句約 2 秒就出聲。
//
// 為什麼放在 core/：切句規則是純邏輯，而且每一條都是踩過的坑
//（3.14 被切成「3.」「14」、URL 被切成兩半、「好。」自己開一個 PowerShell
// 行程），必須測得到。實際的管線在 core/tts_segment_pipeline.h。
//
// 只在句末切（。！？；…），逗號只有在單句超過上限時才拿來軟切 ——
// 語調的斷裂感主要來自「在不該停的地方停」，句號本來就要停。

#include <cstddef>
#include <string>
#include <vector>

namespace l2m {

struct SegmentOptions {
  // 單段的碼位數上限。超過就在逗號之類的軟切點再切一刀。
  size_t maxChars = 60;
  // 比這短的段會被併回前一段 —— 不然「好。」會自己開一次合成請求，
  // 對本機推論引擎來說那是純粹的浪費。
  size_t minChars = 8;
};

// 切句。回傳的每一段都非空，而且把所有段接起來、兩邊都去掉空白之後
// 與原文相同（這條不變式由測試釘住）。
std::vector<std::string> splitIntoSpeechSegments(const std::string& text, const SegmentOptions& options = {});

}  // namespace l2m
