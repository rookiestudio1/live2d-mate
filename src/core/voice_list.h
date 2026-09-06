#pragma once

// 語音清單的顯示規則（排序、過濾、引擎顯示名）。
//
// 原本是 src/windows/tray.cpp 匿名命名空間裡的 static 函式，測不到；
// 但「哪些地區要顯示、UI 語系對應哪個語音地區、全落空時退到哪」是真正的
// 產品規則 —— 使用者切到韓文就該先看到韓文語音，不必自己往下捲 500 筆。
// 抽到 l2m_core 才驗得起來（tests/test_voice_list.cpp）。
//
// 設定視窗的語音分頁與未來任何語音選擇 UI 都應該走這裡，不要各自排一次。

#include <string>
#include <vector>

#include "tts_types.h"

namespace l2m {

// 語音清單只顯示這些地區，其他請直接編輯設定檔。順序即為顯示順序。
const std::vector<std::string>& voiceDisplayLocales();

// 語音清單完全對不上偏好地區（例如 GPT-SoVITS 的自訂 preset）時，
// 最多退而顯示幾個
inline constexpr int kVoiceFallbackLimit = 40;

// UI 語系對應的語音地區；沒有對應（未知語系）回空字串。
std::string preferredVoiceLocale(const std::string& uiLocale);

// 顯示順序：UI 語系對應的地區排最前，其餘照 voiceDisplayLocales() 的順序，
// 同地區內維持引擎回報的原順序。完全對不上偏好地區時照原樣回傳前
// kVoiceFallbackLimit 個。
std::vector<VoiceInfo> orderVoicesForDisplay(const std::vector<VoiceInfo>& voices, const std::string& uiLocale);

// 內建引擎的顯示名 i18n key（"tts.engine.edge" …）。
// 未知引擎回 nullptr，呼叫端退回引擎自己回報的 name。
const char* engineLabelKey(const std::string& engineId);

}  // namespace l2m
