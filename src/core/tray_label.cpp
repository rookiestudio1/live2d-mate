#include "tray_label.h"

#include "i18n.h"

namespace l2m {

std::string formatNamedLabel(const std::string& locale, const std::string& meaning, const std::string& raw, const std::string& suffix) {
  const std::string name = raw.empty() ? i18n::translate(locale, "tray.unnamedGroup") : raw;
  if (!meaning.empty()) {
    return i18n::translate(locale, "tray.namedLabel", i18n::TParams().arg("meaning", meaning).arg("raw", name).arg("suffix", suffix));
  }
  return i18n::translate(locale, "tray.plainLabel", i18n::TParams().arg("raw", name).arg("suffix", suffix));
}

}  // namespace l2m
