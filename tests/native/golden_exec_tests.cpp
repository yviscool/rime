// Golden contract executor consumer (C++ desktop side): the runtime twin of
// the `failure` and `semantic` layers in contracts/golden.
//
//   failure  - every declared rejection is executed through the real Action
//              Kernel and the real desktop executor; the executor must answer
//              with the golden's error code (and, when the golden declares
//              one, a message fragment). Every case is refused inside the
//              executor's validation before the target resolves, so the phase
//              runs with no service started: a validation hole would surface
//              as InvalidState instead of a desktop side effect, and a
//              clipboard witness proves the phase wrote nothing.
//   semantic - the round-1 whitelist kinds execute against the desktop:
//              window.rect (fixture window vs WindowService::placement_rect),
//              process.exists (the golden payload launches the fixture
//              process, which is terminated again) and clipboard.text (write
//              through the executor, read back, restore the saved text).
//
// shape, lifecycle and envelope stay with rime_golden_tests (ctest name
// rime_golden_contract); this runner never re-validates schemas.

#include "rime/action/kernel.hpp"
#include "rime/automation/uia_executor.hpp"
#include "rime/automation/uia_service.hpp"
#include "rime/core/json.hpp"
#include "rime/core/lane.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/clipboard.hpp"
#include "rime/win32/clipboard_executor.hpp"
#include "rime/win32/input.hpp"
#include "rime/win32/input_executor.hpp"
#include "rime/win32/process.hpp"
#include "rime/win32/process_executor.hpp"
#include "rime/win32/window.hpp"
#include "rime/win32/window_executor.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using rime::core::json::Value;
using rime::win32::Rect;
using rime::win32::TitleMatchMode;
using rime::win32::UiThreadState;
using rime::win32::WindowInfo;
using rime::win32::WindowQuery;
using rime::win32::WindowService;

constexpr wchar_t kFixtureWindowTitleWide[] = L"Rime Golden Exec Window";
constexpr const char* kFixtureWindowTitle = "Rime Golden Exec Window";
// Golden process.exists payload launches this image; compared lowercased
// against ProcessInfo::name so the cleanup never touches a pre-existing
// fixture (only pids that appear during this run are terminated).
constexpr const char* kFixtureProcessName = "notepad.exe";

[[noreturn]] void fail(const std::string& message) {
  std::fprintf(stderr, "golden exec failure: %s\n", message.c_str());
  std::fflush(stderr);
  std::abort();
}

void expect(const bool condition, const std::string& message) {
  if (!condition) fail(message);
}

