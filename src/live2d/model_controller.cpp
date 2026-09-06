#include "model_controller.h"

#include <GL/glew.h>

#include <QBuffer>
#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>
#include <QImage>
#include <QImageReader>
#include <QRunnable>
#include <QString>
#include <QThread>
#include <QThreadPool>

#include <CubismDefaultParameterId.hpp>
#include <CubismModelSettingJson.hpp>
#include <Effect/CubismBreath.hpp>
#include <Effect/CubismEyeBlink.hpp>
#include <Id/CubismIdManager.hpp>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionQueueEntry.hpp>
#include <Physics/CubismPhysics.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include <Utils/CubismString.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>

#include "core/canvas_center.h"
#include "core/hit_area_semantics.h"
#include "core/layout_fit.h"
#include "core/model_assets.h"
#include "core/model_settings.h"
#include "core/motion_meta.h"
#include "core/png_probe.h"
#include "core/texture_format.h"
#include "core/wind_targets.h"
#include "cubism_runtime.h"
#include "png_decoder.h"

using namespace Live2D::Cubism::Framework;
using namespace Live2D::Cubism::Framework::DefaultParameterId;

namespace l2m {

namespace {

std::string motionKeyOf(const std::string& group, int index) { return group + "_" + std::to_string(index); }

// ── 貼圖解碼的平行化 ──
//
// ModelController::load() 全程同步、跑在 GUI 執行緒上，而 8192² 的貼圖
// 一張解碼後就是 256 MiB —— huohuo 有三張，實測整個載入約 5 秒。
//
// 能搬到 worker 的只有「PNG 解碼 + 格式轉換」這兩步，但它們正好佔了九成時間。
// 其餘每一步都不行：CreateRenderer / glTexImage2D 需要 current GL context，
// 而 setupModel / preloadMotions 碰的 CubismNativeFramework 5-r.5 整棵原始碼樹
// 連一個 mutex / atomic / thread_local 都沒有 —— CubismIdManager::RegisterId()
// 是「線性搜尋 + PushBack」，GUI 執行緒每幀查 id 時 worker 註冊 id 會把正在
// iterate 的陣列 realloc 掉。**worker 裡不准出現任何 Csm:: 符號。**
constexpr quint64 kDecodeBudgetBytes = 512ull * 1024 * 1024;
// 再多執行緒也不會更快：PNG 解碼卡在 zlib inflate 與記憶體頻寬
constexpr int kMaxDecodeThreads = 4;
// 單張貼圖允許的解碼配置上限（MB），用來覆蓋 Qt 自己那條預設 256 MB 的守衛。
// 為什麼需要它、又為什麼不乾脆解除，寫在 setupTextures() 裡動用它的地方。
constexpr int kMaxDecodeAllocationMb = 1024;
// 解碼期間輪詢進度的節奏。GUI 執行緒在這段是睡著的，只負責把 splash 推上螢幕。
constexpr int kDecodePollMs = 16;

// 讀 PNG 檔頭要多少位元組。IHDR 固定在前 33 個（8 簽章 + 25），JPEG 的 SOF
// 可能被 EXIF 推到後面一點，抓 4 KiB 綽綽有餘。zip 模型靠這個維持
// 「尺寸探測是微秒級」—— 整包 inflate 一張 8192² 的 PNG 是幾十毫秒。
constexpr int kHeaderProbeBytes = 4096;

struct TextureSource {
  int index = 0;
  std::string rel;    // 相對於模型根目錄的路徑（zip 內外都是同一種寫法）
  quint64 bytes = 0;  // 解碼後的估計佔用（w×h×4），只讀檔頭算出來
};

// worker 拿到的是**已經在 GUI 執行緒讀好的壓縮 bytes**，不是檔案路徑：
// zip 版的來源是 miniz，而 miniz 的 reader 有共用緩衝，不能兩條執行緒同時解。
// 資料夾模型也走同一條路，兩種容器才不會有兩套程式碼（原本是 QImage(path)）。
class DecodeTask : public QRunnable {
public:
  DecodeTask(QByteArray data, QImage::Format format, QImage* out, std::atomic<int>* done) : data_(std::move(data)), format_(format), out_(out), done_(done) {}

  void run() override {
    // PNG 走 libspng + zlib-ng 的快路徑（見 png_decoder.h：實測 2.2x，而且
    // 差別幾乎全在 inflate，不是反濾波的 SIMD）。不是 PNG、或那條解不動時
    // 一律退回 QImage —— jpg／webp 與各種 PNG 變體都靠它，所以兩條都要留。
    QImage image = decodePngRgba8(data_, quint64(kMaxDecodeAllocationMb) * 1024 * 1024);
    if (image.isNull()) image.loadFromData(data_);
    // convertTo 而不是 convertToFormat：32bpp ↔ 32bpp 走 Qt 的就地 converter，
    // 省掉一整份 256 MiB 的配置與一趟 256 MiB 的 memcpy
    // （spng 交出來的是 RGBA8888，轉成預乘就是這一步，實測 512 MiB 約 26 ms）
    if (!image.isNull()) image.convertTo(format_);
    *out_ = std::move(image);
    // release/acquire 這一對就是 worker 寫 *out_ 與 GUI 讀它之間的 happens-before
    done_->fetch_add(1, std::memory_order_release);
  }

private:
  QByteArray data_;
  QImage::Format format_;
  QImage* out_;
  std::atomic<int>* done_;
};

// 貼圖上傳的 GL 那一段（原本就在 setupTextures 裡，只是搬出來讓批次流程好讀）。
// 必須留在有 current context 的 GUI 執行緒。
void uploadTexture(Rendering::CubismRenderer_OpenGLES2* renderer, int index, const QImage& image, std::vector<unsigned int>& textures) {
  if (image.isNull()) {
    qWarning() << "[live2d] 貼圖載入失敗, index =" << index;
    return;
  }
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  while (glGetError() != GL_NO_ERROR) {
  }  // 清空舊錯誤，下面的判定才可信

  // 上傳失敗（虛擬機 GPU 對 8192² × 4B = 256MB 的配置常常過不了，即使
  // GL_MAX_TEXTURE_SIZE 自稱夠大）就逐半縮小重試 —— 糊的角色比全黑剪影好。
  QImage current = image;
  GLenum uploadErr = GL_NO_ERROR;
  for (;;) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, current.width(), current.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, current.constBits());
    uploadErr = glGetError();
    if (uploadErr == GL_NO_ERROR) break;
    if (current.width() <= 1024 || current.height() <= 1024) break;
    qWarning() << "[live2d] 貼圖上傳失敗 (GL 0x" << Qt::hex << uploadErr << Qt::dec << "), index =" << index << ", " << current.width() << "x" << current.height() << "→縮半重試";
    current = current.scaled(current.width() / 2, current.height() / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(image.format());
  }
  if (uploadErr != GL_NO_ERROR) {
    qWarning() << "[live2d] 貼圖上傳徹底失敗 (GL 0x" << Qt::hex << uploadErr << Qt::dec << "), index =" << index;
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &texture);
    return;
  }

  // 必須產 mipmap：Cubism renderer 取樣時假設有完整 mipmap chain，
  // 缺了會變成不完整紋理（整隻黑剪影，實測確認）。
  // mipmap 產不出來（同樣是虛擬機 GPU 的記憶體問題）就退回單層線性過濾 ——
  // MIN_FILTER 不吃 mipmap 時單層紋理就是完整的，縮小時差一點但至少有顏色。
  glGenerateMipmap(GL_TEXTURE_2D);
  if (glGetError() == GL_NO_ERROR) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  } else {
    qWarning() << "[live2d] glGenerateMipmap 失敗, index =" << index << "，退回單層線性過濾";
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  textures.push_back(texture);
  renderer->BindTexture(static_cast<csmUint32>(index), texture);
}

}  // namespace

