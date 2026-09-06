#include "png_probe.h"

#include <cstring>

namespace l2m {

namespace {

// PNG 規格 §5.2 的固定簽章
constexpr unsigned char kSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

// 簽章 8 + 長度 4 + 型別 4 + IHDR 13 = 29，再往後才是 CRC
constexpr size_t kHeaderBytes = 29;

uint32_t readBe32(const unsigned char* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }

}  // namespace

bool isPngSignature(const void* data, size_t size) {
  if (!data || size < sizeof(kSignature)) return false;
  return std::memcmp(data, kSignature, sizeof(kSignature)) == 0;
}

std::optional<PngHeader> readPngHeader(const void* data, size_t size) {
  if (!isPngSignature(data, size) || size < kHeaderBytes) return std::nullopt;
  const auto* p = static_cast<const unsigned char*>(data);

  // IHDR 必須是第一個 chunk，長度固定 13 —— 這兩件事一起檢查，才不會把
  // 「簽章對但後面是別的東西」的檔案當成 PNG 交給 spng
  if (readBe32(p + 8) != 13 || std::memcmp(p + 12, "IHDR", 4) != 0) return std::nullopt;

  PngHeader header;
  header.width = readBe32(p + 16);
  header.height = readBe32(p + 20);
  header.bitDepth = p[24];
  header.colorType = p[25];
  // p[26] compression、p[27] filter 規格上只有 0 這一個合法值，交給 spng 去挑剔
  header.interlace = p[28];

  // 規格 §11.2.2：寬高是 1 ~ 2^31-1。上界不能省 —— 呼叫端會把它們
  // `static_cast<int>` 去建 QSize（`model_controller.cpp` 的檔頭探測），
  // 2^31 以上在 MSVC 上會變成負數，接著被 `std::max(1, ...)` 夾成 1，
  // 於是「解碼後多大」整個算錯而併發估過頭。
  constexpr uint32_t kMaxDimension = 2147483647u;
  if (header.width == 0 || header.height == 0) return std::nullopt;
  if (header.width > kMaxDimension || header.height > kMaxDimension) return std::nullopt;
  switch (header.bitDepth) {
    case 1:
    case 2:
    case 4:
    case 8:
    case 16:
      break;
    default:
      return std::nullopt;
  }
  // 色彩型別與位元深度的合法組合（規格表 11.2.2）：調色盤最多 8 位元，
  // 帶 alpha 的兩種只有 8/16。放行不合法的組合等於把工作丟給 spng 去失敗，
  // 那時已經配置過整張影像的記憶體了。
  switch (header.colorType) {
    case 0:
      break;
    case 3:
      if (header.bitDepth == 16) return std::nullopt;
      break;
    case 2:
    case 4:
    case 6:
      if (header.bitDepth < 8) return std::nullopt;
      break;
    default:
      return std::nullopt;
  }
  if (header.interlace > 1) return std::nullopt;
  return header;
}

uint64_t pngRgba8Bytes(const PngHeader& header) {
  if (header.width == 0 || header.height == 0) return 0;
  const uint64_t width = header.width;
  const uint64_t height = header.height;
  // PNG 的寬高上限是 2^31-1，兩個相乘最壞是 2^62，再乘 4 就會溢出 uint64 ——
  // 所以要先擋，不能算完再檢查
  constexpr uint64_t kMax = UINT64_MAX / 4;
  if (height != 0 && width > kMax / height) return 0;
  return width * height * 4;
}

}  // namespace l2m
