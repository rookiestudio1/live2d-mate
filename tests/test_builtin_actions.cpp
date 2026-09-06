// 內建動作／表情：槽位解析（標準 id 與 cdi3 名稱關鍵字）、可用性判定、
// 表情互斥時要放掉哪些參數、動作關鍵影格的參數代換
#include <QtTest>

#include <algorithm>
#include <string>
#include <vector>

#include "core/builtin_actions.h"
#include "core/model_commands.h"

using namespace l2m;

namespace {

ParameterInfo param(const char* id, const char* name, ParameterRole role = ParameterRole::Free) { return {id, name, "", role}; }

// Cubism 標準命名齊全的模型（Hiyori 那一類）
std::vector<ParameterInfo> standardModel() {
  return {
    param("ParamAngleX", "角度 X"),       param("ParamAngleY", "角度 Y"),        param("ParamAngleZ", "角度 Z"),         param("ParamEyeLOpen", "左眼 開閉"),    param("ParamEyeROpen", "右眼 開閉"),
    param("ParamEyeLSmile", "左眼 微笑"), param("ParamEyeRSmile", "右眼 微笑"),  param("ParamEyeBallX", "眼珠 X"),       param("ParamEyeBallY", "眼珠 Y"),       param("ParamBrowLY", "左眉 上下"),
    param("ParamBrowRY", "右眉 上下"),    param("ParamBrowLAngle", "左眉 角度"), param("ParamBrowRAngle", "右眉 角度"),  param("ParamMouthForm", "嘴 變形"),     param("ParamMouthOpenY", "嘴 開閉"),
    param("ParamArmRA", "右腕 A"),        param("ParamArmRB", "右腕 B"),         param("ParamBodyAngleY", "身體旋轉 Y"), param("ParamBodyAngleZ", "身體旋轉 Z"),
  };
}

// 身體角度被綁成物理輸出的模型（March 7th 那一類：ParamBodyAngleY 是
// ParamAngleY 經物理算出來的，也就是「頭往下看時身體會微蹲」的來源）
std::vector<ParameterInfo> physicsBodyModel() {
  auto params = standardModel();
  for (auto& p : params) {
    if (p.id == "ParamBodyAngleY" || p.id == "ParamBodyAngleZ") p.role = ParameterRole::PhysicsOutput;
  }
  return params;
}

// 頭部角度被綁成物理輸出的模型（ariu 那一類：ParamAngle{X,Y,Z} 同時是物理的
// Input 與 Output，把頭的慣性做成自我回授。三個角度全被判成 PhysicsOutput）
std::vector<ParameterInfo> physicsHeadModel() {
  auto params = standardModel();
  for (auto& p : params) {
    if (p.id == "ParamAngleX" || p.id == "ParamAngleY" || p.id == "ParamAngleZ") p.role = ParameterRole::PhysicsOutput;
  }
  return params;
}

// 使用者手上那隻魔女的縮影：手臂不是標準 id，只有作者在 cdi3 取的中文名
std::vector<ParameterInfo> witchModel() {
  return {
    param("ParamAngleX", "角度 X[AngleX]", ParameterRole::PhysicsInput),
    param("ParamAngleY", "角度 Y[AngleY]", ParameterRole::PhysicsInput),
    param("ParamAngleZ", "角度 Z[AngleZ]", ParameterRole::PhysicsInput),
    param("ParamEyeLOpen", "左眼　開閉[EyeLOpen]", ParameterRole::PhysicsInput),
    param("ParamEyeLSmile", "左眼　微笑[EyeLSmile]"),
    param("ParamEyeROpen", "右眼　開閉[EyeROpen]", ParameterRole::PhysicsInput),
    param("ParamEyeRSmile", "右眼　微笑[EyeRSmile]"),
    param("ParamMouthForm", "嘴部　變形[MouthForm]"),
    param("ParamMouthOpenY", "嘴巴　張開和閉合[MouthOpenY]", ParameterRole::PhysicsInput),
    param("Param28", "招手1"),
    param("Param30", "招手2"),
    param("Param38", "左耳1"),
    param("Param53", "生氣Angry"),
  };
}

std::vector<std::string> namesOf(const std::vector<BuiltinActionInfo>& actions) {
  std::vector<std::string> out;
  for (const auto& a : actions) out.push_back(a.name);
  return out;
}

const BuiltinActionInfo* find(const std::vector<BuiltinActionInfo>& actions, const char* name) {
  const auto it = std::find_if(actions.begin(), actions.end(), [name](const BuiltinActionInfo& a) { return a.name == name; });
  return it == actions.end() ? nullptr : &*it;
}

}  // namespace

class TestBuiltinActions : public QObject {
  Q_OBJECT

private slots:
  // === 可用性 ===

