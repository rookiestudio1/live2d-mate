#include "hit_area_semantics.h"

#include "string_util.h"

namespace l2m {

namespace {

struct Pattern {
  const char* needle;  // 小寫子字串（UTF-8，中日文名稱原樣比對）
  BodyPart part;
};

// 順序即優先序：Head 要排在 Body 之前（"HeadBody" 這種合體名算頭）。
// 收錄標準 hit area（Head/Body）、VTube Studio 慣用名與常見的日文命名。
const Pattern kPatterns[] = {
  {"head", BodyPart::Head},         {"hair", BodyPart::Head},  {"atama", BodyPart::Head},        {"\xE9\xA0\xAD", BodyPart::Head},   // 頭
  {"face", BodyPart::Face},         {"kao", BodyPart::Face},   {"\xE9\xA1\x94", BodyPart::Face},                                     // 顔
  {"\xE8\x84\xB8", BodyPart::Face},                                                                                                  // 脸
  {"chest", BodyPart::Chest},       {"bust", BodyPart::Chest}, {"mune", BodyPart::Chest},        {"\xE8\x83\xB8", BodyPart::Chest},  // 胸
  {"hand", BodyPart::Hand},         {"arm", BodyPart::Hand},   {"ude", BodyPart::Hand},          {"\xE6\x89\x8B", BodyPart::Hand},   // 手
  {"leg", BodyPart::Leg},           {"foot", BodyPart::Leg},   {"ashi", BodyPart::Leg},          {"\xE8\x85\xB3", BodyPart::Leg},    // 脚
  {"\xE8\x85\xBF", BodyPart::Leg},                                                                                                   // 腿
  {"body", BodyPart::Body},         {"torso", BodyPart::Body}, {"karada", BodyPart::Body},       {"\xE4\xBD\x93", BodyPart::Body},   // 体
  {"\xE8\xBA\xAB", BodyPart::Body},                                                                                                  // 身
};

}  // namespace

BodyPart bodyPartFor(const std::string& areaName) {
  const std::string lower = strutil::toLowerAscii(areaName);
  for (const auto& pattern : kPatterns) {
    if (lower.find(pattern.needle) != std::string::npos) return pattern.part;
  }
  return BodyPart::Unknown;
}

BodyPart bodyPartFor(const std::vector<std::string>& areas) {
  for (const auto& area : areas) {
    const BodyPart part = bodyPartFor(area);
    if (part != BodyPart::Unknown) return part;
  }
  return BodyPart::Unknown;
}

}  // namespace l2m