ModelController::ModelController()
  : idParamAngleX_(CubismFramework::GetIdManager()->GetId(ParamAngleX)),
    idParamAngleY_(CubismFramework::GetIdManager()->GetId(ParamAngleY)),
    idParamAngleZ_(CubismFramework::GetIdManager()->GetId(ParamAngleZ)),
    idParamBodyAngleX_(CubismFramework::GetIdManager()->GetId(ParamBodyAngleX)),
    idParamEyeBallX_(CubismFramework::GetIdManager()->GetId(ParamEyeBallX)),
    idParamEyeBallY_(CubismFramework::GetIdManager()->GetId(ParamEyeBallY)) {}

ModelController::~ModelController() {
  for (auto* motion : retiredAiMotions_) ACubismMotion::Delete(motion);
  for (auto& [key, motion] : motions_) ACubismMotion::Delete(motion);
  for (auto& [name, expression] : expressions_) ACubismMotion::Delete(expression);
  if (!textures_.empty()) glDeleteTextures(static_cast<GLsizei>(textures_.size()), textures_.data());
}

bool ModelController::load(const std::filesystem::path& entryPath) {
  // 載入分段計時掛在既有的 L2M_PROFILE 慣例底下（FrameProfiler 只涵蓋每幀，
  // 載入路徑一直沒有計時）。實測 huohuo（8192² 貼圖 ×3）冷啟動：
  //   setupModel 29 ms → CreateRenderer +26 ms → setupTextures +1430 ms
  // CreateRenderer 原本要 2.3~3.0 秒 —— Cubism 5-r.5 會在這裡把 482 支 shader
  // 一次編譯連結完，其中 474 支是絕大多數模型根本用不到的混合模式。
  // 現在由 patches/cubism-framework-lazy-blend-shaders.patch 改成延遲產生，
  // 只剩必用的 8 支（shader manager 是 static singleton，同一個 process 裡
  // 第二次切模型仍是 0 ms）。下面那行順便印讀檔次數，用來分辨時間是花在
  // 磁碟還是 GL 編譯 —— 若哪天這個數字又跳回 1912，就是 patch 沒套上。
  static const bool profile = qEnvironmentVariableIsSet("L2M_PROFILE");
  QElapsedTimer stageClock;
  stageClock.start();

  loadError_.clear();
  loadHint_.clear();

  // entryPath 可以是資料夾裡的 model3.json，也可以是一整個 *.zip；
  // 兩者的差別到這一行為止，之後全走 ModelAssets。
  assets_ = openModelAssets(entryPath);
  if (!assets_) {
    loadError_ = "Could not open the model files.";
    loadHint_ = "Check that the model folder or zip still exists at " + entryPath.u8string() + ".";
    qWarning() << "[live2d] 開不了模型:" << QString::fromStdString(entryPath.u8string());
    return false;
  }

  // 讀取 + 執行期補全（VTube Studio 綁定、斷路徑修復、孤兒 pose3）
  std::string enrichedJson;
  try {
    enrichedJson = loadCubism4Settings(*assets_).json();
  } catch (const std::exception& err) {
    loadError_ = std::string("The model settings could not be parsed: ") + err.what();
    loadHint_ = "The model3.json may be malformed, or reference files that are not in the package.";
    qWarning() << "[live2d] 模型設定解析失敗:" << err.what();
    return false;
  }

  qDebug() << "[live2d] enriched json bytes =" << enrichedJson.size();
  setting_ = std::make_unique<CubismModelSettingJson>(reinterpret_cast<const csmByte*>(enrichedJson.data()), static_cast<csmSizeInt>(enrichedJson.size()));
  qDebug() << "[live2d] setting 建立完成, moc =" << (setting_->GetModelFileName() ? setting_->GetModelFileName() : "(null)");

  setupModel();
  if (!loaded_) {
    loadError_ = "The moc3 could not be loaded.";
    loadHint_ = "Check that FileReferences.Moc points at a moc3 this Cubism runtime can read.";
    return false;
  }
  if (profile) qInfo() << "[perf] setupModel @" << stageClock.elapsed() << "ms";

  // 5-r.5 起需給遮罩緩衝尺寸
  const CubismFileLoadStats ioBefore = CubismRuntime::fileLoadStats();
  CreateRenderer(2048, 2048, 1);
  qDebug() << "[live2d] renderer 建立完成";
  if (profile) {
    const CubismFileLoadStats ioAfter = CubismRuntime::fileLoadStats();
    qInfo() << "[perf] CreateRenderer @" << stageClock.elapsed() << "ms"
            << "(其中讀檔" << (ioAfter.calls - ioBefore.calls) << "次／" << (ioAfter.micros - ioBefore.micros) / 1000 << "ms／" << (ioAfter.bytes - ioBefore.bytes) / 1024 << "KB)";
  }
  const int decodedTextures = setupTextures();
  qDebug() << "[live2d] 貼圖載入完成:" << decodedTextures;
  // 一張都沒成功時**一定要當成載入失敗**：Cubism renderer 會把 texture id 為 0 的
  // drawable 整個跳過（CubismRenderer_OpenGLES2 的 `if (_textures[...] == 0) return;`），
  // 畫面上是一隻完全透明的模型。這裡照樣回 true 的話就再也沒有第二個地方會告訴使用者
  // 發生了什麼事 —— 實測 webp 貼圖的模型就是「模型載入完成」配一片空白，連錯誤對話框
  // 都不會出現。loadError_ 非空即代表「有貼圖而且出過事」，沒有貼圖的模型不受影響。
  if (decodedTextures == 0 && !loadError_.empty()) {
    qWarning() << "[live2d] 貼圖全數失敗，視為載入失敗";
    return false;
  }
  if (profile) qInfo() << "[perf] setupTextures @" << stageClock.elapsed() << "ms";
  captureBaseline();
  if (profile) qInfo() << "[perf] load 全部完成 @" << stageClock.elapsed() << "ms";
  return true;
}

std::optional<std::vector<char>> ModelController::readAsset(const std::string& rel) const {
  if (!assets_) return std::nullopt;
  const auto text = assets_->read(rel);
  if (!text) return std::nullopt;
  return std::vector<char>(text->begin(), text->end());
}

