#include "png_decoder.h"

#include <spng.h>

#include <QDebug>

#include "core/png_probe.h"

namespace l2m {

QImage decodePngRgba8(const QByteArray& data, quint64 maxDecodedBytes) {
  const auto header = readPngHeader(data.constData(), static_cast<size_t>(data.size()));
  if (!header) return {};  // 不是 PNG（或檔頭就不合規格）—— 交給 QImage

  const quint64 needed = pngRgba8Bytes(*header);
  if (needed == 0 || needed > maxDecodedBytes) return {};

  spng_ctx* ctx = spng_ctx_new(0);
  if (!ctx) return {};

  // 把上面驗過的尺寸釘給 spng，等於「檔頭說多大就只准解多大」。
  // **這不是獨立的第二道防線** —— 傳進去的值就是從同一份 IHDR 解出來的，
  // 真正擋住解壓縮炸彈的是上面那句 `needed > maxDecodedBytes`。
  // 留著的意義是「spng 內部再算一次時用的是我們同意過的上限」。
  // 回傳值不檢查：唯一會失敗的情況是尺寸超過 2^31-1，而 readPngHeader 已經擋掉了。
  spng_set_image_limits(ctx, header->width, header->height);
  if (spng_set_png_buffer(ctx, data.constData(), static_cast<size_t>(data.size())) != 0) {
    spng_ctx_free(ctx);
    return {};
  }

  QImage image(static_cast<int>(header->width), static_cast<int>(header->height), QImage::Format_RGBA8888);
  if (image.isNull()) {
    spng_ctx_free(ctx);
    return {};
  }
  // spng 寫出來的是**緊密排列**的 w×4，而 QImage 的列間距是自己算的。
  // 32bpp 下 Qt 算出來就是 w×4（沒有補白），但那是實作細節不是保證 ——
  // 對不上就退回 QImage，總比照著寫下去把整張圖畫成斜的好。
  if (image.bytesPerLine() != static_cast<qsizetype>(header->width) * 4) {
    spng_ctx_free(ctx);
    return {};
  }

  // SPNG_DECODE_TRNS 不能省：tRNS 是調色盤／灰階 PNG 表達透明的方式，
  // 不給這個旗標 spng 就當它不存在，而 libpng 是會套用的 —— 症狀是那類貼圖
  // 該透明的地方變成不透明的純色（本專案的貼圖幾乎都是 RGBA，這條是為了
  // 「有一天真的遇到」而寫的，不是現在會踩到的路徑）。
  const int err = spng_decode_image(ctx, image.bits(), static_cast<size_t>(image.sizeInBytes()), SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
  spng_ctx_free(ctx);
  if (err != 0) {
    // 檔頭是合法 PNG 卻解不動 —— 值得記一行，但**不當成失敗**：截斷的檔案、
    // APNG 之類的變體交給 QImage 再試一次，真的都失敗時由呼叫端統一報告
    qWarning() << "[live2d] libspng 解碼失敗:" << spng_strerror(err) << "，退回 QImage";
    return {};
  }
  return image;
}

}  // namespace l2m
