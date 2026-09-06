#pragma once

// 設定的持久化與變更通知。
//
// 壞掉的 config.json 會被備份成 config.bak.json 後以預設值重建，
// 不讓使用者手改壞一個欄位就整個 app 起不來。

#include <QObject>
#include <QTimer>

#include <filesystem>
#include <string>

#include "config_schema.h"

namespace l2m {

class ConfigStore : public QObject {
  Q_OBJECT

public:
  explicit ConfigStore(std::filesystem::path filePath, QObject* parent = nullptr);

  const AppConfig& get() const { return data_; }
  const std::filesystem::path& path() const { return filePath_; }

  // 套用部分更新（兩層的 JSON 物件，例如 {"model":{"scale":1.5}}）並回傳新設定；
  // 沒有實際變化時不會觸發事件與寫檔。不合法的值丟 std::runtime_error。
  const AppConfig& patch(const std::string& patchJson);

  // 立刻寫入磁碟（app 結束前需呼叫，避免最後一次變更遺失）
  void flush();

signals:
  void changed(const l2m::AppConfig& config);

private:
  AppConfig load();
  void backup(const std::string& reason);
  void scheduleWrite();

  std::filesystem::path filePath_;
  AppConfig data_;
  QTimer writeTimer_;
};

}  // namespace l2m
