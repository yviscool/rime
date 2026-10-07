#include "rime/win32/window_seam.hpp"

#include "rime/core/types.hpp"

#include <mutex>
#include <optional>

namespace rime::win32::window_seam {

namespace {

std::mutex& mutex() {
  static std::mutex instance;
  return instance;
}

std::optional<Fault>& armed() {
  static std::optional<Fault> fault;
  return fault;
}

}  // namespace

bool arm(Fault fault) {
  std::lock_guard lock(mutex());
  if (armed().has_value()) return false;
  armed() = std::move(fault);
  return true;
}

void clear() {
  std::lock_guard lock(mutex());
  armed().reset();
}

bool consume(const std::string& method, rime::core::Error& out) {
  std::lock_guard lock(mutex());
  if (!armed().has_value()) return false;
  Fault& fault = *armed();
  if (!fault.method.empty() && fault.method != method) return false;
  switch (fault.kind) {
    case FaultKind::Denied:
      out = {rime::core::Error::Code::ExecutionFailed,
             "injected foreground denial (window_seam)"};
      break;
    case FaultKind::Timeout:
      out = {rime::core::Error::Code::Timeout, "injected timeout (window_seam)"};
      break;
    case FaultKind::TargetGone:
      out = {rime::core::Error::Code::TargetGone, "injected target-gone (window_seam)"};
      break;
  }
  if (fault.times > 0 && --fault.times == 0) armed().reset();
  return true;
}

}  // namespace rime::win32::window_seam