void ModelController::setupModel() {
  _updating = true;
  _initialized = false;

  // ── moc3 ──
  const std::string mocFile = setting_->GetModelFileName();
  if (mocFile.empty()) {
    qWarning() << "[live2d] model3.json 沒有指定 moc3";
    return;
  }
  const auto mocBytes = readAsset(mocFile);
  if (!mocBytes) {
    qWarning() << "[live2d] 讀不到 moc3:" << QString::fromStdString(mocFile);
    return;
  }
  qDebug() << "[live2d] moc3 bytes =" << mocBytes->size();
  LoadModel(reinterpret_cast<const csmByte*>(mocBytes->data()), static_cast<csmSizeInt>(mocBytes->size()), true);
  if (!_model) {
    qWarning() << "[live2d] moc3 載入失敗";
    return;
  }
  qDebug() << "[live2d] moc3 載入成功, 參數數 =" << _model->GetParameterCount();
  {
    // 診斷：混合模式使用情況（Cubism 5 的 offscreen 路徑是否會被觸發）
    std::map<int, int> colorBlendCounts;
    for (csmInt32 i = 0; i < _model->GetDrawableCount(); ++i) {
      colorBlendCounts[_model->GetDrawableBlendModeType(i).GetColorBlendType()]++;
    }
    QString blends;
    for (const auto& [type, count] : colorBlendCounts) blends += QStringLiteral("%1:%2 ").arg(type).arg(count);
    qDebug() << "[live2d] drawables =" << _model->GetDrawableCount() << "offscreens =" << _model->GetOffscreenCount() << "blendModeEnabled =" << _model->IsBlendModeEnabled()
             << "colorBlend(type:count) =" << blends;
  }

  // ── 表情 ──
  for (csmInt32 i = 0; i < setting_->GetExpressionCount(); ++i) {
    const std::string name = setting_->GetExpressionName(i);
    const std::string file = setting_->GetExpressionFileName(i);
    const auto bytes = readAsset(file);
    if (!bytes) continue;
    ACubismMotion* motion = LoadExpression(reinterpret_cast<const csmByte*>(bytes->data()), static_cast<csmSizeInt>(bytes->size()), name.c_str());
    if (!motion) continue;
    // 同名表情後蓋前（跟官方範例一致）
    auto existing = expressions_.find(name);
    if (existing != expressions_.end()) {
      ACubismMotion::Delete(existing->second);
      existing->second = motion;
    } else {
      expressions_[name] = motion;
    }
  }

  // ── physics ──
  // 順手算出環境風的遮罩：用的是同一份位元組，而 Framework 解析完 rig 之後
  // 就不再留 physics3.json（粒子數拿得到，輸出的參數 id 拿不到）。
  windMask_.clear();
  const std::string physicsFile = setting_->GetPhysicsFileName();
  if (!physicsFile.empty()) {
    const auto bytes = readAsset(physicsFile);
    if (bytes) {
      LoadPhysics(reinterpret_cast<const csmByte*>(bytes->data()), static_cast<csmSizeInt>(bytes->size()));
      windMask_ = windTargetMask(std::string(bytes->begin(), bytes->end()));
    }
  }

  // ── pose ──
  const std::string poseFile = setting_->GetPoseFileName();
  if (!poseFile.empty()) {
    const auto bytes = readAsset(poseFile);
    if (bytes) LoadPose(reinterpret_cast<const csmByte*>(bytes->data()), static_cast<csmSizeInt>(bytes->size()));
  }

  // ── 眨眼 ──
  if (setting_->GetEyeBlinkParameterCount() > 0) {
    _eyeBlink = CubismEyeBlink::Create(setting_.get());
  }

  // ── 呼吸 ──
  _breath = CubismBreath::Create();
  {
    csmVector<CubismBreath::BreathParameterData> breathParameters;
    breathParameters.PushBack(CubismBreath::BreathParameterData(idParamAngleX_, 0.0f, 15.0f, 6.5345f, 0.5f));
    breathParameters.PushBack(CubismBreath::BreathParameterData(idParamAngleY_, 0.0f, 8.0f, 3.5345f, 0.5f));
    breathParameters.PushBack(CubismBreath::BreathParameterData(idParamAngleZ_, 0.0f, 10.0f, 5.5345f, 0.5f));
    breathParameters.PushBack(CubismBreath::BreathParameterData(idParamBodyAngleX_, 0.0f, 4.0f, 15.5345f, 0.5f));
    breathParameters.PushBack(CubismBreath::BreathParameterData(CubismFramework::GetIdManager()->GetId(ParamBreath), 0.5f, 0.5f, 3.2345f, 0.5f));
    _breath->SetParameters(breathParameters);
  }

  // ── 眨眼／口型參數 id ──
  for (csmInt32 i = 0; i < setting_->GetEyeBlinkParameterCount(); ++i) eyeBlinkIds_.PushBack(setting_->GetEyeBlinkParameterId(i));
  for (csmInt32 i = 0; i < setting_->GetLipSyncParameterCount(); ++i) lipSyncIds_.PushBack(setting_->GetLipSyncParameterId(i));
  if (lipSyncIds_.GetSize() == 0) {
    // 模型沒宣告時退回慣例參數 ParamMouthOpenY
    lipSyncIds_.PushBack(CubismFramework::GetIdManager()->GetId(ParamMouthOpenY));
  }
  lipSyncIdNames_.clear();
  for (csmUint32 i = 0; i < lipSyncIds_.GetSize(); ++i) {
    lipSyncIdNames_.push_back(lipSyncIds_[i]->GetString().GetRawString());
  }

  // ── 畫布資訊 ──
  //
  // 畫布中心相對模型原點的位移（算式與實測數字見 core/canvas_center.h）。
  // Framework 只轉了 canvas 的尺寸與 PixelsPerUnit 出來，原點沒有對應的 getter，
  // 所以直接問 Core。**要排在 Layout 前面**：底下判斷 Layout 能不能用時要拿它，
  // 而 Framework 的位置算式少的正是這一項。
  double canvasWidthUnits = 0;
  double canvasHeightUnits = 0;
  {
    Live2D::Cubism::Core::csmVector2 sizeInPixels{};
    Live2D::Cubism::Core::csmVector2 originInPixels{};
    float pixelsPerUnit = 0.0f;
    Live2D::Cubism::Core::csmReadCanvasInfo(_model->GetModel(), &sizeInPixels, &originInPixels, &pixelsPerUnit);
    const CanvasOffset offset = canvasCenterOffset(sizeInPixels.X, sizeInPixels.Y, originInPixels.X, originInPixels.Y, pixelsPerUnit);
    canvasCenterX_ = static_cast<float>(offset.x);
    canvasCenterY_ = static_cast<float>(offset.y);
    if (pixelsPerUnit > 0.0f) {
      canvasWidthUnits = sizeInPixels.X / pixelsPerUnit;
      canvasHeightUnits = sizeInPixels.Y / pixelsPerUnit;
    }
  }

  // ── Layout ──
  csmMap<csmString, csmFloat32> layout;
  setting_->GetLayoutMap(layout);
  // 作者有沒有指定大小／位置，以及**那份 Layout 到底能不能信** ——
  // 三個判斷與踩過的坑都在 core/layout_fit.h。順序要跟 model3.json 一致，
  // 同一軸寫了兩個鍵時後面那個會蓋掉前面（可莉的 top 蓋掉 bottom）。
  std::vector<LayoutEntry> layoutEntries;
  for (auto ite = layout.Begin(); ite != layout.End(); ++ite) {
    layoutEntries.push_back({ite->First.GetRawString(), static_cast<double>(ite->Second)});
  }
  const LayoutPlan plan = planLayout(layoutEntries, canvasWidthUnits, canvasHeightUnits, canvasCenterX_, canvasCenterY_);
  layoutSpecifiesSize_ = plan.specifiesSize;
  layoutSpecifiesPosition_ = plan.specifiesPosition;
  layoutUsable_ = plan.usable;
  // 條件刻意看兩個旗標而不是「Layout 非空」：只寫了不認得的鍵時 usable 也會是 false，
  // 但那種 Layout 本來就不影響任何分支，行為一行都沒變，報出來只是噪音
  if ((plan.specifiesSize || plan.specifiesPosition) && !plan.usable) {
    qWarning() << "[live2d] Layout 套下去會讓畫布超出視野，改用自動置中（見 core/layout_fit.h）:" << QString::fromUtf8(setting_->GetModelFileName());
  }
  _modelMatrix->SetupFromLayout(layout);

  _model->SaveParameters();

  preloadMotions();

  // 參數索引是綁在模型上的，換模型之後上一隻的索引指到的是完全不同的參數
  carryPastPhysicsIndices_.clear();
  carriedThisFrame_.clear();

  _updating = false;
  _initialized = true;
  loaded_ = true;
  qDebug() << "[live2d] setupModel 完成";
}

