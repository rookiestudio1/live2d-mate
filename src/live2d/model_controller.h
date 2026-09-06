#pragma once

// Live2D 模型控制器：載入、更新、渲染、動作優先權、表情、視線、命中測試。
// 直接建立在 CubismNativeFramework 之上，自己掌控整條 update 管線。
//
// update() 的順序刻意固定（M4 的參數覆寫層靠這兩個掛點）：
//   LoadParameters → motion → SaveParameters → [早寫掛點] → 眨眼 → 表情
//   → 視線加成 → 呼吸 → 拖曳搖晃 → physics → 口型 → [晚寫掛點] → pose → Update()
//
// 早寫：讓 physics 對覆寫值有反應（頭髮衣服跟著動）。
// 晚寫：不被眨眼／呼吸／視線蓋掉。
//
// 檔案一律透過 core/model_assets.h 的 ModelAssets 取得，所以 load() 吃的 entryPath
// 可以是資料夾裡的 model3.json，也可以是一整個 *.zip —— 這一層完全看不出差別。
// **執行緒契約**：ModelAssets（zip 版底下是 miniz）不是執行緒安全的，
// 所有 read() 只在 GUI 執行緒呼叫；貼圖的 worker 只做 PNG 解碼，拿到的是
// 已經在 GUI 執行緒讀好的 bytes。這條與「worker 裡不准出現任何 Csm:: 符號」
// （見 .cpp 的貼圖解碼區塊註解）是並列的兩條硬性規定。

#include <CubismFramework.hpp>
#include <ICubismModelSetting.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Model/CubismUserModel.hpp>

#include "core/model_assets.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/ambient_wind.h"
#include "core/drag_swing.h"
#include "core/idle_motion_pick.h"
#include "core/model_regions.h"
#include "core/model_types.h"
#include "core/motion_builder.h"
#include "core/motion_curve_ids.h"

namespace l2m {

class ModelController : public Csm::CubismUserModel {
public:
  // 動作優先權
  enum Priority { PriorityNone = 0, PriorityIdle = 1, PriorityNormal = 2, PriorityForce = 3 };

  ModelController();
  ~ModelController() override;

  // 從 model3.json 入口載入（內部先跑執行期補全）。GL context 需為 current。
  // 失敗回 false（檔案缺漏、moc3 壞掉、貼圖一張都解不出來）。
  bool load(const std::filesystem::path& entryPath);

  // load() 失敗的原因與修正建議，**英文**，直接給 CommandResult 與錯誤對話框用
  //（面向使用者／AI 的字串一律英文，見 core/model_commands.h）。
  // load() 回 false 時一定有值。貼圖只是部分失敗（模型照樣載入得起來）時也會填，
  // 但目前沒有任何呼叫端在成功路徑上讀它 —— 那種模型會畫成缺一塊的樣子，說明只在
  // log 裡看得到。要讓它浮上畫面的話，從 CharacterWindow／ViewerCanvas 的成功分支下手。
  // 讀取時機只有一個 —— load() 回來的當下：載入失敗的 ModelController 會被呼叫端
  // 當場丟掉（windows/character_window.cpp 與 viewer/viewer_canvas.cpp 都是本地
  // unique_ptr），字串要在那之前抄走。
  const std::string& loadError() const { return loadError_; }
  const std::string& loadHint() const { return loadHint_; }

  // 每幀更新參數（不含渲染）
  void update(float deltaSeconds);

  // 渲染到目前的 GL framebuffer；viewport 為視窗實際像素尺寸。
  // alpha < 1 時整個模型等比變透明（淡入淡出用，見 core/fade_timing.h）。
  // 預設 1.0 是刻意的：命中遮罩走的是同一支 draw()（live2d/hit_mask.cpp），
  // 遮罩若跟著淡，切換模型時量不到舊模型的腳底（modelBottomNormalized 會回
  // nullopt、腳底對齊整個失效），淡出中的視窗形狀也會一路縮成空。
  void draw(int viewportWidth, int viewportHeight, float alpha = 1.0f);