  // 標準命名的模型十五個內建全中，動作排在表情前面
  void standardModelOffersEverything() {
    QCOMPARE(namesOf(availableBuiltinActions(standardModel())),
             (std::vector<std::string>{"wave", "nod", "shake", "tilt", "grin", "wink", "look_away", "sigh", "doze", "yawn", "excited", "smile", "surprised", "sleepy", "sad", "angry"}));
  }

  // 沒有參數表（模型沒有 cdi3.json）時一個都不提供
  void noParametersOffersNothing() { QVERIFY(availableBuiltinActions({}).empty()); }

  // 只有頭部角度的模型至少拿得到點頭與搖頭 —— 這一區不會整個空掉。
  // look_away 也只吃 AngleX、sigh 吃 AngleX+AngleY，所以一起進來
  //（EyeBallX／EyeBallY／眉毛都是選用的）
  void angleOnlyModelStillNodsAndShakes() {
    const std::vector<ParameterInfo> params = {param("ParamAngleX", "角度 X"), param("ParamAngleY", "角度 Y")};
    QCOMPARE(namesOf(availableBuiltinActions(params)), (std::vector<std::string>{"nod", "shake", "look_away", "sigh"}));
  }

  // 沒有眉毛角度的模型只拿得到 sad：sad 與 angry 的差別全在眉毛角度，
  // 兩個長得一模一樣的項目比少一個更糟 —— AI 選哪個都一樣就等於沒得選
  void browlessModelDropsAngryButKeepsSad() {
    auto params = standardModel();
    params.erase(std::remove_if(params.begin(), params.end(), [](const ParameterInfo& p) { return p.id.rfind("ParamBrow", 0) == 0; }), params.end());
    const auto names = namesOf(availableBuiltinActions(params));
    QVERIFY(std::find(names.begin(), names.end(), "sad") != names.end());
    QVERIFY(std::find(names.begin(), names.end(), "angry") == names.end());
  }

  // 新槽位真的解析得到：別開視線會用上眼珠參數
  void lookAwayResolvesEyeBall() {
    const auto actions = availableBuiltinActions(standardModel());
    const BuiltinActionInfo* look = find(actions, "look_away");
    QVERIFY(look != nullptr);
    QVERIFY(std::find(look->params.begin(), look->params.end(), "ParamEyeBallX") != look->params.end());
  }

  // required 槽位缺席就整個不提供：沒有手臂參數就沒有 wave
  void missingRequiredSlotDropsAction() {
    auto params = standardModel();
    params.erase(std::remove_if(params.begin(), params.end(), [](const ParameterInfo& p) { return p.id == "ParamArmRA" || p.id == "ParamArmRB"; }), params.end());
    const auto names = namesOf(availableBuiltinActions(params));
    QVERIFY(std::find(names.begin(), names.end(), "wave") == names.end());
    QVERIFY(std::find(names.begin(), names.end(), "nod") != names.end());
  }

  // 選用槽位缺席只是少寫那一項，動作照樣提供
  void missingOptionalSlotKeepsAction() {
    std::vector<ParameterInfo> params = {param("ParamArmRA", "右腕 A")};
    const auto actions = availableBuiltinActions(params);
    const BuiltinActionInfo* wave = find(actions, "wave");
    QVERIFY(wave != nullptr);
    QCOMPARE(wave->params, (std::vector<std::string>{"ParamArmRA"}));
  }

  // === 槽位解析 ===

  // 標準 id 不在時改用 cdi3 名稱：魔女的「招手1／招手2」對到 ArmWave 與 ArmWave2
  void keywordFallbackResolvesWitchArms() {
    const auto actions = availableBuiltinActions(witchModel());
    const BuiltinActionInfo* wave = find(actions, "wave");
    QVERIFY(wave != nullptr);
    // 順序＝槽位在關鍵影格裡首次出現的順序：ArmWave、ArmWave2、AngleZ
    QCOMPARE(wave->params, (std::vector<std::string>{"Param28", "Param30", "ParamAngleZ"}));
  }

  // 一個參數只能填一個槽位，否則「招手1」會同時當成兩隻手臂而同相位擺動
  void oneParameterFillsOnlyOneSlot() {
    std::vector<ParameterInfo> params = {param("Param28", "招手1")};
    const auto actions = availableBuiltinActions(params);
    const BuiltinActionInfo* wave = find(actions, "wave");
    QVERIFY(wave != nullptr);
    QCOMPARE(wave->params, (std::vector<std::string>{"Param28"}));
  }

  // 標準 id 優先於名稱關鍵字
  void standardIdWinsOverKeyword() {
    std::vector<ParameterInfo> params = {param("Param28", "招手1"), param("ParamArmRA", "右腕 A")};
    const auto actions = availableBuiltinActions(params);
    const BuiltinActionInfo* wave = find(actions, "wave");
    QVERIFY(wave != nullptr);
    QCOMPARE(wave->params[0], std::string("ParamArmRA"));
  }

