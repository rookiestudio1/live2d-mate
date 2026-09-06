#include <GL/glew.h>

#include "hit_mask.h"

#include <QDebug>

#include <algorithm>
#include <cstring>

#include "core/bottom_align.h"
#include "core/frame_profiler.h"
#include "model_controller.h"

namespace l2m {

namespace {

// 這一份 RGBA 的 alpha 是不是「有透明也有不透明」（門檻與 rebuildMaskBits 一致的 > 0）。
// 兩種退化都代表這份資料不能用，見 AlphaHitMask::commitBlitCandidate()。
bool alphaIsMixed(const uint8_t* rgba, size_t bytes) {
  bool sawTransparent = false;
  bool sawOpaque = false;
  for (size_t i = 3; i < bytes; i += 4) {
    if (rgba[i] > 0) {
      sawOpaque = true;
    } else {
      sawTransparent = true;
    }
    if (sawOpaque && sawTransparent) return true;
  }
  return false;
}

// 可攜的 popcount（只用在 L2M_PROFILE 的診斷路徑，不必求快）
int popcount64(uint64_t v) {
  v = v - ((v >> 1) & 0x5555555555555555ULL);
  v = (v & 0x3333333333333333ULL) + ((v >> 2) & 0x3333333333333333ULL);
  v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
  return static_cast<int>((v * 0x0101010101010101ULL) >> 56);
}

}  // namespace

AlphaHitMask::~AlphaHitMask() {
  // 呼叫端需保證 GL context 有效（CharacterWindow 解構時 makeCurrent）
  if (fence_) glDeleteSync(static_cast<GLsync>(fence_));
  if (pbo_) glDeleteBuffers(1, &pbo_);
  if (fbo_) glDeleteFramebuffers(1, &fbo_);
  if (texture_) glDeleteTextures(1, &texture_);
}

void AlphaHitMask::ensureResources() {
  if (fbo_) return;
  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindTexture(GL_TEXTURE_2D, 0);

  glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // 非同步回讀需要 PBO（GL 2.1）與 fence（GL 3.2）；缺任一就走同步路徑
  asyncReadback_ = (GLEW_VERSION_2_1 || GLEW_ARB_pixel_buffer_object) && (GLEW_VERSION_3_2 || GLEW_ARB_sync);
  // 縮圖 blit 需要 glBlitFramebuffer（GL 3.0 / ARB_framebuffer_object）
  blitSupported_ = (GLEW_VERSION_3_0 || GLEW_ARB_framebuffer_object) && glBlitFramebuffer != nullptr;
  if (asyncReadback_) {
    glGenBuffers(1, &pbo_);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
    glBufferData(GL_PIXEL_PACK_BUFFER, kPixelBytes, nullptr, GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }
}

void AlphaHitMask::update(ModelController& controller, const Source& source, double nowMs, double intervalMs) {
  ensureResources();

  // 先收上一輪的成果；收得到才會有新的 version 讓呼叫端重建形狀
  tryCollect();

  // fence 卡死救援。glClientWaitSync 用的是 timeout=0 且不帶 FLUSH_COMMANDS，
  // 正常情況 swap 會把指令串沖出去、一兩幀內必然 signaled；但實測（Linux/Mesa）
  // 遇過一次 fence 永遠不 signaled 的情況 —— pending_ 卡住後這裡再也不發新回讀，
  // maskBits_ 永遠是空的，點擊穿透就整窗失效且無聲。回讀正常只要 ~40ms，
  // 卡超過 2 秒就放棄這座 fence、改走同步路徑（症狀是每次更新多 ~6ms，
  // 比整個功能無聲失效好得多）。
  if (pending_ && nowMs - pendingSinceMs_ > 2000.0) {
    qWarning() << "[live2d] 命中遮罩的 fence 卡死（>2s 未 signaled），退回同步回讀";
    if (fence_) {
      glDeleteSync(static_cast<GLsync>(fence_));
      fence_ = nullptr;
    }
    pending_ = false;
    dropPending_ = false;
    asyncReadback_ = false;
  }

  if (pending_) return;  // 還有一次回讀在途中，不重複發
  if (lastIssueMs_ >= 0 && nowMs - lastIssueMs_ < intervalMs) return;
  lastIssueMs_ = nowMs;
  pendingSinceMs_ = nowMs;
  issueReadback(controller, source);
}

bool AlphaHitMask::tryCollect() {
  if (!pending_ || !fence_) return false;
  ProfileScope scope(FrameProfiler::StageMaskRead);

  // timeout 0：只問「GPU 畫完了沒」，沒好就立刻返回，主執行緒絕不阻塞
  const GLenum status = glClientWaitSync(static_cast<GLsync>(fence_), 0, 0);
  if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) return false;

  glDeleteSync(static_cast<GLsync>(fence_));
  fence_ = nullptr;
  pending_ = false;

  // 在途回讀屬於已卸載的舊模型：直接丟棄，維持「invalidateForModelSwitch 之後
  // 的每一次 version 遞增必為新模型」的契約
  if (dropPending_) {
    dropPending_ = false;
    return false;
  }

  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
  const void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, kPixelBytes, GL_MAP_READ_BIT);
  bool committed = false;
  if (mapped) {
    // **一律先 memcpy 再驗**，不要直接在 mapped 記憶體上跨步掃 alpha ——
    // mapped PBO 常是 write-combined，跨步讀特別慢，而這條路在游標靠近角色時
    // （intervalMs = 0）是每幀都要跑的。memcpy 是循序的，一趟就把資料搬完。
    if (pendingFromBlit_) {
      if (scratch_.size() != kPixelBytes) scratch_.resize(kPixelBytes);
      std::memcpy(scratch_.data(), mapped, kPixelBytes);
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      committed = commitBlitCandidate();
    } else {
      if (pixels_.size() != kPixelBytes) pixels_.resize(kPixelBytes);
      std::memcpy(pixels_.data(), mapped, kPixelBytes);
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      rebuildMaskBits();
      version_++;
      committed = true;
    }
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  pendingFromBlit_ = false;
  return committed;
}

bool AlphaHitMask::commitBlitCandidate() {
  // blit 來源的健檢，**兩個方向都要驗**：
  //   全透明   → 這台驅動的縮圖 blit 交不出 alpha
  //   全不透明 → 來源根本沒有 alpha channel（setAlphaBufferSize(8) 是請求不是保證）
  // 兩者都會讓形狀壞掉而且完全無聲，只是方向相反：前者點擊穿透整窗失效，
  // 後者反過來把角色周圍的空白全部吃掉點擊，連 isOpaque 都到處回 true
  //（點空白也會播 tap 動作）。
  //
  // 但**不是一次就判死**：模型真的有可能有一幀什麼都沒畫出來（MCP 的
  // set_parameters 把 opacity 寫成 0、動作把模型帶出畫布），單幀就永久
  // 退回慢路徑太粗暴。連續 kBlitStrikeLimit 次才熄火；中間那幾次一律丟掉不收，
  // 形狀停在上一份還能用的，比收下一份壞的好。
  if (alphaIsMixed(scratch_.data(), kPixelBytes)) {
    blitStrikes_ = 0;
    pixels_.swap(scratch_);
    rebuildMaskBits();
    version_++;
    return true;
  }

  if (++blitStrikes_ >= kBlitStrikeLimit) {
    qWarning() << "[live2d] 命中遮罩：主畫布縮圖 blit 連續" << kBlitStrikeLimit << "次交出全透明或全不透明的結果，永久退回重畫路徑";
    blitDisabled_ = true;
    lastIssueMs_ = -1;  // 下一次 update 不受節流限制，立刻用重畫路徑補一份
  }
  return false;
}

bool AlphaHitMask::blitFrom(const Source& source) {
  if (!blitSupported_ || blitDisabled_) return false;
  if (!source.usable || source.width <= 0 || source.height <= 0) return false;

  // 清掉別人留下的舊錯誤，下面的判定才可信。**迴圈一定要有上限** ——
  // 裝置遺失時，帶 KHR_robustness 的驅動會持續回 GL_CONTEXT_LOST 而不是回一次
  // 就轉成 GL_NO_ERROR，無上限的話 GUI 執行緒會在這裡原地空轉、整隻桌寵無回應。
  for (int drain = 0; drain < 32 && glGetError() != GL_NO_ERROR; ++drain) {
  }

  glBindFramebuffer(GL_READ_FRAMEBUFFER, source.fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
  // GL_LINEAR：縮放比約 2.08，每個目標像素取 2×2 來源，細髮絲的覆蓋率幾乎沒掉
  //（逐格比對的結果寫在標頭）。之後還會外擴 kDilate=4 格，這點差異進不了結果。
  glBlitFramebuffer(0, 0, source.width, source.height, 0, 0, kWidth, kHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  const GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    // 最可能的原因是來源是多重取樣的 framebuffer（MSAA → 單取樣的縮放 blit 不合法）。
    // 不重試、直接永久退回重畫：這是驅動與 surface 格式決定的，下一幀不會變。
    qWarning() << "[live2d] 命中遮罩：glBlitFramebuffer 失敗 (GL 0x" << Qt::hex << err << Qt::dec << ")，永久退回重畫路徑";
    blitDisabled_ = true;
    return false;
  }
  return true;
}

void AlphaHitMask::issueReadback(ModelController& controller, const Source& source) {
  // 保存目前的 FBO 與 viewport，畫完遮罩要還原
  GLint prevFbo = 0;
  GLint prevViewport[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
  glGetIntegerv(GL_VIEWPORT, prevViewport);

  {
    ProfileScope scope(FrameProfiler::StageMaskDraw);
    pendingFromBlit_ = blitFrom(source);
    if (!pendingFromBlit_) {
      // 退回重畫。走 drawSilhouette() 而不是 draw()：遮罩不需要高精度遮罩，
      // 而那條路在同一支模型上要多付 4~8 ms（理由與實測寫在 model_controller.h）。
      glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
      glViewport(0, 0, kWidth, kHeight);
      glClearColor(0, 0, 0, 0);
      glClear(GL_COLOR_BUFFER_BIT);
      controller.drawSilhouette(kWidth, kHeight);
    }
  }

  // 回讀一律從遮罩 FBO 讀（blit 那條路把 READ 綁到主畫布去了，要綁回來）
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

  {
    ProfileScope scope(FrameProfiler::StageMaskRead);
    if (asyncReadback_) {
      // 目標是 PBO：glReadPixels 立刻返回，DMA 由 GPU 自己在背景做完
      glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
      glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
      fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      pending_ = fence_ != nullptr;
      if (!pending_) asyncReadback_ = false;  // fence 建不出來就退回同步路徑
    }
    if (!asyncReadback_) {
      // blit 來的那一份要先驗過才能收（規則全在 commitBlitCandidate()）。
      // **只有要驗的時候才用暫存** —— 重畫那條路直接讀進 pixels_，
      // 維持改寫前的行為，也不必每幀多配置一份 216 KB。
      if (pendingFromBlit_) {
        if (scratch_.size() != kPixelBytes) scratch_.resize(kPixelBytes);
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, scratch_.data());
        commitBlitCandidate();
        pendingFromBlit_ = false;
      } else {
        if (pixels_.size() != kPixelBytes) pixels_.resize(kPixelBytes);
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
        rebuildMaskBits();
        version_++;
      }
    }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
  glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}

void AlphaHitMask::rebuildMaskBits() {
  if (pixels_.empty()) return;

  // 1. 門檻化：alpha > 0 的格子點成 1。門檻取 0 而非 kAlphaThreshold，
  //    是為了讓裁切邊界完全落在全透明區，不會咬到看得見的光暈。
  std::vector<uint64_t> rows(size_t(kHeight) * kWords, 0);
  for (int my = 0; my < kHeight; ++my) {
    const int py = kHeight - 1 - my;  // GL 讀回的像素上下顛倒
    const uint8_t* alpha = pixels_.data() + size_t(py) * kWidth * 4 + 3;
    uint64_t* dst = rows.data() + size_t(my) * kWords;
    for (int mx = 0; mx < kWidth; ++mx) {
      if (alpha[size_t(mx) * 4] > 0) dst[mx >> 6] |= uint64_t(1) << (mx & 63);
    }
  }

  // 診斷：形狀永遠比畫面晚一幀（PBO 非同步回讀的代價）。
  // 統計新一幀的角色有多少格落在「上一幀的形狀」之外——那正是會被裁掉的部分。
  if (FrameProfiler::enabled() && !maskBits_.empty()) {
    int outside = 0;
    int total = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
      outside += popcount64(rows[i] & ~maskBits_[i]);
      total += popcount64(rows[i]);
    }
    FrameProfiler::instance().addClipSample(outside, total);
  }

  // 2. 水平膨脹：整列往左右各位移 1~kDilate 格後 OR 回來。
  //    原本每格掃 5×5 鄰域是 55296×25 ≈ 1.38M 次比較，換成位移 OR 只剩幾千次。
  std::vector<uint64_t> hRows = rows;
  for (int my = 0; my < kHeight; ++my) {
    const uint64_t* src = rows.data() + size_t(my) * kWords;
    uint64_t* dst = hRows.data() + size_t(my) * kWords;
    for (int n = 1; n <= kDilate; ++n) {
      for (int w = kWords - 1; w >= 0; --w) {  // 往 mx 增加的方向
        uint64_t v = src[w] << n;
        if (w > 0) v |= src[w - 1] >> (64 - n);
        dst[w] |= v;
      }
      for (int w = 0; w < kWords; ++w) {  // 往 mx 減少的方向
        uint64_t v = src[w] >> n;
        if (w + 1 < kWords) v |= src[w + 1] << (64 - n);
        dst[w] |= v;
      }
    }
  }

  // 3. 垂直膨脹：一列的結果 = 上下 kDilate 列的聯集
  maskBits_.assign(rows.size(), 0);
  for (int my = 0; my < kHeight; ++my) {
    uint64_t* dst = maskBits_.data() + size_t(my) * kWords;
    const int y0 = std::max(0, my - kDilate);
    const int y1 = std::min(kHeight - 1, my + kDilate);
    for (int y = y0; y <= y1; ++y) {
      const uint64_t* src = hRows.data() + size_t(y) * kWords;
      for (int w = 0; w < kWords; ++w) dst[w] |= src[w];
    }
  }
}

QRegion AlphaHitMask::clickableRegion(int windowWidth, int windowHeight) const {
  if (windowWidth <= 0 || windowHeight <= 0) return QRegion();
  if (maskBits_.empty()) {
    // 遮罩還沒好之前整窗可點，不要讓角色點不到
    return QRegion(0, 0, windowWidth, windowHeight);
  }

  // 一格對到哪一條像素列：py = floor(my × H / kHeight)。**視窗比遮罩矮的時候
  // 好幾個格子列會落在同一條像素列上**（角色縮到 0.2 倍時 H=120、kHeight=288），
  // 所以先照 py 分組、把同一組的位元 OR 起來，再一次吐出那一條帶子的矩形。
  // 這樣帶子之間剛好首尾相接（不重疊也不留縫），沒有任何一列的內容被丟掉，
  // 而且天然滿足 setRects 的「同一個 top 等高」與「互不相交」。
  std::vector<QRect> rects;
  rects.reserve(size_t(kHeight) * 2);
  std::vector<uint64_t> merged(size_t(kWords), 0);
  int bandRow = -1;

  // 把 merged 這一條帶子（[bandRow, endRow) 這幾條像素列）掃成矩形
  const auto flush = [&](int endRow) {
    if (bandRow < 0 || endRow <= bandRow) return;
    const size_t bandFirst = rects.size();
    int runStart = -1;
    for (int mx = 0; mx <= kWidth; ++mx) {
      const bool on = mx < kWidth && ((merged[size_t(mx >> 6)] >> (mx & 63)) & 1) != 0;
      if (on && runStart < 0) runStart = mx;
      if (on || runStart < 0) continue;
      // 水平方向刻意保守（左邊 floor、右邊 ceil，跟改寫前一致），代價是相鄰的
      // 兩段可能貼在一起甚至重疊 —— setRects 不准這樣，所以就地跟前一段併掉。
      const int x0 = static_cast<int>(int64_t(runStart) * windowWidth / kWidth);
      const int x1 = static_cast<int>((int64_t(mx) * windowWidth + kWidth - 1) / kWidth);
      runStart = -1;
      if (x1 <= x0) continue;
      if (rects.size() > bandFirst && x0 <= rects.back().right() + 1) {
        rects.back().setRight(std::max(rects.back().right(), x1 - 1));
      } else {
        rects.emplace_back(x0, bandRow, x1 - x0, endRow - bandRow);
      }
    }
  };

  for (int my = 0; my < kHeight; ++my) {
    const int py = std::min(windowHeight - 1, static_cast<int>(int64_t(my) * windowHeight / kHeight));
    if (py != bandRow) {
      flush(py);
      bandRow = py;
      std::fill(merged.begin(), merged.end(), uint64_t(0));
    }
    const uint64_t* row = maskBits_.data() + size_t(my) * kWords;
    for (int w = 0; w < kWords; ++w) merged[size_t(w)] |= row[w];
  }
  flush(windowHeight);

  QRegion region;
  if (!rects.empty()) region.setRects(rects.data(), static_cast<int>(rects.size()));
  return region;
}

std::optional<double> AlphaHitMask::bottomNormalizedY() const {
  if (pixels_.empty()) return std::nullopt;
  // GL 回讀上下顛倒：row 0 即視窗最底，firstOpaqueRow 從 row 0 往上掃
  const auto row = firstOpaqueRow(pixels_.data(), kWidth, kHeight, size_t(kWidth) * 4, kAlphaThreshold);
  if (!row) return std::nullopt;
  return bottomUpRowToNormalizedBottom(*row, kHeight);
}

std::optional<double> AlphaHitMask::topNormalizedY() const {
  if (pixels_.empty()) return std::nullopt;
  // GL 回讀上下顛倒：最後一列才是視窗最頂，lastOpaqueRow 從那裡往下掃
  const auto row = lastOpaqueRow(pixels_.data(), kWidth, kHeight, size_t(kWidth) * 4, kAlphaThreshold);
  if (!row) return std::nullopt;
  return bottomUpRowToNormalizedTop(*row, kHeight);
}

void AlphaHitMask::invalidateForModelSwitch() {
  if (pending_) dropPending_ = true;
  lastIssueMs_ = -1;  // 下一次 update 不受節流限制，立即發出新模型的回讀
}

bool AlphaHitMask::isOpaque(double normalizedX, double normalizedY) const {
  if (pixels_.empty()) return true;  // 遮罩還沒好之前寧可可點，不要讓角色點不到

  const int cx = std::clamp(static_cast<int>(normalizedX * kWidth), 0, kWidth - 1);
  // GL 讀回的像素是上下顛倒的（原點在左下）
  const int cy = std::clamp(kHeight - 1 - static_cast<int>(normalizedY * kHeight), 0, kHeight - 1);

  // 3×3 鄰域取最大 alpha，細髮絲也抓得到
  int maxAlpha = 0;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      const int x = cx + dx;
      const int y = cy + dy;
      if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) continue;
      maxAlpha = std::max(maxAlpha, int(pixels_[(size_t(y) * kWidth + x) * 4 + 3]));
    }
  }
  return maxAlpha > kAlphaThreshold;
}

}  // namespace l2m
