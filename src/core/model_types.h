#pragma once

// 模型相關的共用型別。
//
// 本專案不支援 Cubism 2.1：所有模型一律是 Cubism 3/4/5（.moc3），
// 所以不攜帶版本欄位。

#include <map>
#include <string>
#include <vector>

namespace l2m {

struct MotionGroupInfo {
  std::string name;
  int count = 0;
  // 群組內每個索引對應的動作檔名（只留檔名），用來在命名視窗裡辨識是哪一段
  std::vector<std::string> files;

  bool operator==(const MotionGroupInfo& other) const { return name == other.name && count == other.count && files == other.files; }
};

// 參數在物理演算裡的角色，決定它能不能被外部寫入、以及要在哪個時機寫。
//
//  - Free           物理碰不到，隨時可寫
//  - PhysicsInput   物理的輸入來源，要在 physics.evaluate() 之前寫，頭髮衣服才會跟著反應
//  - PhysicsOutput  物理的輸出目標，外部寫了每一幀都會被覆蓋掉
enum class ParameterRole { Free, PhysicsInput, PhysicsOutput };

// 模型的一個參數。名稱與群組來自 cdi3.json，角色來自 physics3.json。
struct ParameterInfo {
  // 參數 id，例如 expression6
  std::string id;
  // 作者取的名稱，例如「生氣」；作者沒取名時退回 id
  std::string name;
  // 作者分的群組名稱，例如「表情」；沒分組時是空字串
  std::string group;
  ParameterRole role = ParameterRole::Free;

  bool operator==(const ParameterInfo& other) const { return id == other.id && name == other.name && group == other.group && role == other.role; }
};

// 參數的現值與模型宣告的範圍。
// 名稱與物理角色來自 cdi3/physics3（ParameterInfo），這裡則是 Cubism 模型當下的實際數值，
// 兩者在 parameterReport 才合併成一張表給 AI 看。
struct ParameterSnapshot {
  std::string id;
  double value = 0;
  double min = 0;
  double max = 0;
  double defaultValue = 0;

  bool operator==(const ParameterSnapshot& other) const { return id == other.id && value == other.value && min == other.min && max == other.max && defaultValue == other.defaultValue; }
};

// 由參數升級而來的虛擬表情。
//
// 給臉部追蹤用的 VTuber 模型常常一個 .exp3.json 都沒有，表情全部做成「開關參數」，
// 這裡把它們包成表情的樣子，list_expressions 與命名視窗就不必為這種模型改寫。
struct ParamExpressionRef {
  std::string name;
  std::string param;
  double value = 1;

  bool operator==(const ParamExpressionRef& other) const { return name == other.name && param == other.param && value == other.value; }
};

// 動作／表情的「意義」對應。
//
// 很多模型的動作叫 mtn_03、表情叫 F04，AI 光看名稱根本無從選起，
// 所以讓使用者替每一項寫下人看得懂的說明，再一起餵給 AI。
struct ModelAnnotations {
  // key 為 motionKey()，value 為使用者輸入的意義
  std::map<std::string, std::string> motions;
  std::map<std::string, std::string> expressions;

  bool empty() const { return motions.empty() && expressions.empty(); }
  bool operator==(const ModelAnnotations& other) const { return motions == other.motions && expressions == other.expressions; }
};

struct ModelInfo {
  // models 目錄下的相對路徑（含檔名），同時作為唯一鍵
  std::string id;
  // 顯示名稱，取自模型所在資料夾名
  std::string name;
  // renderer 可直接載入的 live2d:// URL
  std::string url;
  std::vector<MotionGroupInfo> motions;
  // 表情名稱；沒有 .exp3.json 的模型會用 paramExpressions 的名稱頂上
  std::vector<std::string> expressions;
  // 模型的全部參數與物理角色
  std::vector<ParameterInfo> parameters;
  // expressions 裡哪些其實是「寫一個參數」而不是套一份表情檔
  std::vector<ParamExpressionRef> paramExpressions;
  // motions／expressions 裡哪些是 core/builtin_actions.h 合成出來的內建項目。
  // 只存名稱：展開成參數與關鍵影格是純函式（吃 parameters 就算得出來），
  // 存進來只會讓 model_types.h 反過來相依 motion_builder 與 parameter_tracks。
  std::vector<std::string> builtinMotions;
  std::vector<std::string> builtinExpressions;
  // 使用者替動作／表情寫下的意義，存在模型資料夾裡的 *.annotations.json
  ModelAnnotations annotations;
};

// 動作的鍵：群組名代表整個群組，`群組#索引` 代表群組裡的某一段動作。
std::string motionKey(const std::string& group, int index);
std::string motionKey(const std::string& group);

struct MotionKeyParts {
  std::string group;
  // -1 代表沒有索引
  int index = -1;
};

// 把動作鍵拆回群組與索引
MotionKeyParts parseMotionKey(const std::string& key);

}  // namespace l2m
