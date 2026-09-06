#pragma once

// 各式 hit area 名稱 → 身體部位。
//
// hitTest 回傳的是模型作者取的 area 名稱（"Head"、"TapBody"、"顔"、"むね"…），
// 手勢要知道「摸的是哪裡」才能區分摸頭與戳肚子。名稱沒有規格，只能收集
// 常見寫法做大小寫不敏感的子字串比對；認不得的一律 Unknown ——
// 手勢照樣成立，只是不知道部位。

#include <string>
#include <vector>

namespace l2m {

enum class BodyPart { Head, Face, Chest, Body, Hand, Leg, Unknown };

// 單一 area 名稱的判定
BodyPart bodyPartFor(const std::string& areaName);

// hitTest 的整組結果：回第一個認得的部位（模型的 area 通常由前到後排）
BodyPart bodyPartFor(const std::vector<std::string>& areas);

}  // namespace l2m