  // 寫進去每幀都被物理蓋掉的參數不能當動作（沒開 allowPhysicsOutput 的槽位）
  void physicsOutputNeverResolves() {
    std::vector<ParameterInfo> params = {param("ParamMouthOpenY", "嘴 開閉", ParameterRole::PhysicsOutput), param("Param28", "招手1", ParameterRole::PhysicsOutput)};
    QVERIFY(availableBuiltinActions(params).empty());
  }

  // 頭部角度被綁成物理輸出（ariu 那一類自我回授的慣性 rig）時，保底的那一批
  // 動作仍然要生得出來 —— 少了它們這種模型只剩四個內建動作
  void headAnglePhysicsOutputStillResolves() {
    const auto params = physicsHeadModel();
    const auto actions = availableBuiltinActions(params);
    for (const char* name : {"nod", "shake", "tilt", "look_away", "sigh", "doze"}) {
      QVERIFY2(find(actions, name) != nullptr, name);
    }
  }

  // ……而且那些值要列進 carryPastPhysics，播放端才會在物理之後補寫一次
  void headAnglePhysicsOutputCarriesPastPhysics() {
    const auto nod = builtinMotionFor(physicsHeadModel(), "nod");
    QVERIFY(nod.has_value());
    QCOMPARE(nod->carryPastPhysics, (std::vector<std::string>{"ParamAngleY"}));

    // tilt 同時寫 AngleZ 與 AngleX，兩個都要帶（順序照關鍵影格裡第一次出現的先後）
    const auto tilt = builtinMotionFor(physicsHeadModel(), "tilt");
    QVERIFY(tilt.has_value());
    QCOMPARE(tilt->carryPastPhysics, (std::vector<std::string>{"ParamAngleZ", "ParamAngleX"}));

    // 對照組：角度不是物理輸出的一般模型，這條路完全不啟用
    const auto plain = builtinMotionFor(standardModel(), "nod");
    QVERIFY(plain.has_value());
    QVERIFY(plain->carryPastPhysics.empty());
  }

  // AngleY 開了 allowPhysicsOutput 之後，槽位表的順序才變得有意義：關鍵字是
  // 不分大小寫的子字串比對，"bodyangley" 本身就含有 "angley"。BodyAngle* 若排在
  // 後面，這種「沒有標準 id、只有 cdi3 名稱」的身體參數會被 AngleY 搶走 ——
  // excited 整個消失，nod 還會把點頭曲線寫到身體上。
  void bodyAngleKeywordIsNotStolenByHeadAngle() {
    const std::vector<ParameterInfo> params = {
      param("Param10", "身體旋轉 Y[BodyAngleY]", ParameterRole::PhysicsOutput),
      param("ParamEyeLOpen", "左眼 開閉"),
      param("ParamEyeROpen", "右眼 開閉"),
    };
    // excited 的 required 是 {BodyAngleY, EyeLOpen, EyeROpen}：身體參數被搶走就整個不見
    const auto excited = builtinMotionFor(params, "excited");
    QVERIFY(excited.has_value());
    QCOMPARE(excited->carryPastPhysics, (std::vector<std::string>{"Param10"}));

    // 這隻模型沒有任何頭部角度：nod 不該存在，更不該落到那個身體參數上
    QVERIFY(!builtinMotionFor(params, "nod").has_value());
  }

  // 同一件事的 X 版本。X 比 Y 難發現得多：沒有任何內建的 required 掛在身體 X 上，
  // 所以不會有東西從 list_motions 消失 —— 只是 shake／tilt／look_away／sigh 把搖頭的
  // 曲線寫到**身體 X 旋轉**上，動作照播、部位全錯、一句錯誤訊息都沒有。
  void bodyAngleXKeywordIsNotStolenByHeadAngle() {
    const std::vector<ParameterInfo> params = {
      // 身體參數刻意排在頭部參數前面：槽位是先到先得，順序錯的時候就是這樣被搶走的
      param("Param01", "身體旋轉 X[BodyAngleX]"),
      param("Param04", "角度 X[AngleX]"),
      param("ParamEyeLOpen", "左眼 開閉"),
      param("ParamEyeROpen", "右眼 開閉"),
    };
    const auto shake = builtinMotionFor(params, "shake");
    QVERIFY(shake.has_value());

    bool touchesHead = false;
    bool touchesBody = false;
    for (const auto& kf : shake->keyframes) {
      for (const auto& p : kf.params) {
        if (p.first == "Param04") touchesHead = true;
        if (p.first == "Param01") touchesBody = true;
      }
    }
    QVERIFY(touchesHead);
    QVERIFY(!touchesBody);
  }

  // 作者沒取名的參數（Name 就是 Id）名稱裡沒有語意，不拿來比對
  void unnamedParameterIsNotMatched() {
    std::vector<ParameterInfo> params = {param("wave", "wave")};
    QVERIFY(availableBuiltinActions(params).empty());
  }