void ModelController::preloadMotions() {
  // 待機動作的挑選要看「宣告了哪些群組與檔名」，跟預載成不成功無關，
  // 所以在同一趟走訪裡順手收集（見 startIdleMotion 與 core/idle_motion_pick.h）。
  // files 的索引必須對齊動作索引，所以無論後面有沒有 continue 都要先塞進去。
  std::vector<MotionGroupFiles> groups;

  for (csmInt32 g = 0; g < setting_->GetMotionGroupCount(); ++g) {
    const std::string group = setting_->GetMotionGroupName(g);
    groups.push_back(MotionGroupFiles{group, {}});
    for (csmInt32 i = 0; i < setting_->GetMotionCount(group.c_str()); ++i) {
      const std::string file = setting_->GetMotionFileName(group.c_str(), i);
      groups.back().files.push_back(file);
      const auto bytes = readAsset(file);
      if (!bytes) continue;

      // 野生檔的 Meta 常少報數量，CubismMotion::Parse 會照實際內容越界寫入
      // 把 heap 寫壞（見 core/motion_meta.h）—— 先重算修正；結構壞掉的直接略過
      const std::string rawJson(bytes->begin(), bytes->end());
      const MotionMetaFix metaFix = fixMotionMetaCounts(rawJson);
      if (metaFix.result == MotionMetaResult::Invalid) {
        qWarning() << "[live2d] motion3.json 結構損壞，略過:" << QString::fromStdString(file) << QString::fromStdString(metaFix.error);
        continue;
      }
      if (metaFix.result == MotionMetaResult::Fixed) {
        qWarning() << "[live2d] motion3.json 的 Meta 數量有誤，已於載入時修正:" << QString::fromStdString(file);
      }
      const std::string& motionJson = metaFix.result == MotionMetaResult::Fixed ? metaFix.json : rawJson;

      // shouldCheckMotionConsistency=true 是最後防線：檢查失敗回 NULL 而不是寫壞 heap
      CubismMotion* motion =
        static_cast<CubismMotion*>(LoadMotion(reinterpret_cast<const csmByte*>(motionJson.data()), static_cast<csmSizeInt>(motionJson.size()), nullptr, nullptr, nullptr, nullptr, nullptr, -1, true));
      if (!motion) continue;

      // 淡入淡出：model3.json 有指定就用它
      const csmFloat32 fadeIn = setting_->GetMotionFadeInTimeValue(group.c_str(), i);
      if (fadeIn >= 0.0f) motion->SetFadeInTime(fadeIn);
      const csmFloat32 fadeOut = setting_->GetMotionFadeOutTimeValue(group.c_str(), i);
      if (fadeOut >= 0.0f) motion->SetFadeOutTime(fadeOut);
      motion->SetEffectIds(eyeBlinkIds_, lipSyncIds_);

      const std::string key = motionKeyOf(group, i);
      auto existing = motions_.find(key);
      if (existing != motions_.end()) ACubismMotion::Delete(existing->second);
      motions_[key] = motion;
      // 順手記下這支動作會寫到哪些 id：起播前的 restoreBaseline 要靠它決定
      // 哪些參數留給新動作自己接手（見 core/motion_curve_ids.h）
      motionDrivenIds_[key] = motionDrivenIds(motionJson);
    }
  }

  idleSlot_ = pickIdleMotion(groups);
  if (!idleSlot_) qWarning() << "[live2d] 這個模型找不到待機動作，動作播完之後會停在最後一幀";
}

int ModelController::setupTextures() {
  auto* renderer = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
  const size_t alreadyBound = textures_.size();

  // 影像格式外掛的登錄表在 GUI 執行緒先初始化一次。PNG handler 是編進 QtGui 的
  // 內建 handler（不是外掛）所以其實不需要，但貼圖是 JPEG 的模型會走 QPluginLoader，
  // 先在單執行緒暖一次比較安全。
  static const bool formatsWarmed = !QImageReader::supportedImageFormats().isEmpty();
  Q_UNUSED(formatsWarmed);

  // 診斷開關：L2M_STRAIGHT_ALPHA 時走官方 native 範例的非預乘路徑
  static const bool straightAlpha = qEnvironmentVariableIsSet("L2M_STRAIGHT_ALPHA");
  const QImage::Format target = straightAlpha ? QImage::Format_RGBA8888 : QImage::Format_RGBA8888_Premultiplied;

  // 1) 先只讀檔頭拿尺寸（微秒級），用來推導併發數。
  //    zip 模型走 readPrefix，只解出前 kHeaderProbeBytes 個位元組 ——
  //    整包 inflate 一張 8192² 的 PNG 只為了問寬高是幾十毫秒。
  std::vector<TextureSource> sources;
  quint64 largest = 0;
  const csmInt32 count = setting_->GetTextureCount();
  for (csmInt32 i = 0; i < count; ++i) {
    const std::string file = setting_->GetTextureFileName(i);
    if (file.empty()) continue;
    TextureSource src;
    src.index = i;
    src.rel = file;

    QSize size;
    if (const auto head = assets_->readPrefix(file, kHeaderProbeBytes)) {
      // PNG 自己讀 IHDR —— 用的正是解碼器判斷「要不要走快路徑」的同一支函式，
      // 這裡估出來的尺寸才跟等一下真的解出來的那張一致。其餘格式才勞煩
      // QImageReader（它要先建 QBuffer、再走一輪外掛查找）。
      if (const auto png = readPngHeader(head->data(), head->size())) {
        size = QSize(static_cast<int>(png->width), static_cast<int>(png->height));
      } else {
        QByteArray probe(head->data(), static_cast<qsizetype>(head->size()));
        QBuffer buffer(&probe);
        buffer.open(QIODevice::ReadOnly);
        size = QImageReader(&buffer).size();
      }
    }
    // 讀不到檔頭不在這裡放棄：尺寸只用來估併發數，真正的失敗留給解碼階段報
    src.bytes = quint64(std::max(1, size.width())) * quint64(std::max(1, size.height())) * 4;
    largest = std::max(largest, src.bytes);
    sources.push_back(std::move(src));
  }
  if (sources.empty()) {
    renderer->IsPremultipliedAlpha(!straightAlpha);
    renderer->UseHighPrecisionMask(true);
    return 0;
  }

  // Qt 6 的 QImageReader 有一條**預設 256 MB** 的配置上限（擋 PNG 解壓縮炸彈）。
  // 8192×16384 的貼圖圖集解碼後正好是 512 MiB，會被它整張拒收 —— 症狀是
  //「檔案好好的，卻說貼圖載不起來」，Qt 只在 qt.gui.imageio 印一行
  // "Rejecting image as it exceeds the current allocation limit"。實測 LiveroiD 系列
  // 整組中招；huohuo 的 8192² 是 256 MiB **剛好卡在上限之內**，所以一直相安無事，
  // 差一格而已。
  // 本專案自己已經用檔頭探到的尺寸算過預算與併發（就是下面兩行），Qt 這一層是
  // 重複的守衛，卻會把合法的大圖集擋掉，所以依這一批真正要解的最大張數把它撐上去。
  // **刻意不完全解除**：檔頭是可以說謊的，一個宣稱 60000×60000 的壞檔案會當場
  // 把記憶體吃光，所以夾在 kMaxDecodeAllocationMb 以內。
  // 只升不降，而且是行程層級的全域設定：降回去會讓正在解的另一張當場被拒。
  const int neededMb = static_cast<int>(std::min<quint64>(largest / (1024 * 1024) + 1, kMaxDecodeAllocationMb));
  if (neededMb > QImageReader::allocationLimit()) QImageReader::setAllocationLimit(neededMb);

  const int byBudget = int(std::max<quint64>(1, kDecodeBudgetBytes / std::max<quint64>(largest, 1)));
  const int concurrency = std::clamp(std::min(byBudget, QThread::idealThreadCount()), 1, kMaxDecodeThreads);
  qDebug() << "[live2d] 貼圖" << sources.size() << "張, 最大單張解碼後 =" << largest / (1024 * 1024) << "MiB, 解碼併發 =" << concurrency;

  QThreadPool pool;  // 解構會 waitForDone()，不會有工作跑到函式外面
  pool.setMaxThreadCount(concurrency);

  // 真的沒產生 GL texture 的那幾張。歸咎範圍必須是**這一份**而不是「全部宣告過的
  // 貼圖」，理由見 core/texture_format.h 的檔頭。
  std::vector<std::string> failedFiles;

  // 2) 以 concurrency 為一批：整批平行解碼 → 依序上傳 → 立刻釋放。
  //    尖峰 = concurrency × 單張位元組，不會超過預算。
  for (size_t begin = 0; begin < sources.size(); begin += size_t(concurrency)) {
    const size_t end = std::min(sources.size(), begin + size_t(concurrency));
    const int batch = int(end - begin);
    std::vector<QImage> images(batch);
    std::atomic<int> done{0};

    // 讀 bytes 這一步刻意留在 GUI 執行緒：zip 版底下的 miniz reader 有共用緩衝，
    // 兩條 worker 同時解同一個壓縮檔會踩到彼此（見 core/zip_archive.h）。
    // 真正吃時間的 PNG 解碼仍然是平行的，所以原本的加速沒有損失。
    for (size_t i = begin; i < end; ++i) {
      const auto data = assets_->read(sources[i].rel);
      QByteArray bytes;
      if (data) {
        bytes = QByteArray(data->data(), static_cast<qsizetype>(data->size()));
      } else {
        qWarning() << "[live2d] 讀不到貼圖:" << QString::fromStdString(sources[i].rel);
      }
      pool.start(new DecodeTask(std::move(bytes), target, &images[i - begin], &done));
    }

    // 3) 等這一批解完。收益純粹是總時間：4 張 8192² 貼圖從約 2.5 秒降到 1.2 秒。
    //    刻意用 msleep 而不是 processEvents 抽事件 —— 後者會讓 MCP 排隊中的
    //    switch_model 重入還沒返回的 loadPendingModel()。啟動畫面不受這裡影響，
    //    它跑在自己的行程（見 app/splash_process.h）。
    while (done.load(std::memory_order_acquire) < batch) QThread::msleep(kDecodePollMs);

    for (size_t i = begin; i < end; ++i) {
      const size_t bound = textures_.size();
      uploadTexture(renderer, sources[i].index, images[i - begin], textures_);
      // uploadTexture 只在真的綁好時 push_back，所以「長度沒變」就是這一張沒成功
      if (textures_.size() == bound) failedFiles.push_back(sources[i].rel);
      images[i - begin] = QImage();  // 立刻歸還這一張的記憶體，別等整批結束
    }
  }

  // 失敗時把原因算成一句話 —— 分成「這個建置解不動這個格式」與「檔案本身壞了」
  // 兩種，修法完全不同（規則與踩過的坑見 core/texture_format.h）。
  const int decoded = static_cast<int>(textures_.size() - alreadyBound);
  if (!failedFiles.empty()) {
    // 支援清單問的是**這個建置**（外掛是執行期載入的），不是編譯期的常數
    std::vector<std::string> supported;
    for (const QByteArray& format : QImageReader::supportedImageFormats()) supported.push_back(format.toStdString());
    const TextureFormatIssue issue = inspectTextureFailure(failedFiles, supported, static_cast<int>(sources.size()));
    loadError_ = issue.error;
    loadHint_ = issue.hint;
    // noquote：句子裡本來就有引號（格式名），再讓 QDebug 包一層會變成 \" 的跳脫地獄
    qWarning().noquote() << "[live2d]" << QString::fromStdString(issue.error);
    qWarning().noquote() << "[live2d]" << QString::fromStdString(issue.hint);
  }

  renderer->IsPremultipliedAlpha(!straightAlpha);
  // 高精度遮罩：預設模式的遮罩 buffer（256×256、NEAREST 取樣）放大到視窗
  // 會讓被遮罩部件（尾巴、滾邊）的邊緣出現塊狀鋸齒；高精度模式改為
  // 每筆繪製即時以全視窗渲染遮罩
  renderer->UseHighPrecisionMask(true);
  return decoded;
}

