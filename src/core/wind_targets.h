#pragma once

// 環境風該吹哪些 PhysicsSetting（core/ambient_wind.h 算出「多大的風」，這裡決定「吹誰」）。
//
// CubismPhysics 的 Options.Wind 是全域選項，所有 PhysicsSetting 一起吃。
// 但模型裡有兩種本質不同的 rig：
//   * 「鏈」—— 頭髮、衣襬。吹了會飄，這才是環境風要的東西。
//   * 「角度跟隨器」—— 把 ParamAngleX/Y/Z 轉成 ParamBodyAngleX/Y/Z 的擺錘。
//     持續風力對它只是把它推到新的平衡角，再乘上作者寫的數十倍輸出倍率
//     寫進身體參數 —— 畫面上就是「身體搖得比頭髮還大，像站不穩」。
//
// 判別規則兩條，缺一不可（實測手邊 30 隻模型）：
//   ① 粒子數 < 3 就不吹。2 粒子＝單節擺錘，是跟隨器的典型寫法。
//   ② 輸出寫進「整體姿勢」的 Cubism 標準參數就不吹，不管幾粒子。
//      只有規則①的話擋不住把跟隨器做成 3~5 節的模型 —— 30 隻裡有 4 隻是這樣
//      （Gan Yu 的 PhysicsSetting2／3 各 5 粒子 → ParamBodyAngleX／Y，
//        椿 15、镜流 14、长离 11 各 3 粒子 → ParamBodyAngleX／Z），
//      症狀就是「開了環境風身體大幅上下擺動，關掉就正常」。
//
// 為什麼不用輸出倍率當判別：倍率看起來很好用（跟隨器 30~74、頭髮約 1），
// 但 Gan Yu 的頭髮 rig 也寫 30 —— 倍率是作者的座標習慣，跨模型不成立。
// setting 的 Name 與自訂參數 id 更猜不得（「身体x」「hf」這種）。
// 「輸出寫進標準姿勢參數」則是規格保證的語意：那六個 id 就是整體姿勢本身。
//
// 規則②刻意只認**精確**的標準 id：镜流 的 PhysicsSetting14 同時輸出
// ParamBodyAngleZ 與作者自訂的 ParamBodyAngleZ2，前綴比對會把後者也算進來，
// 而「含有標準 id 當前綴的自訂參數」很常見（ParamAngleX2、ParamAngleXX…）。
//
// 已知取捨：整個 setting 一起排除（風是作用在粒子上的，沒辦法只排除某一條輸出）。
// 手邊 30 隻裡被排除的那些 setting 輸出的全是姿勢參數，沒有混進頭髮，代價是零；
// 真的把頭髮與身體跟隨寫在同一個 setting 的模型，那撮頭髮不吃風。
//
// 輸出是 std::uint8_t 而不是 std::vector<bool>：這份遮罩要直接餵給
// Framework patch 加的 Options.WindMask（const csmUint8*），
// std::vector<bool> 是位元打包的，沒有 data()。

#include <cstdint>
#include <string>
#include <vector>

namespace l2m {

// 風只吹粒子數 >= 此值的 PhysicsSetting（規則①）。
inline constexpr int kWindMinParticles = 3;

// 一個 PhysicsSetting 攤平之後、判別要用到的兩件事。
struct PhysicsSettingWind {
  int particleCount = 0;
  std::vector<std::string> outputIds;
};

// 規則①②合起來的純判別。
bool settingAcceptsWind(const PhysicsSettingWind& setting);

// 解析 physics3.json 的原始位元組，回傳每個 PhysicsSetting 的遮罩
//（1 ＝ 吹、0 ＝ 不吹），順序與 PhysicsSettings 陣列一致。
//
// 解析不出來（不是 JSON、沒有 PhysicsSettings、空陣列）一律回空 vector ——
// 呼叫端據此把 NULL 交給 Framework，等於維持「全部吃風」的原行為。
// 沒有 physics3.json 的模型根本沒有物理，也就不會走到這裡。
std::vector<std::uint8_t> windTargetMask(const std::string& physicsJson);

}  // namespace l2m
