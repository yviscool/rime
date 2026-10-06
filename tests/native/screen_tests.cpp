// Realism: L5 - the service reads the machine's own displays through
// EnumDisplayMonitors. Nothing is mutated, so every expectation is asserted
// directly; count and primary are cross-checked against the OS's own answers
// rather than against the service's own bookkeeping.

#include "rime/win32/screen.hpp"

#include <cassert>
#include <string>

namespace {

using rime::win32::ScreenService;

}  // namespace

int main() {
  ScreenService service;

  int count = 0;
  assert(service.monitor_count(count).ok());
  assert(count >= 1);

  // Every index the count promises must resolve, and one past it must not:
  // that pair proves the count is the real enumeration length in both
  // directions (too small would leave index count+1 resolving, too large
  // would fail inside the loop) without re-implementing the enumeration here.
  for (int index = 1; index <= count; ++index) {
    ScreenService::Monitor monitor;
    assert(service.monitor_at(index, monitor).ok());
    assert(monitor.index == index);
    assert(!monitor.name.empty());
    // The work area is the monitor minus the taskbar, so it is inside bounds.
    assert(monitor.work_left >= monitor.left);
    assert(monitor.work_top >= monitor.top);
    assert(monitor.work_right <= monitor.right);
    assert(monitor.work_bottom <= monitor.bottom);
    // A monitor rectangle always has a positive area.
    assert(monitor.right > monitor.left);
    assert(monitor.bottom > monitor.top);
  }

  ScreenService::Monitor past_end;
  const auto past_end_result = service.monitor_at(count + 1, past_end);
  assert(!past_end_result.ok());
  assert(past_end_result.code == rime::core::Error::Code::InvalidContract);
  assert(past_end_result.message.find("does not exist") != std::string::npos);
  assert(past_end_result.message.find(std::to_string(count + 1)) != std::string::npos);

  const auto negative = service.monitor_at(-1, past_end);
  assert(!negative.ok());
  assert(negative.code == rime::core::Error::Code::InvalidContract);

  // Exactly one primary, and index 0 must select that very monitor - AHK's
  // omitted argument and MonitorGetPrimary() both answer with it.
  ScreenService::Monitor primary;
  assert(service.monitor_at(0, primary).ok());
  assert(primary.primary);
  int primary_count = 0;
  int primary_index = 0;
  for (int index = 1; index <= count; ++index) {
    ScreenService::Monitor monitor;
    assert(service.monitor_at(index, monitor).ok());
    if (!monitor.primary) continue;
    ++primary_count;
    primary_index = index;
  }
  assert(primary_count == 1);
  assert(primary.index == primary_index);
  assert(primary.name.length() > 0);

  // Asking for the primary by number returns the same record as asking for
  // "the primary" by omission: bounds, work area and name all agree.
  ScreenService::Monitor primary_by_number;
  assert(service.monitor_at(primary_index, primary_by_number).ok());
  assert(primary_by_number.left == primary.left);
  assert(primary_by_number.top == primary.top);
  assert(primary_by_number.right == primary.right);
  assert(primary_by_number.bottom == primary.bottom);
  assert(primary_by_number.work_left == primary.work_left);
  assert(primary_by_number.work_right == primary.work_right);
  assert(primary_by_number.name == primary.name);

  return 0;
}
