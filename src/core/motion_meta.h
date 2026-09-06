#pragma once

// motion3.json 的 Meta 數量修復。
//
// 為什麼需要：CubismMotion::Parse 依 Meta.TotalPointCount / TotalSegmentCount
// 「先配陣列、再照 Curves 實際內容寫入」，途中完全不檢查邊界。野生模型
// （轉換工具輸出的舊檔）常見 Meta 少報 —— models/001 的 15 個動作檔每個都
// 少報 22~28 點，載入時越界寫入把 heap 寫壞，之後第一個 free 的人中獎
// （症狀堆疊停在 CubismAllocator::Deallocate 的 CubismJson 樹解構，開 full
// page heap 才抓到真兇在 CubismMotion::Parse 的 Points[] 寫入）。Web 版
// Framework 的陣列會自動長大，同一批檔案在瀏覽器裡一直好好的 ——
// 所以這裡選擇「重算修正 Meta」而不是拒載，這些模型才不會在這裡突然載不了。
//
// 走訪規則與 CubismMotionJson::HasConsistency 一字不差：每條 curve 的
// Segments 開頭 2 個數字是起點（1 點），之後每段依型別前進 ——
// 0=線性、2=階梯、3=反階梯：3 個數字、1 點；1=貝茲：7 個數字、3 點。
// 結構走不完（未知型別、數字用完）判定 Invalid：這種檔連 HasConsistency
// 都會踩 CSM_ASSERT（Debug 直接 abort），一個位元組都不能交給 Framework。

#include <string>

namespace l2m {

enum class MotionMetaResult {
  Ok,       // Meta 與實際一致，用原位元組即可
  Fixed,    // Meta 有誤已改寫，改用 MotionMetaFix::json
  Invalid,  // 結構本身壞掉，這份檔不能交給 CubismMotion
};

struct MotionMetaFix {
  MotionMetaResult result = MotionMetaResult::Invalid;
  std::string json;   // result == Fixed 時：改寫 Meta 後的序列化結果（pretty，
                      // CubismJson 的數字解析需要換行或逗號作結，同 model_settings.h）
  std::string error;  // result == Invalid 時的原因（寫 log 用）
};

// 重算 Curves 的實際數量並修正 Meta.CurveCount / TotalSegmentCount / TotalPointCount。
MotionMetaFix fixMotionMetaCounts(const std::string& motionJson);

}  // namespace l2m
