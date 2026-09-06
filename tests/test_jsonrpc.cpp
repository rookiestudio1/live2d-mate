// JSON-RPC 2.0 的 wire 層。
//
// 這一層是自己刻的，所以「id 原封不動照抄」「通知不回應」「批次」這幾件事
// 必須釘住 —— 弄錯的話 client 會靜靜地等一個永遠不來的回覆。
#include <QtTest>

#include <string>

#include "core/json_doc.h"
#include "core/jsonrpc.h"

using namespace l2m;

namespace {

std::optional<std::vector<RpcMessage>> parse(const jsonu::Doc& doc) { return parseRequest(doc.root()); }

}  // namespace

class TestJsonRpc : public QObject {
  Q_OBJECT

private slots:
  // 單筆請求
  void parsesSingleRequest() {
    auto doc = jsonu::Doc::parse(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    QVERIFY(doc.has_value());
    auto messages = parse(*doc);
    QVERIFY(messages.has_value());
    QCOMPARE(messages->size(), size_t(1));
    QCOMPARE((*messages)[0].method, std::string("tools/list"));
    QCOMPARE((*messages)[0].idJson, std::string("1"));
    QVERIFY(!(*messages)[0].isNotification());
  }

  // id 可以是字串，回覆時必須原封不動照抄（1 與 "1" 是不同的東西）
  void keepsStringIdVerbatim() {
    auto doc = jsonu::Doc::parse(R"({"jsonrpc":"2.0","id":"abc","method":"ping"})");
    QVERIFY(doc.has_value());
    auto messages = parse(*doc);
    QVERIFY(messages.has_value());
    QCOMPARE((*messages)[0].idJson, std::string("\"abc\""));
    QCOMPARE(resultResponse((*messages)[0].idJson, "{}"), std::string(R"({"jsonrpc":"2.0","id":"abc","result":{}})"));
  }

  // 沒有 id（或 id 為 null）就是通知，不必回應
  void notificationsHaveNoId() {
    auto doc = jsonu::Doc::parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    QVERIFY(doc.has_value());
    auto messages = parse(*doc);
    QVERIFY(messages.has_value());
    QVERIFY((*messages)[0].isNotification());

    auto nulled = jsonu::Doc::parse(R"({"jsonrpc":"2.0","id":null,"method":"x"})");
    QVERIFY(nulled.has_value());
    QVERIFY(parse(*nulled)->at(0).isNotification());
  }

  // 批次請求
  void parsesBatch() {
    auto doc = jsonu::Doc::parse(R"([{"jsonrpc":"2.0","id":1,"method":"a"},{"jsonrpc":"2.0","method":"b"}])");
    QVERIFY(doc.has_value());
    auto messages = parse(*doc);
    QVERIFY(messages.has_value());
    QCOMPARE(messages->size(), size_t(2));
    QCOMPARE((*messages)[0].method, std::string("a"));
    QVERIFY((*messages)[1].isNotification());
  }

  // 空批次與缺 method 都是 Invalid Request
  void rejectsMalformed() {
    auto empty = jsonu::Doc::parse("[]");
    QVERIFY(empty.has_value());
    QVERIFY(!parse(*empty).has_value());

    auto noMethod = jsonu::Doc::parse(R"({"jsonrpc":"2.0","id":1})");
    QVERIFY(noMethod.has_value());
    QVERIFY(!parse(*noMethod).has_value());

    auto notObject = jsonu::Doc::parse("42");
    QVERIFY(notObject.has_value());
    QVERIFY(!parse(*notObject).has_value());
  }

  // params 節點指回原始文件
  void exposesParams() {
    auto doc = jsonu::Doc::parse(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"speak"}})");
    QVERIFY(doc.has_value());
    auto messages = parse(*doc);
    QVERIFY(messages.has_value());
    QCOMPARE(jsonu::getString((*messages)[0].params, "name"), std::string("speak"));
  }

  // 錯誤回覆的形狀與錯誤碼
  void errorResponseShape() {
    const std::string text = errorResponse("7", kJsonRpcMethodNotFound, "Method not found: nope");
    auto doc = jsonu::Doc::parse(text);
    QVERIFY(doc.has_value());
    QCOMPARE(jsonu::getString(doc->root(), "jsonrpc"), std::string("2.0"));
    yyjson_val* error = jsonu::get(doc->root(), "error");
    QCOMPARE(static_cast<int>(yyjson_get_num(jsonu::get(error, "code"))), -32601);
    QCOMPARE(jsonu::getString(error, "message"), std::string("Method not found: nope"));
  }

  // 沒有 id 時（解析失敗）回覆的 id 是 null
  void errorWithoutIdUsesNull() {
    auto doc = jsonu::Doc::parse(errorResponse("", kJsonRpcParseError, "Parse error"));
    QVERIFY(doc.has_value());
    QVERIFY(yyjson_is_null(jsonu::get(doc->root(), "id")));
  }

  // 訊息裡的引號與換行要逸出，不然整份回覆會變成壞掉的 JSON
  void escapesErrorMessage() {
    auto doc = jsonu::Doc::parse(errorResponse("1", -32000, "bad \"quote\"\nand newline"));
    QVERIFY(doc.has_value());
    QCOMPARE(jsonu::getString(jsonu::get(doc->root(), "error"), "message"), std::string("bad \"quote\"\nand newline"));
  }

  // 批次回覆：一筆時直接回那一筆，多筆才包成陣列，全是通知時整個空著
  void batchResponseShape() {
    QCOMPARE(batchResponse({}), std::string());
    QCOMPARE(batchResponse({"{\"a\":1}"}), std::string("{\"a\":1}"));
    QCOMPARE(batchResponse({"{\"a\":1}", "{\"b\":2}"}), std::string("[{\"a\":1},{\"b\":2}]"));
  }
};

QTEST_APPLESS_MAIN(TestJsonRpc)
#include "test_jsonrpc.moc"
