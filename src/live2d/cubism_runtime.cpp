#include "cubism_runtime.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QScopeGuard>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "core/json_doc.h"

namespace l2m {

namespace {

CubismAllocator allocator;
Csm::CubismFramework::Option option;
bool initialized = false;

// 累計的讀檔量測（用途見 cubism_runtime.h 的 CubismFileLoadStats）。
// 全部在 GUI 執行緒上被呼叫，不必上鎖。
CubismFileLoadStats fileStats;

// Cubism 的 log 轉給 Qt
void cubismLog(const char* message) { qDebug() << "[cubism]" << message; }

// 5-r.5 起 Framework 的 shader 原始碼改成執行期從檔案載入（透過這個回呼）。
// 路徑形如 "VertShaderSrc.vert" 或 "FrameworkShaders/FragShaderSrcColorBlend.frag"。
// 解析順序：開發環境用 FetchContent 原始碼裡的 Shaders/Standard，
// 部署環境用執行檔旁的 FrameworkShaders/。
Csm::csmByte* loadFileBytes(const std::string filePath, Csm::csmSizeInt* outSize) {
  namespace fs = std::filesystem;
  QElapsedTimer ioClock;
  ioClock.start();
  ++fileStats.calls;
  // 不管走哪條 return，離開時都把耗時記進去
  const auto tally = qScopeGuard([&ioClock, outSize] {
    fileStats.micros += ioClock.nsecsElapsed() / 1000;
    fileStats.bytes += *outSize;
  });
  *outSize = 0;

  // 去掉 "FrameworkShaders/" 前綴後的純檔名
  std::string name = filePath;
  const std::string prefix = "FrameworkShaders/";
  if (name.rfind(prefix, 0) == 0) name = name.substr(prefix.size());

  std::vector<fs::path> candidates;
#ifdef L2M_CUBISM_SHADER_DIR
  candidates.push_back(fs::u8path(L2M_CUBISM_SHADER_DIR) / fs::u8path(name));
#endif
  if (QCoreApplication::instance()) {
    const fs::path exeDir = fs::u8path(QCoreApplication::applicationDirPath().toStdString());
    candidates.push_back(exeDir / "FrameworkShaders" / fs::u8path(name));
  }
  candidates.push_back(fs::u8path(filePath));  // 原樣路徑（最後手段）

  for (const auto& path : candidates) {
    const auto bytes = jsonu::readFileUtf8(path);
    if (!bytes) continue;
    auto* buffer = static_cast<Csm::csmByte*>(std::malloc(bytes->size()));
    if (!buffer) break;
    std::memcpy(buffer, bytes->data(), bytes->size());
    *outSize = static_cast<Csm::csmSizeInt>(bytes->size());
    return buffer;
  }

  qWarning() << "[cubism] 讀不到檔案:" << QString::fromStdString(filePath);
  *outSize = 0;
  return nullptr;
}

void releaseFileBytes(Csm::csmByte* byteData) { std::free(byteData); }

}  // namespace

void* CubismAllocator::Allocate(Csm::csmSizeType size) { return std::malloc(size); }

void CubismAllocator::Deallocate(void* memory) { std::free(memory); }

// 對齊配置：多要一塊 offset 空間，把原始指標藏在對齊位址前面
void* CubismAllocator::AllocateAligned(Csm::csmSizeType size, Csm::csmUint32 alignment) {
  const size_t offset = alignment - 1 + sizeof(void*);
  void* raw = std::malloc(size + offset);
  if (!raw) return nullptr;
  auto aligned = reinterpret_cast<void**>((reinterpret_cast<size_t>(raw) + offset) & ~(size_t(alignment) - 1));
  aligned[-1] = raw;
  return aligned;
}

void CubismAllocator::DeallocateAligned(void* alignedMemory) {
  if (!alignedMemory) return;
  std::free(static_cast<void**>(alignedMemory)[-1]);
}

void CubismRuntime::initialize() {
  if (initialized) return;
  option.LogFunction = cubismLog;
  option.LoggingLevel = Csm::CubismFramework::Option::LogLevel_Warning;
  option.LoadFileFunction = loadFileBytes;
  option.ReleaseBytesFunction = releaseFileBytes;
  Csm::CubismFramework::StartUp(&allocator, &option);
  Csm::CubismFramework::Initialize();
  initialized = true;
}

CubismFileLoadStats CubismRuntime::fileLoadStats() { return fileStats; }

void CubismRuntime::dispose() {
  if (!initialized) return;
  Csm::CubismFramework::Dispose();
  initialized = false;
}

}  // namespace l2m
