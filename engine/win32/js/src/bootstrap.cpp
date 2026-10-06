#include "rime/win32/bootstrap.hpp"

#include "rime/automation/uia_executor.hpp"
#include "rime/core/json.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/clipboard_executor.hpp"
#include "rime/win32/control_executor.hpp"
#include "rime/win32/input_executor.hpp"
#include "rime/win32/process_executor.hpp"
#include "rime/win32/registry_executor.hpp"
#include "rime/win32/storage_executor.hpp"
#include "rime/win32/window_executor.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>

namespace rime::win32 {

namespace {

namespace json = rime::core::json;

rime::core::Error module_error(const char* module, const rime::core::Error& error) {
  return {error.code, std::string(module) + " module registration failed: " + error.message};
}

// Residency probe (AHK Persistent / script residency), evaluated on the JS
// thread through evaluate_module. It reports the two signals settle has to
// be judged by: the effective persistent flag (force flag or live
// declarative registrations) and the pending delay timers from
// runtime.inspect() - runtime.persistent() alone never covers a plain
// runtime.delay, so pendingTimers is what separates a long timer from a
// hung promise. evaluate_module returns only Error (no completion-value
// channel), so the probe stays silent while residency remains and throws
// its report as the message once it is gone; the failure then carries the
// JSON below and parse_residency reads it back.
constexpr const char* k_residency_probe =
    "import { runtime } from \"rime:runtime\";\n"
    "const report = JSON.parse(runtime.inspect());\n"
    "const residency = { persistent: runtime.persistent(),\n"
    "                    pendingTimers: report.pendingTimers,\n"
    "                    pendingCompletions: report.pendingCompletions };\n"
    "if (!residency.persistent && residency.pendingTimers === 0)\n"
    "  throw new Error(JSON.stringify(residency));\n";

// What one residency probe answered.
struct ResidencyReport {
  // The probe answered: either it evaluated cleanly or its failure carried
  // a parseable JSON report. False means the evaluation failed for some
  // other reason (interrupt, probe bug) - the probe_ok miss in the design,
  // which must not be mistaken for residency.
  bool ok{false};
  bool resident{false};  // persistent || pendingTimers > 0
  bool persistent{false};
  std::uint64_t pending_timers{0};
};

ResidencyReport parse_residency(const rime::core::Error& probe) {
  ResidencyReport report;
  if (probe.ok()) {
    // A clean evaluation means the probe's own rule held (persistent ||
    // pendingTimers > 0), so residency is answered; `persistent` is reported
    // as true because the pump only asks while quiescent, and quiescence
    // (host idle) already rules pending delay timers out.
    report.ok = true;
    report.resident = true;
    report.persistent = true;
    return report;
  }
  // Failure: the thrown Error stringifies as "Error: {json}", so the object
  // between the first brace and the last bracket is the report.
  const std::size_t begin = probe.message.find('{');
  const std::size_t end = probe.message.rfind('}');
  if (begin == std::string::npos || end == std::string::npos || end <= begin) return report;
  const auto parsed = json::parse(std::string_view(probe.message).substr(begin, end - begin + 1));
  if (!parsed.ok()) return report;
  const json::Value& value = *parsed.value;
  const json::Value* persistent = value.find("persistent");
  const json::Value* pending_timers = value.find("pendingTimers");
  if (!persistent || !persistent->is_bool() || !pending_timers || !pending_timers->is_number()) {
    return report;
  }
  report.ok = true;
  report.persistent = persistent->as_bool();
  report.pending_timers = static_cast<std::uint64_t>(pending_timers->as_number());
  report.resident = report.persistent || report.pending_timers > 0;
  return report;
}

}  // namespace

Bootstrap::Bootstrap(std::unordered_set<std::string> capabilities)
    : kernel_(std::make_shared<rime::action::StaticCapabilityPolicy>(std::move(capabilities))),
      dispatcher_(kernel_, rime::action::default_dispatch_policy()) {
  input_binding_.service = &input_service_;
  input_binding_.window_service = &window_service_;
  input_binding_.clipboard_service = &clipboard_service_;
  input_binding_.kernel = &kernel_;
  input_binding_.dispatcher = &dispatcher_;
  input_binding_.next_action_id = &next_action_id_;
  process_binding_ = {&process_service_, &kernel_, &dispatcher_, &next_action_id_};
  clipboard_binding_ = {&clipboard_service_, &kernel_, &dispatcher_, &next_action_id_};
  window_binding_ = {&window_service_, &kernel_, &dispatcher_, &next_action_id_};
  control_binding_ = {&window_service_, &kernel_, &dispatcher_, &next_action_id_};
  automation_binding_ = {&automation_service_, &kernel_, &dispatcher_, &next_action_id_};
  storage_binding_ = {&storage_service_, &kernel_, &dispatcher_, &next_action_id_};
  registry_binding_ = {&registry_service_, &kernel_, &dispatcher_, &next_action_id_};
  screen_binding_ = {&screen_service_, &kernel_};
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
  if (const auto error = register_control_module(runtime, &control_binding_); !error.ok()) {
    return module_error("rime:control", error);
  }
  if (const auto error = register_automation_module(runtime, &automation_binding_); !error.ok()) {
    return module_error("rime:automation", error);
  }
  if (const auto error = register_storage_module(runtime, &storage_binding_); !error.ok()) {
    return module_error("rime:storage", error);
  }
  if (const auto error = register_registry_module(runtime, &registry_binding_); !error.ok()) {
    return module_error("rime:registry", error);
  }
  if (const auto error = register_screen_module(runtime, &screen_binding_); !error.ok()) {
    return module_error("rime:screen", error);
  }
  return rime::core::Error::none();
}

rime::core::Error Bootstrap::register_executors() {
  // Every implemented action type in contracts/registry/actions.json: the
  // window family comes from window_action_types(), the executor's own accept
  // set, so the production wiring cannot fall behind it (it once did - six
  // golden types answered `unsupported` here while the executor dispatched
  // them). Each executor re-validates its own type on every call, so a
  // mismatch between this list and an executor's accept set fails loudly in
  // the slice tests.
  const auto window_executor = std::make_shared<WindowExecutor>(window_service_);
  for (const std::string& type : window_action_types()) {
    if (const auto error = kernel_.register_executor(type, window_executor); !error.ok()) {
      return error;
    }
  }
  const auto control_executor = std::make_shared<ControlExecutor>(window_service_);
  for (const char* type : {"control.click", "control.focus", "control.settext",
                           "control.gettext", "control.sendtext", "control.list.add",
                           "control.list.delete", "control.list.choose", "control.list.find",
                           "control.list.index", "control.list.choice", "control.list.items",
                           "control.tab.select", "control.edit.count", "control.edit.caret",
                           "control.edit.line", "control.edit.selected", "control.edit.paste",
                           "control.setchecked", "control.ischecked", "control.show",
                           "control.hide", "control.move", "control.setenabled",
                           "control.tab.index"}) {
    if (const auto error = kernel_.register_executor(type, control_executor); !error.ok()) {
      return error;
    }
  }
  const auto input_executor = std::make_shared<InputExecutor>(input_service_);
  for (const char* type : {"input.send", "input.mouse"}) {
    if (const auto error = kernel_.register_executor(type, input_executor); !error.ok()) {
      return error;
    }
  }
  const auto clipboard_executor = std::make_shared<ClipboardExecutor>(clipboard_service_);
  for (const char* type : {"clipboard.write", "clipboard.restore"}) {
    if (const auto error = kernel_.register_executor(type, clipboard_executor); !error.ok()) {
      return error;
    }
  }
  const auto process_executor = std::make_shared<ProcessExecutor>(process_service_);
  for (const char* type :
       {"process.launch", "process.terminate", "process.set.priority", "process.runas",
        "process.shutdown"}) {
    if (const auto error = kernel_.register_executor(type, process_executor); !error.ok()) {
      return error;
    }
  }
  if (const auto error =
          kernel_.register_executor("storage.write", std::make_shared<StorageExecutor>(storage_service_));
      !error.ok()) {
    return error;
  }
  if (const auto error = kernel_.register_executor(
          "registry.write", std::make_shared<RegistryExecutor>(registry_service_));
      !error.ok()) {
    return error;
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
  // The two file/dir selectors browse the window service's pump; attaching
  // only after every service is up keeps a rolled-back start from leaving a
  // dangling pump reference behind.
  storage_service_.set_ui_thread(&window_service_.ui());
  started_ = true;
  return rime::core::Error::none();
}

rime::core::Error Bootstrap::stop() {
  if (!started_) return rime::core::Error::none();
  started_ = false;
  rime::core::Error first = rime::core::Error::none();
  // Detach and sweep the storage/process services first: the selectors stop
  // seeing the pump, residual file handles close and outstanding wait
  // references release before the threads they borrow go away.
  storage_service_.set_ui_thread(nullptr);
  (void)storage_service_.stop();
  (void)process_service_.stop();
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
  // contracts/registry/actions.json; `test-only` probes stay ungranted.
  return {"windows.window.read", "windows.window.write", "windows.clipboard.read",
          "windows.clipboard.write", "windows.input.inject", "windows.input.read",
          "windows.hook.global", "windows.automation.find", "windows.automation.read",
          "windows.automation.invoke", "windows.automation.control", "process.inspect", "process.launch",
          "process.terminate", "process.manage", "process.runas", "process.shutdown",
          "filesystem.read", "filesystem.write", "registry.read", "registry.write",
          "screen.capture", "media.sound"};
}

// Reload ceiling (AHK Reload itself has none): a script that asks for a
// reload on every pass would otherwise spin the host forever on the same
// file, so the embedder allows this many reloads for one script path and
// then fails with a clear diagnostic on stderr instead of looping.
constexpr int k_reload_limit = 8;

int run_bundle_file(const std::string& path, std::unordered_set<std::string> capabilities) {
  // Declaration order matters: every runtime below is scoped to one pass and
  // destroyed before the bootstrap, so no module callback runs against a
  // dead binding, and the bootstrap's destructor stops any service an error
  // path left running. The bootstrap itself lives outside the reload loop on
  // purpose - it can only start once (its Win32 services stay up across a
  // reload), so the script side is what gets torn down and rebuilt.
  Bootstrap bootstrap(std::move(capabilities));
  bool bootstrap_started = false;
  int reloads = 0;

  for (;;) {
    // Re-read on every pass: like AHK Reload the file may have changed (or
    // disappeared) since the previous run. The handle is released before the
    // script runs, so a writer can replace the file while this pass is live.
    std::string source;
    {
      std::ifstream input(path, std::ios::binary);
      if (!input) {
        std::cerr << "cannot open script: " << path << '\n';
        if (bootstrap_started) {
          if (const auto stopped = bootstrap.stop(); !stopped.ok()) {
            std::cerr << "rime: bootstrap stop failed: " << stopped.message << '\n';
          }
        }
        return 2;
      }
      source.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    rime::js::Runtime runtime;
    if (const auto error = bootstrap.register_modules(runtime); !error.ok()) {
      std::cerr << "rime: module registration failed: " << error.message << '\n';
      return 1;
    }
    if (!bootstrap_started) {
      if (const auto error = bootstrap.start(); !error.ok()) {
        std::cerr << "rime: bootstrap start failed: " << error.message << '\n';
        return 1;
      }
      bootstrap_started = true;
    }
    if (const auto error = runtime.set_reload_count(static_cast<std::uint64_t>(reloads));
        !error.ok()) {
      std::cerr << "rime: reload state setup failed: " << error.message << '\n';
      return 1;
    }
    if (const auto error = runtime.start(); !error.ok()) {
      std::cerr << "QuickJS runtime failed to start: " << error.message << '\n';
      return 1;
    }
    const auto evaluation = runtime.evaluate_module(source, path).get();
    // runtime.exit(code) and runtime.reload() both unwind the script with an
    // exception; treat either as a deliberate outcome, not a failure: no
    // settle, no __rim_failure check. Deviation from the async-failure
    // contract: an explicit exit outranks a queued failure report, and a
    // pending reload outranks it for the same reason - the script asked to
    // be replaced, so its queued report belongs to the instance going away.
    if (!runtime.exit_requested() && !runtime.reload_requested()) {
      if (!evaluation.ok()) {
        std::cerr << "QuickJS script failed: " << evaluation.message << '\n';
        (void)runtime.stop();
        return 1;
      }
      // Let async work (SDK queries, action chains) run to completion.
      // settle returns early once an exit or a reload is requested, so all
      // three orders - unwound before settle, unwound while settling, not
      // unwound - land on the checks below.
      const auto settled = runtime.settle(std::chrono::seconds(5));
      // An unwind that landed between the timeout and this branch must not be
      // read as a hang: the probe below would run into the armed interrupt
      // and answer nonsense, so exit/reload outrank the report.
      if (!settled.ok() && !runtime.exit_requested() && !runtime.reload_requested()) {
        // settle timed out, and the timeout has two legitimate explanations
        // that must be told apart before the pre-residency hard error is
        // adopted: script-visible work the host idle predicate never observes
        // (input hooks, hotkeys, hotstrings, setTimer registrations - the
        // probe's persistent flag) and a runtime.delay longer than the budget
        // (pendingTimers; runtime.persistent() alone does not cover those).
        // A probe that reports neither is the hung-promise case, which is
        // exactly what this branch reported before residency existed.
        const ResidencyReport report =
            parse_residency(runtime.evaluate_module(k_residency_probe, "residency-probe.mjs").get());
        if (!report.ok || !report.resident) {
          std::cerr << "QuickJS script did not settle: " << settled.message << '\n';
          (void)runtime.stop();
          return 1;
        }
        // Residency or a long delay explains the timeout: fall through into
        // the residency pump instead of failing.
      }
      if (!runtime.exit_requested() && !runtime.reload_requested()) {
        // Residency pump (AHK Persistent): settle only covers the work the
        // host observes, so a resident script has to be kept alive here until
        // it clears its registrations / force flag or calls runtime.exit.
        // The probe is evaluated only while the loop is quiescent, so residency
        // cannot change between the check and the answer, and wait_for paces
        // the polls; exit and reload outrank both, which is why they are
        // checked first. The probe evaluation itself is safe here - stop() has
        // not run, so the run loop is still alive to execute it.
        for (;;) {
          if (runtime.exit_requested()) break;
          if (runtime.reload_requested()) break;
          if (runtime.quiescent()) {
            const ResidencyReport report = parse_residency(
                runtime.evaluate_module(k_residency_probe, "residency-probe.mjs").get());
            if (!report.ok || !report.persistent) break;  // residency over
          }
          runtime.wait_for(std::chrono::milliseconds(100));
        }
      }
      if (!runtime.exit_requested() && !runtime.reload_requested()) {
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
    }
    // Exit path or reload path: stop the runtime first, which drains the
    // exit handlers with {"reason":"exit"|"reload"|"stop"} inside
    // Runtime::stop (run()'s post-loop dynamic payload). Stop/bootstrap
    // failures are reported but never outrank a requested exit code, so both
    // stops are best-effort here.
    if (const auto stopped = runtime.stop(); !stopped.ok()) {
      std::cerr << "QuickJS runtime stop failed: " << stopped.message << '\n';
    }
    // Exit outranks reload at the last possible moment too: a handler that ran
    // while the reload was being torn down may have asked for an exit, and the
    // script is gone at this point either way.
    if (runtime.exit_requested()) {
      if (const auto stopped = bootstrap.stop(); !stopped.ok()) {
        std::cerr << "rime: bootstrap stop failed: " << stopped.message << '\n';
      }
      return runtime.exit_code();
    }
    if (runtime.reload_requested()) {
      if (reloads >= k_reload_limit) {
        std::cerr << "rime: reload limit exceeded (" << k_reload_limit
                  << " reloads): " << path << '\n';
        if (const auto stopped = bootstrap.stop(); !stopped.ok()) {
          std::cerr << "rime: bootstrap stop failed: " << stopped.message << '\n';
        }
        return 1;
      }
      ++reloads;
      continue;  // re-read the file and run it in a fresh runtime
    }
    // Unreachable in practice: the block above only leaves on exit/reload or
    // returns. Kept so an unexpected state still stops the bootstrap cleanly.
    if (const auto stopped = bootstrap.stop(); !stopped.ok()) {
      std::cerr << "rime: bootstrap stop failed: " << stopped.message << '\n';
    }
    return 0;
  }
}

}  // namespace rime::win32