void ModelController::update(float deltaSeconds) {
  if (!loaded_ || !_model) return;

  _dragManager->Update(deltaSeconds);
  const float dragX = _dragManager->GetX();
  const float dragY = _dragManager->GetY();

  // 1. 還原上一幀存的基準參數，讓 motion 從乾淨狀態出發
  _model->LoadParameters();
  csmBool motionUpdated = false;
  if (_motionManager->IsFinished()) {
    // 換掉的合成動作可能還被淡出中的佇列項目指著，等這裡確定空了才真的釋放
    if (!retiredAiMotions_.empty()) {
      for (auto* motion : retiredAiMotions_) ACubismMotion::Delete(motion);
      retiredAiMotions_.clear();
      playingAiSlot_.reset();
      loopingAiMotion_ = false;
    }
    // 剛結束的動作優先度 > 待機時才還原 baseline（邊緣觸發，見標頭註解）。
    // Cubism 的淡出是凍結在上一幀值而非回預設；待機動作只拉回自己曲線裡的
    // 參數，動作獨有的道具／特效參數會永遠殘留。硬切與現有三處
    // restoreBaseline 一致：道具瞬間消失是期望行為，主姿勢會被待機淡入蓋掉。
    const bool needsRestore = lastStartedMotionPriority_ > PriorityIdle;
    lastStartedMotionPriority_ = PriorityNone;  // 消耗，避免接不回待機時每幀 restore

    // 沒有動作在播就自動接待機動作。**接不出待機時連 restore 都不做** ——
    // 還原的目標是作者的編輯狀態（多出來的手腳），那比停在最後一幀更糟。
    startIdleMotion(PriorityIdle, needsRestore);
  }
  motionUpdated = _motionManager->UpdateMotion(_model, deltaSeconds);
  _model->SaveParameters();

  // 1.5 記下要越過物理的參數值（見 playSynthesizedMotion 的 carryPastPhysics）。
  // 一定要在這裡取：這是動作曲線（含淡入淡出權重）唯一乾淨的時刻，
  // 第 5 步的視線加成與第 7 步的物理都還沒動過它。
  // motionUpdated 為 false 時刻意留空 —— 動作播完之後這裡讀到的會是上一幀的
  // 物理輸出，寫回去等於把身體凍在最後那個姿勢。
  carriedThisFrame_.clear();
  if (motionUpdated) {
    for (const int index : carryPastPhysicsIndices_) carriedThisFrame_.emplace_back(index, _model->GetParameterValue(index));
  }

  // 2. 早寫掛點：寫在 physics 之前，頭髮衣服才會對覆寫值有反應
  if (earlyParameterHook) earlyParameterHook(_model);

  // 3. 眨眼（有動作在動眼睛時讓給動作）
  if (!motionUpdated && _eyeBlink) _eyeBlink->UpdateParameters(_model, deltaSeconds);

  // 4. 表情
  if (_expressionManager) _expressionManager->UpdateMotion(_model, deltaSeconds);

  // 5. 視線／拖曳加成（疊加，不覆寫動作）
  _model->AddParameterValue(idParamAngleX_, dragX * 30);
  _model->AddParameterValue(idParamAngleY_, dragY * 30);
  _model->AddParameterValue(idParamAngleZ_, dragX * dragY * -30);
  _model->AddParameterValue(idParamBodyAngleX_, dragX * 10);
  _model->AddParameterValue(idParamEyeBallX_, dragX);
  _model->AddParameterValue(idParamEyeBallY_, dragY);

  // 6. 呼吸
  if (_breath) _breath->UpdateParameters(_model, deltaSeconds);

  // 6.5 拖曳搖晃（角度偏移＋風力）與環境風。
  // 角度是物理前疊加，頭髮衣服才會對它反應。
  // 已知限制：MCP set_parameters 以 Set 釘住 ParamAngleX 時，第 9 步的晚寫
  // 會把角度覆寫回釘值，變成「頭不動、頭髮在甩」—— 屬可接受的邊角。
  //
  // 風力的寫入點刻意放在兩個 provider **之外**：Wind 是全域選項（所有 SubRig
  // 一起吃），兩個來源各寫一次會互相蓋掉，所以先相加再一次覆寫成「這一刻的
  // 總風力」，兩個來源都沒有時就是 0。保險絲（kWindMax）放在相加之後才有意義
  // —— 單一來源各自觸不到上限，相加之後才可能真的碰到。
  double windX = 0;
  double windY = 0;
  if (dragSwingProvider) {
    const DragSwing::Output swing = dragSwingProvider();
    if (swing.active) {
      addClamped(idParamAngleX_, swing.angleXDeg);
      addClamped(idParamAngleY_, swing.angleYDeg);
      addClamped(idParamAngleZ_, swing.angleZDeg);
      addClamped(idParamBodyAngleX_, swing.bodyXDeg);
      windX += swing.windX;
      windY += swing.windY;
    }
  }
  if (ambientWindProvider) {
    const WindVector ambient = ambientWindProvider();
    windX += ambient.x;
    windY += ambient.y;
  }
  if (_physics) {
    // 每幀覆寫；來源都靜止時歸零，放手或關開關後風力立即消失。
    // Gravity 保留原值（預設 (0,-1)）。
    CubismPhysics::Options options = _physics->GetOptions();
    options.Wind.X = static_cast<csmFloat32>(std::clamp(windX, -kWindMax, kWindMax));
    options.Wind.Y = static_cast<csmFloat32>(std::clamp(windY, -kWindMax, kWindMax));
    // 風只吹「鏈」（頭髮、衣襬），不吹把頭身角度轉成身體傾斜的跟隨 rig
    //（那種輸出倍率動輒數十倍，持續風力會讓整個身體搖得比頭髮還大）。
    // 判別在載入時就算好了（core/wind_targets.h，兩條規則與實測數字在檔頭），
    // 這裡只是把遮罩指過去。欄位來自本專案的 Framework patch
    //（patches/cubism-framework-wind-mask.patch）；空的話交 NULL，
    // Framework 退回「全部吃風」的原行為。
    options.WindMask = windMask_.empty() ? nullptr : windMask_.data();
    options.WindMaskCount = static_cast<csmInt32>(windMask_.size());
    _physics->SetOptions(options);
  }

  // 7. 物理
  if (_physics) _physics->Evaluate(_model, deltaSeconds);

  // 8. 口型（音量驅動；說話中 overlay 對 lipSyncIds 的寫入會跳過）
  if (mouthOpenProvider) {
    const float value = mouthOpenProvider();
    for (csmUint32 i = 0; i < lipSyncIds_.GetSize(); ++i) {
      _model->AddParameterValue(lipSyncIds_[i], value, 0.8f);
    }
  }

  // 9. 晚寫掛點：不被眨眼／呼吸／視線蓋掉
  //
  // 動作的「越過物理」值排在覆寫層之前，讓 MCP 的 set_parameters 仍然壓得過內建動作
  //（覆寫層是使用者／AI 的明示指令，優先度最高）。
  for (const auto& [index, value] : carriedThisFrame_) _model->SetParameterValue(index, value);
  if (lateParameterHook) lateParameterHook(_model);

  // 10. pose（手臂互斥）
  if (_pose) _pose->UpdateParameters(_model, deltaSeconds);

  _model->Update();
}

