#include "rime/core/runtime.hpp"
#include "rime/desktop/host.hpp"

#ifdef RIME_DESKTOP_HAS_SCRIPT
#include "rime/win32/bootstrap.hpp"
#endif

#include <iostream>

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
