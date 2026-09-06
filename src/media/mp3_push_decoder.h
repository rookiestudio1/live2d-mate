#ifndef L2M_MP3_PUSH_DECODER_H
#define L2M_MP3_PUSH_DECODER_H

// 串流 MP3 解碼的 C 介面。實作在 miniaudio_impl.c —— 那是唯一看得到
// ma_dr_mp3dec 的 TU（它宣告在 miniaudio.h 的 IMPLEMENTATION 區塊裡），
// 所以型別以「不透明位元組塊」跨出來。為什麼不用高階的 ma_decoder，
// 理由寫在 miniaudio_impl.c 的實作上方。

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 呼叫端要配置這麼多位元組給 decoder 狀態
size_t l2mMp3DecoderSize(void);

void l2mMp3DecoderInit(void* decoder);

// 從 mp3[0..mp3Bytes) 解一個 frame。
//
// pcmOut 至少要 2304 個 short（MA_DR_MP3_MAX_SAMPLES_PER_FRAME）。
// **輸出是 s16 不是 float**：MA_DR_MP3_FLOAT_OUTPUT 在這份 miniaudio 裡
// 從來沒有被 #define 過（全檔只有 #ifndef 與 #if defined）。
//
// 回傳解出的 PCM frame 數（每聲道）。frameBytes 是這一輪吃掉的位元組數：
//   frameBytes == 0            → 資料不足，補更多位元組再叫一次（沒有壞狀態）
//   frameBytes > 0 且回傳 0    → 跳過了垃圾（例如 ID3 標頭），前進就好
int l2mMp3DecoderDecode(void* decoder, const unsigned char* mp3, int mp3Bytes, short* pcmOut, int* frameBytes, int* channels, int* sampleRate);

#ifdef __cplusplus
}
#endif

#endif  // L2M_MP3_PUSH_DECODER_H