  // 關鍵字保守：不會把「左耳1」「生氣Angry」這種無關參數收進手臂槽位
  void keywordsDoNotOvermatch() {
    std::vector<ParameterInfo> params = {param("Param38", "左耳1"), param("Param53", "生氣Angry"), param("Param99", "右腿")};
    QVERIFY(availableBuiltinActions(params).empty());
  }

  // === 表情 ===

  // 套用 smile 寫 3 個參數，且不會去放掉自己正在寫的那些
  void smileWritesItsOwnParameters() {
    const auto apply = applyBuiltinExpression(standardModel(), std::string("smile"));
    QCOMPARE(apply.set.size(), size_t(3));
    QCOMPARE(apply.set[0].id, std::string("ParamMouthForm"));
    QCOMPARE(apply.set[0].value, 1.0);
    QVERIFY(apply.set[0].durationMs.has_value());
    QCOMPARE(*apply.set[0].durationMs, kExpressionFadeMs);
    for (const auto& req : apply.set) {
      QVERIFY(std::find(apply.release.begin(), apply.release.end(), req.id) == apply.release.end());
    }
  }

  // 切到 sleepy 時，smile 用過而 sleepy 沒用到的參數要被放掉（淡回模型自己的值）——
  // 這裡不能像 virtualExpressionParams 那樣一律寫 0：ParamEyeLOpen 的靜止值是 1
  void switchingReleasesLeftoverParameters() {
    const auto apply = applyBuiltinExpression(standardModel(), std::string("sleepy"));
    QVERIFY(std::find(apply.release.begin(), apply.release.end(), "ParamEyeLSmile") != apply.release.end());
    QVERIFY(std::find(apply.release.begin(), apply.release.end(), "ParamEyeLOpen") == apply.release.end());
  }

  // 眉毛齊全時 sad 與 angry 的眉毛角度必須反向（八字眉 vs 豎眉），
  // 同向的話兩個表情在畫面上就分不出來了
  void sadAndAngryDifferInBrowAngle() {
    // 哨兵用 999 而不是 0：這支測試要斷言的正是「根本沒寫入」，
    // 用 0 的話跟「真的寫了 0」分不開
    const auto valueOf = [](const BuiltinExpressionApply& apply, const char* id) {
      for (const auto& req : apply.set) {
        if (req.id == id) return req.value;
      }
      return 999.0;
    };
    const auto sad = applyBuiltinExpression(standardModel(), std::string("sad"));
    const auto angry = applyBuiltinExpression(standardModel(), std::string("angry"));
    QVERIFY(valueOf(sad, "ParamBrowLAngle") < 0);
    QVERIFY(valueOf(angry, "ParamBrowLAngle") > 0);
    // 兩個都不碰眼睛開閉：釘住就不眨眼了，而這兩個表情會掛很久
    QCOMPARE(valueOf(sad, "ParamEyeLOpen"), 999.0);
    QCOMPARE(valueOf(angry, "ParamEyeLOpen"), 999.0);
  }

  // 吃驚是「瞪大眼睛＋頭往上仰一點」。仰頭那一項必須是**疊加**：
  // 表情是掛著不走的，用覆寫釘住 ParamAngleY 就等於在這個表情期間關掉視線追蹤
  //（頭不再跟著游標轉）。其餘的臉部參數照舊是覆寫
  void surprisedTiltsTheHeadUpAsAnAddition() {
    const auto apply = applyBuiltinExpression(standardModel(), std::string("surprised"));
    const auto reqOf = [&apply](const char* id) -> const SetParameterRequest* {
      for (const auto& req : apply.set) {
        if (req.id == id) return &req;
      }
      return nullptr;
    };
    const SetParameterRequest* angle = reqOf("ParamAngleY");
    QVERIFY(angle != nullptr);
    QVERIFY(angle->value > 0);  // 正值是抬頭
    QCOMPARE(angle->mode, ParameterMode::Add);
    const SetParameterRequest* eye = reqOf("ParamEyeLOpen");
    QVERIFY(eye != nullptr);
    QVERIFY(eye->value > 1.0);  // 瞪大：超過靜止的 1，範圍只到 1.0 的模型由 Cubism 夾回去
    QCOMPARE(eye->mode, ParameterMode::Set);
    // 切走時要把仰頭放掉，否則頭會一直仰著
    const auto next = applyBuiltinExpression(standardModel(), std::string("smile"));
    QVERIFY(std::find(next.release.begin(), next.release.end(), "ParamAngleY") != next.release.end());
  }

  // 清除（nullopt）時什麼都不寫，內建表情碰得到的參數全部放掉
  void clearingReleasesEverything() {
    const auto apply = applyBuiltinExpression(standardModel(), std::nullopt);
    QVERIFY(apply.set.empty());
    for (const char* id : {"ParamMouthForm", "ParamEyeLSmile", "ParamEyeRSmile", "ParamEyeLOpen", "ParamEyeROpen", "ParamMouthOpenY"}) {
      QVERIFY2(std::find(apply.release.begin(), apply.release.end(), id) != apply.release.end(), id);
    }
  }

