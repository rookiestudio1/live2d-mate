#include "audio_stream_format.h"

#include <cstring>

namespace l2m {

namespace {

bool startsWith(const char* data, size_t size, const char* magic, size_t magicSize) { return size >= magicSize && std::memcmp(data, magic, magicSize) == 0; }

}  // namespace

AudioContainer sniffContainer(const char* data, size_t size) {
  if (!data || size < kContainerSniffBytes) return AudioContainer::NeedMore;

  const auto* bytes = reinterpret_cast<const unsigned char*>(data);

  // RIFF....WAVE。只看 RIFF 不夠 —— AVI 與 WEBP 也是 RIFF
  if (startsWith(data, size, "RIFF", 4) && std::memcmp(data + 8, "WAVE", 4) == 0) {
    return AudioContainer::Wav;
  }
  if (startsWith(data, size, "fLaC", 4)) return AudioContainer::Flac;
  // ID3v2 標頭之後才是 MP3 的 frame，但容器就是 MP3
  if (startsWith(data, size, "ID3", 3)) return AudioContainer::Mp3;
  // MPEG audio 的 frame sync：11 個 1。第二個位元組的高 3 位是 111，
  // 剩下的位要排除「保留」的組合，不然隨機位元組很容易誤判成 MP3
  if (bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0) {
    const unsigned char version = (bytes[1] >> 3) & 0x03;  // 01 是保留
    const unsigned char layer = (bytes[1] >> 1) & 0x03;    // 00 是保留
    const unsigned char bitrate = (bytes[2] >> 4) & 0x0F;  // 1111 是無效
    const unsigned char rate = (bytes[2] >> 2) & 0x03;     // 11 是保留
    if (version != 0x01 && layer != 0x00 && bitrate != 0x0F && rate != 0x03) {
      return AudioContainer::Mp3;
    }
  }
  // OggS、webm 之類的認得出來也解不了，跟完全不認得歸同一類：
  // 交給整段解碼器去 init，失敗時它會給出面向使用者的錯誤訊息
  return AudioContainer::Other;
}

}  // namespace l2m
