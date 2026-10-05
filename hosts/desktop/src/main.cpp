#include "rime/core/json.hpp"
#include "rime/core/runtime.hpp"
#include "rime/desktop/host.hpp"

#ifdef RIME_DESKTOP_HAS_SCRIPT
#include "rime/win32/bootstrap.hpp"
#endif

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

// Stable wire names for TraceKind, mirroring the enum so JSON Lines consumers
// (CLI, golden diffs) key on names instead of raw ordinals.
const char* trace_kind_name(const rime::core::TraceKind kind) {
  switch (kind) {
    case rime::core::TraceKind::EventAccepted:
      return "EventAccepted";
    case rime::core::TraceKind::EventDispatchStarted:
      return "EventDispatchStarted";
    case rime::core::TraceKind::EventDispatchFinished:
      return "EventDispatchFinished";
    case rime::core::TraceKind::ActionStarted:
      return "ActionStarted";
    case rime::core::TraceKind::ActionFinished:
      return "ActionFinished";
    case rime::core::TraceKind::StateChanged:
      return "StateChanged";
    case rime::core::TraceKind::ActionAccepted:
      return "ActionAccepted";
    case rime::core::TraceKind::ActionRefused:
      return "ActionRefused";
  }
  return "Unknown";
}

// Reads RIME_TRACE without touching MSVC's deprecated getenv (C4996 would
// become noise or an error under /WX): the secure CRT on MSVC, std::getenv
// everywhere else.
std::string trace_flag() {
#if defined(_MSC_VER)
  char* buffer = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&buffer, &size, "RIME_TRACE") != 0 || buffer == nullptr) return {};
  std::string value(buffer);
  free(buffer);
  return value;
#else
  const char* value = std::getenv("RIME_TRACE");
  return value == nullptr ? std::string{} : std::string(value);
#endif
}

// Opt-in read surface: RIME_TRACE=1 (any non-empty value except "0") dumps
// the trace snapshot as JSON Lines on stderr. Default (unset/empty/0) keeps
// the host silent, exactly as before.
bool trace_dump_enabled() {
  const std::string value = trace_flag();
  return !value.empty() && value != "0";
}

// Best-effort JSON Lines dump, one TraceEntry per line, field names matching
// the struct. Never throws and never influences the exit code: this runs from
// a destructor on every exit path (including start/post/stop failures).
void dump_trace(const std::shared_ptr<rime::core::InMemoryTrace>& trace) {
  if (!trace_dump_enabled()) return;
  try {
    for (const rime::core::TraceEntry& entry : trace->snapshot()) {
      rime::core::json::Value line = rime::core::json::Value::object();
      line.set("sequence", rime::core::json::Value::number(static_cast<double>(entry.sequence)));
      line.set("kind", rime::core::json::Value::string(trace_kind_name(entry.kind)));
      line.set("subject", rime::core::json::Value::string(entry.subject));
      line.set("detail", rime::core::json::Value::string(entry.detail));
      line.set("actionId", rime::core::json::Value::number(static_cast<double>(entry.action_id)));
      line.set("capability", rime::core::json::Value::string(entry.capability));
      line.set("resultCode", rime::core::json::Value::string(entry.result_code));
      line.set("durationMs", rime::core::json::Value::number(static_cast<double>(entry.duration_ms)));
      line.set("unixMs", rime::core::json::Value::number(static_cast<double>(entry.unix_ms)));
      line.set("queueWaitMs",
               rime::core::json::Value::number(static_cast<double>(entry.queue_wait_ms)));
      std::cerr << rime::core::json::stringify(line) << '\n';
    }
    std::cerr.flush();
  } catch (...) {
    // Diagnostics only: a broken sink, allocation failure or stream error
    // must not change the host's exit code.
  }
}

struct TraceDumpOnExit {
  std::shared_ptr<rime::core::InMemoryTrace> trace;
  ~TraceDumpOnExit() { dump_trace(trace); }
};

}  // namespace

int main(int argc, char** argv) {
#ifdef RIME_DESKTOP_HAS_SCRIPT
  // Script mode: the production host runs one script on the full wiring
  // (services, kernel policy, executors, JS modules) with the production
  // capability grant. This is the same Bootstrap the bundle harness runs.
  if (argc == 2) {
    return rime::win32::run_bundle_file(argv[1], rime::win32::production_capabilities());
  }
  if (argc > 2) {
    std::cerr << "usage: rime_host [<script>]\n";
    return 2;
  }
#else
  (void)argc;
  (void)argv;
#endif
  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  // Covers every remaining return path below, so a failed run still dumps.
  TraceDumpOnExit trace_dump{trace};
  rime::core::Runtime runtime(64, trace);
  rime::desktop::DesktopHost host(runtime);
  runtime.set_handler([](const rime::core::Event& event, rime::core::CancellationToken token) {
    if (!token.cancelled()) std::cout << event.name << "\n";
  });
  const auto start_result = host.start();
  if (!start_result.ok()) {
    std::cerr << "desktop host start failed: " << start_result.message << "\n";
    return 1;
  }
  const auto post_result =
      runtime.post({0, rime::core::EventKind::Input, "host.ready", "", ""});
  if (!post_result.ok()) {
    std::cerr << "desktop host post failed: " << post_result.message << "\n";
    // The runtime started above, so stop the host before exiting instead of
    // leaking a running host; the exit code is unchanged (1).
    const auto stop_after_post_error = host.stop();
    if (!stop_after_post_error.ok()) {
      std::cerr << "desktop host stop failed: " << stop_after_post_error.message << "\n";
    }
    return 1;
  }
  // pump() returns the number of dispatched events; the count is observability
  // only here, so discard it explicitly.
  (void)runtime.pump();
  const auto stop_result = host.stop();
  if (!stop_result.ok()) {
    std::cerr << "desktop host stop failed: " << stop_result.message << "\n";
    return 1;
  }
  return 0;
}