  // 不是內建表情的名稱等同清除，不會誤寫參數
  void unknownNameBehavesLikeClear() {
    const auto apply = applyBuiltinExpression(standardModel(), std::string("星星眼"));
    QVERIFY(apply.set.empty());
    QVERIFY(!apply.release.empty());
  }

  // 這個模型撐不起來的內建表情不會被套用
  void unavailableExpressionWritesNothing() {
    const std::vector<ParameterInfo> params = {param("ParamAngleY", "角度 Y")};
    const auto apply = applyBuiltinExpression(params, std::string("smile"));
    QVERIFY(apply.set.empty());
    QVERIFY(apply.release.empty());
  }

  // === 動作 ===

  // 關鍵影格裡的槽位換成這個模型真正的參數 id
  void motionKeyframesUseResolvedIds() {
    const auto motion = builtinMotionFor(witchModel(), "wave");
    QVERIFY(motion.has_value());
    QCOMPARE(motion->keyframes.size(), size_t(6));
    QCOMPARE(motion->keyframes[0].at, 0.0);
    QCOMPARE(motion->keyframes[0].params[0].first, std::string("Param28"));
    QCOMPARE(motion->keyframes[1].params[0].second, 30.0);
    QVERIFY(!motion->options.loop);
    QCOMPARE(*motion->options.fadeInMs, 200.0);
  }

  // 選用槽位缺席時，關鍵影格只留解析得到的那些參數
  void motionSkipsUnresolvedSlots() {
    const std::vector<ParameterInfo> params = {param("ParamArmRA", "右腕 A")};
    const auto motion = builtinMotionFor(params, "wave");
    QVERIFY(motion.has_value());
    for (const auto& kf : motion->keyframes) {
      QCOMPARE(kf.params.size(), size_t(1));
      QCOMPARE(kf.params[0].first, std::string("ParamArmRA"));
    }
  }

  // 編得出 motion3.json（buildMotion3 對關鍵影格有最少兩格、時間遞增等要求）
  void motionCompilesToMotion3() {
    const auto motion = builtinMotionFor(standardModel(), "nod");
    QVERIFY(motion.has_value());
    const Motion3 built = buildMotion3(motion->keyframes, motion->options);
    QCOMPARE(built.meta.duration, 1.2);
    QCOMPARE(built.curves.size(), size_t(1));
    QCOMPARE(built.curves[0].id, std::string("ParamAngleY"));
  }

  // 雙眼閉合的笑臉刻意做成動作而不是表情：表情掛著不走，眼睛被釘在 0 就再也不會眨
  //（覆寫層在晚寫掛點、眨眼在它前面），掛久了是睡臉不是笑臉。
  // 動作有時長，最後一格把眼睛寫回 1，播完不會停在閉眼
  void grinIsAMotionSoTheEyesReopen() {
    const auto actions = availableBuiltinActions(standardModel());
    const BuiltinActionInfo* grin = find(actions, "grin");
    QVERIFY(grin != nullptr);
    QVERIFY(grin->motion);
    const auto motion = builtinMotionFor(standardModel(), "grin");
    QVERIFY(motion.has_value());
    QCOMPARE(motion->keyframes.back().params[0].first, std::string("ParamEyeLOpen"));
    QCOMPARE(motion->keyframes.back().params[0].second, 1.0);
  }

  // 嘆氣的語意就是先後順序：低頭與視線垂下在前，搖頭在後。
  // 而且搖頭那一段必須維持低著 —— 中途沒有把 AngleY 再寫一次的話，
  // buildMotion3 會從低頭那格一路內插回 0，變成邊抬頭邊搖頭
  void sighLowersHeadBeforeShaking() {
    const auto motion = builtinMotionFor(standardModel(), "sigh");
    QVERIFY(motion.has_value());
    const auto firstAt = [&motion](const char* id, double want) {
      for (const auto& kf : motion->keyframes) {
        for (const auto& p : kf.params) {
          if (p.first == id && p.second == want) return kf.at;
        }
      }
      return -1.0;
    };
    const double headDown = firstAt("ParamAngleY", -26);
    const double gazeDown = firstAt("ParamEyeBallY", -0.85);
    const double firstSwing = firstAt("ParamAngleX", -10);
    QVERIFY(headDown > 0);
    QCOMPARE(gazeDown, headDown);
    QVERIFY(firstSwing > headDown);
    // 搖頭結束時還低著頭，最後一格才回正
    QVERIFY(firstAt("ParamAngleY", -26) < motion->keyframes.back().at);
    QCOMPARE(motion->keyframes.back().params[0].second, 0.0);
  }

