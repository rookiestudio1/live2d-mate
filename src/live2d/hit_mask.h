#pragma once

// alpha 命中遮罩。
//
// 把整個舞台縮小渲染到小尺寸的 FBO，讀回像素當「哪裡是角色本體」的遮罩，
// 供點擊穿透判斷：滑鼠在透明區 → 穿透；踩到角色（含髮絲）→ 接收事件。
// 3×3 鄰域取最大 alpha 是為了抓住細髮絲；門檻 12 過濾抗鋸齒邊緣的雜訊。
//
// 回讀走 PBO + fence 的非同步路徑：同步 glReadPixels 會讓 CPU 等整條 GL
// 管線排空，實測固定吃掉 ~6ms（Release 也一樣，跟編譯最佳化無關），
// 每次遮罩更新都會撞破一整個 vsync 預算。改成「這幀發出回讀、之後某幀
// 用 timeout=0 的 glClientWaitSync 問好了沒」，主執行緒完全不阻塞，
// 代價只是遮罩資料晚一幀（~16ms）才可用。
//
// **遮罩的來源是主畫布的縮圖，不是把模型再畫一次。**
// 原本這裡是 controller.draw(192, 288)，也就是每幀第二次完整繪製整個模型。
// 那很貴而且貴得沒道理 —— 主畫布這一幀已經有一份一模一樣的 alpha 了。
// 實測 Intel UHD 730 / 400×600 / 250 幀，三種做法在**同一幀內依序各做一次**
//（分成三輪各量各的會被機器負載的漂移蓋過去 —— 同一段 draw 兩輪之間量到過
//  p50 7.5 → 15.1 ms）。取 min 而不是 p50：量的時候別的程式也在吃 GPU，
// p50 整段被排隊時間墊高，min 才是近似無競爭的成本。
//
//                                    March 7th（29 個帶遮罩）  藿藿（59 個）
//   重畫模型 @192×288（高精度遮罩）   3.38 ms                  5.92 ms
//   重畫模型 @192×288（一般遮罩）     1.37 ms                  1.93 ms
//   glBlitFramebuffer 主畫布→192×288  0.28 ms                  0.35 ms
//
// 也就是 blit 比原本快 11.9x / 16.9x。連同回讀與位元膨脹，整個 update()
// 從 min 2.24 / 2.33 ms 掉到 0.57 / 0.64 ms。
// 對照組：400×600 的 vsync 預算是 16.7 ms，而改寫前游標一靠近角色，
// 整幀就是 p50 18.6（March 7th）/ 26.1 ms（藿藿）—— 必掉幀。
//
// 逐格比對過兩種來源的差異：兩邊都有 10701 / 14403 格，只有重畫有的 138~154 格、
// 只有 blit 有的 106~121 格，各約 1% —— 而收下之後的 kDilate=4（~10.4 px）本來就
// 遠大於這種一格的邊緣差異。
//
// 兩種情況仍然要退回重畫（見 Source::usable 與 blitDisabled_）：
// 淡入淡出中（主畫布乘了 fade alpha，拿來當遮罩形狀會跟著縮成空），
// 以及驅動不吃這種 blit。退回時走的是 drawSilhouette()（一般遮罩），不是 draw()。

#include <QRegion>

#include <cstdint>
#include <optional>
#include <vector>

namespace l2m {

class ModelController;

class AlphaHitMask {
public:
  // 遮罩解析度與視窗同長寬比（400×600 比例）。
  // 形狀視窗的裁切邊界直接取決於此：96×144 會讓光暈邊緣出現階梯，
  // 取 192×288（格子 ~4px）就看不見了，讀回也才 216KB/次。
  static constexpr int kWidth = 192;
  static constexpr int kHeight = 288;
  // 3×3 鄰域最大 alpha 要超過這個值才算不透明
  static constexpr int kAlphaThreshold = 12;
  // 最短更新間隔（毫秒）
  static constexpr double kUpdateIntervalMs = 200;

  ~AlphaHitMask();