void ModelController::addClamped(const Csm::CubismId* id, double delta) {
  const csmInt32 index = _model->GetParameterIndex(id);
  if (index < 0) return;
  const double current = _model->GetParameterValue(index);
  const double lo = _model->GetParameterMinimumValue(index);
  const double hi = _model->GetParameterMaximumValue(index);
  const double clamped = std::clamp(delta, lo - current, hi - current);
  _model->AddParameterValue(index, static_cast<csmFloat32>(clamped));
}

void ModelController::setRenderTargetSize(int width, int height) {
  if (!loaded_ || width <= 0 || height <= 0) return;
  GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->SetRenderTargetSize(static_cast<csmUint32>(width), static_cast<csmUint32>(height));
}

void ModelController::draw(int viewportWidth, int viewportHeight, float alpha) {
  if (!loaded_ || !_model || viewportWidth <= 0 || viewportHeight <= 0) return;

  // 官方範例的 GetCanvasWidth() > 1.0f 只對 1~2 單位的常規 canvas 成立：
  // PixelsPerUnit 偏小的模型（haru：33.3×62.5 單位）寬度必然 >1，直向模型
  // 被誤判成橫幅、強制以寬度貼齊，高度 2.5 NDC 直接上下截斷。改成比長寬比
  // 挑「受限維度」去 fit；作者用 Layout 指定過大小的則完全不強制，
  // 投影固定以寬度為參考軸，構圖才不會被每幀蓋掉。
  CubismMatrix44 projection;
  const float canvasAspect = _model->GetCanvasHeight() > 0.0f ? _model->GetCanvasWidth() / _model->GetCanvasHeight() : 1.0f;
  const float viewAspect = static_cast<float>(viewportWidth) / viewportHeight;
  if (layoutSpecifiesSize_ && layoutUsable_) {
    // 作者的 Layout 是對「標準視野 [-1,1]²」寫的，所以要保證那一整塊看得見：
    // 橫向視窗以高度為準（左右留白）、直向以寬度為準。原本一律 Scale(1, viewAspect)
    // 等於假設視窗永遠是直的 —— 桌寵那個 400×600 的舞台剛好成立，但檢視器可以拉成
    // 任何形狀，橫的視窗會把看得見的高度壓到 2 以下，作者寫 height 2.x 就整個爆出去。
    if (viewAspect > 1.0f) {
      projection.Scale(1.0f / viewAspect, 1.0f);
    } else {
      projection.Scale(1.0f, viewAspect);
    }
  } else if (canvasAspect > viewAspect) {
    // canvas 比視窗寬：以寬度貼齊
    GetModelMatrix()->SetWidth(2.0f);
    projection.Scale(1.0f, viewAspect);
  } else {
    // canvas 比視窗高：以高度貼齊
    GetModelMatrix()->SetHeight(2.0f);
    projection.Scale(1.0f / viewAspect, 1.0f);
  }
  // 把畫布中心對到視窗中心。上面的 fit 只算了「縮到多大」，沒有算「擺在哪」——
  // 原點不在畫布正中央的模型就會整個偏出去（實測症狀與數字見 core/canvas_center.h）。
  // **每幀都要重設**：位移是「模型單位 × fit 的縮放」，而縮放隨視窗大小變。
  // 作者在 Layout 指定過位置就一步都不碰 —— Translate() 是絕對指派，補下去等於把
  // SetupFromLayout 擺好的位置蓋掉。原點本來就在中央時兩個位移都是 0，
  // 等於寫回 (0, 0)，跟改動前一模一樣。
  //
  // 位移放在 _modelMatrix 而不是 projection 是刻意的：visibleBoundsView() 用
  // TransformX/Y、IsHit() 用 InvertTransformX/Y，兩支都吃這個矩陣的平移；
  // 而 screenToView() 只除以 projection 的縮放，projection 保持純縮放才對得起來。
  // layoutUsable_ 為 false 時整份 Layout 都不採用：上面的 fit 已經用 SetWidth／
  // SetHeight 蓋掉它的縮放（那兩支寫的是絕對值），這裡再蓋掉它的平移。
  if (!layoutSpecifiesPosition_ || !layoutUsable_) {
    const float fitScale = GetModelMatrix()->GetScaleX();
    GetModelMatrix()->Translate(-canvasCenterX_ * fitScale, -canvasCenterY_ * fitScale);
  }

  projectionScaleX_ = projection.GetArray()[0];
  projectionScaleY_ = projection.GetArray()[5];

  projection.MultiplyByMatrix(_modelMatrix);

  auto* renderer = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
  renderer->SetMvpMatrix(&projection);
  // 整體淡入淡出：GetModelColorWithOpacity 會把這個 A 乘進每個 drawable 的
  // 不透明度（premultiplied alpha 的模型也一併處理），所以一行就夠。
  // 這是 renderer 的持續狀態，每次 draw 都要重設 —— 遮罩那邊吃預設的 1.0f。
  renderer->SetModelColor(1.0f, 1.0f, 1.0f, alpha);
  renderer->DrawModel();
}

void ModelController::drawSilhouette(int viewportWidth, int viewportHeight) {
  if (!loaded_ || !_model) return;
  auto* renderer = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
  // 存回原值而不是寫死 true：這個旗標是 renderer 的持續狀態，
  // 哪天 setupTextures 改成不開高精度，這裡也不該偷偷把它打開。
  const csmBool previous = renderer->IsUsingHighPrecisionMask();
  renderer->UseHighPrecisionMask(false);
  draw(viewportWidth, viewportHeight);
  renderer->UseHighPrecisionMask(previous);
}

