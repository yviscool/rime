#include "rime/action/kernel.hpp"
#include "rime/core/lane.hpp"
#include "rime/core/trace.hpp"
#include "rime/win32/window.hpp"
#include "rime/win32/window_executor.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rime::win32::Rect;
using rime::win32::TitleMatchMode;
using rime::win32::WindowInfo;
using rime::win32::WindowService;

constexpr wchar_t kTestWindowTitle[] = L"Rime WindowService Test Window";

std::optional<WindowInfo> find_by_title(const std::vector<WindowInfo>& windows) {
  for (const auto& window : windows) {
    if (window.title.find("Rime WindowService Test Window") != std::string::npos) return window;
  }
  return std::nullopt;
}

}  // namespace

int main() {
  WindowService service;
  // Executors require the worker lane; this harness executes on the main thread.
  assert(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Worker).ok());
  std::vector<WindowInfo> windows;

  // Work before start is rejected.
  assert(!service.list(windows).ok());
  assert(service.start().ok());
  assert(!service.start().ok());  // start-once

  // Create the test window (with one child edit for the control reads) on
  // the UI thread.
  HWND created = nullptr;
  HWND child_edit = nullptr;
  assert(service.ui()
             .call([&] {
               created = CreateWindowExW(0, L"STATIC", kTestWindowTitle,
                                         WS_OVERLAPPED | WS_VISIBLE, 120, 80, 640, 480, nullptr,
                                         nullptr, GetModuleHandleW(nullptr), nullptr);
               assert(created != nullptr);
               child_edit = CreateWindowExW(
                   0, L"EDIT", L"Rime Control Text", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10,
                   10, 300, 24, created, nullptr, GetModuleHandleW(nullptr), nullptr);
               assert(child_edit != nullptr);
             })
             .ok());

  // list() reports it with the creation geometry and our process.
  assert(service.list(windows).ok());
  const auto found = find_by_title(windows);
  assert(found.has_value());
  const std::uint64_t id = found->id;
  assert(found->visible);
  assert(found->process_id == GetCurrentProcessId());
  assert(found->rect.left == 120);
  assert(found->rect.top == 80);
  assert(found->rect.width() == 640);
  assert(found->rect.height() == 480);

  // move("left") snaps to the left half of the primary work area.
  assert(service.move(id, "left").ok());
  WindowInfo moved;
  assert(service.info(id, moved).ok());
  RECT work{};
  assert(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) != FALSE);
  const Rect left_half{work.left, work.top, work.left + (work.right - work.left) / 2,
                       work.bottom};
  assert(moved.rect == left_half);

  // placement_rect resolves independently and matches the move.
  Rect placement{};
  assert(service.placement_rect("left", placement).ok());
  assert(placement == left_half);
  assert(service.placement_rect("right", placement).ok());
  assert(placement.left == work.left + (work.right - work.left) / 2);
  assert(!service.placement_rect("diagonal", placement).ok());

  // Explicit rect moves round-trip through info().
  const Rect target{50, 60, 450, 360};
  assert(service.move_rect(id, target).ok());
  WindowInfo resized;
  assert(service.info(id, resized).ok());
  assert(resized.rect == target);

  // focus is a weak assertion by necessity: SetForegroundWindow may refuse
  // while another window owns the foreground (interactive/CI dependent), so
  // a foreground-lock ExecutionFailed is acceptable; stale ids are not.
  const auto focus_result = service.focus(id);
  assert(focus_result.ok() ||
         focus_result.code == rime::core::Error::Code::ExecutionFailed);

  // Snapshot fields beyond geometry: class, process image and state.
  WindowInfo snapshot;
  assert(service.info(id, snapshot).ok());
  // The system class registers as "Static"; queries match it case-insensitively.
  assert(snapshot.class_name == "Static");
  assert(snapshot.process_name.size() > 4);
  assert(snapshot.process_name.find(".exe") != std::string::npos ||
         snapshot.process_name.find(".EXE") != std::string::npos);
  assert(snapshot.state == "normal");
  assert(!snapshot.minimized);

  // WinGetProcessPath: the full image path, not just the basename (empty is
  // only legal for processes that cannot be opened, and ours cannot hide).
  assert(!snapshot.process_path.empty());
  assert(snapshot.process_path.find('\\') != std::string::npos);
  assert(snapshot.process_path.size() > snapshot.process_name.size());
  assert(snapshot.process_path.compare(snapshot.process_path.size() -
                                            snapshot.process_name.size(),
                                        std::string::npos, snapshot.process_name) == 0);

  // WinGetControls/WinGetControlsHwnd (stable ids, AHK ClassNN numbering) and
  // WinGetText ("\r\n" after each non-empty control text).
  std::vector<rime::win32::ControlInfo> controls;
  assert(service.controls(id, controls).ok());
  // An IME that attaches when the edit receives focus may add its own
  // notification child (e.g. OimeTsfNotifyWindow), so look our control up
  // by ClassNN instead of asserting an exact set - the same rule as the
  // hidden-children check below.
  assert(controls.size() >= 1);
  const rime::win32::ControlInfo* edit_control = nullptr;
  for (const auto& control : controls) {
    if (control.class_nn == "Edit1") edit_control = &control;
  }
  assert(edit_control != nullptr);
  assert(edit_control->class_name == "Edit");
  assert(edit_control->id != id);  // a control id is not its parent's id
  std::string window_text_value;
  assert(service.text(id, window_text_value).ok());
  assert(window_text_value == "Rime Control Text\r\n");

  // Extended snapshot fields (WinGet* family): client area in screen
  // coordinates, unsigned style bits, enable/topmost flags, the min/max
  // triple, and layered attributes that stay unset (-1 / "") for a window
  // that never opted into layered mode.
  assert(snapshot.client_rect.left >= snapshot.rect.left);
  assert(snapshot.client_rect.top >= snapshot.rect.top);
  assert(snapshot.client_rect.right <= snapshot.rect.right);
  assert(snapshot.client_rect.bottom <= snapshot.rect.bottom);
  assert(snapshot.client_rect.width() > 0);
  assert(snapshot.client_rect.height() > 0);
  assert(snapshot.style > 0);  // WS_VISIBLE at minimum
  assert(snapshot.ex_style >= 0);
  assert(snapshot.enabled);
  assert(!snapshot.always_on_top);
  assert(snapshot.min_max == 0);
  assert(snapshot.transparent == -1);
  assert(snapshot.trans_color.empty());

  // Existence probes: WinExist short-circuits on the first match, WinActive
  // only ever inspects the foreground window (so only the negative result
  // of a title no window can carry is asserted here).
  rime::win32::WindowQuery probe;
  probe.title = "Rime WindowService";
  bool probe_found = false;
  assert(service.exists(probe, probe_found).ok());
  assert(probe_found);

  rime::win32::WindowQuery missing;
  missing.title = "No Such Window Anywhere In This Test";
  assert(service.exists(missing, probe_found).ok());
  assert(!probe_found);

  bool foreground_match = true;
  assert(service.matches_active(missing, foreground_match).ok());
  assert(!foreground_match);
  bool foreground_probe = false;
  assert(service.matches_active(probe, foreground_probe).ok());  // value is CI-dependent

  // WinWait family condition evaluation: one deterministic step per
  // condition. Exists/Active carry the target snapshot, Closed/NotActive
  // resolve without one, and a non-positive UI budget fails with Timeout
  // (the error code the wait loop maps to its timeout rejection).
  rime::win32::WaitEvaluation wait_eval;
  assert(service.evaluate_wait(probe, rime::win32::WaitCondition::Exists, wait_eval).ok());
  assert(wait_eval.met && wait_eval.target.has_value());
  assert(wait_eval.target->id == id);
  assert(service.evaluate_wait(probe, rime::win32::WaitCondition::Closed, wait_eval).ok());
  assert(!wait_eval.met && !wait_eval.target.has_value());
  assert(service.evaluate_wait(missing, rime::win32::WaitCondition::Exists, wait_eval).ok());
  assert(!wait_eval.met && !wait_eval.target.has_value());
  assert(service.evaluate_wait(missing, rime::win32::WaitCondition::Closed, wait_eval).ok());
  assert(wait_eval.met && !wait_eval.target.has_value());
  // NotActive is the negation of Active: a query nothing matches is
  // satisfied immediately (there is no window to be active).
  assert(service.evaluate_wait(missing, rime::win32::WaitCondition::NotActive, wait_eval).ok());
  assert(wait_eval.met && !wait_eval.target.has_value());
  assert(service.evaluate_wait(missing, rime::win32::WaitCondition::Active, wait_eval).ok());
  assert(!wait_eval.met && !wait_eval.target.has_value());
  // active:true selects the foreground window itself, so Active is met
  // exactly when a visible foreground window exists (foreground ownership is
  // CI-dependent; only the empty-foreground direction is asserted).
  rime::win32::WindowQuery foreground_query;
  foreground_query.active = true;
  std::optional<WindowInfo> foreground;
  assert(service.active(foreground).ok());
  assert(service.evaluate_wait(foreground_query, rime::win32::WaitCondition::Active, wait_eval)
             .ok());
  if (foreground.has_value() && foreground->visible) {
    assert(wait_eval.met);
  }
  if (!foreground.has_value()) {
    assert(!wait_eval.met);
  }
  assert(service
             .evaluate_wait(probe, rime::win32::WaitCondition::Exists, wait_eval,
                            std::chrono::milliseconds(0))
             .code == rime::core::Error::Code::Timeout);

  // WinTitle-style queries resolve on the UI lane: contains vs exact title,
  // class and executable filters, and the hidden window rule.
  rime::win32::WindowQuery by_title;
  by_title.title = "Rime WindowService";
  std::vector<WindowInfo> matched;
  assert(service.query(by_title, matched).ok());
  assert(find_by_title(matched).has_value());

  rime::win32::WindowQuery exact;
  exact.title = "Rime WindowService Test Window";
  exact.title_match_mode = TitleMatchMode::Exact;
  matched.clear();
  assert(service.query(exact, matched).ok());
  assert(find_by_title(matched).has_value());

  rime::win32::WindowQuery exact_partial;
  exact_partial.title = "Rime WindowService";
  exact_partial.title_match_mode = TitleMatchMode::Exact;
  matched.clear();
  assert(service.query(exact_partial, matched).ok());
  assert(!find_by_title(matched).has_value());

  // WinTitle matching is case-sensitive in every mode (AHK rule); the
  // lowercase spelling of our own title must miss.
  rime::win32::WindowQuery lowercase;
  lowercase.title = "rime windowservice test window";
  matched.clear();
  assert(service.query(lowercase, matched).ok());
  assert(matched.empty());

  // TitleMatchMode 1 (startswith): the needle must be a leading part of the
  // title, not just any substring.
  rime::win32::WindowQuery prefix;
  prefix.title = "Rime WindowService Test";
  prefix.title_match_mode = TitleMatchMode::StartsWith;
  matched.clear();
  assert(service.query(prefix, matched).ok());
  assert(find_by_title(matched).has_value());
  prefix.title = "WindowService Test Window";
  matched.clear();
  assert(service.query(prefix, matched).ok());
  assert(matched.empty());

  // TitleMatchMode 4 (RegEx): pattern search over the title, case-sensitive
  // by default and case-insensitive through AHK's i) option prefix.
  rime::win32::WindowQuery regex_query;
  regex_query.title = "^Rime WindowService Test Window$";
  regex_query.title_match_mode = TitleMatchMode::Regex;
  matched.clear();
  assert(service.query(regex_query, matched).ok());
  assert(find_by_title(matched).has_value());
  regex_query.title = "i)^rime windowservice test window$";
  matched.clear();
  assert(service.query(regex_query, matched).ok());
  assert(find_by_title(matched).has_value());
  regex_query.title = "^Nope";
  matched.clear();
  assert(service.query(regex_query, matched).ok());
  assert(matched.empty());

  // Bad patterns fail the query itself: invalid syntax is InvalidContract,
  // an unknown option prefix letter is Unsupported (never silently ignored).
  rime::win32::WindowQuery bad_regex;
  bad_regex.title = "Nope(";
  bad_regex.title_match_mode = TitleMatchMode::Regex;
  matched.clear();
  const auto bad_pattern = service.query(bad_regex, matched);
  assert(!bad_pattern.ok());
  assert(bad_pattern.code == rime::core::Error::Code::InvalidContract);
  rime::win32::WindowQuery bad_option;
  bad_option.title = "x)foo";
  bad_option.title_match_mode = TitleMatchMode::Regex;
  const auto unsupported_option = service.query(bad_option, matched);
  assert(!unsupported_option.ok());
  assert(unsupported_option.code == rime::core::Error::Code::Unsupported);

  // validate_window_regex is the shared parse-time checker: same codes
  // without going through a query.
  assert(rime::win32::validate_window_regex("^abc$").ok());
  assert(rime::win32::validate_window_regex("i)^abc$").ok());
  assert(rime::win32::validate_window_regex("Nope(").code ==
         rime::core::Error::Code::InvalidContract);
  assert(rime::win32::validate_window_regex("x)foo").code ==
         rime::core::Error::Code::Unsupported);
  // m)/s) need multiline/dotall, which the runtime regex engine cannot
  // provide: rejected as Unsupported rather than silently reinterpreted.
  assert(rime::win32::validate_window_regex("m)foo").code ==
         rime::core::Error::Code::Unsupported);
  assert(rime::win32::validate_window_regex("s)foo").code ==
         rime::core::Error::Code::Unsupported);
  assert(rime::win32::validate_window_regex("i)").code ==
         rime::core::Error::Code::InvalidContract);

  // The AHK settings vocabulary round-trips.
  assert(rime::win32::parse_title_match_mode("1") == TitleMatchMode::StartsWith);
  assert(rime::win32::parse_title_match_mode("3") == TitleMatchMode::Exact);
  assert(rime::win32::parse_title_match_mode("RegEx") == TitleMatchMode::Regex);
  assert(rime::win32::parse_title_match_mode("regex") == TitleMatchMode::Regex);
  assert(rime::win32::parse_title_match_mode("Fast") == std::nullopt);
  assert(rime::win32::title_match_mode_text(TitleMatchMode::Contains) == "2");
  assert(rime::win32::title_match_mode_text(TitleMatchMode::Regex) == "RegEx");

  rime::win32::WindowQuery by_class;
  by_class.class_name = "STATIC";
  matched.clear();
  assert(service.query(by_class, matched).ok());
  assert(find_by_title(matched).has_value());

  rime::win32::WindowQuery by_exe;
  char executable[MAX_PATH] = {};
  assert(GetModuleFileNameA(nullptr, executable, MAX_PATH) > 0);
  const std::string self_name = std::string(executable).substr(
      std::string(executable).find_last_of("\\/") + 1);
  by_exe.process_name = self_name;
  matched.clear();
  assert(service.query(by_exe, matched).ok());
  assert(find_by_title(matched).has_value());

  rime::win32::WindowQuery bogus;
  bogus.title = "No Such Window Title Anywhere";
  matched.clear();
  assert(service.query(bogus, matched).ok());
  assert(matched.empty());

  // Global window settings (SetTitleMatchMode/DetectHiddenWindows/
  // DetectHiddenText): defaults, return-previous semantics, and the effect
  // on queries that do not override the mode themselves.
  const rime::win32::WindowSettings defaults = service.settings();
  assert(defaults.title_match_mode == TitleMatchMode::Contains);
  assert(!defaults.title_match_mode_slow);
  assert(!defaults.detect_hidden_windows);
  assert(!defaults.detect_hidden_text);

  rime::win32::WindowSettingsPatch to_exact;
  to_exact.title_match_mode = TitleMatchMode::Exact;
  const rime::win32::WindowSettings before_exact = service.set_settings(to_exact);
  assert(before_exact.title_match_mode == TitleMatchMode::Contains);
  assert(service.settings().title_match_mode == TitleMatchMode::Exact);
  // A query without matchMode now resolves against the global exact mode:
  // the substring "Rime WindowService" no longer matches.
  rime::win32::WindowQuery implicit_mode;
  implicit_mode.title = "Rime WindowService";
  matched.clear();
  assert(service.query(implicit_mode, matched).ok());
  assert(matched.empty());
  // A per-query matchMode still wins over the global setting.
  implicit_mode.title_match_mode = TitleMatchMode::Contains;
  matched.clear();
  assert(service.query(implicit_mode, matched).ok());
  assert(find_by_title(matched).has_value());
  rime::win32::WindowSettingsPatch to_contains;
  to_contains.title_match_mode = TitleMatchMode::Contains;
  const rime::win32::WindowSettings before_contains = service.set_settings(to_contains);
  assert(before_contains.title_match_mode == TitleMatchMode::Exact);
  assert(service.settings().title_match_mode == TitleMatchMode::Contains);

  // State mutations: hide/show/minimize/maximize/restore round-trip through
  // the snapshot state machine.
  assert(service.hide(id).ok());
  WindowInfo hidden;
  assert(service.info(id, hidden).ok());
  assert(!hidden.visible);
  assert(hidden.state == "hidden");
  matched.clear();
  assert(service.query(by_title, matched).ok());
  assert(!find_by_title(matched).has_value());
  by_title.include_hidden = true;
  matched.clear();
  assert(service.query(by_title, matched).ok());
  assert(find_by_title(matched).has_value());
  by_title.include_hidden = std::nullopt;  // back to the global default

  // DetectHiddenWindows flips the global rule: with it on, queries and the
  // plain list() both see the hidden window; an explicit per-query
  // includeHidden still overrides in either direction.
  rime::win32::WindowSettingsPatch hidden_on;
  hidden_on.detect_hidden_windows = true;
  const rime::win32::WindowSettings before_hidden = service.set_settings(hidden_on);
  assert(!before_hidden.detect_hidden_windows);
  matched.clear();
  assert(service.query(by_title, matched).ok());
  assert(find_by_title(matched).has_value());
  by_title.include_hidden = false;
  matched.clear();
  assert(service.query(by_title, matched).ok());
  assert(!find_by_title(matched).has_value());
  by_title.include_hidden = std::nullopt;
  assert(service.list(windows).ok());
  assert(find_by_title(windows).has_value());
  rime::win32::WindowSettingsPatch hidden_off;
  hidden_off.detect_hidden_windows = false;
  assert(service.set_settings(hidden_off).detect_hidden_windows);
  assert(!service.settings().detect_hidden_windows);
  matched.clear();
  assert(service.query(by_title, matched).ok());
  assert(!find_by_title(matched).has_value());

  assert(service.show(id).ok());
  WindowInfo shown;
  assert(service.info(id, shown).ok());
  assert(shown.visible);
  assert(shown.state == "normal");

  // DetectHiddenText: a hidden child control contributes only while the
  // setting is on; controls() keeps counting hidden children either way.
  HWND hidden_child = nullptr;
  assert(service.ui()
             .call([&] {
               hidden_child = CreateWindowExW(0, L"STATIC", L"Rime Hidden Text", WS_CHILD, 10, 40,
                                              300, 24, created, nullptr,
                                              GetModuleHandleW(nullptr), nullptr);
               assert(hidden_child != nullptr);
             })
             .ok());
  std::string visible_text;
  assert(service.text(id, visible_text).ok());
  assert(visible_text == "Rime Control Text\r\n");
  rime::win32::WindowSettingsPatch text_on;
  text_on.detect_hidden_text = true;
  assert(!service.set_settings(text_on).detect_hidden_text);
  std::string all_text;
  assert(service.text(id, all_text).ok());
  assert(all_text.find("Rime Hidden Text") != std::string::npos);
  assert(all_text.find("Rime Control Text") != std::string::npos);
  rime::win32::WindowSettingsPatch text_off;
  text_off.detect_hidden_text = false;
  assert(service.set_settings(text_off).detect_hidden_text);
  assert(!service.settings().detect_hidden_text);

  // Hidden children participate in ClassNN numbering; a focus-driven IME
  // may also inject its own notification window, so look the names up
  // instead of asserting an exact set.
  std::vector<rime::win32::ControlInfo> controls_with_hidden;
  assert(service.controls(id, controls_with_hidden).ok());
  assert(controls_with_hidden.size() >= 2);
  bool saw_edit = false;
  bool saw_static = false;
  for (const auto& control : controls_with_hidden) {
    if (control.class_nn == "Edit1" && control.class_name == "Edit") saw_edit = true;
    if (control.class_nn == "Static1" && control.class_name == "Static") saw_static = true;
  }
  assert(saw_edit);
  assert(saw_static);

  assert(service.minimize(id).ok());
  WindowInfo minimized;
  assert(service.info(id, minimized).ok());
  assert(minimized.minimized);
  assert(minimized.state == "minimized");

  assert(service.restore(id).ok());
  WindowInfo restored;
  assert(service.info(id, restored).ok());
  assert(!restored.minimized);
  assert(restored.state == "normal");

  assert(service.maximize(id).ok());
  WindowInfo maximized;
  assert(service.info(id, maximized).ok());
  assert(maximized.state == "maximized");
  assert(service.restore(id).ok());

  // close(): WM_CLOSE runs inline on this thread, so the id is stale once
  // the call returns.
  HWND disposable = nullptr;
  assert(service.ui()
             .call([&] {
               disposable = CreateWindowExW(0, L"STATIC", L"Rime Close Target",
                                            WS_OVERLAPPED | WS_VISIBLE, 40, 40, 320, 240,
                                            nullptr, nullptr, GetModuleHandleW(nullptr),
                                            nullptr);
               assert(disposable != nullptr);
             })
             .ok());
  std::vector<WindowInfo> close_match;
  rime::win32::WindowQuery close_query;
  close_query.title = "Rime Close Target";
  assert(service.query(close_query, close_match).ok());
  assert(close_match.size() == 1);
  const std::uint64_t close_id = close_match.front().id;
  assert(service.close(close_id).ok());
  WindowInfo gone;
  assert(service.info(close_id, gone).code == rime::core::Error::Code::TargetGone);

  // active() is a weak assertion by necessity: there may be no foreground
  // window at all (headless/locked session), so only a present value must
  // resolve to a live, inspectable window.
  std::optional<WindowInfo> active;
  assert(service.active(active).ok());
  if (active.has_value()) {
    WindowInfo again;
    assert(service.info(active->id, again).ok());
  }

  // WindowExecutor: a contract-valid window.move reaches the service and
  // records the trace pair.
  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(
      std::make_shared<rime::action::StaticCapabilityPolicy>(
          std::unordered_set<std::string>{"windows.window.read", "windows.window.write"}),
      trace);
  const auto window_executor = std::make_shared<rime::win32::WindowExecutor>(service);
  for (const char* type : {"window.move", "window.focus", "window.close", "window.hide",
                           "window.show", "window.minimize", "window.maximize",
                           "window.restore"}) {
    assert(kernel.register_executor(type, window_executor).ok());
  }

  rime::action::Action move_action;
  move_action.id = 1;
  move_action.source = {"test", "win32_tests"};
  move_action.type = "window.move";
  move_action.capability = "windows.window.write";
  move_action.target = {"window", std::to_string(id)};
  move_action.payload = R"({"position":"left"})";
  move_action.deadline_unix_ms =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count()) +
      5000;

  const auto executed = kernel.execute(move_action);
  assert(executed.succeeded);
  assert(executed.value.is_object());
  assert(executed.value.find("id") != nullptr);
  WindowInfo executor_moved;
  assert(service.info(id, executor_moved).ok());
  assert(executor_moved.rect == left_half);
  const auto started_entries = [&] {
    std::size_t count = 0;
    for (const auto& entry : trace->snapshot()) {
      if (entry.kind == rime::core::TraceKind::ActionStarted &&
          entry.subject == "window.move") {
        ++count;
      }
    }
    return count;
  }();
  assert(started_entries == 1);

  // The executor dispatches every other write type through the kernel: each
  // resolves a snapshot and round-trips the visible state machine. focus is
  // weak (foreground-lock denial is environment-dependent, see above).
  const auto run_window_action = [&](const char* type, const std::string& payload,
                                     std::uint64_t action_id) {
    rime::action::Action action = move_action;
    action.id = action_id;
    action.type = type;
    action.payload = payload;
    return kernel.execute(action);
  };
  const auto focus_executed = run_window_action("window.focus", "{}", 6);
  assert(focus_executed.succeeded ||
         focus_executed.error.code == rime::core::Error::Code::ExecutionFailed);

  const auto hide_executed = run_window_action("window.hide", "{}", 7);
  assert(hide_executed.succeeded);
  assert(hide_executed.value.is_object());
  WindowInfo executor_hidden;
  const auto hidden_info = service.info(id, executor_hidden);
  assert(hidden_info.ok());
  assert(executor_hidden.state == "hidden");

  const auto show_executed = run_window_action("window.show", "{}", 8);
  assert(show_executed.succeeded);
  assert(show_executed.value.is_object());
  WindowInfo executor_shown;
  const auto shown_info = service.info(id, executor_shown);
  assert(shown_info.ok());
  assert(executor_shown.state == "normal");

  const auto minimize_executed = run_window_action("window.minimize", "{}", 9);
  assert(minimize_executed.succeeded);
  assert(minimize_executed.value.is_object());
  WindowInfo executor_minimized;
  const auto minimized_info = service.info(id, executor_minimized);
  assert(minimized_info.ok());
  assert(executor_minimized.state == "minimized");

  const auto restore_executed = run_window_action("window.restore", "{}", 10);
  assert(restore_executed.succeeded);
  assert(restore_executed.value.is_object());
  WindowInfo executor_restored;
  const auto restored_info = service.info(id, executor_restored);
  assert(restored_info.ok());
  assert(executor_restored.state == "normal");

  const auto maximize_executed = run_window_action("window.maximize", "{}", 11);
  assert(maximize_executed.succeeded);
  assert(maximize_executed.value.is_object());
  WindowInfo executor_maximized;
  const auto maximized_info = service.info(id, executor_maximized);
  assert(maximized_info.ok());
  assert(executor_maximized.state == "maximized");

  const auto restore_again = run_window_action("window.restore", "{}", 12);
  assert(restore_again.succeeded);
  WindowInfo executor_normal;
  const auto normal_info = service.info(id, executor_normal);
  assert(normal_info.ok());
  assert(executor_normal.state == "normal");

  // Capability denial happens before the executor runs: every registered
  // type is refused once under an empty policy and the window is untouched
  // (denial precedes dispatch, so even window.close is side-effect free).
  WindowInfo before_denied;
  const auto denied_snapshot = service.info(id, before_denied);
  assert(denied_snapshot.ok());
  rime::action::Kernel denied(
      std::make_shared<rime::action::StaticCapabilityPolicy>(
          std::unordered_set<std::string>{}),
      trace);
  constexpr const char* kDeniedTypes[] = {"window.move",  "window.focus",    "window.close",
                                          "window.hide",  "window.show",     "window.minimize",
                                          "window.maximize", "window.restore"};
  for (const char* type : kDeniedTypes) {
    const auto registered = denied.register_executor(type, window_executor);
    assert(registered.ok());
  }
  std::uint64_t denied_id = 20;
  for (const char* type : kDeniedTypes) {
    rime::action::Action refused_action = move_action;
    refused_action.id = denied_id++;
    refused_action.type = type;
    refused_action.payload =
        std::string(type) == "window.move" ? R"({"position":"left"})" : "{}";
    const auto refused = denied.execute(refused_action);
    assert(!refused.succeeded);
    assert(refused.error.code == rime::core::Error::Code::CapabilityDenied);
  }
  WindowInfo unchanged;
  const auto unchanged_info = service.info(id, unchanged);
  assert(unchanged_info.ok());
  assert(unchanged.rect == before_denied.rect);
  assert(unchanged.state == before_denied.state);
  assert(unchanged.visible == before_denied.visible);

  // Malformed payload and unknown placement fail the contract check.
  rime::action::Action bad_payload = move_action;
  bad_payload.id = 2;
  bad_payload.payload = R"({"position":42})";
  const auto bad_payload_result = kernel.execute(bad_payload);
  assert(!bad_payload_result.succeeded);
  assert(bad_payload_result.error.code == rime::core::Error::Code::InvalidContract);

  rime::action::Action bad_placement = move_action;
  bad_placement.id = 3;
  bad_placement.payload = R"({"position":"diagonal"})";
  const auto bad_placement_result = kernel.execute(bad_placement);
  assert(!bad_placement_result.succeeded);
  assert(bad_placement_result.error.message.find("unknown window placement") !=
         std::string::npos);

  // The executor dispatches window.close: the pre-close snapshot is the
  // result value and the id goes stale immediately after.
  HWND exec_disposable = nullptr;
  assert(service.ui()
             .call([&] {
               exec_disposable = CreateWindowExW(0, L"STATIC", L"Rime Executor Close",
                                                 WS_OVERLAPPED | WS_VISIBLE, 60, 60, 320, 240,
                                                 nullptr, nullptr, GetModuleHandleW(nullptr),
                                                 nullptr);
               assert(exec_disposable != nullptr);
             })
             .ok());
  std::vector<WindowInfo> exec_close_match;
  rime::win32::WindowQuery exec_close_query;
  exec_close_query.title = "Rime Executor Close";
  assert(service.query(exec_close_query, exec_close_match).ok());
  assert(exec_close_match.size() == 1);
  rime::action::Action close_action = move_action;
  close_action.id = 4;
  close_action.type = "window.close";
  close_action.payload = "{}";
  close_action.target.id = std::to_string(exec_close_match.front().id);
  const auto close_result = kernel.execute(close_action);
  assert(close_result.succeeded);
  assert(close_result.value.is_object());
  assert(close_result.value.find("title") != nullptr);
  WindowInfo close_gone;
  assert(service.info(exec_close_match.front().id, close_gone).code ==
         rime::core::Error::Code::TargetGone);

  // An expired deadline is rejected by the kernel with Timeout.
  rime::action::Action expired = move_action;
  expired.id = 5;
  expired.deadline_unix_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() -
      1000);
  const auto expired_result = kernel.execute(expired);
  assert(!expired_result.succeeded);
  assert(expired_result.error.code == rime::core::Error::Code::Timeout);

  // A slow task occupies the pump; a queued call hits its deadline with the
  // dedicated Timeout code.
  std::thread slow([&] {
    assert(service.ui().call([] { std::this_thread::sleep_for(500ms); }).ok());
  });
  std::this_thread::sleep_for(100ms);
  const auto queued_timeout = service.ui().call([] {}, 20ms);
  assert(!queued_timeout.ok());
  assert(queued_timeout.code == rime::core::Error::Code::Timeout);
  slow.join();

  // Nested calls run inline on the UI thread.
  rime::core::Error nested = {rime::core::Error::Code::InvalidState, "unset"};
  assert(service.ui().call([&] { nested = service.ui().call([] {}); }).ok());
  assert(nested.ok());

  // Destroying the window invalidates its id.
  assert(service.ui().call([&] { DestroyWindow(created); }).ok());
  WindowInfo dead;
  assert(service.info(id, dead).code == rime::core::Error::Code::TargetGone);
  assert(service.move(id, "left").code == rime::core::Error::Code::TargetGone);

  // Stop is repeatable; work afterwards is rejected.
  assert(service.stop().ok());
  assert(service.stop().ok());
  assert(!service.list(windows).ok());

  // The UI lane was released and can be claimed again.
  assert(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Ui).ok());
  rime::core::LaneRegistry::instance().release(rime::core::Lane::Ui);

  WindowService second;
  assert(second.start().ok());
  // The generation inside the id keeps a reference issued by the first
  // service from ever resolving inside the second one.
  WindowInfo stale_generation;
  assert(second.info(id, stale_generation).code == rime::core::Error::Code::TargetGone);
  assert(second.stop().ok());

  return 0;
}
