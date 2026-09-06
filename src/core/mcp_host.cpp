#include "mcp_host.h"

#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QUrl>

#include <algorithm>
#include <set>

#include "string_util.h"

namespace l2m {

namespace {

// 去掉前後空白與 IPv6 的方括號，比較時才對得上
std::string normalizeHost(const std::string& host) {
  std::string value = strutil::toLowerAscii(strutil::trim(host));
  if (!value.empty() && value.front() == '[') value.erase(value.begin());
  if (!value.empty() && value.back() == ']') value.pop_back();
  return value;
}

// IPv6 位址在網址裡要加方括號
std::string forUrl(const std::string& host) {
  const std::string value = strutil::trim(host);
  if (value.find(':') != std::string::npos && (value.empty() || value.front() != '[')) {
    return "[" + value + "]";
  }
  return value;
}

}  // namespace

bool isLongToolCall(const std::string& tool, bool wait) {
  (void)wait;  // 刻意不看，理由見標頭
  return tool == "speak" || tool == "think" || tool == "perform";
}

bool isLoopbackHost(const std::string& host) {
  const std::string value = normalizeHost(host);
  if (value == "localhost" || value == "::1") return true;
  return value.rfind("127.", 0) == 0;
}

bool requiresToken(const std::string& host) { return !isLoopbackHost(host); }

std::optional<std::string> validateMcpBinding(const std::string& host, const std::optional<std::string>& token) {
  if (!requiresToken(host)) return std::nullopt;
  if (token.has_value() && !strutil::trim(*token).empty()) return std::nullopt;
  return "Listening on " + host + " exposes the MCP server to your network; an access token is required";
}

std::vector<McpHostOption> listHostOptions(const std::vector<NetworkAddress>& interfaces) {
  std::set<std::string> seen;
  std::vector<McpHostOption> found;

  for (const auto& addr : interfaces) {
    if (addr.internal || !addr.ipv4) continue;
    if (!seen.insert(addr.address).second) continue;
    found.push_back({addr.address, McpHostKind::Interface, addr.iface});
  }
  std::sort(found.begin(), found.end(), [](const McpHostOption& a, const McpHostOption& b) { return a.value < b.value; });

  std::vector<McpHostOption> options;
  options.push_back({kLoopbackHost, McpHostKind::Loopback, ""});
  options.push_back({kAllInterfacesHost, McpHostKind::All, ""});
  options.insert(options.end(), found.begin(), found.end());
  return options;
}

std::string resolveAdvertisedHost(const std::string& host, const std::vector<NetworkAddress>& interfaces) {
  const std::string value = normalizeHost(host);
  if (value != kAllInterfacesHost && value != "::") return host;

  for (const auto& option : listHostOptions(interfaces)) {
    if (option.kind == McpHostKind::Interface) return option.value;
  }
  return kLoopbackHost;
}

std::string mcpOrigin(const std::string& host, int port) { return "http://" + forUrl(host) + ":" + std::to_string(port); }

std::string mcpUrl(const std::string& host, int port) { return mcpOrigin(host, port) + "/mcp"; }

std::string generateToken() {
  // 24 bytes → base64url（無填充）
  QByteArray bytes(24, Qt::Uninitialized);
  QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(bytes.data()), reinterpret_cast<quint32*>(bytes.data() + bytes.size()));
  return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals).toStdString();
}

bool isAllowedOrigin(const std::optional<std::string>& origin, const std::string& boundHost) {
  // 非瀏覽器的 MCP 用戶端不送 Origin，沒有就一律放行
  if (!origin.has_value() || origin->empty()) return true;

  const QUrl url(QString::fromStdString(*origin));
  if (!url.isValid() || url.host().isEmpty()) return false;

  const std::string hostname = url.host().toStdString();
  if (isLoopbackHost(hostname)) return true;
  // 綁到區網時，從那個位址開的頁面才算自己人
  return !isLoopbackHost(boundHost) && hostname == normalizeHost(boundHost);
}

std::vector<NetworkAddress> systemNetworkAddresses() {
  std::vector<NetworkAddress> out;
  for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
    if (!iface.flags().testFlag(QNetworkInterface::IsUp)) continue;
    const bool loopback = iface.flags().testFlag(QNetworkInterface::IsLoopBack);
    for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
      const QHostAddress address = entry.ip();
      NetworkAddress record;
      record.address = address.toString().toStdString();
      record.ipv4 = address.protocol() == QAbstractSocket::IPv4Protocol;
      record.internal = loopback || address.isLoopback();
      record.iface = iface.humanReadableName().toStdString();
      out.push_back(std::move(record));
    }
  }
  return out;
}

}  // namespace l2m
