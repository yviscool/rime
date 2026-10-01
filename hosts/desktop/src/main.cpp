#include "rime/core/runtime.hpp"
#include "rime/desktop/host.hpp"

#include <iostream>

int main() {
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
  const auto post_result = runtime.post({0, rime::core::EventKind::Input, "host.ready", ""});
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
