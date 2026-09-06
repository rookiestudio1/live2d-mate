#pragma once

// 回音：think 工具那把「腦內獨白」的音色。
//
// 引擎那邊沒得談 —— Edge／SAPI／GPT-SoVITS 都只交出乾聲，SSML 也沒有殘響的
// 標記，所以只能在解碼之後的 PCM 上自己做。插入點是 AudioPlayer::pump()：
// 解碼完、寫進環形緩衝之前，跑在 GUI 執行緒上，不違反「即時音訊執行緒只做
// memcpy 與四則運算」的契約。格式在那裡已經統一成 f32／立體聲／48 kHz。
//
// 結構是一條帶阻尼的回授延遲線（Freeverb 的 comb）：
//   echo        = line[cursor]
//   store       = echo·(1-damping) + store·damping     ← 一階低通，在回授路徑上
//   line[cursor]= in + store·feedback
//   out         = in·dry + echo·wet
//
// **低通不能省**。純延遲的每一次重複都跟原音一樣亮，聽起來是卡拉 OK 的
// echo；腦內獨白要的是「越重複越悶、越遠」，那一階就是全部的差別。
//
// dry 刻意小於 1：wet 疊上去會把峰值推高，而音量增益是在 miniaudio 的
// dataCallback 返回後才乘、由內建限幅器硬砍到 ±1（見 audio_player.cpp 的
// set_master_volume 註解），不先讓出一點空間就會削頂破音。順帶讓思考的聲音
// 比說話小一點 —— 那本來也是對的。
//
// 時序上的坑：**殘響會在乾聲結束之後才響完**。AudioPlayer 判定「播完了」用的是
// 「已送出 frame ≥ 最後一個真音訊 frame ＋ 裝置尾端」，位元組一收完就宣告
// EOS 的話尾巴會被硬切，聽起來像被拔插頭。所以句子收尾時要再推 tailFrames()
// 個 frame 的靜音進來，把延遲線裡剩下的回音沖出去，沖完才宣告 EOS。

#include <cstddef>
#include <cstdint>
#include <vector>

namespace l2m {

class EchoFilter {
public:
  struct Params {
    double delayMs = 0;
    double feedback = 0;  // 0..0.9，每一輪重複保留的比例
    double damping = 0;   // 0..0.95，越大每一輪越悶
    double dry = 1.0;
    double wet = 0.0;  // 0 代表不啟用
  };

  // think 的音色。手感常數寫死在這裡不進 config，照 GazeDirectorTuning 的慣例
  //（使用者調不動，但也不會有人為了「回音多一點」去改 config.json）。
  //
  // **要調回音強弱就改 .cpp 裡的 kThinkingWet**（0 ＝ 完全沒有回音，
  // 越大越濕）；kThinkingFeedback 則是「重複幾次才消失」，兩顆分開調。
  static Params thinkingVoice();

  // wet 或 delayMs 為 0 時不會啟用，process() 變成 no-op
  void reset(uint32_t sampleRate, uint32_t channels, const Params& params);
  void clear();
  bool active() const { return active_; }

  // 就地處理交錯的 f32 樣本。frames ＝ 樣本數 ÷ channels。
  void process(float* samples, size_t frames);

  // 乾聲收完之後還要再推多少 frame 的靜音，殘響才不會被切掉
  size_t tailFrames() const { return tailFrames_; }

private:
  std::vector<float> line_;   // 延遲線（與輸入同樣是交錯的）
  std::vector<float> store_;  // 每聲道一個低通狀態
  size_t delayFrames_ = 0;
  size_t cursor_ = 0;
  size_t tailFrames_ = 0;
  uint32_t channels_ = 0;
  float feedback_ = 0;
  float damping_ = 0;
  float dry_ = 1;
  float wet_ = 0;
  bool active_ = false;
};

// think 的音量：正常說話音量的幾成。內心話比出聲說話小一點是自然的，
// 也順帶給殘響疊上去的峰值留一點餘裕（見 Params::dry 的說明）。
// 乘在 tts.volume 上，所以使用者調整總音量時比例維持不變。
inline constexpr double kThinkingVolumeScale = 0.8;

}  // namespace l2m