  // 只為了取剪影的繪製（命中遮罩專用）。與 draw() 唯一的差別是**這一次不走高精度遮罩**。
  //
  // 高精度模式下，每個帶遮罩的 drawable 都要各自切一次 FBO、清空、把它的遮罩幾何重畫一遍，
  // 成本跟 viewport 大小無關（所以縮到 192×288 也一點都不便宜）。實測 Intel UHD 730 /
  // 400×600，一次 draw() 的 p50：March 7th 3.6 ms（一般）→ 8.0 ms（高精度）、
  // 藿藿 4.3 ms → 12.4 ms。
  //
  // 但遮罩只拿來回答「哪裡是角色本體」，收下之後還要外擴 kDilate=4 格（~10.4 px），
  // 遮罩邊緣那點塊狀鋸齒根本進不了結果。**畫面本身仍然走高精度** —— 關掉它在藿藿身上
  // 是看得見的退步（外套下襬鋸齒、紅穗子被切短，24 萬像素差 5984 個、最大通道差 199）。
  //
  // 「外擴會吸收掉」這個論證對 AlphaHitMask::bottomNormalizedY()（腳底對齊）**不成立**
  // —— 它刻意讀未膨脹的 pixels_。而這條路正好是淡入那幾幀在跑的，也就是切模型後
  // 量腳底的時機，所以特地量過：March 7th／藿藿／魔女 各 60 幀，高精度重畫、
  // 本函式、blit 三種來源算出來的 bottom **完全相同（差 0.000000）**。
  // 原因是這幾支模型最底下那排可見像素都不是被遮罩的部件。
  // 換句話說這裡沒問題，但它是模型相關的：哪天有模型的最底部是被遮罩的部件
  //（下襬、穗子），對齊就可能差幾個像素 —— 症狀是切模型後角色腳底沒對齊。
  void drawSilhouette(int viewportWidth, int viewportHeight);

  // 同步 renderer 的離屏 buffer 尺寸（Cubism 5 的混合模式會先畫進離屏再以
  // NEAREST 合成回來，尺寸不一致會有棋盤狀縮放紋 —— 跟視窗一致就 1:1 無損）。
  // 視窗大小改變時呼叫；命中遮罩的縮小渲染刻意不動它。
  void setRenderTargetSize(int width, int height);

  // ── 動作 ──
  // index < 0 代表隨機挑一段。回傳是否真的開始播放。
  //
  // loop：這一段要不要一直重播（Live2D Viewer 的「自動重播」用）。
  // **每次起播都會明寫**，不是只在 true 時設 —— motions_ 快取的是同一個
  // ACubismMotion 實例，上一次設過的 loop 會留在物件上，不覆寫的話關掉之後
  // 下一次播同一段還是會繼續循環。
  bool startMotion(const std::string& group, int index, int priority, bool loop = false);
  std::vector<std::string> motionGroups() const;
  int motionCount(const std::string& group) const;

  // ── 表情 ──
  bool setExpression(const std::string& name);
  void clearExpression();
  std::vector<std::string> expressionNames() const;
  const std::optional<std::string>& currentExpression() const { return currentExpression_; }

  // ── 視線／拖曳 ──
  // 邏輯座標 -1..1（視窗座標由 screenToView 換算）
  void setFocus(float x, float y) { _dragManager->Set(x, y); }

  // ── 命中測試 ──
  // 回傳命中的 hit area 名稱（view 座標）
  std::vector<std::string> hitTest(float viewX, float viewY);

  // 視窗像素座標 → view 座標（考慮目前的投影縮放）
  void screenToView(double px, double py, int viewportWidth, int viewportHeight, float* outX, float* outY) const;

