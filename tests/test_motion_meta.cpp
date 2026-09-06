// motion3.json 的 Meta 數量修復：Meta 少報時 native 版 CubismMotion::Parse
// 會越界寫入，所以載入前要重新數一遍
#include <QtTest>

#include "core/json_doc.h"
#include "core/motion_meta.h"

using namespace l2m;

namespace {

// 取序列化結果裡 Meta 的整數欄位
int64_t metaOf(const std::string& json, const char* key) {
  const auto doc = *jsonu::Doc::parse(json);
  yyjson_val* meta = jsonu::get(doc.root(), "Meta");
  return static_cast<int64_t>(yyjson_get_num(jsonu::get(meta, key)));
}

}  // namespace

class TestMotionMeta : public QObject {
  Q_OBJECT

private slots:
  // Meta 正確時原封不動（用原位元組，不重新序列化）
  void consistentPassesThrough() {
    // 1 條 curve：起點 + 線性段 = 2 點、1 段
    const std::string json = R"({
      "Version": 3,
      "Meta": {"CurveCount": 1, "TotalSegmentCount": 1, "TotalPointCount": 2},
      "Curves": [{"Target": "Parameter", "Id": "ParamA", "Segments": [0, 0, 0, 1, 0.5]}]
    })";
    const MotionMetaFix fix = fixMotionMetaCounts(json);
    QCOMPARE(fix.result, MotionMetaResult::Ok);
    QVERIFY(fix.json.empty());
  }

  // models/001 的實況：Meta 少報，重算後改寫（少報就是讓 heap 被寫壞的那種檔）
  void undercountedMetaGetsFixed() {
    // 起點 1 點 + 線性 1 點 + 貝茲 3 點 = 5 點、2 段；Meta 卻宣告 3 點 3 段
    const std::string json = R"({
      "Version": 3,
      "Meta": {"CurveCount": 1, "TotalSegmentCount": 3, "TotalPointCount": 3},
      "Curves": [{"Target": "Parameter", "Id": "ParamA",
                  "Segments": [0, 0, 0, 1, 0.5, 1, 2, 0.6, 3, 0.7, 4, 0.8]}]
    })";
    const MotionMetaFix fix = fixMotionMetaCounts(json);
    QCOMPARE(fix.result, MotionMetaResult::Fixed);
    QCOMPARE(metaOf(fix.json, "CurveCount"), int64_t(1));
    QCOMPARE(metaOf(fix.json, "TotalSegmentCount"), int64_t(2));
    QCOMPARE(metaOf(fix.json, "TotalPointCount"), int64_t(5));
  }

  // 改寫只動 Meta 的三個數字，Curves 資料一個位元組都不動
  void fixKeepsCurveDataIntact() {
    const std::string json = R"({
      "Meta": {"CurveCount": 0, "TotalSegmentCount": 0, "TotalPointCount": 0,
               "Duration": 1.5, "Fps": 30.0},
      "Curves": [{"Target": "Parameter", "Id": "ParamA", "Segments": [0, 0, 2, 1, 0.5]}]
    })";
    const MotionMetaFix fix = fixMotionMetaCounts(json);
    QCOMPARE(fix.result, MotionMetaResult::Fixed);
    const auto doc = *jsonu::Doc::parse(fix.json);
    // Meta 其他欄位保留
    QCOMPARE(yyjson_get_num(jsonu::get(jsonu::get(doc.root(), "Meta"), "Duration")), 1.5);
    // Curves 原樣
    yyjson_val* curve = yyjson_arr_get_first(jsonu::get(doc.root(), "Curves"));
    QCOMPARE(jsonu::getString(curve, "Id"), std::string("ParamA"));
    QCOMPARE(yyjson_arr_size(jsonu::get(curve, "Segments")), size_t(5));
  }

  // 四種段型別的點數：線性/階梯/反階梯 1 點、貝茲 3 點（與 HasConsistency 同規則）
  void countsAllSegmentTypes() {
    const std::string json = R"({
      "Meta": {},
      "Curves": [{"Id": "P", "Segments":
        [0, 0, 0, 1, 0.1, 2, 2, 0.2, 3, 3, 0.3, 1, 4, 0.4, 5, 0.5, 6, 0.6]}]
    })";
    const MotionMetaFix fix = fixMotionMetaCounts(json);
    QCOMPARE(fix.result, MotionMetaResult::Fixed);
    QCOMPARE(metaOf(fix.json, "TotalSegmentCount"), int64_t(4));
    // 起點 1 + 線性 1 + 階梯 1 + 反階梯 1 + 貝茲 3 = 7
    QCOMPARE(metaOf(fix.json, "TotalPointCount"), int64_t(7));
  }

  // Meta 整個缺也補得出來
  void missingMetaGetsCreated() {
    const std::string json = R"({"Curves": [{"Id": "P", "Segments": [0, 0, 0, 1, 1]}]})";
    const MotionMetaFix fix = fixMotionMetaCounts(json);
    QCOMPARE(fix.result, MotionMetaResult::Fixed);
    QCOMPARE(metaOf(fix.json, "CurveCount"), int64_t(1));
    QCOMPARE(metaOf(fix.json, "TotalPointCount"), int64_t(2));
  }

  // 走不完的結構一律 Invalid：這種檔連 HasConsistency 都會踩 CSM_ASSERT
  void truncatedSegmentsIsInvalid() {
    // 貝茲段宣告後數字不夠
    const std::string json = R"({"Meta": {}, "Curves": [{"Id": "P", "Segments": [0, 0, 1, 1, 0.5]}]})";
    const MotionMetaFix fix = fixMotionMetaCounts(json);
    QCOMPARE(fix.result, MotionMetaResult::Invalid);
    QVERIFY(!fix.error.empty());
  }

  void unknownSegmentTypeIsInvalid() {
    const std::string json = R"({"Meta": {}, "Curves": [{"Id": "P", "Segments": [0, 0, 9, 1, 0.5]}]})";
    QCOMPARE(fixMotionMetaCounts(json).result, MotionMetaResult::Invalid);
  }

  void nonNumberSegmentIsInvalid() {
    const std::string json = R"({"Meta": {}, "Curves": [{"Id": "P", "Segments": [0, "oops", 0, 1, 0.5]}]})";
    QCOMPARE(fixMotionMetaCounts(json).result, MotionMetaResult::Invalid);
  }

  void missingCurvesIsInvalid() {
    QCOMPARE(fixMotionMetaCounts(R"({"Meta": {}})").result, MotionMetaResult::Invalid);
    QCOMPARE(fixMotionMetaCounts("not json at all").result, MotionMetaResult::Invalid);
  }

  // 空 Segments 的 curve 不產生點也不算錯（Parse 的迴圈同樣不會跑）
  void emptySegmentsCurveIsCountedButPointless() {
    const std::string json = R"({
      "Meta": {"CurveCount": 1, "TotalSegmentCount": 0, "TotalPointCount": 0},
      "Curves": [{"Id": "P", "Segments": []}]
    })";
    QCOMPARE(fixMotionMetaCounts(json).result, MotionMetaResult::Ok);
  }

  // 改寫輸出必須是 pretty-print：CubismJson 的數字要靠換行或逗號收尾
  void fixedJsonIsPretty() {
    const std::string json = R"({"Meta": {}, "Curves": [{"Id": "P", "Segments": [0, 0, 0, 1, 1]}]})";
    const MotionMetaFix fix = fixMotionMetaCounts(json);
    QCOMPARE(fix.result, MotionMetaResult::Fixed);
    QVERIFY(fix.json.find('\n') != std::string::npos);
  }
};

QTEST_APPLESS_MAIN(TestMotionMeta)
#include "test_motion_meta.moc"
