#pragma once

#include "rime/core/types.hpp"

#include <string>

namespace rime::win32 {

// Text clipboard over CF_UNICODETEXT. The clipboard is a shared critical
// section: opens are retried briefly and always closed (RAII in the .cpp).
class ClipboardService final {
 public:
  // Empty `out` (success) means the clipboard holds no text format.
  rime::core::Error read_text(std::string& out) const;
  rime::core::Error write_text(const std::string& utf8_text) const;
};

}  // namespace rime::win32
