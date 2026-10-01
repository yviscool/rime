#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/win32/clipboard.hpp"
#include "rime/win32/clipboard_executor.hpp"

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

}  // namespace

int main() {
  ClipboardService service;

  // Preserve whatever text the clipboard held before the test.
  // NOTE: save/restore is manual here: if an assert() aborts mid-test the
  // restore at the end never runs. A small RAII scope-guard restoring the
  // original text in its destructor would be safer.
  // TODO(clipboard-test): use an RAII guard for clipboard save/restore so
  // early failures still restore the original content.
  std::string original;
  assert(service.read_text(original).ok());

  // Non-ASCII text round-trips through CF_UNICODETEXT.
  const std::string sample = "rime-clip-\xE6\xB5\x8B\xE8\xAF\x95-\xE2\x9C\x93";
  assert(service.write_text(sample).ok());
  std::string read_back;
  assert(service.read_text(read_back).ok());
  assert(read_back == sample);

  // Writing again replaces the previous content.
  const std::string replacement = "rime-clip-replacement";
  assert(service.write_text(replacement).ok());
  assert(service.read_text(read_back).ok());
  assert(read_back == replacement);

  // The executor drives clipboard.write through the kernel.
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
  const auto result = kernel.execute(write);
  assert(result.succeeded);
  const auto* value = result.value.find("text");
  assert(value && value->is_string());
  assert(value->as_string() == "rime-clipboard-executor");
  assert(service.read_text(read_back).ok());
  assert(read_back == "rime-clipboard-executor");

  // Empty text is a valid write (clears textual content).
  rime::action::Action clear = write;
  clear.id = 2;
  clear.payload = "{\"text\":\"\"}";
  assert(kernel.execute(clear).succeeded);
  assert(service.read_text(read_back).ok());
  assert(read_back.empty());

  // Contract violations reject with InvalidContract.
  // TODO(test): avoid executing the same bad Action twice; store the Result
  // once and reuse it for both assertions (see process_tests fix).
  rime::action::Action missing = write;
  missing.id = 3;
  missing.payload = "{}";
  assert(!kernel.execute(missing).succeeded);
  assert(kernel.execute(missing).error.code == rime::core::Error::Code::InvalidContract);

  rime::action::Action bad_kind = write;
  bad_kind.id = 4;
  bad_kind.target = {"window", "1"};
  assert(!kernel.execute(bad_kind).succeeded);
  assert(kernel.execute(bad_kind).error.code == rime::core::Error::Code::InvalidContract);

  // A policy without the capability denies before the executor runs.
  rime::action::Kernel denied(
      std::make_shared<rime::action::StaticCapabilityPolicy>(std::unordered_set<std::string>{}));
  assert(denied.register_executor("clipboard.write", executor).ok());
  const auto refused = denied.execute(write);
  assert(!refused.succeeded);
  assert(refused.error.code == rime::core::Error::Code::CapabilityDenied);

  // Restore the original clipboard text last.
  // TODO(clipboard-test): see RAII save/restore note at the top of main().
  assert(service.write_text(original).ok());
  return 0;
}
