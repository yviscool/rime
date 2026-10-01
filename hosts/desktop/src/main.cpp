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
  if (!host.start().ok()) return 1;
  runtime.post({0, rime::core::EventKind::Input, "host.ready", ""});
  runtime.pump();
  return host.stop().ok() ? 0 : 1;
}