bool ModelController::startMotion(const std::string& group, int index, int priority, bool loop) {
  if (!loaded_) return false;
  const int count = setting_->GetMotionCount(group.c_str());
  if (count <= 0) return false;
  if (index < 0) index = std::rand() % count;
  if (index >= count) return false;

  if (priority == PriorityForce) {
    _motionManager->SetReservePriority(priority);
  } else if (!_motionManager->ReserveMotion(priority)) {
    return false;
  }

  ACubismMotion* motion = motionAt(group, index);
  if (!motion) {
    _motionManager->SetReservePriority(PriorityNone);
    return false;
  }

  // 播新動作前還原 baseline，清掉上一段動作可能留下的參數殘值。
  // 這支動作自己會驅動的參數留著不動，交給它的淡入接手（見 restoreBaseline 的註解）
  if (priority > PriorityIdle) restoreBaseline(drivenIdsFor(group, index));

  // motions_ 快取的是同一個 ACubismMotion 實例，loop 是寫在物件上的狀態，
  // 所以每次起播都要明寫（不是只在 loop 為 true 時設），否則關掉「自動重播」
  // 之後再播同一段還是會繼續循環。motion3.json 裡的 Loop 欄位不會自動帶進來，
  // 這裡也就是唯一的來源（同 playSynthesizedMotion 的 SetLoop）。
  motion->SetLoop(loop);

  const bool started = _motionManager->StartMotionPriority(motion, false, priority) != InvalidMotionQueueEntryHandleValue;
  // 只在真的起播時記錄：Reserve 被拒或找不到動作時，在播動作的記錄不能被蓋掉
  if (started) {
    lastStartedMotionPriority_ = priority;
    // 「越過物理」的清單是上一個合成動作專屬的，換動作就收掉。
    // 同樣只在真的起播時清 —— 被 Reserve 拒掉時還在播的是舊動作，清了它的身體會當場停住
    carryPastPhysicsIndices_.clear();
  }
  return started;
}

Csm::ACubismMotion* ModelController::motionAt(const std::string& group, int index) const {
  const auto it = motions_.find(motionKeyOf(group, index));
  return it != motions_.end() ? it->second : nullptr;
}

const MotionDrivenIds* ModelController::drivenIdsFor(const std::string& group, int index) const {
  const auto it = motionDrivenIds_.find(motionKeyOf(group, index));
  return it != motionDrivenIds_.end() ? &it->second : nullptr;
}

bool ModelController::startIdleMotion(int priority, bool restoreFirst) {
  if (!loaded_ || !idleSlot_) return false;
  const int count = setting_->GetMotionCount(idleSlot_->group.c_str());
  if (count <= 0) return false;
  // index < 0 ＝群組內隨機（群組名本身就叫 Idle 時，裡面每一支都是待機動畫）
  const int index = idleSlot_->index >= 0 ? idleSlot_->index : std::rand() % count;
  if (index >= count) return false;
  // 還原之前先確認這一支真的預載得起來。idleSlot_ 是從 setting_ 的宣告清單挑的，
  // 而 preloadMotions() 會跳過讀不到／Meta 壞掉／LoadMotion 回 NULL 的動作 ——
  // 兩份清單不保證一致。少了這一步，待機動畫壞掉的模型會「先還原成編輯狀態、
  // 再發現動作根本不存在」，畫面上就是每幀閃一下多出來的手腳。
  if (!motionAt(idleSlot_->group, index)) return false;
  // priority > PriorityIdle 時 startMotion() 會用同一份保留清單自己還原一次，這裡就不必重來
  if (restoreFirst && priority <= PriorityIdle) restoreBaseline(drivenIdsFor(idleSlot_->group, index));
  return startMotion(idleSlot_->group, index, priority);
}

bool ModelController::playSynthesizedMotion(const Motion3& motion, bool loop, std::optional<double> fadeInMs, std::optional<double> fadeOutMs, const std::vector<std::string>& carryPastPhysics) {
  if (!loaded_ || !_model) return false;

  const std::string json = toMotion3Json(motion);
  // 自家 buildMotion3 的輸出理應一致，但這是外部參數驅動的路徑，
  // 一樣開 shouldCheckMotionConsistency：萬一數量對不上要的是拒載不是寫壞 heap
  CubismMotion* compiled =
    static_cast<CubismMotion*>(LoadMotion(reinterpret_cast<const csmByte*>(json.data()), static_cast<csmSizeInt>(json.size()), nullptr, nullptr, nullptr, nullptr, nullptr, -1, true));
  if (!compiled) {
    qWarning() << "[live2d] 合成動作載入失敗";
    return false;
  }

  if (fadeInMs) compiled->SetFadeInTime(static_cast<csmFloat32>(*fadeInMs / 1000.0));
  if (fadeOutMs) compiled->SetFadeOutTime(static_cast<csmFloat32>(*fadeOutMs / 1000.0));
  compiled->SetEffectIds(eyeBlinkIds_, lipSyncIds_);
  // 實測確認：CubismMotionJson::IsMotionLoop() 只填 _motionData->Loop，
  // ACubismMotion::_isLoop 不會從 JSON 帶進來 —— 不明確呼叫 SetLoop，
  // Meta.Loop 寫了也不會循環。
  compiled->SetLoop(loop);

  // 動作管理對「同群組同索引已經在播」直接拒絕，所以兩個插槽輪流用
  const int slot = nextAiMotionSlot(playingAiSlot_);
  const std::string key = motionKeyOf(kAiMotionGroup, slot);
  const auto existing = motions_.find(key);
  if (existing != motions_.end()) {
    // 舊的可能還在淡出佇列裡，先退役不要馬上 delete
    retiredAiMotions_.push_back(existing->second);
    motions_.erase(existing);
  }
  motions_[key] = compiled;

  // 合成動作的曲線 id 就在手上，不必回頭解析 JSON（Motion3 的 Target 一律是 Parameter）
  MotionDrivenIds driven;
  for (const auto& curve : motion.curves) driven.parameterIds.push_back(curve.id);
  restoreBaseline(&driven);
  _motionManager->SetReservePriority(PriorityForce);
  const bool started = _motionManager->StartMotionPriority(compiled, false, PriorityForce) != InvalidMotionQueueEntryHandleValue;
  if (started) {
    playingAiSlot_ = slot;
    loopingAiMotion_ = loop;
    lastStartedMotionPriority_ = PriorityForce;
    // 索引在這裡就算好，update() 每幀只做 vector 走訪（GetParameterIndex 會查字串表）
    carryPastPhysicsIndices_.clear();
    for (const auto& id : carryPastPhysics) {
      const csmInt32 index = _model->GetParameterIndex(CubismFramework::GetIdManager()->GetId(id.c_str()));
      if (index >= 0) carryPastPhysicsIndices_.push_back(index);
    }
  }
  return started;
}

bool ModelController::stopLoopingAiMotion() {
  if (!loopingAiMotion_ || !playingAiSlot_.has_value()) return false;
  _motionManager->StopAllMotions();
  loopingAiMotion_ = false;
  playingAiSlot_.reset();
  returnToIdle();
  return true;
}

void ModelController::returnToIdle() {
  if (!loaded_ || !_model) return;

  // 這裡是「同步清掉佇列之後馬上接回待機」，跟 update() 那條（動作自己播完）
  // 差在 CubismMotionManager 的 _currentPriority 還沒歸零 ——
  // StopAllMotions() 只清佇列，那個值只有 UpdateMotion() 在 IsFinished 時才會清。
  // 合成動作是以 PriorityForce 起播的，所以不先清掉的話，接下來
  // ReserveMotion(PriorityIdle) 會被 `priority <= _currentPriority` 當場擋掉，
  // 待機一定起播失敗（症狀：復原之後硬還原成模型的編輯狀態，也就是多出來的手腳，
  // 要等兩幀後才自己接回來，而那時淡入就是從編輯狀態淡進來的）。
  // 用 dt=0 空跑一次 UpdateMotion 讓 Framework 自己清 —— 佇列已經空了，
  // 它不會寫到任何參數，效果就等同「下一幀才發現播完了」。
  _motionManager->UpdateMotion(_model, 0.0f);

  // 待機動作是以 PriorityIdle 起播的，那條路徑不會 restoreBaseline，
  // 這裡不清的話合成動作的殘值會一直掛在畫面上。
  // 先歸零：startIdleMotion 起播成功會把它改成 PriorityIdle，下一幀的 IsFinished
  // 分支就不會再 restore 一次；接不出待機時才輪到後面那個硬還原。
  lastStartedMotionPriority_ = PriorityNone;
  if (!startIdleMotion(PriorityIdle, /*restoreFirst=*/true)) restoreBaseline();
}