  // 模型此刻在畫面上的外接框（view 座標，同 hitTest 與 screenToView 那一組）。
  //
  // 給 core/model_regions.h 的部位推算用：99% 的模型沒有填 HitAreas，
  // 「點在頭還是身上」只能靠幾何，而幾何要先知道角色佔了畫面的哪一塊。
  //
  // 三個刻意的決定：
  //   1. 量的是 drawable 頂點而不是 alpha 剪影。剪影更準（AlphaHitMask 每幀
  //      本來就有一份），但那是桌寵專屬的；Viewer 的畫布底色是不透明深灰，
  //      根本沒有剪影可讀。走頂點兩邊才是同一份答案。
  //   2. 只算「這一幀真的畫得出來」的 drawable（可見旗標 + 不透明度 > 0）。
  //      很多模型帶著整片畫布大小的備用貼圖（換裝、特效），算進去外接框會
  //      整個撐滿，頭部比例當場失準。
  //   3. 頂點是模型空間，最後用 _modelMatrix 轉成 view —— 與 IsHit() 反向
  //      的那次 InvertTransform 對稱（見 CubismUserModel::IsHit）。
  //      矩陣只有等比縮放與平移，所以轉兩個角就夠。
  //
  // 還沒載入、或一個可見的 drawable 都沒有時回 nullopt。
  //
  // **這個框要 draw() 至少跑過一次才有意義**：_modelMatrix 的縮放與平移都是
  // draw() 每幀依 viewport 現算的（fit 與畫布置中都在那裡），第一次 paint 之前
  // 裡面還是載入時的預設值。現有的兩個呼叫端都是點擊驅動的，必然在 paint 之後。
  std::optional<ModelBox> visibleBoundsView() const;

  // 點擊落在角色的哪個部位（視窗像素座標）。桌寵與 Viewer 共用同一支，
  // 兩邊點同一個位置才會得到同一個答案。
  //
  // areas ＝呼叫端已經做過的 hitTest 結果。刻意由外面傳進來而不是這裡現測：
  // 桌寵是在「按下」那一刻測的（單擊要遞延到 OS 雙擊間隔之後才成立，那時
  // 游標可能已經不在原處），那是它既有的契約。
  //
  // 作者標了 HitArea 就以作者的為準（規格上最準，可惜只有 1.1% 的模型有填），
  // 沒標就拿外接框幾何推（core/model_regions.h）。
  BodyPart tapBodyPart(const std::vector<std::string>& areas, double px, double py, int viewportWidth, int viewportHeight) const;

  // ── baseline 快照／還原 ──
  // 載入時快照參數值與 part 不透明度；播新動作前還原，
  // 解決「動作被中斷後多一雙手」的殘留問題。
  // 動作播完接回待機前也會還原一次（見 update() 第 1 步的邊緣觸發）：
  // 動作獨有的道具／特效參數不在待機曲線裡，不清會永遠殘留在最後一幀。
  //
  // keep：即將起播的那支動作自己會驅動的 id，這些**不還原**。
  // baseline 是 moc3 的預設值，也就是作者的編輯狀態 —— 實測碧藍航線 40 隻裡有
  // 13 隻在那個狀態下所有肢體變體一起開著（yichui_2 是 4 隻腳、aidang_2 是 4 隻手），
  // 全部還原的話，新動作 1 秒的淡入就是從「4 隻腳」淡到正確的那一組，看得一清二楚。
  // 交給新動作接手的參數留著上一段的值，才是 Cubism 原本的交叉淡入行為。
  // nullptr ＝全部還原（沒有後續動作要接手時）。理由詳見 core/motion_curve_ids.h。
  void captureBaseline();
  void restoreBaseline(const MotionDrivenIds* keep = nullptr);

