#pragma once

// WMO weather code（0..99）→ 語意分類與英文描述。
//
// 為什麼值得獨立一支還配一份測試：那張碼表**不連續**（0、1~3、45、48、51~57、
// 61~67、71~77、80~86、95~99，中間全是洞），而且相鄰的號碼分屬完全不同的天氣
// —— 65 是大雨、66 是凍雨、71 是下雪。用區間 if 去猜會讓「下雪講成下雨」
// 這種錯靜默發生，而症狀是桌寵在下雪天叫你帶傘，沒有人會回頭懷疑一張碼表。
//
// 描述字串一律英文：它會進 LLM 的 prompt，也會進日誌（core/model_commands.h
// 明文規定面向使用者／AI 的字串用英文）。

#include "weather_types.h"

namespace l2m {

// 認不得的碼一律 Unknown。
WeatherKind weatherKindFor(int code);

// 「會讓人改變行動」的天氣：毛毛雨以上、凍雨、雪、雷雨。
// 晴、多雲、霧不算 —— 霧對開車有影響，但桌寵提醒不了正在開車的人。
bool weatherKindIsPrecipitation(WeatherKind kind);

// 給 AI 與日誌看的一句英文描述（"heavy rain"、"light snow"…）。
// 認不得的碼回 "unknown"。
const char* weatherCodeDescription(int code);

}  // namespace l2m
