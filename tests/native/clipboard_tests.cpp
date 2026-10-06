// Realism: L5 - real clipboard roundtrip through the real Kernel and
// clipboard executor; the test saves and restores the clipboard it touches.

#include "rime/action/kernel.hpp"
#include "rime/core/lane.hpp"
#include "rime/core/json.hpp"
#include "rime/win32/clipboard.hpp"
#include "rime/win32/clipboard_executor.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <string>
#include <unordered_set>

namespace {

using rime::win32::ClipboardService;

std::uint64_t deadline_ms() {
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) +
         5000;
}

// Restores the pre-test clipboard text when it leaves scope. The explicit
// write-back before the assert phase still covers abort() (which runs no
// destructors); the destructor covers early returns.
struct ClipboardGuard {
  explicit ClipboardGuard(ClipboardService& service) : service_(service) {
    const auto saved = service_.read_text(original);
    assert(saved.ok());
  }
  ~ClipboardGuard() { (void)service_.write_text(original); }

  ClipboardService& service_;
  std::string original;
};

}  // namespace

int main() {
  ClipboardService service;
  // Executors require the worker lane; this harness executes on the main thread.
  assert(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Worker).ok());

  // Preserve whatever text the clipboard held before the test via the guard
  // (the explicit restore below still runs first so an abort() cannot leave
  // the clipboard polluted). Asserts are safe while the clipboard is still
  // untouched.
  ClipboardGuard guard(service);

  // Kernel/executor setup runs before any mutation so a failure here aborts
  // with the user's clipboard unchanged.
  rime::action::Kernel kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"windows.clipboard.write"}));
  const auto executor = std::make_shared<rime::win32::ClipboardExecutor>(service);
  assert(kernel.register_executor("clipboard.write", executor).ok());

  rime::action::Action write;
  write.id = 1;
  write.source = {"test", "clipboard_tests"};
  write.type = "clipboard.write";
  write.capability = "windows.clipboard.write";
  write.target = {"clipboard", "default"};
  write.deadline_unix_ms = deadline_ms();
  write.payload = "{\"text\":\"rime-clipboard-executor\"}";

  // --- Capture phase ---------------------------------------------------
  // Mutations and reads run WITHOUT asserting: assert() calls abort() and
  // abort runs no destructors, so the guard destructor alone cannot restore
  // the clipboard after a mid-phase abort. Instead every outcome is recorded,
  // the original text is written back, and only then are the outcomes
  // asserted below (with the clipboard already restored).
  const std::string sample = "rime-clip-\xE6\xB5\x8B\xE8\xAF\x95-\xE2\x9C\x93";
  const bool wrote_sample = service.write_text(sample).ok();
  std::string read_back;
  const bool read_sample = service.read_text(read_back).ok() && read_back == sample;
  // ClipWait's two predicates against real clipboard state: text present
  // means both "text" and "any" are satisfied.
  const bool wait_text_when_full = service.has_wait_data(false);
  const bool wait_any_when_full = service.has_wait_data(true);

  const std::string replacement = "rime-clip-replacement";
  const bool wrote_replacement = service.write_text(replacement).ok();
  const bool read_replacement = service.read_text(read_back).ok() && read_back == replacement;

  // The executor drives clipboard.write through the kernel.
  const auto result = kernel.execute(write);
  const auto* value = result.value.find("text");
  const bool executor_value =
      result.succeeded && value && value->is_string() &&
      value->as_string() == "rime-clipboard-executor";
  const bool executor_read =
      service.read_text(read_back).ok() && read_back == "rime-clipboard-executor";

  // Empty text is a valid write and leaves the clipboard truly empty (no
  // CF_UNICODETEXT), matching AHK `Clipboard := ""` - ClipWait needs an
  // actually-empty clipboard to be able to wait at all.
  rime::action::Action clear = write;
  clear.id = 2;
  clear.payload = "{\"text\":\"\"}";
  const bool clear_ok = kernel.execute(clear).succeeded;
  const bool clear_read = service.read_text(read_back).ok() && read_back.empty();
  const bool clear_format_gone = !IsClipboardFormatAvailable(CF_UNICODETEXT);
  // An empty clipboard satisfies neither ClipWait predicate - otherwise
  // ClipWait could never observe "not ready yet".
  const bool wait_data_empty = !service.has_wait_data(false) && !service.has_wait_data(true);

  // Contract violations reject with InvalidContract (executed once, asserted
  // twice below).
  rime::action::Action missing = write;
  missing.id = 3;
  missing.payload = "{}";
  const auto missing_result = kernel.execute(missing);

  rime::action::Action bad_kind = write;
  bad_kind.id = 4;
  bad_kind.target = {"window", "1"};
  const auto bad_kind_result = kernel.execute(bad_kind);

  // A policy without the capability denies before the executor runs (and
  // therefore never touches the clipboard).
  rime::action::Kernel denied(
      std::make_shared<rime::action::StaticCapabilityPolicy>(std::unordered_set<std::string>{}));
  const bool denied_registered = denied.register_executor("clipboard.write", executor).ok();
  const auto refused = denied.execute(write);

  // --- Clipboard busy (real OS contention) ------------------------------
  // A real owner window holds the clipboard open (a plain nullptr hold
  // would not contend: the service also opens with a nullptr owner, and
  // Windows treats a same-owner re-open as reentrant success). The held
  // handle changes no content (OpenClipboard fails before any mutation), so
  // the restore below is unaffected; the handle is released before it runs.
  // The executor path proves the error propagates through the Kernel as a
  // Result, not just as a service error.
  const HWND hold_window =
      CreateWindowExW(0, L"STATIC", L"rime-busy-holder", WS_OVERLAPPED, 0, 0, 1, 1, nullptr,
                      nullptr, GetModuleHandleW(nullptr), nullptr);
  const bool clipboard_held = hold_window != nullptr && OpenClipboard(hold_window);
  const auto busy_write = service.write_text("rime-clip-busy-probe");
  const auto busy_result = kernel.execute(write);
  const auto busy_read = service.read_text(read_back);
  if (clipboard_held) CloseClipboard();
  if (hold_window != nullptr) DestroyWindow(hold_window);

  // Restore the original clipboard text before the first assert that could
  // abort: from here on a failure can no longer leave the user's clipboard
  // polluted.
  const bool restored = service.write_text(guard.original).ok();

  // --- Assert phase ----------------------------------------------------
  // Non-ASCII text round-trips through CF_UNICODETEXT.
  assert(wrote_sample);
  assert(read_sample);

  // Writing again replaces the previous content.
  assert(wrote_replacement);
  assert(read_replacement);

  assert(executor_value);
  assert(executor_read);

  assert(clear_ok);
  assert(clear_read);
  // Captured before the restore: the empty write dropped the format itself.
  assert(clear_format_gone);

  assert(wait_text_when_full);
  assert(wait_any_when_full);
  assert(wait_data_empty);

  assert(!missing_result.succeeded);
  assert(missing_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(!bad_kind_result.succeeded);
  assert(bad_kind_result.error.code == rime::core::Error::Code::InvalidContract);

  assert(denied_registered);
  assert(!refused.succeeded);
  assert(refused.error.code == rime::core::Error::Code::CapabilityDenied);

  // Held-open precondition is asserted (not skipped): if OpenClipboard had
  // failed the three probes above would have mutated the clipboard and every
  // busy assertion below would be meaningless.
  assert(clipboard_held);
  assert(!busy_write.ok());
  assert(busy_write.code == rime::core::Error::Code::ExecutionFailed);
  assert(busy_write.message == "clipboard is busy");
  assert(!busy_result.succeeded);
  assert(busy_result.error.code == rime::core::Error::Code::ExecutionFailed);
  assert(busy_result.error.message == "clipboard is busy");
  assert(!busy_read.ok());
  assert(busy_read.code == rime::core::Error::Code::ExecutionFailed);
  assert(busy_read.message == "clipboard is busy");

  assert(restored);
  return 0;
}
