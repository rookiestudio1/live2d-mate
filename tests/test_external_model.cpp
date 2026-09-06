// 外部模型的純邏輯（core/external_model.h）。
//
// 「外部模型」是 Live2D Viewer 直接指定、不在 models 目錄底下的模型，
// 於是 config 的 model.current 可以是一條絕對路徑。這支釘住三組規則：
//
//  1. 「這個 id 算不算外部模型」—— 錯一格就是桌寵開機拿絕對路徑去接
//     modelsDir_，載到一條不存在的路徑，而且沒有任何錯誤訊息。
//  2. 路徑正規化 —— id 同時是清單的唯一鍵，同一個檔案用兩種寫法進來
//     會變成清單上兩隻一模一樣的模型。
//  3. 單一實例的訊息與回覆 —— Viewer 與桌寵之間唯一的協定，
//     路徑含空白（"C:/Program Files/..."）切錯就是整個功能不會動。
#include <QtTest>

#include <optional>
#include <string>
#include <vector>

#include "core/external_model.h"
#include "core/model_scanner.h"

using namespace l2m;

class TestExternalModel : public QObject {
  Q_OBJECT

private slots:
  // 掃描 models 目錄產出的 id 是相對路徑，一律不是外部模型
  void scannedIdsAreNotExternal() {
    QVERIFY(!isExternalModelId("Hutao/Hutao.model3.json"));
    QVERIFY(!isExternalModelId("Foo.zip"));
    QVERIFY(!isExternalModelId(""));
  }

  // Windows 磁碟機開頭是外部模型，正斜線與反斜線都算
  void driveLetterIsExternal() {
    QVERIFY(isExternalModelId("E:/models/Foo.model3.json"));
    QVERIFY(isExternalModelId("E:\\models\\Foo.model3.json"));
    QVERIFY(isExternalModelId("e:/models/Foo.model3.json"));
  }

  // 判定刻意不用 std::filesystem::path::is_absolute()：
  // 那支在 Windows 上對 POSIX 絕對路徑回 false，兩個平台會給出不同答案
  void posixAndUncPathsAreExternal() {
    QVERIFY(isExternalModelId("/home/u/Foo.model3.json"));
    QVERIFY(isExternalModelId("//server/share/Foo.model3.json"));
  }

  // 只是長得像磁碟機不算（冒號前面必須是字母）
  void lookalikeIsNotExternal() {
    QVERIFY(!isExternalModelId("1:/x"));
    QVERIFY(!isExternalModelId("E:"));
    QVERIFY(!isExternalModelId("E:models/Foo.model3.json"));
  }

  // 反斜線一律換成正斜線 —— id 要跟 scanModels 的 POSIX 形式同一種寫法
  void normalizeConvertsToPosixSlashes() { QCOMPARE(normalizeModelPath("E:\\a\\b\\Foo.model3.json"), std::string("E:/a/b/Foo.model3.json")); }

  // "." 與 ".." 要收掉，否則同一個檔案會有兩個 id
  void normalizeCollapsesDotSegments() { QCOMPARE(normalizeModelPath("E:/a/./b/../Foo.model3.json"), std::string("E:/a/Foo.model3.json")); }

  // 尾端斜線去掉；根目錄保持原樣（拔掉之後 "E:" 是磁碟機相對路徑，意思完全不一樣）
  void normalizeStripsTrailingSlash() {
    QCOMPARE(normalizeModelPath("E:/a/b/"), std::string("E:/a/b"));
    QCOMPARE(normalizeModelPath("E:/"), std::string("E:/"));
    QCOMPARE(normalizeModelPath("/"), std::string("/"));
  }

  // 相對路徑要補成絕對再存 —— id 會寫進 config，工作目錄一換就找不到了
  void externalIdMakesRelativePathAbsolute() {
    const std::string id = externalModelId("Foo.model3.json");
    QVERIFY(isExternalModelId(id));
    QVERIFY(id.size() > std::string("Foo.model3.json").size());
    QCOMPARE(id.substr(id.size() - std::string("/Foo.model3.json").size()), std::string("/Foo.model3.json"));
  }

