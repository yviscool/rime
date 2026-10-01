#include "rime/core/types.hpp"

#include <array>
#include <string_view>

namespace rime::core {
namespace {

struct ErrorCodeName {
  Error::Code code;
  std::string_view name;
};

constexpr std::array k_error_code_names{
    ErrorCodeName{Error::Code::None, "none"},
    ErrorCodeName{Error::Code::InvalidState, "invalid_state"},
    ErrorCodeName{Error::Code::QueueClosed, "queue_closed"},
    ErrorCodeName{Error::Code::QueueFull, "queue_full"},
    ErrorCodeName{Error::Code::Cancelled, "cancelled"},
    ErrorCodeName{Error::Code::Timeout, "timeout"},
    ErrorCodeName{Error::Code::CapabilityDenied, "capability_denied"},
    ErrorCodeName{Error::Code::InvalidContract, "invalid_contract"},
    ErrorCodeName{Error::Code::Unsupported, "unsupported"},
    ErrorCodeName{Error::Code::ExecutionFailed, "execution_failed"},
    ErrorCodeName{Error::Code::TargetGone, "target_gone"},
};

}  // namespace

const char* error_code_name(const Error::Code code) {
  for (const ErrorCodeName& entry : k_error_code_names) {
    if (entry.code == code) return entry.name.data();
  }
  return "execution_failed";
}

bool error_code_from_name(const std::string_view name, Error::Code& out) {
  for (const ErrorCodeName& entry : k_error_code_names) {
    if (entry.name == name) {
      out = entry.code;
      return true;
    }
  }
  return false;
}

}  // namespace rime::core