  // 嘆氣的臉借的是 sad 那一組（八字眉＋嘴角下垂），眼睛半睜到 0.7 ——
  // 光低頭搖頭的語意是「否定」，難過的臉才把它定成嘆氣。
  // 這是動作不是表情，所以最後一格一定要把臉與眼睛都寫回靜止值：
  // 停在難過的臉或半睜的眼上，接回待機就是一張壞掉的臉（同 yawn 的理由）
  void sighWearsASadFaceAndNarrowsTheEyes() {
    const auto motion = builtinMotionFor(standardModel(), "sigh");
    QVERIFY(motion.has_value());
    const auto valueAt = [&motion](double at, const char* id) {
      for (const auto& kf : motion->keyframes) {
        if (kf.at != at) continue;
        for (const auto& p : kf.params) {
          if (p.first == id) return p.second;
        }
      }
      return 999.0;
    };
    const double last = motion->keyframes.back().at;
    // 低頭的那一格臉就到位（八字眉是負的角度，嘴角往下）
    QCOMPARE(valueAt(650, "ParamBrowLAngle"), -1.0);
    QCOMPARE(valueAt(650, "ParamMouthForm"), -1.0);
    QCOMPARE(valueAt(650, "ParamEyeLOpen"), 0.7);
    QCOMPARE(valueAt(650, "ParamEyeROpen"), 0.7);
    // 搖頭途中（3900）要再寫一次，不然會從 650 一路淡回去，搖到一半臉就先笑回來
    QCOMPARE(valueAt(3900, "ParamMouthForm"), -1.0);
    QCOMPARE(valueAt(3900, "ParamEyeLOpen"), 0.7);
    // 最後一格全部回到靜止值
    QCOMPARE(valueAt(last, "ParamBrowLAngle"), 0.0);
    QCOMPARE(valueAt(last, "ParamBrowRAngle"), 0.0);
    QCOMPARE(valueAt(last, "ParamMouthForm"), 0.0);
    QCOMPARE(valueAt(last, "ParamEyeLOpen"), 1.0);
    QCOMPARE(valueAt(last, "ParamEyeROpen"), 1.0);
  }

  // 打瞌睡是唯一循環的內建動作：眼睛整段閉著，而且**首尾的頭部角度必須同值** ——
  // 不同值的話每一圈的接點都會跳一下。收掉它的是下一個動作或 stopLoopingAiMotion()
  void dozeLoopsWithEyesClosed() {
    const auto motion = builtinMotionFor(standardModel(), "doze");
    QVERIFY(motion.has_value());
    QVERIFY(motion->options.loop);
    const auto valueAt = [&motion](size_t frame, const char* id) {
      for (const auto& p : motion->keyframes[frame].params) {
        if (p.first == id) return p.second;
      }
      return 999.0;
    };
    const size_t last = motion->keyframes.size() - 1;
    QCOMPARE(valueAt(0, "ParamEyeLOpen"), 0.0);
    QCOMPARE(valueAt(last, "ParamEyeLOpen"), 0.0);
    QCOMPARE(valueAt(0, "ParamAngleY"), valueAt(last, "ParamAngleY"));
    // 頭全程垂著：點頭的頂點也不回到 0
    for (const auto& kf : motion->keyframes) {
      for (const auto& p : kf.params) {
        if (p.first == "ParamAngleY") QVERIFY2(p.second < 0, qPrintable(QString::number(kf.at)));
      }
    }
    // 其他內建動作都不循環（播完自己結束）
    QVERIFY(!builtinMotionFor(standardModel(), "nod")->options.loop);
    QVERIFY(!builtinMotionFor(standardModel(), "sigh")->options.loop);
  }

  // 打哈欠張到最大時眼睛是閉著的，最後一格兩者都回到靜止值 ——
  // 停在半開的嘴或閉著的眼上，接回待機就是一張壞掉的臉
  void yawnOpensMouthAndClosesEyes() {
    const auto motion = builtinMotionFor(standardModel(), "yawn");
    QVERIFY(motion.has_value());
    const auto valueAt = [&motion](double at, const char* id) {
      for (const auto& kf : motion->keyframes) {
        if (kf.at != at) continue;
        for (const auto& p : kf.params) {
          if (p.first == id) return p.second;
        }
      }
      return 999.0;
    };
    QCOMPARE(valueAt(1100, "ParamMouthOpenY"), 1.0);
    QCOMPARE(valueAt(1100, "ParamEyeLOpen"), 0.0);
    // 仰頭跟著張嘴一起到頂（正值是抬頭），而且要看得出來 —— 幅度太小的話
    // 在 400×600 的舞台上只看得出嘴在開。收尾則是垂下去（負值）才有鬆懈感
    QVERIFY(valueAt(1100, "ParamAngleY") > valueAt(500, "ParamAngleY"));
    QVERIFY(valueAt(1100, "ParamAngleY") >= 15);
    QVERIFY(valueAt(2600, "ParamAngleY") < 0);
    const double last = motion->keyframes.back().at;
    QCOMPARE(valueAt(last, "ParamMouthOpenY"), 0.0);
    QCOMPARE(valueAt(last, "ParamEyeLOpen"), 1.0);
    // 沒有嘴部開閉參數的模型不提供（哈欠不張嘴就只是在點頭）
    const std::vector<ParameterInfo> noMouth = {param("ParamEyeLOpen", "左眼 開閉"), param("ParamEyeROpen", "右眼 開閉"), param("ParamAngleY", "角度 Y")};
    QVERIFY(!builtinMotionFor(noMouth, "yawn").has_value());
  }