bool ModelController::hasParameterTable() const { return loaded_ && _model && _model->GetParameterCount() > 0; }

std::vector<ParameterSnapshot> ModelController::parameterSnapshots() const {
  std::vector<ParameterSnapshot> out;
  if (!loaded_ || !_model) return out;

  const csmInt32 count = _model->GetParameterCount();
  out.reserve(static_cast<size_t>(count));
  for (csmInt32 i = 0; i < count; ++i) {
    ParameterSnapshot snapshot;
    snapshot.id = _model->GetParameterId(i)->GetString().GetRawString();
    snapshot.value = _model->GetParameterValue(i);
    snapshot.min = _model->GetParameterMinimumValue(i);
    snapshot.max = _model->GetParameterMaximumValue(i);
    snapshot.defaultValue = _model->GetParameterDefaultValue(i);
    out.push_back(std::move(snapshot));
  }
  return out;
}

std::vector<std::string> ModelController::motionGroups() const {
  std::vector<std::string> groups;
  if (!setting_) return groups;
  for (csmInt32 i = 0; i < setting_->GetMotionGroupCount(); ++i) groups.push_back(setting_->GetMotionGroupName(i));
  return groups;
}

int ModelController::motionCount(const std::string& group) const { return setting_ ? setting_->GetMotionCount(group.c_str()) : 0; }

bool ModelController::setExpression(const std::string& name) {
  const auto it = expressions_.find(name);
  if (it == expressions_.end()) return false;
  _expressionManager->StartMotion(it->second, false);
  currentExpression_ = name;
  return true;
}

void ModelController::clearExpression() {
  if (!currentExpression_) return;
  currentExpression_ = std::nullopt;
  // 停掉表情管理器裡的所有表情（StopAllMotions 是硬切不是淡出；表情寫入在
  // SaveParameters 之後，佇列一空下一幀就不再寫，值自然回到動作管線的輸出）
  _expressionManager->StopAllMotions();
}

std::vector<std::string> ModelController::expressionNames() const {
  std::vector<std::string> names;
  for (const auto& [name, motion] : expressions_) names.push_back(name);
  return names;
}

std::vector<std::string> ModelController::hitTest(float viewX, float viewY) {
  std::vector<std::string> hits;
  if (!loaded_ || _opacity < 1) return hits;
  for (csmInt32 i = 0; i < setting_->GetHitAreasCount(); ++i) {
    if (IsHit(setting_->GetHitAreaId(i), viewX, viewY)) {
      hits.push_back(setting_->GetHitAreaName(i));
    }
  }
  return hits;
}

std::optional<ModelBox> ModelController::visibleBoundsView() const {
  if (!loaded_ || !_model) return std::nullopt;

  bool any = false;
  float left = 0;
  float right = 0;
  float bottom = 0;
  float top = 0;
  for (csmInt32 i = 0; i < _model->GetDrawableCount(); ++i) {
    // 這一幀畫不出來的不算：換裝／特效那些備用貼圖常常是整片畫布大小，
    // 算進去外接框會被撐滿，頭部比例當場失準（見標頭第 2 點）
    if (!_model->GetDrawableDynamicFlagIsVisible(i)) continue;
    if (_model->GetDrawableOpacity(i) <= 0.0f) continue;

    const csmInt32 count = _model->GetDrawableVertexCount(i);
    const auto* vertices = _model->GetDrawableVertexPositions(i);
    if (count <= 0 || !vertices) continue;

    for (csmInt32 v = 0; v < count; ++v) {
      const float x = vertices[v].X;
      const float y = vertices[v].Y;
      if (!any) {
        left = right = x;
        bottom = top = y;
        any = true;
        continue;
      }
      left = std::min(left, x);
      right = std::max(right, x);
      bottom = std::min(bottom, y);
      top = std::max(top, y);
    }
  }
  if (!any) return std::nullopt;

  // 模型空間 → view：IsHit() 是反向做同一件事（InvertTransformX/Y）。
  // 矩陣只有等比縮放與平移，所以轉兩個角就夠。
  ModelBox box;
  box.left = _modelMatrix->TransformX(left);
  box.right = _modelMatrix->TransformX(right);
  box.bottom = _modelMatrix->TransformY(bottom);
  box.top = _modelMatrix->TransformY(top);
  return box;
}

BodyPart ModelController::tapBodyPart(const std::vector<std::string>& areas, double px, double py, int viewportWidth, int viewportHeight) const {
  const BodyPart declared = bodyPartFor(areas);
  if (declared != BodyPart::Unknown) return declared;

  if (viewportWidth <= 0 || viewportHeight <= 0) return BodyPart::Unknown;
  const auto box = visibleBoundsView();
  if (!box) return BodyPart::Unknown;

  float viewX = 0;
  float viewY = 0;
  screenToView(px, py, viewportWidth, viewportHeight, &viewX, &viewY);
  return regionAt(*box, viewX, viewY);
}

void ModelController::screenToView(double px, double py, int viewportWidth, int viewportHeight, float* outX, float* outY) const {
  // 視窗像素 → NDC(-1..1) → 除以投影縮放 = 模型的 view 座標
  const float ndcX = static_cast<float>(px / viewportWidth) * 2.0f - 1.0f;
  const float ndcY = -(static_cast<float>(py / viewportHeight) * 2.0f - 1.0f);
  *outX = projectionScaleX_ != 0 ? ndcX / projectionScaleX_ : ndcX;
  *outY = projectionScaleY_ != 0 ? ndcY / projectionScaleY_ : ndcY;
}

void ModelController::captureBaseline() {
  if (!_model) return;
  baselineParameters_.clear();
  baselineOpacities_.clear();
  for (csmInt32 i = 0; i < _model->GetParameterCount(); ++i) baselineParameters_.push_back(_model->GetParameterValue(i));
  for (csmInt32 i = 0; i < _model->GetPartCount(); ++i) baselineOpacities_.push_back(_model->GetPartOpacity(i));
}

void ModelController::restoreBaseline(const MotionDrivenIds* keep) {
  if (!_model) return;

  // 保留清單先換算成索引。GetParameterIndex／GetPartIndex 對不存在的 id 會回
  // 「count 以上」的位置（Framework 拿去記在 notExist 表裡），所以一定要夾上界。
  const csmInt32 paramCount = static_cast<csmInt32>(baselineParameters_.size());
  const csmInt32 partCount = static_cast<csmInt32>(baselineOpacities_.size());
  std::vector<bool> keepParam(static_cast<size_t>(paramCount), false);
  std::vector<bool> keepPart(static_cast<size_t>(partCount), false);
  if (keep) {
    for (const auto& id : keep->parameterIds) {
      const csmInt32 index = _model->GetParameterIndex(CubismFramework::GetIdManager()->GetId(id.c_str()));
      if (index >= 0 && index < paramCount) keepParam[static_cast<size_t>(index)] = true;
    }
    for (const auto& id : keep->partIds) {
      const csmInt32 index = _model->GetPartIndex(CubismFramework::GetIdManager()->GetId(id.c_str()));
      if (index >= 0 && index < partCount) keepPart[static_cast<size_t>(index)] = true;
    }
  }

  for (csmInt32 i = 0; i < paramCount; ++i) {
    if (!keepParam[static_cast<size_t>(i)]) _model->SetParameterValue(i, baselineParameters_[i]);
  }
  for (csmInt32 i = 0; i < partCount; ++i) {
    if (!keepPart[static_cast<size_t>(i)]) _model->SetPartOpacity(i, baselineOpacities_[i]);
  }
  // 把還原後的狀態存成新的基準，動作管線每幀 LoadParameters 才會從這裡出發
  _model->SaveParameters();
}

}  // namespace l2m
