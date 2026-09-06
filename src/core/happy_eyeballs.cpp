#include "happy_eyeballs.h"

#include <algorithm>

namespace l2m::net {

namespace {

// v4-mapped v6（::ffff:a.b.c.d）實際走 IPv4，歸到 v4 那邊交錯才有意義
bool isV4(const QHostAddress& address) {
  bool ok = false;
  address.toIPv4Address(&ok);
  return ok;
}

}  // namespace

std::vector<QHostAddress> orderForProbe(const std::vector<QHostAddress>& addresses) {
  std::vector<QHostAddress> v6;
  std::vector<QHostAddress> v4;
  for (const QHostAddress& address : addresses) {
    (isV4(address) ? v4 : v6).push_back(address);
  }

  const bool v4First = addresses.empty() || isV4(addresses.front());
  const std::vector<QHostAddress>& first = v4First ? v4 : v6;
  const std::vector<QHostAddress>& second = v4First ? v6 : v4;

  std::vector<QHostAddress> ordered;
  ordered.reserve(addresses.size());
  const size_t longest = std::max(first.size(), second.size());
  for (size_t i = 0; i < longest; ++i) {
    if (i < first.size()) ordered.push_back(first[i]);
    if (i < second.size()) ordered.push_back(second[i]);
  }
  return ordered;
}

}  // namespace l2m::net
