#include "builtin_actions.h"

#include <algorithm>
#include <map>
#include <set>

#include "model_commands.h"
#include "string_util.h"

namespace l2m {

namespace {

// 內建表用的邏輯角色。內建動作只描述「哪個角色要動到哪」，
// 角色對到哪個真實參數 id 由 resolveSlots() 依模型決定。
enum class Slot {
  EyeLOpen,
  EyeROpen,
  EyeLSmile,
  EyeRSmile,
  EyeBallX,
  EyeBallY,
  BrowLY,
  BrowRY,
  BrowLAngle,
  BrowRAngle,
  MouthForm,
  MouthOpen,
  AngleX,
  AngleY,
  AngleZ,
  BodyAngleX,
  BodyAngleY,
  BodyAngleZ,
  ArmWave,
  ArmWave2,
};

struct SlotSpec {
  Slot slot;
  // Cubism 標準 id，依偏好順序。絕大多數模型都靠這一段就對上了。
  std::vector<const char*> ids;
  // 標準 id 缺席時比對 cdi3 裡作者取的名稱（不分大小寫的子字串）。
  // 中文模型很常把英文 id 一起寫進名稱（「左眼　微笑[EyeLSmile]」），
  // 所以英文關鍵字在這裡也有用。
  std::vector<const char*> keywords;
  // 這個槽位收不收「物理輸出」的參數。預設不收 —— 動作寫在物理之前，
  // 寫了會被物理整個蓋掉，做不成動作。
  // 開著的槽位表示「用得到的動作會把值越過物理再寫一次」（BuiltinMotion::carryPastPhysics），
  // 目前是頭部角度與身體角度：那兩組最常被綁成物理輸出、卻又非它不可
  //（一關掉，保底的 nod／shake／tilt 與 excited 就整批消失，理由見下面的槽位表）。
  bool allowPhysicsOutput = false;
};

// 槽位表。ArmWave 與 ArmWave2 共用同一組關鍵字 —— 一個參數只能填一個槽位
//（resolveSlots 的 used 集合保證），所以「招手1」先被 ArmWave 拿走，
// 「招手2」自然落到 ArmWave2。
const std::vector<SlotSpec>& slotSpecs() {
  static const std::vector<SlotSpec> specs = {
    {Slot::EyeLOpen, {"ParamEyeLOpen"}, {"eyelopen"}},
    {Slot::EyeROpen, {"ParamEyeROpen"}, {"eyeropen"}},
    {Slot::EyeLSmile, {"ParamEyeLSmile"}, {"eyelsmile"}},
    {Slot::EyeRSmile, {"ParamEyeRSmile"}, {"eyersmile"}},
    {Slot::EyeBallX, {"ParamEyeBallX"}, {"eyeballx"}},
    {Slot::EyeBallY, {"ParamEyeBallY"}, {"eyebally"}},
    {Slot::BrowLY, {"ParamBrowLY"}, {"browly"}},
    {Slot::BrowRY, {"ParamBrowRY"}, {"browry"}},
    {Slot::BrowLAngle, {"ParamBrowLAngle"}, {"browlangle"}},
    {Slot::BrowRAngle, {"ParamBrowRAngle"}, {"browrangle"}},
    {Slot::MouthForm, {"ParamMouthForm"}, {"mouthform"}},
    {Slot::MouthOpen, {"ParamMouthOpenY", "ParamMouthOpen"}, {"mouthopen"}},
    // 身體角度收物理輸出。很多模型（例如 March 7th）把 ParamBodyAngle* 綁成物理輸出、
    // 由 ParamAngleY 經物理算出來 —— 那正是「頭往下看、身體跟著微蹲」的來源。
    // 照一般規則跳過的話，身體就只能靠轉頭去帶，「身體在跳、頭不動」永遠做不出來。
    // 所以這兩個槽位收物理輸出，代價是用到它們的動作必須把值列進
    // BuiltinMotion::carryPastPhysics，交給播放端在物理之後再寫一次。
    //
    // **必須排在 AngleX/Y/Z 前面**：第 2 段的關鍵字是不分大小寫的**子字串**比對，
    // 而 "bodyangley" 本身就含有 "angley"，槽位又是先到先得（used 集合）。排在後面的話，
    // 一隻「沒有標準 id ParamBodyAngleY、只有作者在 cdi3 把名字寫成 BodyAngleY」的模型
    // 會被 AngleY 先搶走 —— 症狀是 excited（required 掛在 BodyAngleY 上）整個從
    // list_motions 消失，而 nod／sigh／doze 把點頭的曲線寫到**身體**參數上，
    // 動作照播、部位全錯，一句錯誤訊息都沒有。反過來不會誤傷：名稱是 "AngleY" 的參數
    // 不含 "bodyangley"，先比對特定的那個永遠是安全的方向。
    //（AngleX/Y/Z 開 allowPhysicsOutput 之前這個順序無所謂 —— 那時 AngleY 遇到
    // 物理輸出的身體參數會直接跳過，剛好讓 BodyAngleY 撿到。）
    // BodyAngleX **沒有任何內建動作或表情用得到**，放在這裡純粹是為了「先吸收掉名稱」：
    // 上面那段講的搶奪對 X 一樣成立（"bodyanglex" 含有 "anglex"），而 X 少了這一列就沒有
    // 東西擋在 AngleX 前面。症狀比 Y 更難發現 —— 沒有哪個內建會整個消失，只是
    // shake／tilt／look_away／sigh 把搖頭的曲線寫到**身體 X 旋轉**上，動作照播、部位全錯。
    // allowPhysicsOutput 一定要跟著開：關著的話物理輸出的身體 X 會被這個槽位跳過，
    // 然後被開著的 AngleX 撿走，等於這一列白加。
    {Slot::BodyAngleX, {"ParamBodyAngleX"}, {"bodyanglex"}, true},
    {Slot::BodyAngleY, {"ParamBodyAngleY"}, {"bodyangley"}, true},
    {Slot::BodyAngleZ, {"ParamBodyAngleZ"}, {"bodyanglez"}, true},
    // 頭部角度也收物理輸出。原本不收（理由就是那句「寫在物理之前會被蓋掉」），
    // 但實測 ariu 那一類模型會因此把保底動作整批弄不見：它把頭的慣性做成
    // ParamAngle{X,Y,Z} **同時是物理的 Input 也是 Output** 的自我回授 rig
    //（ariu.physics3.json 的 PhysicsSetting1~3：in ParamAngleY → out ParamBodyAngleY + ParamAngleY），
    // 於是這三個一律被判成 PhysicsOutput、三個槽位一起空掉，
    // nod／shake／tilt／look_away／sigh／doze 六個一次全滅 —— 而那六個正是
    // 「只綁一個 Idle 的模型至少也拿得到」的那一批，全滅之後這隻模型只剩四個內建動作。
    // 收下來之後值走 carryPastPhysics 在物理之後補寫，這一類模型反而是最好的結果：
    // 物理之前寫進去的值照樣餵給頭髮那 11 組鏈（它們的 Input 就是 ParamAngle*），頭髮跟著甩；
    // 物理之後再寫回曲線值，頭本身就不會被慣性回授拖成軟綿綿的半套動作。
    // 對「ParamAngle* 不是物理輸出」的絕大多數模型，這個開關是完全的 no-op ——
    // role 不是 PhysicsOutput，carryPastPhysics 就是空的，一行行為都沒變。
    {Slot::AngleX, {"ParamAngleX"}, {"anglex"}, true},
    {Slot::AngleY, {"ParamAngleY"}, {"angley"}, true},
    {Slot::AngleZ, {"ParamAngleZ"}, {"anglez"}, true},
    {Slot::ArmWave, {"ParamArmRA", "ParamArmR"}, {"招手", "揮手", "挥手", "手を振", "wave"}},
    {Slot::ArmWave2, {"ParamArmRB"}, {"招手", "揮手", "挥手", "手を振", "wave"}},
  };
  return specs;
}

struct SlotValue {
  Slot slot;
  double value;
  // 只有內建**表情**看這個欄位（動作是關鍵影格，走動作曲線，本來就是覆寫）。
  // Add 是給「疊在模型自己的動態之上」的那種值用的 —— 表情是掛著不走的，
  // 用 Set 釘住 ParamAngleY 之類的參數，頭就再也不會跟著游標轉了
  //（視線／拖曳加成在第 5 步，覆寫層的晚寫在它後面，見 parameter_overlay.h）。
  ParameterMode mode = ParameterMode::Set;
};

struct ExpressionDef {
  const char* name;
  // 這幾個槽位缺一個，這個內建就不提供 —— 少了關鍵那一項的表情看起來只是壞掉
  std::vector<Slot> required;
  // 解析不到的槽位直接跳過（錦上添花的部分，缺了不影響能不能看）
  std::vector<SlotValue> values;
};

struct MotionFrame {
  double at;
  std::vector<SlotValue> values;
};

struct MotionDef {
  const char* name;
  std::vector<Slot> required;
  double fadeInMs;
  double fadeOutMs;
  std::vector<MotionFrame> frames;
  // 循環播放。放在最後才不必替其他項目補一個 false。
  // 循環的動作首尾必須同值，否則每一圈的接點都會跳一下（見 doze）。
  bool loop = false;
};

// ── 內建動作 ────────────────────────────────────────────
//
// 手感常數寫死在這裡，照 GazeDirectorTuning／AmbientWindTuning 的慣例不進 config。
// 揮手的 ±30 是刻意開大的：掃描期讀不到參數上下限，靠 Cubism 夾成該模型的滿舵
//（見標頭註解）。點頭搖頭只吃 ParamAngleX/Y，那是每隻模型都有的標準參數，
// 所以這一區不會整個空掉 —— 一隻只綁了 Idle 的模型至少也拿得到這兩個。
// 但「有這個參數」不等於「寫得進去」：把頭部慣性做成自我回授物理的模型（ariu）
// 會讓這三個角度全被判成物理輸出，所以那三個槽位開了 allowPhysicsOutput，
// 值改走 carryPastPhysics 在物理之後補寫（理由寫在 slotSpecs()）。
const std::vector<MotionDef>& motionDefs() {
  static const std::vector<MotionDef> defs = {
    {"wave",
     {Slot::ArmWave},
     200,
     300,
     {
       {0, {{Slot::ArmWave, 0}, {Slot::ArmWave2, 0}, {Slot::AngleZ, 0}}},
       {400, {{Slot::ArmWave, 30}, {Slot::ArmWave2, 20}, {Slot::AngleZ, -6}}},
       {800, {{Slot::ArmWave, -30}, {Slot::ArmWave2, 30}, {Slot::AngleZ, 6}}},
       {1200, {{Slot::ArmWave, 30}, {Slot::ArmWave2, 20}, {Slot::AngleZ, -6}}},
       {1600, {{Slot::ArmWave, -30}, {Slot::ArmWave2, 30}, {Slot::AngleZ, 6}}},
       {2100, {{Slot::ArmWave, 0}, {Slot::ArmWave2, 0}, {Slot::AngleZ, 0}}},
     }},
    // ParamAngleY 正值是抬頭，所以點頭要先往負的走。
    // 幅度刻意大（第一拍 −26，接近 ParamAngleY 標準範圍 ±30 的滿舵）—— 小幅度的點頭
    // 在 400×600 的舞台上根本看不出是在點頭，範圍小的模型照樣由 Cubism 夾回去。
    // 跟同樣低到 −26 的 sigh／doze 的分野在節奏：這裡是 250 ms 一去一回，那兩個是慢慢沉下去。
    {"nod",
     {Slot::AngleY},
     150,
     250,
     {
       {0, {{Slot::AngleY, 0}}},
       {250, {{Slot::AngleY, -26}}},
       {550, {{Slot::AngleY, 10}}},
       {850, {{Slot::AngleY, -18}}},
       {1200, {{Slot::AngleY, 0}}},
     }},
    {"shake",
     {Slot::AngleX},
     150,
     250,
     {
       {0, {{Slot::AngleX, 0}}},
       {250, {{Slot::AngleX, -18}}},
       {600, {{Slot::AngleX, 18}}},
       {950, {{Slot::AngleX, -12}}},
       {1300, {{Slot::AngleX, 0}}},
     }},
    // 歪頭。中間兩格刻意等值 —— 一去一回的歪頭看起來像抽搐，
    // 「歪著看你一下」才是要的語意，所以停在歪著的姿勢 1.2 秒再回正。
    // 幅度同 nod 的理由開大（AngleZ 28，接近標準範圍 ±30 的滿舵）——
    // 原本的 18 在 400×600 的舞台上只像頭微微晃了一下，看不出是「歪頭」；
    // 範圍小的模型照樣由 Cubism 夾回它自己的滿舵。AngleX 一起加到 10，
    // 臉跟著往同一側轉一點才不會變成「脖子斷掉、臉還正對前方」。
    {"tilt",
     {Slot::AngleZ},
     250,
     350,
     {
       {0, {{Slot::AngleZ, 0}, {Slot::AngleX, 0}}},
       {450, {{Slot::AngleZ, 28}, {Slot::AngleX, 10}}},
       {1650, {{Slot::AngleZ, 28}, {Slot::AngleX, 10}}},
       {2150, {{Slot::AngleZ, 0}, {Slot::AngleX, 0}}},
     }},
    // 笑瞇眼。**刻意做成動作而不是表情**：表情是掛著不走的，而覆寫層在晚寫掛點、
    // 眨眼在它前面，雙眼被釘在 0 就再也不會眨 —— 掛久了不是笑臉是睡臉。
    // 動作有時長，播完自動放掉；而且動作播放期間 CubismEyeBlink 整個停用
    //（model_controller.cpp 的 `!motionUpdated && _eyeBlink`），閉眼不會被眨回去。
    // 起始格明確寫 1／0 而不是省略：buildMotion3 對沒寫到的頭尾是補平的。
    {"grin",
     {Slot::EyeLOpen, Slot::EyeROpen},
     200,
     300,
     {
       {0, {{Slot::EyeLOpen, 1}, {Slot::EyeROpen, 1}, {Slot::EyeLSmile, 0}, {Slot::EyeRSmile, 0}, {Slot::MouthForm, 0}}},
       {350, {{Slot::EyeLOpen, 0}, {Slot::EyeROpen, 0}, {Slot::EyeLSmile, 1}, {Slot::EyeRSmile, 1}, {Slot::MouthForm, 1}}},
       {1500, {{Slot::EyeLOpen, 0}, {Slot::EyeROpen, 0}, {Slot::EyeLSmile, 1}, {Slot::EyeRSmile, 1}, {Slot::MouthForm, 1}}},
       {1900, {{Slot::EyeLOpen, 1}, {Slot::EyeROpen, 1}, {Slot::EyeLSmile, 0}, {Slot::EyeRSmile, 0}, {Slot::MouthForm, 0}}},
     }},
    // 拋媚眼：閉左眼、停一下、再睜開。EyeLSmile 讓閉起來的那隻眼是笑的弧線而不是一條直線。
    // **原本是表情，改成動作**：表情是掛著不走的，眼睛被覆寫層釘在 0 就一直閉著
    //（覆寫在晚寫掛點、眨眼在它前面），那不是拋媚眼是瞎了一隻眼 —— 同 grin 的理由。
    // 節奏：200 ms 閉上、停 450 ms 讓人看得到、300 ms 睜開；嘴角的笑比眼睛晚一點收，
    // 三個時間點都不同才不像機械式的開關。
    {"wink",
     {Slot::EyeLOpen},
     150,
     300,
     {
       {0, {{Slot::EyeLOpen, 1}, {Slot::EyeLSmile, 0}, {Slot::MouthForm, 0}}},
       {200, {{Slot::EyeLOpen, 0}, {Slot::EyeLSmile, 1}, {Slot::MouthForm, 1}}},
       {650, {{Slot::EyeLOpen, 0}, {Slot::EyeLSmile, 1}, {Slot::MouthForm, 1}}},
       {950, {{Slot::EyeLOpen, 1}, {Slot::EyeLSmile, 0}, {Slot::MouthForm, 1}}},
       {1400, {{Slot::EyeLOpen, 1}, {Slot::EyeLSmile, 0}, {Slot::MouthForm, 0}}},
     }},
    // 別開視線（心虛／害羞）。EyeBallX 走的是 AddParameterValue 而不是覆寫
    //（model_controller.cpp 的視線加成），所以游標在動時會跟視線追蹤疊起來、
    // 靜止時才乾淨。這是刻意接受的：轉頭是主體，眼睛只是加分。
    {"look_away",
     {Slot::AngleX},
     250,
     350,
     {
       {0, {{Slot::AngleX, 0}, {Slot::AngleY, 0}, {Slot::EyeBallX, 0}}},
       {500, {{Slot::AngleX, -14}, {Slot::AngleY, -5}, {Slot::EyeBallX, -0.8}}},
       {1700, {{Slot::AngleX, -14}, {Slot::AngleY, -5}, {Slot::EyeBallX, -0.8}}},
       {2200, {{Slot::AngleX, 0}, {Slot::AngleY, 0}, {Slot::EyeBallX, 0}}},
     }},
    // 嘆氣：先低頭、視線垂下、眉毛跟著垂，停一拍之後**慢慢**搖頭兩次才抬起來。
    // 兩件事的先後就是這個動作的語意，所以搖頭那幾格排在低頭之後，
    // 而且整段維持低著（3900 那格把 AngleY／EyeBallY 再寫一次 −26／−0.85）——
    // buildMotion3 是在「實際被寫到的點」之間線性內插的，不補這一格就會從 650 一路
    // 緩緩抬起，變成邊抬頭邊搖頭。搖頭幅度只有 shake 的一半、單程 550 ms（shake 是 350），
    // 慢才像嘆氣。EyeBallY 跟 look_away 的 EyeBallX 一樣是加成不是覆寫。
    // 臉直接借 sad 那一組（MouthForm −1 的嘴角下垂 + BrowAngle −1 的八字眉）——
    // 只有低頭搖頭的話語意其實是「否定」，難過的臉才把它定成嘆氣。眉毛的垂度沿用
    // 這裡原本的 −0.5 而不是 sad 的 −0.3：嘆氣是一瞬間的動作，垂得比掛著的表情深才看得出來。
    // 眼睛只收到 0.7（sleepy 的半睜是 0.35）—— 一點點就夠，再小會變成想睡而不是嘆氣。
    // 眼睛敢寫是因為這是動作不是表情：播完自動放掉，不會像表情那樣把眼睛釘住不眨
    //（同 grin 的理由，而且動作播放期間 CubismEyeBlink 整個停用，半睜不會被眨回去）。
    // 這幾個臉部值同樣要在 3900 那格補寫一次，理由同上 —— 少寫就會從 650 一路淡回去，
    // 搖到一半臉就先笑回來了。
    {"sigh",
     {Slot::AngleY, Slot::AngleX},
     300,
     400,
     {
       {0,
        {{Slot::AngleY, 0},
         {Slot::AngleX, 0},
         {Slot::EyeBallY, 0},
         {Slot::BrowLY, 0},
         {Slot::BrowRY, 0},
         {Slot::BrowLAngle, 0},
         {Slot::BrowRAngle, 0},
         {Slot::MouthForm, 0},
         {Slot::EyeLOpen, 1},
         {Slot::EyeROpen, 1}}},
       {650,
        {{Slot::AngleY, -26},
         {Slot::EyeBallY, -0.85},
         {Slot::BrowLY, -0.5},
         {Slot::BrowRY, -0.5},
         {Slot::BrowLAngle, -1},
         {Slot::BrowRAngle, -1},
         {Slot::MouthForm, -1},
         {Slot::EyeLOpen, 0.7},
         {Slot::EyeROpen, 0.7}}},
       {1200, {{Slot::AngleX, 0}}},
       {1750, {{Slot::AngleX, -10}}},
       {2300, {{Slot::AngleX, 10}}},
       {2850, {{Slot::AngleX, -10}}},
       {3400, {{Slot::AngleX, 10}}},
       {3900,
        {{Slot::AngleX, 0},
         {Slot::AngleY, -26},
         {Slot::EyeBallY, -0.85},
         {Slot::BrowLY, -0.5},
         {Slot::BrowRY, -0.5},
         {Slot::BrowLAngle, -1},
         {Slot::BrowRAngle, -1},
         {Slot::MouthForm, -1},
         {Slot::EyeLOpen, 0.7},
         {Slot::EyeROpen, 0.7}}},
       {4400,
        {{Slot::AngleY, 0}, {Slot::EyeBallY, 0}, {Slot::BrowLY, 0}, {Slot::BrowRY, 0}, {Slot::BrowLAngle, 0}, {Slot::BrowRAngle, 0}, {Slot::MouthForm, 0}, {Slot::EyeLOpen, 1}, {Slot::EyeROpen, 1}}},
     }},
    // 打瞌睡：閉著眼睛規律地點頭，**唯一一個循環的內建動作** —— 不自己結束，
    // 一直播到下一個動作把它蓋掉，或閒置復原的 stopLoopingAiMotion() 收掉它
    //（不然桌寵會永遠睡下去）。
    // 循環的一圈只有一次下沉，而且**首尾必須同值**（都是 −6）：不同值的話每一圈的
    // 接點都會跳一下。眼睛只在頭尾寫 0，中間不寫 —— buildMotion3 會把整條曲線補平，
    // 循環期間就一直閉著。閉眼撐得住是因為動作播放期間 CubismEyeBlink 整個停用（同 grin），
    // 做成表情就會被眨眼搶回去。
    // 頂點刻意停在 −6 而不是 0：頭本來就垂著，完全抬起來就不是在打瞌睡了。
    {"doze",
     {Slot::EyeLOpen, Slot::EyeROpen, Slot::AngleY},
     300,
     400,
     {
       {0, {{Slot::EyeLOpen, 0}, {Slot::EyeROpen, 0}, {Slot::AngleY, -6}}},
       {900, {{Slot::AngleY, -26}}},
       {1800, {{Slot::EyeLOpen, 0}, {Slot::EyeROpen, 0}, {Slot::AngleY, -6}}},
     },
     true},
    // 打哈欠：抬頭、慢慢張到最大、閉眼，然後合嘴垂頭。
    // MouthOpen 是 LipSync 參數，說話中口型會用 AddParameterValue 疊在這上面
    //（model_controller.cpp 第 8 步，權重 0.8）—— 動作照播，只是嘴型會混。
    // 這跟 surprised 那個「表情期間完全被跳過」不一樣：跳過的是覆寫層，不是動作。
    // 閉眼撐得住的理由同 grin（動作播放期間 CubismEyeBlink 停用），最後一格睜回 1。
    // 仰頭的幅度跟著張嘴走（原本的 6／10／8 太保守，在 400×600 的舞台上只看得出嘴在開）：
    // 半開時 10、張到最大時 18、撐著的那一拍 15。18 是刻意留在 nod 的 26 之下的 ——
    // 打哈欠是仰頭不是往後倒，滿舵會變成整個人向後翻。收尾的 −8 不動，
    // 「仰起來 → 垂下去」的落差本來就是哈欠打完的那個鬆懈感。
    {"yawn",
     {Slot::MouthOpen, Slot::EyeLOpen, Slot::EyeROpen},
     250,
     350,
     {
       {0, {{Slot::MouthOpen, 0}, {Slot::EyeLOpen, 1}, {Slot::EyeROpen, 1}, {Slot::AngleY, 0}}},
       {500, {{Slot::MouthOpen, 0.3}, {Slot::EyeLOpen, 0.5}, {Slot::EyeROpen, 0.5}, {Slot::AngleY, 10}}},
       {1100, {{Slot::MouthOpen, 1}, {Slot::EyeLOpen, 0}, {Slot::EyeROpen, 0}, {Slot::AngleY, 18}}},
       {1900, {{Slot::MouthOpen, 1}, {Slot::EyeLOpen, 0}, {Slot::EyeROpen, 0}, {Slot::AngleY, 15}}},
       {2600, {{Slot::MouthOpen, 0.1}, {Slot::EyeLOpen, 0.3}, {Slot::EyeROpen, 0.3}, {Slot::AngleY, -8}}},
       {3200, {{Slot::MouthOpen, 0}, {Slot::EyeLOpen, 1}, {Slot::EyeROpen, 1}, {Slot::AngleY, 0}}},
     }},
    // 興奮：笑瞇眼配上兩下彈跳，**頭一格都不碰**。
    //
    // 「跳」沒有現成的參數 —— Cubism 的標準表裡沒有整體上下位移。真正在畫面上做出
    // 「微蹲 → 伸展」的是 ParamBodyAngleY：那也是「視線往下看時身體會蹲一點」的來源。
    // 只是在很多模型上（March 7th 就是）它是**物理輸出**，由 ParamAngleY 經物理算出來，
    // 所以照一般做法只能靠上下甩頭去帶身體 —— 那就變成點頭了。
    // 這裡改成直接寫身體、完全不寫 AngleY／AngleZ，靠 carryPastPhysics 讓值越過物理
    //（見 builtin_actions.h 的 BuiltinMotion 與 model_controller 的第 1.5／9 步）。
    //
    // 因此 required 掛在 BodyAngleY 上：沒有身體角度的模型拿不到這個動作。
    // 這是刻意的 —— 這個動作的內容就是身體在跳，退化成「只有笑臉」的版本沒有意義。
    // 值用身體角度的標準範圍（±10）：蹲 −7、彈到 +10 的滿舵，模型範圍小的由 Cubism 夾。
    // 兩下的節奏是 250 ms 一次（跟 nod 同拍）：再慢就變成上下晃，再快看不清是兩下。
    // BodyAngleZ 兩下刻意反向（+4／−4），同向的話第二下跟第一下一模一樣，像跳針。
    // 臉沿用 grin 那一組（眼睛閉成笑弧＋嘴角上揚），起始格與最後一格都明確寫出來 ——
    // 停在閉眼上接回待機就是一張睡臉（同 grin 的理由）。
    // 彈完之後笑臉再多撐 350 ms 才收：眼睛跟身體同時歸位看起來像被按了重設。
    {"excited",
     {Slot::BodyAngleY, Slot::EyeLOpen, Slot::EyeROpen},
     150,
     300,
     {
       {0, {{Slot::EyeLOpen, 1}, {Slot::EyeROpen, 1}, {Slot::EyeLSmile, 0}, {Slot::EyeRSmile, 0}, {Slot::MouthForm, 0}, {Slot::BodyAngleY, 0}, {Slot::BodyAngleZ, 0}}},
       // 蹲一下蓄力，笑臉同時到位
       {250, {{Slot::EyeLOpen, 0}, {Slot::EyeROpen, 0}, {Slot::EyeLSmile, 1}, {Slot::EyeRSmile, 1}, {Slot::MouthForm, 1}, {Slot::BodyAngleY, -7}, {Slot::BodyAngleZ, 0}}},
       // 第一下
       {500, {{Slot::BodyAngleY, 10}, {Slot::BodyAngleZ, 4}}},
       {750, {{Slot::BodyAngleY, -5}, {Slot::BodyAngleZ, 0}}},
       // 第二下，左右反過來
       {1000, {{Slot::BodyAngleY, 10}, {Slot::BodyAngleZ, -4}}},
       {1250, {{Slot::BodyAngleY, -5}, {Slot::BodyAngleZ, 0}}},
       // 站穩，笑臉還掛著（這一格把臉再寫一次，否則會從 250 一路淡回去）
       {1600, {{Slot::BodyAngleY, 0}, {Slot::BodyAngleZ, 0}, {Slot::EyeLOpen, 0}, {Slot::EyeROpen, 0}, {Slot::EyeLSmile, 1}, {Slot::EyeRSmile, 1}, {Slot::MouthForm, 1}}},
       // 收笑臉
       {1950, {{Slot::EyeLOpen, 1}, {Slot::EyeROpen, 1}, {Slot::EyeLSmile, 0}, {Slot::EyeRSmile, 0}, {Slot::MouthForm, 0}}},
     }},
  };
  return defs;
}

// ── 內建表情 ────────────────────────────────────────────
//
// surprised 的張嘴在說話期間看不出來：ParamMouthOpenY 是 LipSync 參數，
// ParameterOverlay 說話中會跳過對它的寫入（不跳過的話嘴型會被凍住）。
// 這是正確行為，不是漏洞。
const std::vector<ExpressionDef>& expressionDefs() {
  static const std::vector<ExpressionDef> defs = {
    {"smile", {Slot::MouthForm}, {{Slot::MouthForm, 1}, {Slot::EyeLSmile, 1}, {Slot::EyeRSmile, 1}}},
    // 睜眼寫 1.4：範圍到 1.4 的模型會真的瞪大，只到 1.0 的夾回 1.0 也不會壞
    //（1.2 在支援 1.4 的模型上只比平常大一點點，看不太出是在吃驚）。
    // AngleY 是**疊加**不是覆寫：頭往上仰一點才像被嚇到往後縮，但表情是掛著不走的，
    // 用覆寫釘住頭部角度就等於在這個表情期間關掉視線追蹤（頭不再跟著游標轉）。
    // 疊加走的是晚寫掛點，所以疊在眨眼呼吸視線之上；代價是 physics 已經算完，
    // 頭髮不會對這一點仰角有反應 —— 靜態表情本來就沒有那個瞬間，可以接受。
    // 幅度只有 12（nod／tilt 那種滿舵是 26~28）：這是會掛很久的姿勢，仰過頭就不是吃驚是在看天花板。
    {"surprised", {Slot::MouthOpen}, {{Slot::MouthOpen, 1}, {Slot::EyeLOpen, 1.4}, {Slot::EyeROpen, 1.4}, {Slot::MouthForm, -0.5}, {Slot::AngleY, 12, ParameterMode::Add}}},
    // 想睡：半睜 0.35 而不是閉起來 —— 表情掛著不走，閉起來就成了睡臉（同 grin 的理由，
    // 只是半睜的樣子撐得住長時間掛著）。已知取捨：眼睛被釘住的期間不會眨眼，
    // 想睡的角色本來就眨得慢，所以留著。對上 IdleLevel::Sleepy。
    {"sleepy", {Slot::EyeLOpen, Slot::EyeROpen}, {{Slot::EyeLOpen, 0.35}, {Slot::EyeROpen, 0.35}, {Slot::MouthForm, -0.2}, {Slot::BrowLY, -0.2}, {Slot::BrowRY, -0.2}}},
    // 難過與生氣的分野只在眉毛角度（BrowLAngle 負＝八字眉、正＝豎眉），嘴角兩邊都是往下。
    // 所以 angry 的 required 掛在 BrowAngle 上：沒有眉毛角度的模型只拿得到 sad ——
    // 兩個長得一模一樣的項目比少一個更糟，AI 選哪個都一樣就等於沒得選。
    // sad 少了眉毛還剩「嘴角下垂」看得出來是難過，所以 required 只掛 MouthForm。
    // 兩個都不碰 EyeLOpen：釘住眼睛就不眨了，而這兩個表情是會掛很久的。
    {"sad", {Slot::MouthForm}, {{Slot::MouthForm, -1}, {Slot::BrowLAngle, -1}, {Slot::BrowRAngle, -1}, {Slot::BrowLY, -0.3}, {Slot::BrowRY, -0.3}}},
    {"angry", {Slot::BrowLAngle, Slot::BrowRAngle}, {{Slot::BrowLAngle, 1}, {Slot::BrowRAngle, 1}, {Slot::BrowLY, -0.8}, {Slot::BrowRY, -0.8}, {Slot::MouthForm, -1}}},
  };
  return defs;
}

// 把槽位對到這個模型真實的參數 id。
//
// 兩段式：先掃標準 id，再用 cdi3 名稱關鍵字補。一個參數只能填一個槽位
//（否則「招手1」會同時當成 ArmWave 與 ArmWave2，揮手就變成整條手臂同相位擺動）。
std::map<Slot, std::string> resolveSlots(const std::vector<ParameterInfo>& params) {
  std::map<std::string, const ParameterInfo*> byId;
  for (const auto& p : params) byId[p.id] = &p;

  std::map<Slot, std::string> out;
  std::set<std::string> used;

  for (const auto& spec : slotSpecs()) {
    // 1. 標準 id
    bool filled = false;
    for (const char* id : spec.ids) {
      const auto it = byId.find(id);
      if (it == byId.end()) continue;
      // 寫進去每幀都被物理蓋掉的參數當不了動作（開了 allowPhysicsOutput 的槽位除外，
      // 那種會由 carryPastPhysics 在物理之後補寫）
      if (it->second->role == ParameterRole::PhysicsOutput && !spec.allowPhysicsOutput) continue;
      if (used.count(it->first)) continue;
      out[spec.slot] = it->first;
      used.insert(it->first);
      filled = true;
      break;
    }
    if (filled) continue;

    // 2. cdi3 名稱關鍵字。依模型自己的參數順序取第一個沒被用掉的
    for (const auto& p : params) {
      if (p.role == ParameterRole::PhysicsOutput && !spec.allowPhysicsOutput) continue;
      if (used.count(p.id)) continue;
      // 作者沒取名的參數（Name 就是 Id）名稱裡沒有語意，比中了也是巧合
      if (p.name == p.id) continue;
      const bool hit = std::any_of(spec.keywords.begin(), spec.keywords.end(), [&p](const char* kw) { return strutil::containsInsensitive(p.name, kw); });
      if (!hit) continue;
      out[spec.slot] = p.id;
      used.insert(p.id);
      break;
    }
  }
  return out;
}

bool hasAll(const std::map<Slot, std::string>& slots, const std::vector<Slot>& required) {
  return std::all_of(required.begin(), required.end(), [&slots](Slot s) { return slots.count(s) > 0; });
}

const ExpressionDef* findExpressionDef(const std::string& name) {
  for (const auto& def : expressionDefs()) {
    if (name == def.name) return &def;
  }
  return nullptr;
}

const MotionDef* findMotionDef(const std::string& name) {
  for (const auto& def : motionDefs()) {
    if (name == def.name) return &def;
  }
  return nullptr;
}

}  // namespace

std::vector<BuiltinActionInfo> availableBuiltinActions(const std::vector<ParameterInfo>& params) {
  const auto slots = resolveSlots(params);
  std::vector<BuiltinActionInfo> out;

  // 動作在前、表情在後 —— 設定畫面就是照這個順序排的
  for (const auto& def : motionDefs()) {
    if (!hasAll(slots, def.required)) continue;
    BuiltinActionInfo info;
    info.name = def.name;
    info.motion = true;
    std::set<std::string> seen;
    for (const auto& frame : def.frames) {
      for (const auto& v : frame.values) {
        const auto it = slots.find(v.slot);
        if (it == slots.end()) continue;
        if (!seen.insert(it->second).second) continue;
        info.params.push_back(it->second);
      }
    }
    out.push_back(std::move(info));
  }

  for (const auto& def : expressionDefs()) {
    if (!hasAll(slots, def.required)) continue;
    BuiltinActionInfo info;
    info.name = def.name;
    info.motion = false;
    for (const auto& v : def.values) {
      const auto it = slots.find(v.slot);
      if (it == slots.end()) continue;
      info.params.push_back(it->second);
    }
    out.push_back(std::move(info));
  }

  return out;
}

bool isBuiltinActionName(const std::string& name) { return findMotionDef(name) != nullptr || findExpressionDef(name) != nullptr; }

BuiltinExpressionApply applyBuiltinExpression(const std::vector<ParameterInfo>& params, const std::optional<std::string>& name) {
  const auto slots = resolveSlots(params);
  BuiltinExpressionApply out;

  // 選中的那一個要寫的參數
  std::set<std::string> keep;
  const ExpressionDef* chosen = name.has_value() ? findExpressionDef(*name) : nullptr;
  if (chosen && hasAll(slots, chosen->required)) {
    for (const auto& v : chosen->values) {
      const auto it = slots.find(v.slot);
      if (it == slots.end()) continue;
      SetParameterRequest req;
      req.id = it->second;
      req.value = v.value;
      req.mode = v.mode;
      req.durationMs = kExpressionFadeMs;
      out.set.push_back(std::move(req));
      keep.insert(it->second);
    }
  }

  // 其餘內建表情碰得到、而這一次沒有寫到的參數一律放掉（淡回模型自己的值）
  std::set<std::string> seen;
  for (const auto& def : expressionDefs()) {
    if (!hasAll(slots, def.required)) continue;
    for (const auto& v : def.values) {
      const auto it = slots.find(v.slot);
      if (it == slots.end()) continue;
      if (keep.count(it->second)) continue;
      if (!seen.insert(it->second).second) continue;
      out.release.push_back(it->second);
    }
  }

  return out;
}

std::optional<BuiltinMotion> builtinMotionFor(const std::vector<ParameterInfo>& params, const std::string& name) {
  const MotionDef* def = findMotionDef(name);
  if (!def) return std::nullopt;

  const auto slots = resolveSlots(params);
  if (!hasAll(slots, def->required)) return std::nullopt;

  std::map<std::string, const ParameterInfo*> byId;
  for (const auto& p : params) byId[p.id] = &p;

  BuiltinMotion out;
  out.options.loop = def->loop;
  out.options.fadeInMs = def->fadeInMs;
  out.options.fadeOutMs = def->fadeOutMs;
  std::set<std::string> carried;
  for (const auto& frame : def->frames) {
    Keyframe kf;
    kf.at = frame.at;
    for (const auto& v : frame.values) {
      const auto it = slots.find(v.slot);
      if (it == slots.end()) continue;
      kf.params.push_back({it->second, v.value});
      // 這隻模型把它綁成物理輸出的話，動作寫在物理之前會被蓋掉 ——
      // 列進來讓播放端在物理之後補寫一次（見 BuiltinMotion::carryPastPhysics）
      const auto info = byId.find(it->second);
      if (info != byId.end() && info->second->role == ParameterRole::PhysicsOutput && carried.insert(it->second).second) {
        out.carryPastPhysics.push_back(it->second);
      }
    }
    // required 保證至少有一個參數在，這裡只是防呆
    if (kf.params.empty()) continue;
    out.keyframes.push_back(std::move(kf));
  }
  if (out.keyframes.size() < 2) return std::nullopt;
  return out;
}

void removeBuiltinActions(ModelInfo& model) {
  if (!model.builtinMotions.empty()) {
    const auto& names = model.builtinMotions;
    model.motions.erase(std::remove_if(model.motions.begin(), model.motions.end(), [&names](const MotionGroupInfo& g) { return std::find(names.begin(), names.end(), g.name) != names.end(); }),
                        model.motions.end());
    model.builtinMotions.clear();
  }
  if (!model.builtinExpressions.empty()) {
    const auto& names = model.builtinExpressions;
    model.expressions.erase(std::remove_if(model.expressions.begin(), model.expressions.end(), [&names](const std::string& e) { return std::find(names.begin(), names.end(), e) != names.end(); }),
                            model.expressions.end());
    model.builtinExpressions.clear();
  }
}

}  // namespace l2m