  // 已經是絕對路徑時，externalModelId 就等於正規化
  void externalIdKeepsAbsolutePath() {
    const std::string id = externalModelId("E:\\a\\.\\b\\Foo.model3.json");
    QCOMPARE(id, std::string("E:/a/b/Foo.model3.json"));
    QVERIFY(isExternalModelId(id));
  }

  // 正規化過的絕對路徑仍然開得起來，而且判定得出是外部模型。
  // 這是「Viewer 送過來的那條路徑能不能變成桌寵清單上的一隻」的最短驗證 ——
  // 正規化把路徑弄壞的話，桌寵那端會安靜地什麼都載不到。
  void normalizedAbsolutePathStillOpens() {
    const std::string id = externalModelId(std::filesystem::path(L2M_FIXTURES_DIR) / "Bare" / "bare.model3.json");
    QVERIFY(isExternalModelId(id));
    const std::optional<ModelInfo> described = describeModel(std::filesystem::u8path(id));
    QVERIFY(described.has_value());
  }

  // models 目錄底下的模型要還原成相對 id。使用者在檢視器裡最順手的瀏覽目標
  // 就是 models 目錄，用絕對路徑再收一次會讓清單上多出一隻同名分身，
  // 而且因為 config 記的是絕對路徑，每次重開都會再長回來
  void modelsDirEntryBecomesRelativeId() {
    QCOMPARE(modelsDirRelativeId("C:/data/models/Hiyori/Hiyori.model3.json", "C:/data/models").value_or(std::string()), std::string("Hiyori/Hiyori.model3.json"));
    QCOMPARE(modelsDirRelativeId("C:\\data\\models\\Foo.zip", "C:/data/models/").value_or(std::string()), std::string("Foo.zip"));
  }

  // 目錄外面的一律是外部模型
  void outsideModelsDirStaysExternal() {
    QVERIFY(!modelsDirRelativeId("E:/downloads/Foo.model3.json", "C:/data/models").has_value());
    // 名字只是**開頭一樣**的鄰居不算在裡面 —— 少了那條斜線 "models2" 會被誤判
    QVERIFY(!modelsDirRelativeId("C:/data/models2/Foo.model3.json", "C:/data/models").has_value());
    // 目錄本身不是模型
    QVERIFY(!modelsDirRelativeId("C:/data/models", "C:/data/models").has_value());
  }

#ifdef Q_OS_WIN
  // Windows 上比對不分大小寫：NTFS 本來就不分，而 QFileDialog 與 QStandardPaths
  // 給的磁碟機大小寫不保證一致，精確比對會讓同一隻模型被當成外部模型
  void modelsDirComparisonIgnoresCaseOnWindows() {
    QCOMPARE(modelsDirRelativeId("c:/DATA/Models/Hiyori/Hiyori.model3.json", "C:/data/models").value_or(std::string()), std::string("Hiyori/Hiyori.model3.json"));
  }
#endif

  // 沒有旗標就沒有值
  void setModelArgAbsentWithoutFlag() {
    QVERIFY(!setModelArg({"live2d_mate.exe"}).has_value());
    QVERIFY(!setModelArg({}).has_value());
  }

  // 旗標後面那個參數就是路徑，出現在任何位置都算
  void setModelArgTakesNextArgument() {
    QCOMPARE(setModelArg({"live2d_mate.exe", "--set-model", "E:/a/Foo.model3.json"}).value_or(std::string()), std::string("E:/a/Foo.model3.json"));
    QCOMPARE(setModelArg({"live2d_mate.exe", "--hidden", "--set-model", "E:/a/Foo.model3.json"}).value_or(std::string()), std::string("E:/a/Foo.model3.json"));
  }

  // 旗標後面沒接東西、或接的是另一個旗標，一律當成沒給
  // —— 把 "--hidden" 當成路徑去載入只會得到一句莫名其妙的錯誤
  void setModelArgRejectsMissingValue() {
    QVERIFY(!setModelArg({"live2d_mate.exe", "--set-model"}).has_value());
    QVERIFY(!setModelArg({"live2d_mate.exe", "--set-model", "--hidden"}).has_value());
  }

