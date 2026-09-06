#pragma once

// 從音訊位元組的開頭認出容器格式。
//
// 存在理由：串流播放要「拿到第一塊就決定用哪個解碼器」，而 mime 不可信 ——
// 自架端點常常回 application/octet-stream，Edge 那邊我們則根本沒有 header。
// miniaudio 自己會嗅探，但它的嗅探藏在 ma_decoder_init 裡，只有整塊資料
// 到齊才能叫；串流時我們得先知道「這是 MP3 嗎」才選得了增量解碼器。
//
// 放在 core/ 是因為這是純函式，而且判斷錯的後果是「有氣泡沒聲音」——
// 那種靜默失敗一定要有測試釘住。

#include <cstddef>
#include <cstdint>

namespace l2m {

// 認出格式最少需要幾個位元組
inline constexpr size_t kContainerSniffBytes = 12;

enum class AudioContainer {
  // 位元組還不夠，再等下一塊
  NeedMore,
  Wav,
  Mp3,
  Flac,
  // 認得出來但不是我們解得了的（ogg、webm…），或完全不認得
  Other,
};

// data 只需要開頭幾個位元組。size < kContainerSniffBytes 時回 NeedMore。
AudioContainer sniffContainer(const char* data, size_t size);

}  // namespace l2m