std::string read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  expect(static_cast<bool>(input), "cannot read " + path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool is_non_empty_string(const Value* value) {
  return value != nullptr && value->is_string() && !value->as_string().empty();
}

std::string require_string(const Value& object, const char* key, const std::string& where) {
  const Value* value = object.find(key);
  if (!is_non_empty_string(value)) fail(where + ": missing non-empty string field '" + std::string(key) + "'");
  return value->as_string();
}

std::string rect_text(const Rect& rect) {
  return "(" + std::to_string(rect.left) + "," + std::to_string(rect.top) + "," +
         std::to_string(rect.right) + "," + std::to_string(rect.bottom) + ")";
}

std::string lowercase(std::string text) {
  for (char& character : text) {
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return text;
}

// One executor-layer rejection declared by a golden file.
struct FailureCase {
  std::string file;
  std::string type;
  std::string capability;
  std::string name;
  std::string code;
  std::string message_contains;  // empty when the golden declares none
  std::string payload;           // canonical JSON object text
  bool has_target{false};        // target rejected before the payload is read
  std::string target_kind;
  std::string target_id;
};

// One expected effect declared by a golden file.
struct SemanticCase {
  std::string file;
  std::string type;
  std::string capability;
  std::string name;
  std::string payload;
  std::string kind;
  Value expect;
};

struct Cases {
  std::vector<FailureCase> failures;
  std::vector<SemanticCase> semantics;
  std::unordered_set<std::string> capabilities;
};

const std::unordered_set<std::string>& rect_expects() {
  static const std::unordered_set<std::string> expects = {
      "work-left-half", "work-right-half", "work-full", "work-top", "work-bottom"};
  return expects;
}

// The default envelope target for a golden entry that carries no `target`:
// a target the executor accepts, so only the payload under test is rejected.
std::pair<std::string, std::string> default_target(const std::string& type) {
  if (type == "window.group.add") return {"group", "golden_group"};
  if (type.starts_with("window.")) return {"window", "active"};
  if (type.starts_with("input.")) return {"input", "default"};
  if (type == "process.launch") return {"process", "new"};
  if (type == "process.terminate") return {"process", "1"};
  if (type == "clipboard.write") return {"clipboard", "default"};
  if (type == "automation.invoke") return {"element", "1"};
  fail("no default target known for action type '" + type + "'");
}

// window.rect expects -> WindowService::placement_rect spelling.
std::string placement_for(const std::string& expect_value) {
  if (expect_value == "work-left-half") return "left";
  if (expect_value == "work-right-half") return "right";
  if (expect_value == "work-full") return "full";
  if (expect_value == "work-top") return "top";
  if (expect_value == "work-bottom") return "bottom";
  fail("unknown window.rect expect '" + expect_value + "'");
}

std::map<std::string, Value> load_goldens(const std::string& root) {
  const std::filesystem::path golden_dir = std::filesystem::path(root) / "contracts" / "golden";
  expect(std::filesystem::is_directory(golden_dir), "missing contracts/golden directory");
  std::map<std::string, Value> goldens;
  for (const auto& entry : std::filesystem::directory_iterator(golden_dir)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (!name.ends_with(".json")) continue;
    auto parsed = rime::core::json::parse(read_file(entry.path().string()));
    expect(parsed.ok(), name + ": malformed JSON: " + parsed.error);
    expect(parsed.value->is_object(), name + ": golden file must be an object");
    goldens.emplace(name, std::move(*parsed.value));
  }
  expect(goldens.size() == 15u,
         "expected exactly 15 golden files, got " + std::to_string(goldens.size()));
  return goldens;
}

// Structural validation of the two executable layers (no schema logic: that
// belongs to rime_golden_tests). Produces the flat case lists the phases run.
Cases collect_cases(const std::map<std::string, Value>& goldens) {
  Cases cases;
  for (const auto& [file, golden] : goldens) {
    const std::string type = require_string(golden, "type", file);
    expect(type + ".json" == file, file + ": type must match the file name");
    const std::string capability = require_string(golden, "capability", file);
    cases.capabilities.insert(capability);

    const Value* failure = golden.find("failure");
    expect(failure != nullptr && failure->is_array(), file + ": failure must be an array");
    std::size_t index = 0;
    for (const Value& entry : failure->as_array()) {
      const std::string where = file + " failure[" + std::to_string(index) + "]";
      ++index;
      const std::string name = require_string(entry, "name", where);
      const std::string label = where + " (" + name + ")";
      const Value* payload = entry.find("payload");
      expect(payload != nullptr && payload->is_object(), label + ": payload must be an object");
      const std::string code = require_string(entry, "code", label);
      rime::core::Error::Code parsed_code = rime::core::Error::Code::None;
      expect(rime::core::error_code_from_name(code, parsed_code),
             label + ": code must be a contract error code");
      FailureCase item;
      item.file = file;
      item.type = type;
      item.capability = capability;
      item.name = name;
      item.code = code;
      if (const Value* message = entry.find("messageContains")) {
        expect(is_non_empty_string(message), label + ": messageContains must be a non-empty string");
        item.message_contains = message->as_string();
      }
      if (const Value* target = entry.find("target")) {
        expect(target->is_object(), label + ": target must be an object");
        expect(is_non_empty_string(target->find("kind")),
               label + ": target.kind must be a non-empty string");
        expect(is_non_empty_string(target->find("id")),
               label + ": target.id must be a non-empty string");
        item.has_target = true;
        item.target_kind = target->find("kind")->as_string();
        item.target_id = target->find("id")->as_string();
      }
      item.payload = rime::core::json::stringify(*payload);
      cases.failures.push_back(std::move(item));
    }

    const Value* semantic = golden.find("semantic");
    expect(semantic != nullptr && semantic->is_array(), file + ": semantic must be an array");
    index = 0;
    for (const Value& entry : semantic->as_array()) {
      const std::string where = file + " semantic[" + std::to_string(index) + "]";
      ++index;
      const std::string name = require_string(entry, "name", where);
      const std::string label = where + " (" + name + ")";
      const Value* payload = entry.find("payload");
      expect(payload != nullptr && payload->is_object(), label + ": payload must be an object");
      const Value* check = entry.find("check");
      expect(check != nullptr && check->is_object(), label + ": check must be an object");
      const std::string kind = require_string(*check, "kind", label);
      const Value* expect_value = check->find("expect");
      expect(expect_value != nullptr, label + ": check.expect must be present");
      if (kind == "window.rect") {
        expect(expect_value->is_string() && rect_expects().contains(expect_value->as_string()),
               label + ": window.rect expect must be a work-area placement");
      } else if (kind == "process.exists") {
        expect(expect_value->is_bool() && expect_value->as_bool(),
               label + ": process.exists expects true");
      } else if (kind == "clipboard.text") {
        expect(expect_value->is_string(), label + ": clipboard.text expects a string");
      } else {
        fail(label + ": check.kind must be window.rect, process.exists or clipboard.text");
      }
      SemanticCase item;
      item.file = file;
      item.type = type;
      item.capability = capability;
      item.name = name;
      item.payload = rime::core::json::stringify(*payload);
      item.kind = kind;
      item.expect = *expect_value;
      cases.semantics.push_back(std::move(item));
    }
  }
  expect(!cases.failures.empty(), "the golden set must declare failure cases");
  expect(!cases.semantics.empty(), "the golden set must declare semantic cases");
  return cases;
}

// Distinct, nonzero action ids for every executed case (the kernel rejects
// id 0 and the trace report keys on the id).
rime::core::ActionId next_action_id() {
  static rime::core::ActionId next = 0;
  return ++next;
}

rime::action::Action make_action(const std::string& type, const std::string& capability,
                                 const std::string& target_kind, const std::string& target_id,
                                 const std::string& payload) {
  rime::action::Action action;
  action.id = next_action_id();
  action.source = {"golden", "golden_exec"};
  action.type = type;
  action.capability = capability;
  action.target = {target_kind, target_id};
  action.payload = payload;
  // Real wall clock, comfortably in the future: the kernel only checks the
  // deadline, every case is decided by executor validation.
  action.deadline_unix_ms = static_cast<std::uint64_t>(
                                rime::core::SystemClock::instance().unix_ms()) +
                            5000;
  return action;
}

void register_executors(rime::action::Kernel& kernel, const std::map<std::string, Value>& goldens,
                        WindowService& window_service, rime::win32::InputService& input_service,
                        rime::win32::ProcessService& process_service,
                        rime::win32::ClipboardService& clipboard_service,
                        rime::automation::UiaService& uia_service) {
  const auto window_executor = std::make_shared<rime::win32::WindowExecutor>(window_service);
  const auto input_executor = std::make_shared<rime::win32::InputExecutor>(input_service);
  const auto process_executor = std::make_shared<rime::win32::ProcessExecutor>(process_service);
  const auto clipboard_executor =
      std::make_shared<rime::win32::ClipboardExecutor>(clipboard_service);
  const auto uia_executor = std::make_shared<rime::automation::UiaExecutor>(
      uia_service, rime::automation::UiaExecutor::Op::Invoke);
  for (const auto& [file, golden] : goldens) {
    const std::string type = require_string(golden, "type", file);
    std::shared_ptr<rime::action::Executor> executor;
    if (type.starts_with("window.")) {
      executor = window_executor;
    } else if (type.starts_with("input.")) {
      executor = input_executor;
    } else if (type.starts_with("process.")) {
      executor = process_executor;
    } else if (type == "clipboard.write") {
      executor = clipboard_executor;
    } else if (type == "automation.invoke") {
      executor = uia_executor;
    } else {
      fail(file + ": no desktop executor known for action type '" + type + "'");
    }
    expect(kernel.register_executor(type, executor).ok(),
           file + ": executor registration for " + type + " must succeed");
  }
}

// Phase 1: every golden rejection, executed. Side-effect free by construction
// (no service is started), which the caller witnesses on the clipboard.
std::size_t run_failures(rime::action::Kernel& kernel, const std::vector<FailureCase>& cases) {
  std::size_t passed = 0;
  for (const FailureCase& item : cases) {
    std::string target_kind;
    std::string target_id;
    if (item.has_target) {
      target_kind = item.target_kind;
      target_id = item.target_id;
    } else {
      const auto target = default_target(item.type);
      target_kind = target.first;
      target_id = target.second;
    }
    const rime::action::Action action =
        make_action(item.type, item.capability, target_kind, target_id, item.payload);
    const rime::action::Result result = kernel.execute(action);
    const std::string label =
        "failure " + item.file + " / " + item.name + " (type " + item.type + ")";
    rime::core::Error::Code expected = rime::core::Error::Code::None;
    const bool known = rime::core::error_code_from_name(item.code, expected);
    expect(known, label + ": expected code must be a contract error code");
    expect(!result.succeeded,
           label + ": expected rejection, but the executor accepted the action");
    expect(result.error.code == expected,
           label + ": expected code '" + item.code + "', got '" +
               std::string(rime::core::error_code_name(result.error.code)) + "' (" +
               result.error.message + ")");
    if (!item.message_contains.empty()) {
      expect(result.error.message.find(item.message_contains) != std::string::npos,
             label + ": message must contain '" + item.message_contains + "', got '" +
                 result.error.message + "'");
    }
    std::printf("[failure]  %-28s %-30s PASS (%s)\n", item.file.c_str(), item.name.c_str(),
                item.code.c_str());
    ++passed;
  }
  return passed;
}

// Phase 2: window.rect expectations against a fixture window. The fixture is
// destroyed before any assertion so a failure never leaves a window behind.
std::size_t run_window_semantics(const std::vector<SemanticCase>& cases,
                                 rime::action::Kernel& kernel, WindowService& service) {
  struct Outcome {
    const SemanticCase* item{nullptr};
    bool executed{false};
    std::string failure;
    bool rect_read{false};
    Rect actual{};
    bool expected_read{false};
    Rect expected{};
  };

  std::vector<Outcome> outcomes;
  for (const SemanticCase& item : cases) {
    if (item.kind != "window.rect") continue;
    Outcome outcome;
    outcome.item = &item;
    outcomes.push_back(std::move(outcome));
  }
  if (outcomes.empty()) return 0;

  expect(service.start().ok(), "the window service must start for window.rect semantics");
  HWND fixture = nullptr;
  expect(service.ui()
             .call([&] {
               fixture = CreateWindowExW(0, L"STATIC", kFixtureWindowTitleWide,
                                         WS_OVERLAPPED | WS_VISIBLE, 150, 120, 480, 360, nullptr,
                                         nullptr, GetModuleHandleW(nullptr), nullptr);
             })
             .ok(),
         "the fixture window must be created on the UI thread");
  expect(fixture != nullptr, "CreateWindowExW must build the fixture window");

  WindowQuery query;
  query.title = kFixtureWindowTitle;
  query.title_match_mode = TitleMatchMode::Exact;
  std::vector<WindowInfo> matches;
  expect(service.query(query, matches).ok(), "the fixture window query must succeed");
  expect(matches.size() == 1u,
         "expected exactly one fixture window, got " + std::to_string(matches.size()));
  const std::uint64_t fixture_id = matches.front().id;

  for (Outcome& outcome : outcomes) {
    const SemanticCase& item = *outcome.item;
    const rime::action::Action action =
        make_action(item.type, item.capability, "window", std::to_string(fixture_id), item.payload);
    const rime::action::Result result = kernel.execute(action);
    outcome.executed = result.succeeded;
    if (!result.succeeded) {
      outcome.failure = std::string(rime::core::error_code_name(result.error.code)) + ": " +
                        result.error.message;
      continue;
    }
    WindowInfo moved;
    if (service.info(fixture_id, moved).ok()) {
      outcome.rect_read = true;
      outcome.actual = moved.rect;
    } else {
      outcome.failure = "info() did not resolve the moved fixture window";
    }
    Rect expected{};
    if (service.placement_rect(placement_for(item.expect.as_string()), expected).ok()) {
      outcome.expected_read = true;
      outcome.expected = expected;
    } else {
      outcome.failure =
          "placement_rect() did not resolve " + item.expect.as_string();
    }
  }
  expect(service.ui().call([&] { DestroyWindow(fixture); }).ok(),
         "the fixture window must be destroyed before the assertions");

  std::size_t passed = 0;
  for (const Outcome& outcome : outcomes) {
    const SemanticCase& item = *outcome.item;
    const std::string label = "semantic " + item.file + " / " + item.name + " (window.rect)";
    expect(outcome.executed, label + ": window.move did not run: " + outcome.failure);
    expect(outcome.rect_read && outcome.expected_read, label + ": " + outcome.failure);
    expect(outcome.actual == outcome.expected,
           label + ": expected rect " + rect_text(outcome.expected) + ", got " +
               rect_text(outcome.actual));
    std::printf("[semantic] %-28s %-30s PASS (window.rect %s)\n", item.file.c_str(),
                item.name.c_str(), item.expect.as_string().c_str());
    ++passed;
  }
  return passed;
}

// Fixture processes by image name; false when the snapshot cannot be taken.
bool collect_fixture_pids(const rime::win32::ProcessService& service,
                          std::vector<std::uint32_t>& out) {
  std::vector<rime::win32::ProcessInfo> processes;
  if (!service.list(processes).ok()) return false;
  for (const auto& process : processes) {
    if (lowercase(process.name) == kFixtureProcessName) out.push_back(process.pid);
  }
  return true;
}

// Terminates every fixture process that was not already running before the
// launch (parent, child, or a straggler that appears late) and waits until
// none is left. No assertions inside: the caller cleans up first, asserts
// afterwards, so a failure can never leave an orphan behind.
bool cleanup_fixture_processes(const rime::win32::ProcessService& service,
                               const std::vector<std::uint32_t>& before) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  for (;;) {
    std::vector<std::uint32_t> current;
    if (!collect_fixture_pids(service, current)) return false;
    std::vector<std::uint32_t> leftovers;
    for (const std::uint32_t pid : current) {
      if (std::find(before.begin(), before.end(), pid) == before.end()) leftovers.push_back(pid);
    }
    if (leftovers.empty()) return true;
    for (const std::uint32_t pid : leftovers) {
      static_cast<void>(service.terminate(pid, 1));
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

// ProcessService::launch hands the bare command to CreateProcessW as
// lpApplicationName, which Win32 resolves against the parent's current
// directory only - no PATH or system-directory search. The golden payload
// ships a bare "notepad.exe", so the launch runs with the system directory
// as the current directory and the previous directory is restored as soon
// as the action returns.
class ScopedCurrentDirectory {
 public:
  explicit ScopedCurrentDirectory(const wchar_t* directory) {
    const DWORD written = GetCurrentDirectoryW(MAX_PATH, previous_);
    if (written > 0 && written < MAX_PATH) {
      has_previous_ = SetCurrentDirectoryW(directory) != 0;
    }
  }
  ~ScopedCurrentDirectory() {
    if (has_previous_) static_cast<void>(SetCurrentDirectoryW(previous_));
  }
  ScopedCurrentDirectory(const ScopedCurrentDirectory&) = delete;
  ScopedCurrentDirectory& operator=(const ScopedCurrentDirectory&) = delete;

 private:
  wchar_t previous_[MAX_PATH]{};
  bool has_previous_ = false;
};

// Phase 3: process.exists. The golden payload launches the fixture process;
// everything is captured, the process tree is terminated, and only then are
// the outcomes asserted.
std::size_t run_process_semantics(const std::vector<SemanticCase>& cases,
                                  rime::action::Kernel& kernel,
                                  rime::win32::ProcessService& service) {
  std::size_t passed = 0;
  for (const SemanticCase& item : cases) {
    if (item.kind != "process.exists") continue;
    const std::string label = "semantic " + item.file + " / " + item.name + " (process.exists)";
    std::vector<std::uint32_t> before;
    const bool snapshot_before = collect_fixture_pids(service, before);
    expect(snapshot_before, label + ": the process snapshot must resolve before the launch");

    const rime::action::Action action =
        make_action(item.type, item.capability, "process", "new", item.payload);
    const rime::action::Result result = [&] {
      wchar_t system_dir[MAX_PATH];
      const DWORD system_len = GetSystemDirectoryW(system_dir, MAX_PATH);
      expect(system_len > 0 && system_len < MAX_PATH,
             label + ": the Windows system directory must resolve");
      const ScopedCurrentDirectory fixture_cwd(system_dir);
      return kernel.execute(action);
    }();
    std::string failure;
    if (!result.succeeded) {
      failure = std::string(rime::core::error_code_name(result.error.code)) + ": " +
                result.error.message;
    }
    std::uint32_t pid = 0;
    if (const Value* launched_pid = result.value.find("pid");
        result.succeeded && launched_pid != nullptr && launched_pid->is_number()) {
      pid = static_cast<std::uint32_t>(launched_pid->as_number());
    }
    bool exists = false;
    if (pid != 0) {
      rime::win32::ProcessInfo info;
      exists = service.info(pid, info).ok();
    }
    const bool cleaned = cleanup_fixture_processes(service, before);

    expect(result.succeeded, label + ": process.launch did not run: " + failure);
    expect(pid != 0, label + ": the launch result must carry a pid");
    expect(exists, label + ": the launched fixture process must exist right after launch");
    expect(cleaned, label + ": every fixture process must be terminated (no orphans)");
    expect(item.expect.is_bool() && item.expect.as_bool(), label + ": expects the process to exist");
    std::printf("[semantic] %-28s %-30s PASS (process.exists, pid %u)\n", item.file.c_str(),
                item.name.c_str(), static_cast<unsigned>(pid));
    ++passed;
  }
  return passed;
}

// Phase 4: clipboard.text. Capture, restore the saved text, then assert.
std::size_t run_clipboard_semantics(const std::vector<SemanticCase>& cases,
                                    rime::action::Kernel& kernel,
                                    rime::win32::ClipboardService& service,
                                    const std::string& original, const bool original_readable) {
  std::size_t passed = 0;
  for (const SemanticCase& item : cases) {
    if (item.kind != "clipboard.text") continue;
    const std::string label = "semantic " + item.file + " / " + item.name + " (clipboard.text)";
    const rime::action::Action action =
        make_action(item.type, item.capability, "clipboard", "default", item.payload);
    const rime::action::Result result = kernel.execute(action);
    std::string failure;
    if (!result.succeeded) {
      failure = std::string(rime::core::error_code_name(result.error.code)) + ": " +
                result.error.message;
    }
    std::string read_back;
    const bool read_ok = service.read_text(read_back).ok();
    const bool restored = original_readable ? service.write_text(original).ok() : true;

    expect(result.succeeded, label + ": clipboard.write did not run: " + failure);
    expect(read_ok, label + ": the clipboard must be readable after the write");
    expect(read_back == item.expect.as_string(),
           label + ": expected clipboard text '" + item.expect.as_string() + "', got '" +
               read_back + "'");
    expect(restored, label + ": the pre-test clipboard text must be restored");
    std::printf("[semantic] %-28s %-30s PASS (clipboard.text)\n", item.file.c_str(),
                item.name.c_str());
    ++passed;
  }
  return passed;
}

}  // namespace

int main(int argc, char** argv) {
  expect(argc == 2, "usage: rime_golden_exec_tests <repo-root>");
  const std::string root = argv[1];

  const std::map<std::string, Value> goldens = load_goldens(root);
  const Cases cases = collect_cases(goldens);

  // Executors require the worker lane; this harness executes on the main
  // thread, exactly like the sibling service tests.
  expect(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Worker).ok(),
         "the main thread must own the worker lane");

  // Services stay unstarted until their semantic phase: phase 1 runs with
  // nothing to mutate, and each later phase starts only what it drives.
  WindowService window_service;
  rime::win32::ProcessService process_service;
  rime::win32::ClipboardService clipboard_service;
  rime::win32::InputService input_service;   // input failures stop in payload validation
  rime::automation::UiaService uia_service;  // the automation failure stops on the target

  // Every capability any golden action declares, so only the case under test
  // can decide the outcome.
  rime::action::Kernel kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
      cases.capabilities));
  register_executors(kernel, goldens, window_service, input_service, process_service,
                     clipboard_service, uia_service);

  // Phase 1: failure layer, witnessed as side-effect free.
  std::string clipboard_before;
  const bool clipboard_readable = clipboard_service.read_text(clipboard_before).ok();
  expect(clipboard_readable, "the pre-test clipboard text must be readable");
  const std::size_t failure_passed = run_failures(kernel, cases.failures);
  std::string clipboard_after;
  expect(clipboard_service.read_text(clipboard_after).ok(),
         "the clipboard must stay readable after the failure layer");
  expect(clipboard_after == clipboard_before, "the failure layer must not write the clipboard");
  expect(window_service.state() == UiThreadState::Created,
         "the failure layer must not start the window service");
  std::printf("[failure]  %zu/%zu entries rejected as declared, clipboard untouched\n",
              failure_passed, cases.failures.size());

  // Phases 2-4: semantic layer.
  const std::size_t window_passed = run_window_semantics(cases.semantics, kernel, window_service);
  expect(window_service.stop().ok(), "the window service must stop again");
  const std::size_t process_passed = run_process_semantics(cases.semantics, kernel, process_service);
  const std::size_t clipboard_passed = run_clipboard_semantics(
      cases.semantics, kernel, clipboard_service, clipboard_before, clipboard_readable);

  const std::size_t semantic_passed = window_passed + process_passed + clipboard_passed;
  expect(failure_passed == cases.failures.size(),
         "every failure entry must execute, ran " + std::to_string(failure_passed) + " of " +
             std::to_string(cases.failures.size()));
  expect(semantic_passed == cases.semantics.size(),
         "every semantic entry must execute, ran " + std::to_string(semantic_passed) + " of " +
             std::to_string(cases.semantics.size()));

  std::printf("golden exec checks passed (%zu files, %zu failure, %zu semantic)\n",
              goldens.size(), failure_passed, semantic_passed);
  return 0;
}
