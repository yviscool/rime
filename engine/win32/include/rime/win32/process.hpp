#pragma once

#include "rime/core/json.hpp"
#include "rime/core/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rime::win32 {

struct ProcessInfo {
  std::uint32_t pid{0};
  std::uint32_t parent_pid{0};
  // Executable file name, e.g. "notepad.exe".
  std::string name;
  // Full image path; empty when the process cannot be queried.
  std::string exe_path;
};

struct LaunchSpec {
  std::wstring executable;
  std::wstring arguments;
  std::wstring working_dir;
};

// Snapshot-based process queries; `launch`/`terminate` own no long-lived
// handles, so the service needs no lifecycle of its own.
class ProcessService final {
 public:
  [[nodiscard]] rime::core::Error list(std::vector<ProcessInfo>& out) const;
  [[nodiscard]] rime::core::Error info(std::uint32_t pid, ProcessInfo& out) const;
  [[nodiscard]] rime::core::Error launch(const LaunchSpec& spec, std::uint32_t& pid) const;
  [[nodiscard]] rime::core::Error terminate(std::uint32_t pid, int exit_code) const;
};

[[nodiscard]] rime::core::json::Value process_info_json(const ProcessInfo& process);

}  // namespace rime::win32