  // 遮罩的來源畫面：這一幀的主畫布。
  struct Source {
    // 主畫布的 framebuffer id。QOpenGLWindow 給的是 defaultFramebufferObject()
    //（NoPartialUpdate 時就是 0，也就是視窗的 back buffer —— blit 讀得到，
    //  前提是這時候還沒 swap，而 paintGL 正是在 swap 之前）。
    unsigned int fbo = 0;
    // 主畫布的實際像素尺寸（含 devicePixelRatio）
    int width = 0;
    int height = 0;
    // 這一幀的主畫布能不能當遮罩用。**淡入淡出中一律 false** ——
    // 主畫布那一次 draw() 乘了 fade alpha，而遮罩要的是 alpha=1 的剪影
    //（理由見 model_controller.h 的 draw() 註解：遮罩跟著淡的話，切模型時
    //  量不到舊模型的腳底，淡出中的視窗形狀也會一路縮成空）。
    bool usable = false;
  };

  // 推進遮罩狀態機（GL context 需為 current）：
  //   1. 先用非阻塞方式收上一輪飛在途中的回讀，收到就更新 pixels_ 與 version()
  //   2. 沒有回讀在途中、且距上次發出已超過 intervalMs → 取得新的一份離屏影像
  //      （優先 blit 主畫布，不行才 drawSilhouette 重畫）並發出新的回讀
  // nowMs 用於節流；intervalMs 可調，游標在視窗上時用較短間隔讓形狀跟緊動畫。
  void update(ModelController& controller, const Source& source, double nowMs, double intervalMs = kUpdateIntervalMs);

  // 視窗內座標（0..1 正規化）是否踩在角色不透明區上
  bool isOpaque(double normalizedX, double normalizedY) const;

  bool ready() const { return !pixels_.empty(); }

  // 模型視覺底部的 normalized Y（0=視窗頂，1=視窗底），切換模型的腳底對齊用。
  // 量「未膨脹」的原始 alpha（pixels_）—— maskBits_ 為了點擊穿透外擴了 kDilate
  // 格，會把底部一律往下推 ~8px。遮罩未就緒或全透明回 nullopt。
  std::optional<double> bottomNormalizedY() const;

  // 模型視覺頂端的 normalized Y（0=視窗頂，1=視窗底），氣泡錨點用。
  // 與 bottomNormalizedY() 同一份未膨脹的原始 alpha、同一組門檻，只是掃描方向
  // 相反。遮罩未就緒或全透明回 nullopt。
  std::optional<double> topNormalizedY() const;

  // 模型切換時呼叫：在途中的舊模型回讀收到時直接丟棄（不進 pixels_、不
  // version_++），並重設節流讓下一次 update 立即發出新模型的回讀。
  // 契約：呼叫之後每一次 version() 遞增必然是新模型畫的 —— 腳底對齊靠這條
  // 判斷「新模型的遮罩落地了」，收進舊模型的在途像素會把舊底部當新模型量。
  void invalidateForModelSwitch();

  // 每次真的收到新的一份遮罩就 +1，呼叫端據此判斷要不要重建視窗形狀
  int version() const { return version_; }

