#include "interaction_logic.h"

#include "string_util.h"

namespace l2m {

namespace {

// 一個候選：某個群組、或群組裡的某一段。
// key 一律小寫（群組名或動作檔名去掉副檔名）。
struct Candidate {
  std::string group;
  int index = -1;
  std::string key;
};

// "touch_head.motion3.json" → "touch_head"。
// 從第一個點切：副檔名是 .motion3.json 兩節，rfind 只會切掉最後一節。
std::string stemOf(const std::string& file) {
  const size_t dot = file.find('.');
  return dot == std::string::npos ? file : file.substr(0, dot);
}

bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

// 觸摸池的判定（見標頭）。中日文的寫法一併收：触／觸／撫。
bool isTouchKey(const std::string& key) {
  static const char* kNeedles[] = {"touch", "tap", "pat", "poke", "\xE8\xA7\xA6", "\xE8\xA7\xB8", "\xE6\x92\xAB"};
  for (const char* needle : kNeedles) {
    if (contains(key, needle)) return true;
  }
  return false;
}

// 部位 → 動作名稱裡會出現的關鍵字（依序比對）。
// Chest 的最後一個是 special：碧藍航線全系列的「特殊觸摸」就是胸口那一下，
// 而那個字本身沒有部位含意，所以排在真正的部位字之後當補救。
std::vector<std::string> keywordsFor(BodyPart part) {
  switch (part) {
    case BodyPart::Head:
      return {"head", "atama", "\xE9\xA0\xAD", "hair"};  // 頭
    case BodyPart::Face:
      return {"face", "kao", "\xE9\xA1\x94", "\xE8\x84\xB8", "head", "\xE9\xA0\xAD"};  // 顔 脸 頭
    case BodyPart::Chest:
      return {"chest", "bust", "mune", "\xE8\x83\xB8", "special"};  // 胸
    case BodyPart::Body:
      return {"body", "karada", "\xE4\xBD\x93", "\xE8\xBA\xAB"};  // 体 身
    case BodyPart::Hand:
      return {"hand", "arm", "ude", "\xE6\x89\x8B"};  // 手
    case BodyPart::Leg:
      return {"leg", "foot", "ashi", "\xE8\x84\x9A", "\xE8\x85\xBF"};  // 脚 腿
    case BodyPart::Unknown:
      break;
  }
  return {};
}

}  // namespace

std::optional<TapMotionPick> pickTapMotion(const std::vector<std::string>& areas, BodyPart part, const std::vector<MotionGroupInfo>& motions) {
  // 群組候選與逐段候選分開收：兩者都能對上時優先整個群組（index = -1），
  // 群組裡有好幾段變體時才會每次隨機挑一段，而不是永遠播第 0 段。
  std::vector<Candidate> groups;
  std::vector<Candidate> files;
  for (const auto& group : motions) {
    // files 是空的＝內建合成動作，model3.json 裡沒有這個群組（見標頭）
    if (group.count <= 0 || group.files.empty()) continue;
    if (!group.name.empty()) groups.push_back({group.name, -1, strutil::toLowerAscii(group.name)});
    for (size_t i = 0; i < group.files.size(); ++i) {
      files.push_back({group.name, static_cast<int>(i), strutil::toLowerAscii(stemOf(group.files[i]))});
    }
  }

  const auto scan = [&groups, &files](const auto& match) -> std::optional<TapMotionPick> {
    for (const auto* pool : {&groups, &files}) {
      for (const auto& candidate : *pool) {
        if (match(candidate)) return TapMotionPick{candidate.group, candidate.index};
      }
    }
    return std::nullopt;
  };

  // 1. 作者標的 HitArea 名稱（最準，但只有 1.1% 的模型有填）
  for (const auto& area : areas) {
    const std::string key = strutil::toLowerAscii(area);
    if (key.empty()) continue;
    if (auto hit = scan([&key](const Candidate& c) { return c.key == "tap" + key || c.key == "tap_" + key || c.key == "touch" + key || c.key == "touch_" + key || c.key == key; })) return hit;
    // 包含關係一樣**先問觸摸池**：一隻有 `body_idle`（身體待機搖擺，很常見的命名）
    // 又有 `touch_body_01` 的模型，不分先後的話點身體播的會是待機搖擺。
    // 觸摸池裡沒有才放寬到全部 —— 作者既然標了 area，那個名稱本身就是強訊號。
    if (auto hit = scan([&key](const Candidate& c) { return isTouchKey(c.key) && contains(c.key, key); })) return hit;
    if (auto hit = scan([&key](const Candidate& c) { return contains(c.key, key); })) return hit;
  }

  // 2. 部位關鍵字（只在觸摸池裡找 —— 「頭」在別處太容易誤命中）
  for (const auto& keyword : keywordsFor(part)) {
    if (auto hit = scan([&keyword](const Candidate& c) { return isTouchKey(c.key) && contains(c.key, keyword); })) return hit;
  }

  // 3. 觸摸池裡不含 idle／drag 的任一段
  if (auto hit = scan([](const Candidate& c) { return isTouchKey(c.key) && !contains(c.key, "idle") && !contains(c.key, "drag"); })) return hit;

  // 4. 觸摸池全部
  return scan([](const Candidate& c) { return isTouchKey(c.key); });
}

}  // namespace l2m
