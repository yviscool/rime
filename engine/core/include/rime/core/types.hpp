#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rime::core {

using Sequence = std::uint64_t;
using ActionId = std::uint64_t;

struct Error {
  enum class Code : std::uint16_t {
    None = 0,
    InvalidState,
    QueueClosed,
    QueueFull,
    Cancelled,
    Timeout,
    CapabilityDenied,
    InvalidContract,
    Unsupported,
    ExecutionFailed,
    // Appended after the v1 codes so existing numeric values never shift:
    // the directed target of an operation (e.g. a WindowId) resolved to
    // nothing - destroyed, or issued by a previous service generation.
    TargetGone,
  };

  Code code{Code::None};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == Code::None; }
  static Error none() { return {}; }
};

// Contract-level lowercase names for Error::Code, shared by the action
// codec, the host ABI protocol and Trace output.
const char* error_code_name(Error::Code code);
bool error_code_from_name(std::string_view name, Error::Code& out);

}  // namespace rime::core