  // 接回待機動作（動作佇列空掉、或使用者按下「停止動作」時）。
  //
  // 待機群組不是 model3.json 的規格而是官方範例的慣例，寫死 `"Idle"` 會讓
  // 「全部動作塞在同一個空字串群組、待機只以檔名存在」的模型（碧藍航線整批）
  // 永遠接不回來 —— 挑選規則與實測數字在 core/idle_motion_pick.h。
  //
  // **挑不出待機動作時刻意什麼都不做，連 baseline 都不還原**：那等於把模型丟回
  // 作者的編輯狀態（多出來的手腳），比留著上一段動作的最後一幀更糟。
  //
  // restoreFirst ＝ 剛結束的動作優先度高於待機，要清它的殘值。
  // 注意這個旗標**只在 `priority <= PriorityIdle` 時有效** —— 更高的優先度一律由
  // `startMotion()` 自己還原（它對 `priority > PriorityIdle` 是無條件做的），
  // 傳 false 也擋不掉。目前沒有呼叫端需要「高優先度但不還原」。
  bool startIdleMotion(int priority, bool restoreFirst);

  // 「把 AI／表演留下的狀態收乾淨，然後接回待機」的唯一漏斗。
  // 清殘值與接回待機**必須綁在一起**：只還原不接待機，等於把模型丟回作者的編輯狀態
  // 而且沒有人會再蓋掉它（見 startIdleMotion 的註解）。閒置復原（AppController::
  // resetToIdle）與 stopLoopingAiMotion() 走的都是這一支。
  // 呼叫前請自行把佇列清乾淨（StopAllMotions）或確認佇列已空。
  void returnToIdle();

  // ── AI 合成動作（MCP 的 animate）──
  // 把 buildMotion3 編好的 motion3.json 直接從記憶體載入播放。
  // model3.json 裡沒有 AIMotion 這個群組，所以不能走 startMotion()（那條會先查
  // setting_->GetMotionCount 而直接回絕），這裡自己接上動作管理。
  //
  // carryPastPhysics：這幾個參數的動作曲線值要「越過物理」再寫一次。
  // 動作在 update() 的第 1 步寫入、物理在第 7 步，所以模型把某個參數設成物理輸出時
  //（例如 March 7th 的 ParamBodyAngleY 是由 ParamAngleY 經物理算出來的），
  // 動作寫進去的值會在第 7 步整個被蓋掉，畫面上完全沒反應。
  // 帶上這份清單的話，第 1 步之後會把這些參數的值記下來，第 9 步（物理之後）再寫回去
  // —— 於是「身體上下彈跳但頭完全不動」這種原本做不到的動作就成立了。
  // 空清單＝舊行為。MCP 的 animate 一律傳空的：那條路在 validateAnimateParams
  // 就把物理輸出參數擋掉了，內建動作才走這個掛號（見 core/builtin_actions.h）。
  bool playSynthesizedMotion(const Motion3& motion, bool loop, std::optional<double> fadeInMs, std::optional<double> fadeOutMs, const std::vector<std::string>& carryPastPhysics = {});

  // 停掉循環中的合成動作；本來就沒在循環時回 false（閒置復原會呼叫）
  bool stopLoopingAiMotion();

  // ── 參數 ──
  // 模型有沒有參數表（缺 cdi3 的模型可能是空的）
  bool hasParameterTable() const;

  // 目前每個參數的值與上下限，供 parameterReport 與覆寫層取用
  std::vector<ParameterSnapshot> parameterSnapshots() const;

  // 口型參數 id（字串形式）。覆寫層說話中要跳過這些，免得把嘴巴凍住。
  const std::vector<std::string>& lipSyncParameterIds() const { return lipSyncIdNames_; }

  // ── 參數覆寫掛點（M4 接上）──
  std::function<void(Csm::CubismModel*)> earlyParameterHook;
  std::function<void(Csm::CubismModel*)> lateParameterHook;

  // 口型：每幀回報 0..1 的嘴巴開合（M5 由音訊 RMS 提供）
  std::function<float()> mouthOpenProvider;

