#include "bottom_align.h"

#include <cmath>

namespace l2m {

namespace {

// 這一列有沒有超過門檻的像素
bool rowHasOpaque(const uint8_t* rgba, int width, int row, size_t strideBytes, int alphaThreshold) {
  const uint8_t* alpha = rgba + size_t(row) * strideBytes + 3;
  for (int x = 0; x < width; ++x) {
    if (alpha[size_t(x) * 4] > alphaThreshold) return true;
  }
  return false;
}

}  // namespace

std::optional<int> firstOpaqueRow(const uint8_t* rgba, int width, int height, size_t strideBytes, int alphaThreshold) {
  if (!rgba || width <= 0 || height <= 0) return std::nullopt;
  for (int row = 0; row < height; ++row) {
    if (rowHasOpaque(rgba, width, row, strideBytes, alphaThreshold)) return row;
  }
  return std::nullopt;
}

std::optional<int> lastOpaqueRow(const uint8_t* rgba, int width, int height, size_t strideBytes, int alphaThreshold) {
  if (!rgba || width <= 0 || height <= 0) return std::nullopt;
  for (int row = height - 1; row >= 0; --row) {
    if (rowHasOpaque(rgba, width, row, strideBytes, alphaThreshold)) return row;
  }
  return std::nullopt;
}

double bottomUpRowToNormalizedBottom(int row, int height) { return double(height - row) / height; }

double bottomUpRowToNormalizedTop(int row, int height) { return double(height - row - 1) / height; }

int bottomOffsetPx(double bottomNormalized, int windowHeightPx) { return static_cast<int>(std::lround(bottomNormalized * windowHeightPx)); }

}  // namespace l2m