  // 給了兩次就取第一次；"--set-model=path" 那種寫法不支援（會整段當成旗標名而略過）
  void setModelArgTakesFirstOccurrence() {
    QCOMPARE(setModelArg({"exe", "--set-model", "A.model3.json", "--set-model", "B.model3.json"}).value_or(std::string()), std::string("A.model3.json"));
    QVERIFY(!setModelArg({"exe", "--set-model=A.model3.json"}).has_value());
  }

  // 訊息一行一則，結尾換行就是框界
  void encodesRequests() {
    QCOMPARE(encodeShowRequest(), std::string("show\n"));
    QCOMPARE(encodeSetModelRequest("E:/a/Foo.model3.json"), std::string("setmodel E:/a/Foo.model3.json\n"));
  }

  // verb 取到第一個空白為止，其餘整段都是路徑
  // —— "C:/Program Files/..." 這種路徑照樣完整
  void parsesSetModelWithSpacesInPath() {
    const InstanceRequest req = parseInstanceRequest("setmodel C:/Program Files/models/Foo.model3.json\n");
    QCOMPARE(req.kind, InstanceRequestKind::SetModel);
    QCOMPARE(req.path, std::string("C:/Program Files/models/Foo.model3.json"));
  }

  // 舊版送的是不帶換行的 "show"，收端仍然要認得
  void parsesShowWithAndWithoutNewline() {
    QCOMPARE(parseInstanceRequest("show\n").kind, InstanceRequestKind::Show);
    QCOMPARE(parseInstanceRequest("show").kind, InstanceRequestKind::Show);
    QCOMPARE(parseInstanceRequest("show\r\n").kind, InstanceRequestKind::Show);
  }

  // 認不得的一律 Unknown，收端才不會拿空路徑去載入
  void parsesUnknownRequests() {
    QCOMPARE(parseInstanceRequest("setmodel\n").kind, InstanceRequestKind::Unknown);
    QCOMPARE(parseInstanceRequest("setmodel \n").kind, InstanceRequestKind::Unknown);
    QCOMPARE(parseInstanceRequest("garbage\n").kind, InstanceRequestKind::Unknown);
    QCOMPARE(parseInstanceRequest("").kind, InstanceRequestKind::Unknown);
  }

  // 回覆也是一行：成功只有 ok，失敗帶訊息
  void encodesReplies() {
    QCOMPARE(encodeInstanceReply(true, ""), std::string("ok\n"));
    QCOMPARE(encodeInstanceReply(false, "Model not found"), std::string("error Model not found\n"));
    // 沒有訊息時不留一個尾端空白 —— 這是走在線上的一行，不是給人看的字串
    QCOMPARE(encodeInstanceReply(false, ""), std::string("error\n"));
  }

  // 訊息裡的換行要吃掉，否則一則回覆會被切成兩則
  void replyMessageIsSingleLine() { QCOMPARE(encodeInstanceReply(false, "bad\npath"), std::string("error bad path\n")); }

  // 請求那一側同理。Windows 的檔名放不進換行，但 POSIX 可以，
  // 而換行是這個協定的框界 —— 收端會在中途以為訊息結束
  void setModelRequestIsSingleLine() { QCOMPARE(encodeSetModelRequest("/a/b\nc.model3.json"), std::string("setmodel /a/b c.model3.json\n")); }

  // 回覆解析：成功、失敗、以及對方沒回話（連線斷了）
  void parsesReplies() {
    QVERIFY(parseInstanceReply("ok\n").ok);
    const InstanceReply failed = parseInstanceReply("error Model not found\n");
    QVERIFY(!failed.ok);
    QCOMPARE(failed.message, std::string("Model not found"));
    QVERIFY(!parseInstanceReply("").ok);
  }
};

QTEST_APPLESS_MAIN(TestExternalModel)
#include "test_external_model.moc"