  // 拖曳搖晃：每幀回報要疊加的角度偏移與物理風力（AppController 的 DragSwing 提供）。
  // 寫入點在呼吸之後、physics 之前 —— SaveParameters 在第 1 步末就存好了，
  // 這裡的 Add 不會累積到下一幀，每幀疊當幀算好的絕對貢獻即可。
  std::function<DragSwing::Output()> dragSwingProvider;

  // 環境風：每幀回報要疊加到 CubismPhysics Options.Wind 的向量（AppController 提供）。
  // 與拖曳搖晃的風力**相加**後才一次寫入 —— 兩個來源各寫一次會互相蓋掉
  //（見 update() 第 6.5 步）。
  std::function<WindVector()> ambientWindProvider;

  bool loaded() const { return loaded_; }

private:
  void setupModel();
  // 疊加參數但把貢獻夾在「目前值到上下限的餘裕」內 ——
  // Cubism 的 AddParameterValue 不做 clamp，超出上下限是未定義行為
  void addClamped(const Csm::CubismId* id, double delta);
  // 回傳「成功解出並上傳了幾張貼圖」。解不出來的那幾張不會有 GL texture id，
  // 所以這個數字小於 model3.json 宣告的張數就代表出過事，原因寫進 loadError_。
  int setupTextures();
  void preloadMotions();
  Csm::ACubismMotion* motionAt(const std::string& group, int index) const;
  // 這一支動作會驅動哪些 id；沒預載過回 nullptr（＝restoreBaseline 全部還原）
  const MotionDrivenIds* drivenIdsFor(const std::string& group, int index) const;
  // 模型資源的唯一入口；只能在 GUI 執行緒呼叫（見檔頭的執行緒契約）
  std::optional<std::vector<char>> readAsset(const std::string& rel) const;

  std::unique_ptr<Csm::ICubismModelSetting> setting_;
  std::unique_ptr<ModelAssets> assets_;
  bool loaded_ = false;
  std::string loadError_;
  std::string loadHint_;

  // 動作：`群組_索引` → 預載的 motion
  std::map<std::string, Csm::ACubismMotion*> motions_;
  // 動作：`群組_索引` → 那支動作會驅動的參數／部件 id（restoreBaseline 的保留清單）。
  // 在 preloadMotions() 順手從同一份 JSON 解出來，起播時不必再讀一次檔。
  std::map<std::string, MotionDrivenIds> motionDrivenIds_;
  // 載入時就挑好的待機動作（見 startIdleMotion）。每幀都重挑的話，接不出待機的
  // 模型會在 update() 的 IsFinished 分支上每幀重新攤平一次整份群組清單。
  std::optional<IdleMotionSlot> idleSlot_;
  // 表情：名稱 → 預載的 expression motion
  std::map<std::string, Csm::ACubismMotion*> expressions_;
  std::optional<std::string> currentExpression_;

  Csm::csmVector<Csm::CubismIdHandle> eyeBlinkIds_;
  Csm::csmVector<Csm::CubismIdHandle> lipSyncIds_;

  // 最近一次成功起播的動作優先度。CubismMotionManager::UpdateMotion 在
  // IsFinished 時會把 _currentPriority 歸 0，所以「剛結束的是什麼優先度」
  // 只能自己記。用途：update() 的 IsFinished 分支要邊緣觸發 restoreBaseline
  // —— 沒動作在播時 IsFinished 恆為 true（一支待機動作都挑不出來的模型尤其如此），
  // 不消耗這個值會每幀 restore；待機動作自己也會循環播完，不比對優先度
  // 會每輪循環硬切一次。
  int lastStartedMotionPriority_ = PriorityNone;

