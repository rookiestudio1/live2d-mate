// 回音：脈衝響應的落點與衰減、尾巴長度、聲道獨立、reset 之後真的乾淨
#include <QtTest>

#include <cmath>
#include <vector>

#include "core/echo_filter.h"

using namespace l2m;

namespace {

constexpr uint32_t kRate = 48000;

// 單聲道脈衝響應：第 0 個 frame 餵 1，其餘餵 0
std::vector<float> impulseResponse(EchoFilter& filter, size_t frames) {
  std::vector<float> buffer(frames, 0.0f);
  buffer[0] = 1.0f;
  filter.process(buffer.data(), frames);
  return buffer;
}

bool near(double actual, double expected, double tolerance = 1e-5) { return std::abs(actual - expected) <= tolerance; }

}  // namespace

class TestEchoFilter : public QObject {
  Q_OBJECT

private slots:
  // 沒 reset 過、或 wet 是 0：整條路徑必須是 no-op，說話那條路不能被碰到
  void inactiveLeavesSamplesUntouched() {
    EchoFilter filter;
    QVERIFY(!filter.active());

    std::vector<float> buffer{0.5f, -0.25f, 1.0f, 0.0f};
    const std::vector<float> before = buffer;
    filter.process(buffer.data(), buffer.size());
    QCOMPARE(buffer, before);

    EchoFilter::Params silent;
    silent.delayMs = 100;
    silent.feedback = 0.5;
    silent.wet = 0;  // 不啟用
    filter.reset(kRate, 1, silent);
    QVERIFY(!filter.active());
    filter.process(buffer.data(), buffer.size());
    QCOMPARE(buffer, before);
  }

  // 第一次回音落在正好 delayMs 的位置，振幅就是 wet；中間一律安靜。
  // 落點差一個 frame 是聽不出來的，但差一個「聲道」就會變成單邊回音
  void firstEchoLandsExactlyAfterTheDelay() {
    EchoFilter::Params params;
    params.delayMs = 10;  // 480 frame
    params.feedback = 0.5;
    params.damping = 0.4;
    params.dry = 0.8;
    params.wet = 0.5;

    EchoFilter filter;
    filter.reset(kRate, 1, params);
    QVERIFY(filter.active());

    const size_t delay = 480;
    const std::vector<float> response = impulseResponse(filter, delay * 3);

    QVERIFY(near(response[0], params.dry));
    for (size_t i = 1; i < delay; ++i) QVERIFY2(near(response[i], 0.0), qPrintable(QString("frame %1 = %2").arg(i).arg(response[i])));
    QVERIFY(near(response[delay], params.wet));
  }

  // 每一輪都比前一輪小，而且小的比例就是 feedback×(1-damping) ——
  // 低通是「越重複越悶」的來源，被拿掉的話這一條會變成單純的 feedback
  void repeatsDecayByFeedbackAndDamping() {
    EchoFilter::Params params;
    params.delayMs = 10;
    params.feedback = 0.5;
    params.damping = 0.4;
    params.dry = 1.0;
    params.wet = 1.0;

    EchoFilter filter;
    filter.reset(kRate, 1, params);
    const size_t delay = 480;
    const std::vector<float> response = impulseResponse(filter, delay * 4);

    QVERIFY(near(response[delay], 1.0));
    QVERIFY(near(response[delay * 2], params.feedback * (1.0 - params.damping)));
    QVERIFY(response[delay * 3] > 0);
    QVERIFY(response[delay * 3] < response[delay * 2]);
  }

  // 尾巴要夠長才不會把殘響切掉，但也不能無限長（那段時間 wait=true 純粹在乾等）
  void tailCoversTheAudibleRepeats() {
    EchoFilter::Params params = EchoFilter::thinkingVoice();
    EchoFilter filter;
    filter.reset(kRate, 2, params);

    const size_t delay = static_cast<size_t>(std::llround(params.delayMs * kRate / 1000.0));
    QVERIFY(filter.tailFrames() > delay);
    QVERIFY(filter.tailFrames() <= static_cast<size_t>(1.2 * kRate));

    // 尾巴跑完之後，殘響已經在聽覺門檻以下
    EchoFilter mono;
    mono.reset(kRate, 1, params);
    std::vector<float> buffer(mono.tailFrames() + delay, 0.0f);
    buffer[0] = 1.0f;
    mono.process(buffer.data(), buffer.size());
    for (size_t i = mono.tailFrames(); i < buffer.size(); ++i) QVERIFY(std::abs(buffer[i]) < 0.01f);
  }

  // feedback 越大尾巴越長 —— 反過來的話收尾會提早，殘響被硬切
  void strongerFeedbackNeedsALongerTail() {
    EchoFilter::Params weak = EchoFilter::thinkingVoice();
    weak.feedback = 0.2;
    EchoFilter::Params strong = weak;
    strong.feedback = 0.6;

    EchoFilter a;
    EchoFilter b;
    a.reset(kRate, 2, weak);
    b.reset(kRate, 2, strong);
    QVERIFY(b.tailFrames() > a.tailFrames());
  }

  // 立體聲：兩個聲道各有自己的延遲線與低通狀態，不互相漏音
  void channelsDoNotBleed() {
    EchoFilter::Params params = EchoFilter::thinkingVoice();
    params.delayMs = 10;
    EchoFilter filter;
    filter.reset(kRate, 2, params);

    const size_t delay = 480;
    std::vector<float> buffer(delay * 2 * 2, 0.0f);
    buffer[0] = 1.0f;  // 只有左聲道有訊號
    filter.process(buffer.data(), delay * 2);

    QVERIFY(near(buffer[delay * 2], params.wet));      // 左聲道有回音
    QVERIFY(near(buffer[delay * 2 + 1], 0.0));          // 右聲道全程安靜
    for (size_t frame = 0; frame < delay * 2; ++frame) {
      if (frame != 0) QVERIFY(near(buffer[frame * 2 + 1], 0.0));
    }
  }

  // reset 要把延遲線與低通狀態一起清乾淨：上一句的殘響漏到下一句是實際會發生的
  //（同一顆 AudioPlayer 連著播兩句），症狀是句首多一段別人的尾音
  void resetClearsPreviousTail() {
    EchoFilter::Params params = EchoFilter::thinkingVoice();
    params.delayMs = 10;

    EchoFilter filter;
    filter.reset(kRate, 1, params);
    const std::vector<float> first = impulseResponse(filter, 1440);

    filter.reset(kRate, 1, params);
    const std::vector<float> second = impulseResponse(filter, 1440);
    QCOMPARE(second, first);

    // 清空之後也是 no-op
    filter.clear();
    QVERIFY(!filter.active());
  }
};

QTEST_APPLESS_MAIN(TestEchoFilter)
#include "test_echo_filter.moc"
