#include "rime/action/kernel.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/clipboard.hpp"
#include "rime/win32/input.hpp"
#include "rime/win32/js_clipboard.hpp"
#include "rime/win32/js_input.hpp"
#include "rime/win32/js_process.hpp"
#include "rime/win32/js_window.hpp"
#include "rime/win32/process.hpp"
#include "rime/win32/window.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: rime_js_bundle <bundle.js>\n";
    return 2;
  }

  std::ifstream input(argv[1], std::ios::binary);
  if (!input) {
    std::cerr << "cannot open bundle: " << argv[1] << '\n';
    return 2;
  }
  const std::string source((std::istreambuf_iterator<char>(input)), {});

  // Services outlive the runtime (destroyed after it) so teardown never
  // touches a dangling binding. The policy grants exactly what the demo app
  // needs (reads plus the input hook); mutations still fail the policy with
  // a clear reason.
  rime::win32::InputService input_service;
  rime::win32::ProcessService process_service;
  rime::win32::ClipboardService clipboard_service;
  rime::win32::WindowService window_service;
  rime::action::Kernel kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"windows.window.read", "windows.clipboard.read",
                                      "process.inspect", "windows.hook.global"}));
  std::atomic<std::uint64_t> next_action_id{0};
  rime::win32::InputModuleBinding input_binding;
  input_binding.service = &input_service;
  input_binding.kernel = &kernel;
  rime::win32::ProcessModuleBinding process_binding{&process_service, &kernel, &next_action_id};
  rime::win32::ClipboardModuleBinding clipboard_binding{&clipboard_service, &kernel,
                                                        &next_action_id};
  rime::win32::WindowModuleBinding window_binding{&window_service, &kernel, &next_action_id};

  rime::js::Runtime runtime;
  if (const auto error = rime::win32::register_input_module(runtime, &input_binding); !error.ok()) {
    std::cerr << "rime:input registration failed: " << error.message << '\n';
    return 1;
  }
  if (const auto error =
          rime::win32::register_process_module(runtime, &process_binding); !error.ok()) {
    std::cerr << "rime:process registration failed: " << error.message << '\n';
    return 1;
  }
  if (const auto error =
          rime::win32::register_clipboard_module(runtime, &clipboard_binding); !error.ok()) {
    std::cerr << "rime:clipboard registration failed: " << error.message << '\n';
    return 1;
  }
  if (const auto error = rime::win32::register_window_module(runtime, &window_binding);
      !error.ok()) {
    std::cerr << "rime:window registration failed: " << error.message << '\n';
    return 1;
  }

  const auto started = window_service.start();
  if (!started.ok()) {
    std::cerr << "window service failed to start: " << started.message << '\n';
    return 1;
  }

  const auto startup = runtime.start();
  if (!startup.ok()) {
    std::cerr << "QuickJS runtime failed to start: " << startup.message << '\n';
    (void)window_service.stop();
    return 1;
  }
  const auto error = runtime.evaluate_module(source, argv[1]).get();
  if (!error.ok()) {
    std::cerr << "QuickJS bundle failed: " << error.message << '\n';
    (void)runtime.stop();
    (void)window_service.stop();
    return 1;
  }
  // Let async bundle work (SDK queries, delay chains) run to completion.
  if (const auto settled = runtime.settle(std::chrono::seconds(5)); !settled.ok()) {
    std::cerr << "QuickJS bundle did not settle: " << settled.message << '\n';
    (void)runtime.stop();
    (void)window_service.stop();
    return 1;
  }
  // Bundles report async failures by publishing globalThis.__rim_failure.
  const auto failure = runtime
                           .evaluate_module(
                               "if (globalThis.__rim_failure)\n"
                               "  throw new Error(globalThis.__rim_failure);\n",
                               "rim-main-check.mjs")
                           .get();
  if (!failure.ok()) {
    std::cerr << "QuickJS bundle async failure: " << failure.message << '\n';
    (void)runtime.stop();
    (void)window_service.stop();
    return 1;
  }
  const auto runtime_stopped = runtime.stop();
  if (!runtime_stopped.ok()) {
    std::cerr << "QuickJS runtime stop failed: " << runtime_stopped.message << '\n';
    (void)window_service.stop();
    return 1;
  }
  (void)window_service.stop();
  return 0;
}