  // 由遮罩產生「角色本體」的視窗形狀（點擊穿透用；座標為視窗像素）。
  // 實際的門檻與膨脹在收到回讀時就算好存進 maskBits_，這裡只做逐列合併。
  //
  // 交出去的方式是一次 QRegion::setRects()，不是逐個矩形 +=。
  // operator+= 每加一個矩形就跟現有區域做一次聯集，實測 p50 0.36~0.66 ms
  //（p95 0.80~0.85），而且這是游標靠近時**每幀**都要付的；改成 setRects 之後
  // 是 p50 0.08~0.09 ms、p95 0.12~0.14 ms。setRects 一次吃下排好的陣列，
  // 但它對陣列有四條要求（YX 排序、互不相交、同一個 top 等高、左右不相鄰），
  // 違反時 QRegion 不會報錯，只會安靜地給出一個壞掉的形狀 —— 下面的縱橫兩段合併
  // 就是為了守住那四條，特別是角色縮到 0.2 倍時「一格還不到一個像素」的情況。
  QRegion clickableRegion(int windowWidth, int windowHeight) const;

private:
  static constexpr size_t kPixelBytes = size_t(kWidth) * kHeight * 4;
  // 一列 kWidth 格用 kWords 個 uint64 表示（192 正好 3 個，沒有尾端 padding）
  static constexpr int kWords = (kWidth + 63) / 64;
  // 形狀的膨脹格數（1 格 ≈ 視窗 2.6px）。兩個作用：
  //   1. 低 alpha 光暈區的格子交錯開洞會變成點狀裁切鋸齒，膨脹把洞填起來、
  //      把階梯推到全透明區
  //   2. 形狀永遠比畫面晚一幀（PBO 非同步回讀），快動作時角色會衝出舊形狀
  //      被裁掉；膨脹量就是這段位移的緩衝。實測 2 格（5.2px）不夠，
  //      最差一幀有 ~93 格衝出去，放大到 4 格（10.4px）。
  // 形狀比角色大只會多留一圈看不見的透明區，代價僅是點擊判定跟著外擴。
  static constexpr int kDilate = 4;

  void ensureResources();
  // 把剛收到的 pixels_ 門檻化（alpha > 0）並膨脹，存進 maskBits_
  void rebuildMaskBits();
  // 非阻塞地取回已完成的回讀；還沒好（或這一份是驗不過的 blit 結果）就返回 false
  bool tryCollect();
  // 驗 scratch_ 裡那一份 blit 結果，過了就 swap 進 pixels_ 並 version_++。
  // 連續驗不過 kBlitStrikeLimit 次就永久退回重畫路徑。
  bool commitBlitCandidate();
  // 把離屏遮罩畫面準備好並發出一次非同步回讀
  void issueReadback(ModelController& controller, const Source& source);
  // 用 glBlitFramebuffer 把主畫布縮進遮罩 FBO；驅動不吃就回 false（呼叫端退回重畫）
  bool blitFrom(const Source& source);

  unsigned int fbo_ = 0;
  unsigned int texture_ = 0;
  unsigned int pbo_ = 0;
  // GLsync（本身就是指標型別）；以 void* 存放，免得這個標頭要引入 GL
  void* fence_ = nullptr;
  bool asyncReadback_ = false;  // 驅動不支援 PBO/fence 時退回同步路徑
  bool blitSupported_ = false;  // glBlitFramebuffer 可不可用（GL 3.0 / ARB_framebuffer_object）
  // blit 交出退化的 alpha（全透明或全不透明）→ 這條路在這台驅動上不可信，
  // 永久退回重畫。兩種退化的症狀都完全無聲，寧可每幀多付幾毫秒。
  bool blitDisabled_ = false;
  // 連續幾次退化才判死。模型真的可能有一幀什麼都沒畫（opacity 被寫成 0、
  // 動作把模型帶出畫布），一次就熄火會把快路徑白白丟掉一整場。
  static constexpr int kBlitStrikeLimit = 3;
  int blitStrikes_ = 0;
  bool pendingFromBlit_ = false;  // 在途的這一次回讀是 blit 來的（收到時要做健檢）
  bool pending_ = false;          // 有一次回讀還在途中
  double pendingSinceMs_ = 0;     // 這次回讀發出的時刻，fence 卡死救援用
  bool dropPending_ = false;      // 在途回讀屬於已卸載的舊模型，收到時直接丟棄
  std::vector<uint8_t> pixels_;   // RGBA，供 isOpaque 的 3×3 取樣用
  // 同步回讀路徑上驗 blit 結果用的暫存（驗過才 swap 進 pixels_）。
  // 只有「沒有 PBO/fence 的驅動」＋「這一份是 blit 來的」時才會用到。
  std::vector<uint8_t> scratch_;
  std::vector<uint64_t> maskBits_;  // 門檻化 + 膨脹後的位元遮罩，視窗形狀的來源
  double lastIssueMs_ = -1;
  int version_ = 0;
};

}  // namespace l2m
