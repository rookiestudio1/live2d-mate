#pragma once

// Happy Eyeballs（RFC 8305）的位址排序（純算，不碰 socket）。
//
// 為什麼需要：Qt 的 QAbstractSocket/QWebSocket 對 DNS 回傳的位址是**逐一嘗試**，
// 每個要等滿 OS 的 TCP 連線逾時（Windows 實測約 21 秒）才換下一個。
// speech.platform.bing.com 的 DNS 會回 2 個 AAAA + 2 個 A，其中某個 IPv6
// 在部分網路是黑洞（SYN 無回應），實測讓 wss 連線卡了 42.3 秒、
// 語音清單 GET 撞滿 15 秒逾時 —— 使用者按「測試語音」要 20 秒以上才出聲。
// curl 與 Chromium 因為有 Happy Eyeballs 並行嘗試所以從不受影響。
//
// 這裡只做「探測順序」的決策：維持 DNS 回傳的家族內順序，但讓位址家族交錯、
// 第一個位址的家族先行。搭配 media/endpoint_prober.h 以固定間隔錯開發起連線，
// 單一家族整段壞掉最多只多等一個間隔，而不是 N × 21 秒。
//
// 抽到 l2m_core 的理由是可測性：排序是純函式，對應 tests/test_happy_eyeballs.cpp；
// 真正開 socket 探測的在 l2m_media 那側。

#include <QHostAddress>

#include <vector>

namespace l2m::net {

// 每個連線嘗試之間的錯開間隔（RFC 8305 的 Connection Attempt Delay 建議值）
inline constexpr int kAttemptDelayMs = 250;

// 探測的整體上限：超過就放棄，退回 Qt 原本的行為（直接用主機名連線）
inline constexpr int kProbeTimeoutMs = 4000;

// 探測出來的贏家位址的快取壽命。這類服務是 anycast，路由會漂移，
// 不能永久信任同一個位址
inline constexpr int kWinnerTtlMs = 10 * 60 * 1000;

// 位址家族交錯排序：第一個位址的家族先行，家族內維持原順序。
// 例：[v6a, v6b, v4a, v4b] → [v6a, v4a, v6b, v4b]
std::vector<QHostAddress> orderForProbe(const std::vector<QHostAddress>& addresses);

}  // namespace l2m::net
