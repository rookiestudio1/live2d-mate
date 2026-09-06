// miniaudio 的唯一實作 TU。
//
// 獨立成一個 .c 檔以 C 編譯：它是四萬行的單檔函式庫，塞進任何 .cpp 都會讓
// 那個檔案的編譯時間爆掉，而且 C 版本的警告比 C++ 少得多。
// 只留解碼與播放，編碼／生成／資源管理／node graph／engine 全部關掉
// （關閉開關在 CMakeLists 以 target_compile_definitions 指定）。
//
// **vendored 的 miniaudio.h 有一處在地修改，升版時務必重做**
// （miniaudio.h:21606 起，函式 ma_context_get_MMDevice__wasapi）：上游那一行無條件的
// CoUninitialize 會把 Qt 在 GUI 執行緒建立的 OLE apartment 拆掉，症狀是桌寵講過一次話
// 之後，設定視窗與所有對話框裡的複製／剪下全部靜默失效（日誌會一路是
// OleSetClipboard ... COM error 0x800401f0）。症狀／根因／修法寫在那個函式的註解裡。

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "mp3_push_decoder.h"

// ---------------------------------------------------------------------------
// 串流 MP3 的 push 解碼包裝（Edge TTS 用）。
//
// **為什麼要包一層**：ma_dr_mp3dec 這組宣告在 miniaudio.h 的 IMPLEMENTATION
// 區塊裡（miniaudio.h:61089-61090，區塊起於 :11507），只有這個 TU 看得到 ——
// stream_decoders.cpp 連 sizeof(ma_dr_mp3dec) 都拿不到，所以型別以
// 「不透明位元組塊」跨出去。
//
// **為什麼不用高階的 ma_decoder + 自訂 onRead**：dr_mp3 的 pull 版在
// miniaudio.h:92637（「手上有資料但湊不出 frame → 再向我要 → 我給 0 位元組」）
// 直接把 atEnd 設成 MA_TRUE，而 atEnd 是 sticky 的（全檔只有 :92982 的
// seek-to-start 會清掉）—— 一次餓死，這個 decoder 就永久報廢。
// 更要命的是它的補料門檻 MA_DR_MP3_MIN_DATA_CHUNK_SIZE = 16384（:92415），
// 在 Edge 的 24kHz/48kbps（6000 B/s）上等於 **2.73 秒**：要保證不餓死就得先
// 緩衝 2.7 秒才敢解第一個 frame，串流就完全沒有意義了。
//
// push 版沒有任何 sticky 狀態：資料不夠就回 frameBytes == 0，補上再叫一次。
// ---------------------------------------------------------------------------

size_t l2mMp3DecoderSize(void) { return sizeof(ma_dr_mp3dec); }

void l2mMp3DecoderInit(void* decoder) {
  MA_ZERO_MEMORY(decoder, sizeof(ma_dr_mp3dec));
  ma_dr_mp3dec_init((ma_dr_mp3dec*)decoder);
}

int l2mMp3DecoderDecode(void* decoder, const unsigned char* mp3, int mp3Bytes, short* pcmOut, int* frameBytes, int* channels, int* sampleRate) {
  ma_dr_mp3dec_frame_info info;
  int frames;
  MA_ZERO_OBJECT(&info);
  frames = ma_dr_mp3dec_decode_frame((ma_dr_mp3dec*)decoder, mp3, mp3Bytes, pcmOut, &info);
  *frameBytes = info.frame_bytes;
  *channels = info.channels;
  *sampleRate = info.hz;
  return frames;
}