  // 興奮＝笑瞇眼 + 身體上下彈兩下，**頭一格都不碰**。
  // 「跳」沒有現成的參數，畫面上做出「微蹲 → 伸展」的是 ParamBodyAngleY，
  // 所以要驗的是「真的有兩下、兩下之間有蹲回去」，以及「完全沒寫頭部角度」——
  // 只要 AngleY 沾上一點，這個動作就退化成點頭了
  void excitedBouncesTheBodyTwiceWithoutMovingTheHead() {
    const auto motion = builtinMotionFor(standardModel(), "excited");
    QVERIFY(motion.has_value());
    const auto valueAt = [&motion](double at, const char* id) {
      for (const auto& kf : motion->keyframes) {
        if (kf.at != at) continue;
        for (const auto& p : kf.params) {
          if (p.first == id) return p.second;
        }
      }
      return 999.0;
    };
    // 蓄力那一格先蹲（負值），兩個彈起的頂點等高，中間蹲回負值
    QVERIFY(valueAt(250, "ParamBodyAngleY") < 0);
    QVERIFY(valueAt(500, "ParamBodyAngleY") > 0);
    QCOMPARE(valueAt(1000, "ParamBodyAngleY"), valueAt(500, "ParamBodyAngleY"));
    QVERIFY(valueAt(750, "ParamBodyAngleY") < 0);
    // 兩下的左右擺動反向，否則第二下跟第一下一模一樣像跳針
    QCOMPARE(valueAt(1000, "ParamBodyAngleZ"), -valueAt(500, "ParamBodyAngleZ"));
    // 頭部角度一個都不准出現
    for (const auto& kf : motion->keyframes) {
      for (const auto& p : kf.params) {
        QVERIFY2(p.first.rfind("ParamAngle", 0) != 0, p.first.c_str());
      }
    }
    // 笑瞇眼：蓄力那一格臉就到位，最後一格睜回來（停在閉眼上接回待機是睡臉）
    QCOMPARE(valueAt(250, "ParamEyeLOpen"), 0.0);
    QCOMPARE(valueAt(250, "ParamEyeLSmile"), 1.0);
    QCOMPARE(valueAt(250, "ParamMouthForm"), 1.0);
    const double last = motion->keyframes.back().at;
    QCOMPARE(valueAt(last, "ParamEyeLOpen"), 1.0);
    QCOMPARE(valueAt(last, "ParamEyeRSmile"), 0.0);
  }

  // 身體角度是唯一收「物理輸出」的槽位：很多模型的 ParamBodyAngleY 是由
  // ParamAngleY 經物理算出來的（頭往下看身體就微蹲）。跳過那種參數的話，
  // 身體就只能靠甩頭去帶 —— 那是點頭不是彈跳。所以照收，改成把值列進
  // carryPastPhysics，交給播放端在物理之後補寫一次
  void excitedReportsPhysicsDrivenBodyForLateWrite() {
    // 身體寫得動的模型不需要補寫
    const auto free = builtinMotionFor(standardModel(), "excited");
    QVERIFY(free.has_value());
    QVERIFY(free->carryPastPhysics.empty());

    // 身體被物理接管的模型：動作照樣成立，但要指名哪幾個得越過物理
    const auto driven = builtinMotionFor(physicsBodyModel(), "excited");
    QVERIFY(driven.has_value());
    QCOMPARE(driven->carryPastPhysics, (std::vector<std::string>{"ParamBodyAngleY", "ParamBodyAngleZ"}));
    // 關鍵影格本身完全一樣 —— 差別只在播放端要不要補寫
    QCOMPARE(driven->keyframes.size(), free->keyframes.size());

    // 設定畫面也看得到身體參數（關鍵字比對有猜的成分，使用者要看得見）
    const auto actions = availableBuiltinActions(physicsBodyModel());
    const BuiltinActionInfo* excited = find(actions, "excited");
    QVERIFY(excited != nullptr);
    QVERIFY(excited->motion);
    QVERIFY(std::find(excited->params.begin(), excited->params.end(), "ParamBodyAngleY") != excited->params.end());
  }

