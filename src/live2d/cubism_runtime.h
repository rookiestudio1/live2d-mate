#pragma once

// CubismFramework 的啟動與收尾（含自訂記憶體配置器）。
// 整個行程只需要一份；在第一個 GL context 建立後、載入模型前初始化。

#include <CubismFramework.hpp>
#include <ICubismAllocator.hpp>

namespace l2m {

// 對應官方範例的 LAppAllocator：直接用 malloc/free，對齊配置自行補位
class CubismAllocator : public Csm::ICubismAllocator {
public:
  void* Allocate(Csm::csmSizeType size) override;
  void Deallocate(void* memory) override;
  void* AllocateAligned(Csm::csmSizeType size, Csm::csmUint32 alignment) override;
  void DeallocateAligned(void* alignedMemory) override;
};

// 診斷用：Framework 透過 LoadFileFunction 讀了幾次檔、累計多久。
// 5-r.5 的 GenerateShaders 會為 482 支 shader program 讀將近 1900 次檔
//（每支混合 shader 要讀 vert / frag / colorBlend / alphaBlend 四個檔），
// 這組數字是用來判定 CreateRenderer 的時間到底花在磁碟還是 GL 編譯上的。
struct CubismFileLoadStats {
  long long calls = 0;
  long long micros = 0;
  long long bytes = 0;
};

class CubismRuntime {
public:
  // 初始化 CubismFramework（重複呼叫安全）
  static void initialize();
  // 結束時釋放
  static void dispose();
  // 取目前的累計值；相減即可得到某一段區間的量
  static CubismFileLoadStats fileLoadStats();
};

}  // namespace l2m