  // AI 合成動作：目前佔用的插槽與是否循環中
  std::optional<int> playingAiSlot_;
  bool loopingAiMotion_ = false;
  // 被換掉但可能還在淡出佇列裡的合成動作，等動作管理空了才真的釋放
  std::vector<Csm::ACubismMotion*> retiredAiMotions_;
  // 要越過物理再寫一次的參數索引（見 playSynthesizedMotion 的 carryPastPhysics）。
  // 只有合成動作會設，任何 startMotion() 都會清掉 —— 留著的話下一個動作
  //（通常是自動接上的待機）會把「上一幀的物理輸出」當成動作值一直寫回去，
  // 身體就凍在最後那個姿勢了。
  std::vector<int> carryPastPhysicsIndices_;
  // 這一幀第 1 步之後記下的值，第 9 步寫回去。動作沒在播就是空的。
  std::vector<std::pair<int, float>> carriedThisFrame_;
  // lipSyncIds_ 的字串形式（覆寫層跳過清單用）
  std::vector<std::string> lipSyncIdNames_;

  // 環境風該吹哪些 PhysicsSetting（1 ＝ 吹）。載入時由 core/wind_targets.h 算好，
  // update() 第 6.5 步只是把 data() 指給 Framework 的 Options.WindMask ——
  // 那個欄位存的是**指標不是複本**，所以這份 vector 的生命週期要跟 _physics 一樣長，
  // 而且不能在載入之後改動（會 realloc 成新位址）。空的話交 NULL ＝ 全部吃風。
  std::vector<std::uint8_t> windMask_;

  // baseline 快照
  std::vector<float> baselineParameters_;
  std::vector<float> baselineOpacities_;

  // 最近一次 draw 用的投影縮放（供座標換算）
  float projectionScaleX_ = 1.0f;
  float projectionScaleY_ = 1.0f;

  // model3.json 的 Layout 有沒有指定 width/height。
  // 有的話 draw() 不做每幀的強制 fit，作者構圖才不會被 SetWidth(2.0f) 蓋掉；
  // 沒有的話依 canvas 與視窗的長寬比挑受限維度去 fit。
  bool layoutSpecifiesSize_ = false;

  // model3.json 的 Layout 有沒有指定位置（x／y／center_x／center_y／
  // top／bottom／left／right 任一）。有指定＝作者自己決定了模型擺哪，
  // draw() 就不再自動置中 —— CubismModelMatrix 的 Translate() 是絕對指派，
  // 補一次置中等於把 SetupFromLayout 擺好的位置蓋掉。
  //
  // **判別的是「位置」而不是「有沒有 Layout」**：SetupFromLayout 只有碰到上面
  // 那八個鍵才會寫平移，width／height 走的是 Scale()、平移從頭到尾是 0。
  // 用「有沒有 Layout」當條件的話，只寫了 {"height": 2.2} 的模型會白白跳過置中
  // —— 明明沒有任何東西會被蓋掉，卻讓「只看得到下半身」原封不動地留著。
  bool layoutSpecifiesPosition_ = false;

  // 那份 Layout 套下去之後畫布還在不在標準視野裡。false = 整份不採用，
  // 退回自動 fit + 置中。判別方式、為什麼「作者寫了就照做」不夠，以及
  // 《原神》可莉／派蒙的實測數字都在 core/layout_fit.h。
  bool layoutUsable_ = true;

  // 畫布中心相對「模型原點」的位移（模型單位，Y 向上；原點在正中央時就是 0,0）。
  // 算式、實測數字與「為什麼非得是純函式」都寫在 core/canvas_center.h。
  float canvasCenterX_ = 0.0f;
  float canvasCenterY_ = 0.0f;

  // 渲染用的 GL 貼圖 id（釋放時歸還）
  std::vector<unsigned int> textures_;

  // 常用參數 id（視線加成用）
  const Csm::CubismId* idParamAngleX_;
  const Csm::CubismId* idParamAngleY_;
  const Csm::CubismId* idParamAngleZ_;
  const Csm::CubismId* idParamBodyAngleX_;
  const Csm::CubismId* idParamEyeBallX_;
  const Csm::CubismId* idParamEyeBallY_;
};

}  // namespace l2m
