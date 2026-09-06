#pragma once

// --splash 子行程：把啟動畫面搬到獨立的行程裡跑。
//
// 為什麼需要獨立行程：模型載入全程同步阻塞主行程的 GUI 執行緒，其中
// CreateRenderer（Cubism 整套 shader 的一次性 compile+link）實測就要 2.3 秒，
// 是單一 Framework 呼叫又需要 current GL context，既不能細分插進度點、
// 也不能丟 worker。同一個行程裡不管怎麼推畫面，那段動畫都一定是凍的。
// 另一個行程有自己的訊息迴圈，主行程卡多久都跟它無關。
//
// 這支程式跟 --mcp-stdio 一樣，是同一個執行檔裡完全不同的程式：
// 在 main() 最前面短路，不建 ConfigStore、不碰 GL、不搶 single-instance 鎖。
//
// 生命週期靠一條 QLocalSocket 連線表達，不需要任何平台專屬的行程監控：
//   splash 子行程 listen → 主行程連上來並「保持連線」
//   → 模型載入完成，主行程主動斷線 → splash 淡出後結束
//   → 主行程若崩潰，OS 會自動關閉 socket，splash 同樣收到 disconnected
// 也就是說「該收了」與「主行程死了」是同一個訊號，孤兒視窗的清理是免費的。

#include <QString>

namespace l2m {

// 短路旗標，用法與 core/autostart_command.h 的 kHiddenFlag 一致
inline constexpr const char* kSplashFlag = "--splash";

// 子行程進入點。argv: --splash <serverName>
int runSplashProcess(int argc, char* argv[]);

// 主行程用：這次啟動專用的 local server 名稱（帶 pid，避免兩次啟動撞名）
QString splashServerName();

}  // namespace l2m