  // 沒有身體角度的模型不提供興奮：這個動作的內容就是身體在跳，
  // 退化成「只有一張笑臉」的版本沒有意義
  void excitedNeedsABody() {
    auto noBody = standardModel();
    noBody.erase(std::remove_if(noBody.begin(), noBody.end(), [](const ParameterInfo& p) { return p.id.rfind("ParamBodyAngle", 0) == 0; }), noBody.end());
    const auto names = namesOf(availableBuiltinActions(noBody));
    QVERIFY(std::find(names.begin(), names.end(), "excited") == names.end());
    QVERIFY(!builtinMotionFor(noBody, "excited").has_value());
    // 其他動作不受影響
    QVERIFY(std::find(names.begin(), names.end(), "grin") != names.end());
  }

  // 拋媚眼是「眨一下就睜開」，所以是動作不是表情：表情掛著不走，眼睛被釘在 0
  // 就一直閉著，那不是拋媚眼是瞎了一隻眼。只閉左眼，右眼從頭到尾不碰
  void winkReopensTheEye() {
    const auto actions = availableBuiltinActions(standardModel());
    const BuiltinActionInfo* wink = find(actions, "wink");
    QVERIFY(wink != nullptr);
    QVERIFY(wink->motion);
    QVERIFY(std::find(wink->params.begin(), wink->params.end(), "ParamEyeROpen") == wink->params.end());
    const auto motion = builtinMotionFor(standardModel(), "wink");
    QVERIFY(motion.has_value());
    QVERIFY(!motion->options.loop);
    QCOMPARE(motion->keyframes[0].params[0].first, std::string("ParamEyeLOpen"));
    QCOMPARE(motion->keyframes[0].params[0].second, 1.0);
    QCOMPARE(motion->keyframes.back().params[0].second, 1.0);
    // 而且它已經不是表情了：套用它什麼都不會寫
    QVERIFY(applyBuiltinExpression(standardModel(), std::string("wink")).set.empty());
  }

  // 名稱不是內建動作、或這個模型撐不起來時回 nullopt
  void unknownOrUnavailableMotionReturnsNullopt() {
    QVERIFY(!builtinMotionFor(standardModel(), "Idle").has_value());
    QVERIFY(!builtinMotionFor(standardModel(), "smile").has_value());
    QVERIFY(!builtinMotionFor({param("ParamAngleY", "角度 Y")}, "wave").has_value());
  }

  // === 名稱 ===

  // 撞名判定用的查詢涵蓋動作與表情兩張表
  void knowsItsOwnNames() {
    for (const char* name : {"wave", "nod", "shake", "tilt", "grin", "look_away", "sigh", "doze", "yawn", "excited", "smile", "wink", "surprised", "sleepy", "sad", "angry"}) {
      QVERIFY2(isBuiltinActionName(name), name);
    }
    QVERIFY(!isBuiltinActionName("Idle"));
    QVERIFY(!isBuiltinActionName("Wave"));
  }

  // === 拿掉內建項目（Live2D Viewer 用） ===

  // 只有登記在 builtinMotions／builtinExpressions 裡的才會被拿掉，
  // 模型自己做的一個都不能少，順序也要維持原樣。
  void removeBuiltinActionsKeepsOnlyAuthoredItems() {
    ModelInfo model;
    model.motions = {{"Idle", 2, {}}, {"nod", 1, {}}, {"TapBody", 1, {}}, {"wave", 1, {}}};
    model.expressions = {"照相", "smile", "脸红", "angry"};
    model.builtinMotions = {"nod", "wave"};
    model.builtinExpressions = {"smile", "angry"};

    removeBuiltinActions(model);

    QCOMPARE(model.motions.size(), size_t(2));
    QCOMPARE(model.motions[0].name, std::string("Idle"));
    QCOMPARE(model.motions[0].count, 2);
    QCOMPARE(model.motions[1].name, std::string("TapBody"));
    QCOMPARE(model.expressions, (std::vector<std::string>{"照相", "脸红"}));
    // 清單清空之後，action_player 的兩支就不會再把任何名稱當成內建的來解析
    QVERIFY(model.builtinMotions.empty());
    QVERIFY(model.builtinExpressions.empty());
  }

  // 模型自己就有同名動作時，掃描端根本不會把它登記成內建的（撞名一律跳過），
  // 所以這裡也不該把作者做的那一個誤刪。
  void removeBuiltinActionsSpareSameNameAuthoredItems() {
    ModelInfo model;
    model.motions = {{"wave", 3, {}}};
    model.expressions = {"smile"};

    removeBuiltinActions(model);

    QCOMPARE(model.motions.size(), size_t(1));
    QCOMPARE(model.motions[0].count, 3);
    QCOMPARE(model.expressions, (std::vector<std::string>{"smile"}));
  }
};

QTEST_APPLESS_MAIN(TestBuiltinActions)
#include "test_builtin_actions.moc"
