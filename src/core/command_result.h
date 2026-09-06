#pragma once

// 應用層命令的統一回傳值。
//
// 失敗時除了 error 還一定要給 hint，把「有哪些是有效的」一併列出來 ——
// 這條規則是整個 MCP 介面對 AI 好用的關鍵：AI 讀完 hint 就能自己修正重試，
// 不必再多打一輪 list_* 工具。系統匣的錯誤對話框也吃同一組字串。
//
// data 是已經序列化好的 JSON 字串（不是型別參數）：這一層的呼叫者不是系統匣
// 就是 MCP，兩邊最後都要 JSON，中間再包一層泛型只是徒增樣板。

#include <string>
#include <utility>

namespace l2m {

struct CommandResult {
  bool ok = false;
  std::string error;
  std::string hint;
  // 成功時的酬載，JSON 文字；沒有酬載就留空
  std::string dataJson;

  explicit operator bool() const { return ok; }

  static CommandResult success(std::string dataJson = {}) {
    CommandResult r;
    r.ok = true;
    r.dataJson = std::move(dataJson);
    return r;
  }

  static CommandResult failure(std::string error, std::string hint = {}) {
    CommandResult r;
    r.ok = false;
    r.error = std::move(error);
    r.hint = std::move(hint);
    return r;
  }
};

}  // namespace l2m
