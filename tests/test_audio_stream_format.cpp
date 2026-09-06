// 音訊容器嗅探（core/audio_stream_format.h）。
//
// 這一組守的是「有氣泡沒聲音」那類靜默失敗：串流時要靠開頭幾個位元組
// 決定用哪個解碼器，認錯了就是整句話無聲，而且不會有任何錯誤訊息。
#include <QtTest>

#include <string>
#include <vector>

#include "core/audio_stream_format.h"

using namespace l2m;

namespace {

std::vector<char> bytes(std::initializer_list<int> values) {
  std::vector<char> out;
  for (const int value : values) out.push_back(static_cast<char>(value));
  return out;
}

AudioContainer sniff(const std::vector<char>& data) { return sniffContainer(data.data(), data.size()); }

// 最小的 RIFF/WAVE 標頭前綴
std::vector<char> wavHeader() {
  std::vector<char> out;
  const char* riff = "RIFF";
  const char* wave = "WAVE";
  out.insert(out.end(), riff, riff + 4);
  out.insert(out.end(), {0x24, 0x08, 0x00, 0x00});
  out.insert(out.end(), wave, wave + 4);
  return out;
}

}  // namespace

class TestAudioStreamFormat : public QObject {
  Q_OBJECT

private slots:
  // 位元組不夠就說不夠，不要亂猜
  void reportsNeedMoreWhenTooShort() {
    QCOMPARE(sniff(bytes({'R', 'I', 'F', 'F'})), AudioContainer::NeedMore);
    QCOMPARE(sniffContainer(nullptr, 100), AudioContainer::NeedMore);
    std::vector<char> almost = wavHeader();
    almost.pop_back();
    QCOMPARE(sniff(almost), AudioContainer::NeedMore);
  }

  void recognisesWav() { QCOMPARE(sniff(wavHeader()), AudioContainer::Wav); }

  // RIFF 不等於 WAVE —— AVI 與 WEBP 也是 RIFF，餵給 wav 解碼器只會靜默失敗
  void riffWithoutWaveIsNotWav() {
    std::vector<char> avi = wavHeader();
    avi[8] = 'A';
    avi[9] = 'V';
    avi[10] = 'I';
    avi[11] = ' ';
    QCOMPARE(sniff(avi), AudioContainer::Other);
  }

  void recognisesFlac() {
    std::vector<char> data = {'f', 'L', 'a', 'C', 0, 0, 0, 0x22, 0, 0, 0, 0};
    QCOMPARE(sniff(data), AudioContainer::Flac);
  }

  // Edge TTS 的訊框直接就是 MPEG frame sync，沒有任何容器標頭
  void recognisesRawMp3FrameSync() {
    // FF FB = MPEG1 Layer3，90 = 128kbps/44.1kHz
    QCOMPARE(sniff(bytes({0xFF, 0xFB, 0x90, 0x00, 0, 0, 0, 0, 0, 0, 0, 0})), AudioContainer::Mp3);
    // Edge 實際用的是 MPEG2 Layer3 24kHz：FF F3
    QCOMPARE(sniff(bytes({0xFF, 0xF3, 0x48, 0xC4, 0, 0, 0, 0, 0, 0, 0, 0})), AudioContainer::Mp3);
  }

  void recognisesId3TaggedMp3() {
    std::vector<char> data = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0x23, 'T', 'I'};
    QCOMPARE(sniff(data), AudioContainer::Mp3);
  }

  // 保留／無效的欄位組合不能算 MP3，不然隨機位元組很容易誤判
  void rejectsReservedMpegFields() {
    // version = 01（保留）
    QCOMPARE(sniff(bytes({0xFF, 0xEB, 0x90, 0x00, 0, 0, 0, 0, 0, 0, 0, 0})), AudioContainer::Other);
    // layer = 00（保留）
    QCOMPARE(sniff(bytes({0xFF, 0xF9, 0x90, 0x00, 0, 0, 0, 0, 0, 0, 0, 0})), AudioContainer::Other);
    // bitrate = 1111（無效）
    QCOMPARE(sniff(bytes({0xFF, 0xFB, 0xF0, 0x00, 0, 0, 0, 0, 0, 0, 0, 0})), AudioContainer::Other);
    // sample rate = 11（保留）
    QCOMPARE(sniff(bytes({0xFF, 0xFB, 0x9C, 0x00, 0, 0, 0, 0, 0, 0, 0, 0})), AudioContainer::Other);
  }

  // 解不了的容器要落到 Other，由整段解碼器去報出面向使用者的錯誤
  void unsupportedContainersFallToOther() {
    std::vector<char> ogg = {'O', 'g', 'g', 'S', 0, 2, 0, 0, 0, 0, 0, 0};
    QCOMPARE(sniff(ogg), AudioContainer::Other);
    std::vector<char> json = {'{', '"', 'e', 'r', 'r', 'o', 'r', '"', ':', ' ', '1', '}'};
    QCOMPARE(sniff(json), AudioContainer::Other);
  }
};

QTEST_APPLESS_MAIN(TestAudioStreamFormat)
#include "test_audio_stream_format.moc"
