#include "rime/win32/bootstrap.hpp"

#include "rime/automation/uia_executor.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/clipboard_executor.hpp"
#include "rime/win32/input_executor.hpp"
#include "rime/win32/process_executor.hpp"
#include "rime/win32/window_executor.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <utility>

namespace rime::win32 {

namespace {

rime::core::Error module_error(const char* module, const rime::core::Error& error) {
  return {error.code, std::string(module) + " module registration failed: " + error.message};
}

}  // namespace

Bootstrap::Bootstrap(std::unordered_set<std::string> capabilities)
    : kernel_(std::make_shared<rime::action::StaticCapabilityPolicy>(std::move(capabilities))),
      dispatcher_(kernel_, rime::action::default_dispatch_policy()) {
  input_binding_.service = &input_service_;
  input_binding_.window_service = &window_service_;
  input_binding_.kernel = &kernel_;
  input_binding_.dispatcher = &dispatcher_;
  input_binding_.next_action_id = &next_action_id_;
  process_binding_ = {&process_service_, &kernel_, &dispatcher_, &next_action_id_};
  clipboard_binding_ = {&clipboard_service_, &kernel_, &dispatcher_, &next_action_id_};
  window_binding_ = {&window_service_, &kernel_, &dispatcher_, &next_action_id_};
  automation_binding_ = {&automation_service_, &kernel_, &dispatcher_, &next_action_id_};
}

Bootstrap::~Bootstrap() { (void)stop(); }

rime::core::Error Bootstrap::register_modules(rime::js::Runtime& runtime) {
  if (const auto error = register_input_module(runtime, &input_binding_); !error.ok()) {
    return module_error("rime:input", error);
  }
  if (const auto error = register_process_module(runtime, &process_binding_); !error.ok()) {
    return module_error("rime:process", error);
  }
  if (const auto error = register_clipboard_module(runtime, &clipboard_binding_); !error.ok()) {
    return module_error("rime:clipboard", error);
  }
  if (const auto error = register_window_module(runtime, &window_binding_); !error.ok()) {
    return module_error("rime:window", error);
  }
  if (const auto error = register_automation_module(runtime, &automation_binding_); !error.ok()) {
    return module_error("rime:automation", error);
  }
  return rime::core::Error::none();
}

rime::core::Error Bootstrap::register_executors() {
  // The 15 action types in contracts/registry/actions.json. Each executor
  // re-validates its own type on every call, so a mismatch between this list
  // and an executor's accept set fails loudly in the slice tests.
  const auto window_executor = std::make_shared<WindowExecutor>(window_service_);
  for (const char* type : {"window.move", "window.focus", "window.close", "window.hide",
                           "window.show", "window.minimize", "window.maximize",
                           "window.restore"}) {
    if (const auto error = kernel_.register_executor(type, window_executor); !error.ok()) {
      return error;
    }
  }
  const auto input_executor = std::make_shared<InputExecutor>(input_service_);
  for (const char* type : {"input.send", "input.mouse"}) {
    if (const auto error = kernel_.register_executor(type, input_executor); !error.ok()) {
      return error;
    }
  }
  if (const auto error = kernel_.register_executor(
          "clipboard.write", std::make_shared<ClipboardExecutor>(clipboard_service_));
      !error.ok()) {
    return error;
  }
  const auto process_executor = std::make_shared<ProcessExecutor>(process_service_);
  for (const char* type : {"process.launch", "process.terminate"}) {
    if (const auto error = kernel_.register_executor(type, process_executor); !error.ok()) {
      return error;
    }
  }
  if (const auto error = kernel_.register_executor(
          "automation.find",
          std::make_shared<rime::automation::UiaExecutor>(
              automation_service_, rime::automation::UiaExecutor::Op::Find));
      !error.ok()) {
    return error;
  }
  if (const auto error = kernel_.register_executor(
          "automation.read",
          std::make_shared<rime::automation::UiaExecutor>(
              automation_service_, rime::automation::UiaExecutor::Op::Read));
      !error.ok()) {
    return error;
  }
  if (const auto error = kernel_.register_executor(
          "automation.invoke",
          std::make_shared<rime::automation::UiaExecutor>(
              automation_service_, rime::automation::UiaExecutor::Op::Invoke));
      !error.ok()) {
    return error;
  }
  return rime::core::Error::none();
}

rime::core::Error Bootstrap::start() {
  if (started_) {
    return {rime::core::Error::Code::InvalidState, "bootstrap can only start once"};
  }
  if (const auto error = register_executors(); !error.ok()) {
    return error;
  }
  if (const auto error = input_service_.start(); !error.ok()) {
    return error;
  }
  if (const auto error = window_service_.start(); !error.ok()) {
    (void)input_service_.stop();
    return error;
  }
  if (const auto error = automation_service_.start(); !error.ok()) {
    (void)window_service_.stop();
    (void)input_service_.stop();
    return error;
  }
  started_ = true;
  return rime::core::Error::none();
}

rime::core::Error Bootstrap::stop() {
  if (!started_) return rime::core::Error::none();
  started_ = false;
  rime::core::Error first = rime::core::Error::none();
  if (const auto error = automation_service_.stop(); !error.ok()) first = error;
  if (const auto error = window_service_.stop(); !error.ok() && first.ok()) first = error;
  if (const auto error = input_service_.stop(); !error.ok() && first.ok()) first = error;
  return first;
}

std::unordered_set<std::string> demo_capabilities() {
  return {"windows.window.read", "windows.clipboard.read", "process.inspect",
          "windows.hook.global"};
}

std::unordered_set<std::string> production_capabilities() {
  // Every capability whose status is `implemented` in
  // contracts/registry/actions.json; planned capabilities (registry.*,
  // media.sound) stay ungranted until their executors land.
  return {"windows.window.read", "windows.window.write", "windows.clipboard.read",
          "windows.clipboard.write", "windows.input.inject", "windows.input.read",
          "windows.hook.global", "windows.automation.find", "windows.automation.read",
          "windows.automation.invoke", "process.inspect", "process.launch",
          "process.terminate"};
}

int run_bundle_file(const std::string& path, std::unordered_set<std::string> capabilities) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::cerr << "cannot open script: " << path << '\n';
    return 2;
  }
  const std::string source((std::istreambuf_iterator<char>(input)), {});

  // Declaration order matters: the runtime must be destroyed before the
  // bootstrap so no module callback runs against a dead binding, and the
  // bootstrap's destructor stops any service an error path left running.
  Bootstrap bootstrap(std::move(capabilities));
  rime::js::Runtime runtime;

  if (const auto error = bootstrap.register_modules(runtime); !error.ok()) {
    std::cerr << "rime: module registration failed: " << error.message << '\n';
    return 1;
  }
  if (const auto error = bootstrap.start(); !error.ok()) {
    std::cerr << "rime: bootstrap start failed: " << error.message << '\n';
    return 1;
  }
  if (const auto error = runtime.start(); !error.ok()) {
    std::cerr << "QuickJS runtime failed to start: " << error.message << '\n';
    return 1;
  }
  if (const auto error = runtime.evaluate_module(source, path).get(); !error.ok()) {
    std::cerr << "QuickJS script failed: " << error.message << '\n';
    (void)runtime.stop();
    return 1;
  }
  // Let async work (SDK queries, action chains) run to completion.
  if (const auto settled = runtime.settle(std::chrono::seconds(5)); !settled.ok()) {
    std::cerr << "QuickJS script did not settle: " << settled.message << '\n';
    (void)runtime.stop();
    return 1;
  }
  // Scripts report async failures by publishing globalThis.__rim_failure.
  const auto failure = runtime
                           .evaluate_module(
                               "if (globalThis.__rim_failure)\n"
                               "  throw new Error(globalThis.__rim_failure);\n",
                               "rim-main-check.mjs")
                           .get();
  if (!failure.ok()) {
    std::cerr << "QuickJS script async failure: " << failure.message << '\n';
    (void)runtime.stop();
    return 1;
  }
  if (const auto stopped = runtime.stop(); !stopped.ok()) {
    std::cerr << "QuickJS runtime stop failed: " << stopped.message << '\n';
    return 1;
  }
  if (const auto stopped = bootstrap.stop(); !stopped.ok()) {
    std::cerr << "rime: bootstrap stop failed: " << stopped.message << '\n';
    return 1;
  }
  return 0;
}

}  // namespace rime::win32
