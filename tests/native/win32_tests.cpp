#include "rime/action/kernel.hpp"
#include "rime/core/lane.hpp"
#include "rime/core/trace.hpp"
#include "rime/win32/window.hpp"
#include "rime/win32/window_executor.hpp"

#include <windows.h>

#include <cassert>
#include <atomic>
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

// Counts WM_PAINT deliveries so the redraw test can observe that
// InvalidateRect actually queued a paint on the owner's pump.
std::atomic<int> g_paints{0};

LRESULT CALLBACK count_paint_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_PAINT) ++g_paints;
  return DefWindowProcW(window, message, wparam, lparam);
}

// Answers queries (so snapshots do not block) but stalls on WM_CLOSE for
// longer than the kill budget - a deterministic stand-in for a window that
// refuses to close.
LRESULT CALLBACK stall_close_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_CLOSE) {
    Sleep(700);  // kill waits at most 500ms
    return 0;    // handled-but-ignored: the window survives
  }
  return DefWindowProcW(window, message, wparam, lparam);
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

  // Window groups: spec dedup and the missing-group policy are exact;
  // activation/deactivation stay weak because the foreground lock can deny
  // focus. Two disposable victims carry the group and are torn down through
  // the same query afterwards so later sections never see them.
  HWND group_victim_a = nullptr;
  HWND group_victim_b = nullptr;
  assert(service.ui()
             .call([&] {
               group_victim_a = CreateWindowExW(0, L"STATIC", L"Rime Group A",
                                                WS_OVERLAPPED | WS_VISIBLE, 100, 100, 320, 240,
                                                nullptr, nullptr, GetModuleHandleW(nullptr),
                                                nullptr);
               group_victim_b = CreateWindowExW(0, L"STATIC", L"Rime Group B",
                                                WS_OVERLAPPED | WS_VISIBLE, 140, 140, 320, 240,
                                                nullptr, nullptr, GetModuleHandleW(nullptr),
                                                nullptr);
               assert(group_victim_a != nullptr);
               assert(group_victim_b != nullptr);
             })
             .ok());
  rime::win32::WindowQuery victim_spec;
  victim_spec.title = "Rime Group";
  victim_spec.title_match_mode = TitleMatchMode::StartsWith;
  std::size_t group_specs = 0;
  assert(service.group_add("native_group", victim_spec, group_specs).ok());
  assert(group_specs == 1);
  // An exact duplicate spec is skipped (AHK GroupAdd's dedup).
  assert(service.group_add("native_group", victim_spec, group_specs).ok());
  assert(group_specs == 1);
  rime::win32::WindowQuery distinct_spec = victim_spec;
  distinct_spec.process_name = "definitely-not-running.exe";
  assert(service.group_add("native_group", distinct_spec, group_specs).ok());
  assert(group_specs == 2);
  assert(service.group_add("", victim_spec, group_specs).code ==
         rime::core::Error::Code::InvalidContract);

  // GroupActivate creates a missing group (which resolves nullopt when
  // empty); Deactivate and Close require a group that exists.
  std::optional<WindowInfo> group_out;
  assert(service.group_activate("native_new", false, group_out).ok());
  assert(!group_out.has_value());
  std::uint64_t group_closed = 0;
  assert(service.group_deactivate("native_missing", false, group_out).code ==
         rime::core::Error::Code::InvalidContract);
  assert(service.group_close("native_missing", "", group_closed, group_out).code ==
         rime::core::Error::Code::InvalidContract);
  assert(service.group_close("native_group", "bogus", group_closed, group_out).code ==
         rime::core::Error::Code::InvalidContract);

  // Weak: focus can be denied by the foreground lock; when a target does
  // resolve it must be a live, inspectable window.
  const auto group_activate_result = service.group_activate("native_group", false, group_out);
  assert(group_activate_result.ok() ||
         group_activate_result.code == rime::core::Error::Code::ExecutionFailed);
  if (group_out.has_value()) {
    WindowInfo group_target;
    assert(service.info(group_out->id, group_target).ok());
  }
  const auto group_deactivate_result = service.group_deactivate("native_group", false, group_out);
  assert(group_deactivate_result.ok() ||
         group_deactivate_result.code == rime::core::Error::Code::ExecutionFailed);
  if (group_out.has_value()) {
    WindowInfo group_target;
    assert(service.info(group_out->id, group_target).ok());
  }
  // Advance-only close: whatever happened to the foreground, the call either
  // succeeds or reports the focus denial - it must not corrupt group state.
  const auto group_noop_close = service.group_close("native_group", "", group_closed, group_out);
  assert(group_noop_close.ok() ||
         group_noop_close.code == rime::core::Error::Code::ExecutionFailed);

  // Tear any surviving victim down through the same query so the window
  // set is deterministic for the sections that follow.
  std::vector<WindowInfo> victim_leftovers;
  assert(service.query(victim_spec, victim_leftovers).ok());
  for (const auto& leftover : victim_leftovers) {
    assert(service.close(leftover.id).ok());
  }

  // z-order (AHK WinMoveTop/WinMoveBottom): relative order between two
  // disposable victims, checked by walking GW_HWNDNEXT - other desktop
  // windows in between do not matter. The lambda stays in scope for the
  // executor section below.
  auto zorder_above = [](HWND higher, HWND lower) {
    for (HWND walk = higher; (walk = GetWindow(walk, GW_HWNDNEXT)) != nullptr;) {
      if (walk == lower) return true;
    }
    return false;
  };
  HWND zorder_a = nullptr;
  HWND zorder_b = nullptr;
  assert(service.ui()
             .call([&] {
               zorder_a = CreateWindowExW(0, L"STATIC", L"Rime ZOrder A",
                                          WS_OVERLAPPED | WS_VISIBLE, 40, 40, 160, 120, nullptr,
                                          nullptr, GetModuleHandleW(nullptr), nullptr);
               zorder_b = CreateWindowExW(0, L"STATIC", L"Rime ZOrder B",
                                          WS_OVERLAPPED | WS_VISIBLE, 240, 40, 160, 120, nullptr,
                                          nullptr, GetModuleHandleW(nullptr), nullptr);
               assert(zorder_a != nullptr);
               assert(zorder_b != nullptr);
             })
             .ok());
  rime::win32::WindowQuery zorder_query;
  zorder_query.title = "Rime ZOrder";
  zorder_query.title_match_mode = TitleMatchMode::StartsWith;
  std::vector<WindowInfo> zorder_matches;
  assert(service.query(zorder_query, zorder_matches).ok());
  assert(zorder_matches.size() == 2);
  std::uint64_t zorder_id_a = 0;
  std::uint64_t zorder_id_b = 0;
  for (const auto& match : zorder_matches) {
    if (match.title == "Rime ZOrder A") zorder_id_a = match.id;
    if (match.title == "Rime ZOrder B") zorder_id_b = match.id;
  }
  assert(zorder_id_a != 0 && zorder_id_b != 0);
  // Bottom: A sinks below B regardless of its creation order; top: A rises.
  assert(service.zorder(zorder_id_a, true).ok());
  assert(zorder_above(zorder_b, zorder_a));
  assert(service.zorder(zorder_id_a, false).ok());
  assert(zorder_above(zorder_a, zorder_b));
  assert(service.zorder(zorder_id_b, true).ok());
  assert(zorder_above(zorder_a, zorder_b));
  // Stale ids refuse with TargetGone like every other write.
  assert(service.ui().call([&] { DestroyWindow(zorder_a); }).ok());
  WindowInfo zorder_gone;
  assert(service.info(zorder_id_a, zorder_gone).code ==
         rime::core::Error::Code::TargetGone);
  assert(service.zorder(zorder_id_a, false).code == rime::core::Error::Code::TargetGone);
  assert(service.ui().call([&] { DestroyWindow(zorder_b); }).ok());

  // redraw (AHK WinRedraw): InvalidateRect queues a WM_PAINT that the
  // owner's pump then delivers - observed through a temporary subclass.
  HWND redraw_victim = nullptr;
  WNDPROC original_proc = nullptr;
  assert(service.ui()
             .call([&] {
               redraw_victim = CreateWindowExW(0, L"STATIC", L"Rime Redraw Target",
                                               WS_OVERLAPPED | WS_VISIBLE, 60, 60, 200, 140,
                                               nullptr, nullptr, GetModuleHandleW(nullptr),
                                               nullptr);
               assert(redraw_victim != nullptr);
               original_proc = reinterpret_cast<WNDPROC>(
                   SetWindowLongPtrW(redraw_victim, GWLP_WNDPROC,
                                     reinterpret_cast<LONG_PTR>(count_paint_proc)));
               assert(original_proc != nullptr);
             })
             .ok());
  rime::win32::WindowQuery redraw_query;
  redraw_query.title = "Rime Redraw Target";
  std::vector<WindowInfo> redraw_match;
  assert(service.query(redraw_query, redraw_match).ok());
  assert(redraw_match.size() == 1);
  // Let the creation paint settle, then require a fresh one after redraw.
  std::this_thread::sleep_for(50ms);
  g_paints.store(0);
  assert(service.redraw(redraw_match.front().id).ok());
  for (int waited = 0; waited < 200 && g_paints.load() == 0; ++waited) {
    std::this_thread::sleep_for(10ms);
  }
  assert(g_paints.load() > 0);
  assert(service.ui().call([&] {
    SetWindowLongPtrW(redraw_victim, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original_proc));
  }).ok());
  // Redraw on a stale id refuses with TargetGone.
  assert(service.ui().call([&] { DestroyWindow(redraw_victim); }).ok());
  assert(service.redraw(redraw_match.front().id).code ==
         rime::core::Error::Code::TargetGone);

  // kill (AHK WinKill): a same-process, pumping target takes the WM_CLOSE
  // path (DefWindowProc destroys it) and never reaches the terminate
  // fallback.
  HWND kill_victim = nullptr;
  assert(service.ui()
             .call([&] {
               kill_victim = CreateWindowExW(0, L"STATIC", L"Rime Kill Target",
                                             WS_OVERLAPPED | WS_VISIBLE, 120, 160, 200, 140,
                                             nullptr, nullptr, GetModuleHandleW(nullptr),
                                             nullptr);
               assert(kill_victim != nullptr);
             })
             .ok());
  rime::win32::WindowQuery kill_query;
  kill_query.title = "Rime Kill Target";
  std::vector<WindowInfo> kill_match;
  assert(service.query(kill_query, kill_match).ok());
  assert(kill_match.size() == 1);
  assert(service.kill(kill_match.front().id).ok());
  WindowInfo kill_gone;
  assert(service.info(kill_match.front().id, kill_gone).code ==
         rime::core::Error::Code::TargetGone);

  // A window that answers queries but stalls on WM_CLOSE past the 500ms
  // budget makes the terminate fallback fire; it then refuses our own
  // process instead of suicide.
  std::atomic<bool> hung_ready{false};
  std::atomic<bool> hung_quit{false};
  HWND hung_victim = nullptr;
  std::thread hung([&] {
    hung_victim = CreateWindowExW(0, L"STATIC", L"Rime Hung Target", WS_OVERLAPPED, 10, 10,
                                  160, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(hung_victim, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(stall_close_proc));
    hung_ready.store(true);
    // Pump so WM_GETTEXT (snapshots) keeps working; quit is honored once
    // the stall inside the window proc returns.
    MSG message{};
    while (!hung_quit.load()) {
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) break;
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      if (hung_quit.load()) break;
      MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    if (hung_victim) DestroyWindow(hung_victim);
  });
  while (!hung_ready.load()) std::this_thread::sleep_for(5ms);
  rime::win32::WindowQuery hung_query;
  hung_query.title = "Rime Hung Target";
  hung_query.include_hidden = true;
  std::vector<WindowInfo> hung_match;
  assert(service.query(hung_query, hung_match).ok());
  assert(hung_match.size() == 1);
  const auto hung_kill = service.kill(hung_match.front().id, std::chrono::seconds(3));
  assert(hung_kill.code == rime::core::Error::Code::ExecutionFailed);
  assert(hung_kill.message.find("own process") != std::string::npos);
  hung_quit.store(true);
  hung.join();
  // After the thread destroyed its window the id is stale.
  assert(service.kill(hung_match.front().id).code == rime::core::Error::Code::TargetGone);

  // minimize_all (AHK WinMinimizeAll / WinMinimizeAllUndo): posts the shell
  // tray command; the effect is asynchronous, so both halves poll the
  // victim. The undo always runs before the assertions so a failure never
  // leaves the desktop minimized. The victim must be a full overlapped
  // window: the shell's minimize-all skips borderless windows.
  HWND minimizeall_victim = nullptr;
  assert(service.ui()
             .call([&] {
               minimizeall_victim = CreateWindowExW(0, L"STATIC", L"Rime MinimizeAll Target",
                                                    WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 260,
                                                    220, 140, nullptr, nullptr,
                                                    GetModuleHandleW(nullptr), nullptr);
               assert(minimizeall_victim != nullptr);
             })
             .ok());
  rime::win32::WindowQuery minimizeall_query;
  minimizeall_query.title = "Rime MinimizeAll Target";
  std::vector<WindowInfo> minimizeall_match;
  assert(service.query(minimizeall_query, minimizeall_match).ok());
  assert(minimizeall_match.size() == 1);
  // Let the freshly created window finish its first show before the shell
  // handles the tray command; a window created microseconds earlier can be
  // skipped by the desktop minimize pass.
  std::this_thread::sleep_for(200ms);
  const auto minimizeall_sent = service.minimize_all(false);
  bool minimizeall_observed = false;
  if (minimizeall_sent.ok()) {
    for (int waited = 0; waited < 300 && !minimizeall_observed; ++waited) {
      WindowInfo ma_snapshot;
      if (service.info(minimizeall_match.front().id, ma_snapshot).ok() && ma_snapshot.minimized) {
        minimizeall_observed = true;
      } else {
        std::this_thread::sleep_for(10ms);
      }
    }
  }
  const auto minimizeall_undo = service.minimize_all(true);
  bool minimizeall_restored = false;
  if (minimizeall_undo.ok()) {
    for (int waited = 0; waited < 300 && !minimizeall_restored; ++waited) {
      WindowInfo ma_snapshot;
      if (service.info(minimizeall_match.front().id, ma_snapshot).ok() && !ma_snapshot.minimized) {
        minimizeall_restored = true;
      } else {
        std::this_thread::sleep_for(10ms);
      }
    }
  }
  assert(minimizeall_sent.ok());
  assert(minimizeall_observed);
  assert(minimizeall_undo.ok());
  assert(minimizeall_restored);
  assert(service.ui().call([&] { DestroyWindow(minimizeall_victim); }).ok());

  // WindowExecutor: a contract-valid window.move reaches the service and
  // records the trace pair.
  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(
      std::make_shared<rime::action::StaticCapabilityPolicy>(
          std::unordered_set<std::string>{"windows.window.read", "windows.window.write"}),
      trace);
  const auto window_executor = std::make_shared<rime::win32::WindowExecutor>(service);
  for (const char* type : {"window.move",       "window.focus",  "window.close",
                            "window.hide",       "window.show",   "window.minimize",
                            "window.maximize",   "window.restore", "window.zorder",
                            "window.kill",       "window.redraw",
                            "window.group.add",
                            "window.group.activate", "window.group.deactivate",
                            "window.group.close", "window.minimizeall",
                            "window.minimizeall.undo"}) {
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
                                          "window.maximize", "window.restore", "window.zorder",
                                          "window.kill",  "window.redraw",
                                          "window.group.add", "window.group.activate",
                                          "window.group.deactivate", "window.group.close",
                                          "window.minimizeall", "window.minimizeall.undo"};
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

  // Group actions dispatch through the same kernel. Two disposable victims
  // back a fresh group; the close-all count is exact because nothing else
  // touches that pair, while activate/deactivate stay weak (foreground
  // lock). Ids continue above the earlier executor actions.
  HWND exec_group_a = nullptr;
  HWND exec_group_b = nullptr;
  assert(service.ui()
             .call([&] {
               exec_group_a = CreateWindowExW(0, L"STATIC", L"Rime Group Close A",
                                              WS_OVERLAPPED | WS_VISIBLE, 180, 180, 320, 240,
                                              nullptr, nullptr, GetModuleHandleW(nullptr),
                                              nullptr);
               exec_group_b = CreateWindowExW(0, L"STATIC", L"Rime Group Close B",
                                              WS_OVERLAPPED | WS_VISIBLE, 220, 220, 320, 240,
                                              nullptr, nullptr, GetModuleHandleW(nullptr),
                                              nullptr);
               assert(exec_group_a != nullptr);
               assert(exec_group_b != nullptr);
             })
             .ok());
  rime::action::Action group_add_action = move_action;
  group_add_action.id = 40;
  group_add_action.type = "window.group.add";
  group_add_action.target = {"group", "executor_group"};
  group_add_action.payload = R"({"title":"Rime Group Close","matchMode":"startswith"})";
  const auto group_added = kernel.execute(group_add_action);
  assert(group_added.succeeded);
  assert(group_added.value.find("count") != nullptr);
  assert(group_added.value.find("count")->as_number() == 1.0);
  // An exact duplicate spec is skipped, so the count stays at one.
  group_add_action.id = 41;
  const auto group_added_again = kernel.execute(group_add_action);
  assert(group_added_again.succeeded);
  assert(group_added_again.value.find("count")->as_number() == 1.0);

  // Contract failures: an empty spec, the wrong target kind, a reverse
  // value that is not a boolean and an unknown close mode.
  rime::action::Action empty_spec = group_add_action;
  empty_spec.id = 42;
  empty_spec.payload = "{}";
  const auto empty_spec_result = kernel.execute(empty_spec);
  assert(!empty_spec_result.succeeded);
  assert(empty_spec_result.error.code == rime::core::Error::Code::InvalidContract);

  rime::action::Action bad_group_kind = group_add_action;
  bad_group_kind.id = 43;
  bad_group_kind.target = {"window", std::to_string(id)};
  const auto bad_kind_result = kernel.execute(bad_group_kind);
  assert(!bad_kind_result.succeeded);
  assert(bad_kind_result.error.code == rime::core::Error::Code::InvalidContract);

  rime::action::Action group_focus_action = move_action;
  group_focus_action.id = 44;
  group_focus_action.type = "window.group.activate";
  group_focus_action.target = {"group", "executor_group"};
  group_focus_action.payload = R"({"reverse":"yes"})";
  const auto bad_reverse_result = kernel.execute(group_focus_action);
  assert(!bad_reverse_result.succeeded);
  assert(bad_reverse_result.error.code == rime::core::Error::Code::InvalidContract);

  // Reverse activation and deactivation: weak, but a resolved target must
  // be inspectable.
  group_focus_action.id = 45;
  group_focus_action.payload = R"({"reverse":true})";
  const auto group_focused = kernel.execute(group_focus_action);
  assert(group_focused.succeeded ||
         group_focused.error.code == rime::core::Error::Code::ExecutionFailed);
  group_focus_action.id = 46;
  group_focus_action.type = "window.group.deactivate";
  group_focus_action.payload = R"({"reverse":true})";
  const auto group_deactivated = kernel.execute(group_focus_action);
  assert(group_deactivated.succeeded ||
         group_deactivated.error.code == rime::core::Error::Code::ExecutionFailed);

  rime::action::Action group_close_action = move_action;
  group_close_action.id = 47;
  group_close_action.type = "window.group.close";
  group_close_action.target = {"group", "executor_group"};
  group_close_action.payload = R"({"mode":"bogus"})";
  const auto bad_mode_result = kernel.execute(group_close_action);
  assert(!bad_mode_result.succeeded);
  assert(bad_mode_result.error.code == rime::core::Error::Code::InvalidContract);

  // Reverse close advances the cycle: weak on focus, exact on the payload
  // shape it reports.
  group_close_action.id = 48;
  group_close_action.payload = R"({"mode":"reverse"})";
  const auto group_reverse_close = kernel.execute(group_close_action);
  assert(group_reverse_close.succeeded ||
         group_reverse_close.error.code == rime::core::Error::Code::ExecutionFailed);
  if (group_reverse_close.succeeded) {
    assert(group_reverse_close.value.find("closed") != nullptr);
    assert(group_reverse_close.value.find("activated") != nullptr);
  }

  // Close-all: exact count and no activation. The pair only lives in this
  // group, so exactly two closes are observed.
  group_close_action.id = 49;
  group_close_action.payload = R"({"mode":"all"})";
  const auto group_close_all = kernel.execute(group_close_action);
  assert(group_close_all.succeeded);
  assert(group_close_all.value.find("closed") != nullptr);
  assert(group_close_all.value.find("closed")->as_number() == 2.0);
  assert(group_close_all.value.find("activated") != nullptr);
  assert(group_close_all.value.find("activated")->is_null());
  rime::win32::WindowQuery closeall_query;
  closeall_query.title = "Rime Group Close";
  closeall_query.title_match_mode = TitleMatchMode::StartsWith;
  std::vector<WindowInfo> closeall_leftovers;
  assert(service.query(closeall_query, closeall_leftovers).ok());
  assert(closeall_leftovers.empty());

  // window.zorder through the kernel: bottom then top reorders the pair,
  // and a bad placement refuses as InvalidContract before any Win32 call.
  HWND exec_zorder_a = nullptr;
  HWND exec_zorder_b = nullptr;
  assert(service.ui()
             .call([&] {
               exec_zorder_a = CreateWindowExW(0, L"STATIC", L"Rime ZOrder Exec",
                                               WS_OVERLAPPED | WS_VISIBLE, 90, 90, 160, 120,
                                               nullptr, nullptr, GetModuleHandleW(nullptr),
                                               nullptr);
               exec_zorder_b = CreateWindowExW(0, L"STATIC", L"Rime ZOrder Exec B",
                                               WS_OVERLAPPED | WS_VISIBLE, 290, 90, 160, 120,
                                               nullptr, nullptr, GetModuleHandleW(nullptr),
                                               nullptr);
               assert(exec_zorder_a != nullptr);
               assert(exec_zorder_b != nullptr);
             })
             .ok());
  rime::win32::WindowQuery exec_zorder_query;
  exec_zorder_query.title = "Rime ZOrder Exec";
  exec_zorder_query.title_match_mode = TitleMatchMode::StartsWith;
  std::vector<WindowInfo> exec_zorder_matches;
  assert(service.query(exec_zorder_query, exec_zorder_matches).ok());
  assert(exec_zorder_matches.size() == 2);
  std::uint64_t exec_zorder_id_a = 0;
  std::uint64_t exec_zorder_id_b = 0;
  for (const auto& match : exec_zorder_matches) {
    if (match.title == "Rime ZOrder Exec") exec_zorder_id_a = match.id;
    if (match.title == "Rime ZOrder Exec B") exec_zorder_id_b = match.id;
  }
  assert(exec_zorder_id_a != 0 && exec_zorder_id_b != 0);
  rime::action::Action zorder_action = move_action;
  zorder_action.id = 46;
  zorder_action.type = "window.zorder";
  zorder_action.target.id = std::to_string(exec_zorder_id_a);
  zorder_action.payload = R"({"placement":"bottom"})";
  auto zorder_result = kernel.execute(zorder_action);
  assert(zorder_result.succeeded);
  assert(zorder_result.detail.find("z-order") != std::string::npos);
  assert(zorder_above(exec_zorder_b, exec_zorder_a));
  zorder_action.id = 47;
  zorder_action.payload = R"({"placement":"top"})";
  zorder_result = kernel.execute(zorder_action);
  assert(zorder_result.succeeded);
  assert(zorder_above(exec_zorder_a, exec_zorder_b));
  zorder_action.id = 48;
  zorder_action.payload = R"({"placement":"middle"})";
  zorder_result = kernel.execute(zorder_action);
  assert(!zorder_result.succeeded);
  assert(zorder_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(service.ui()
             .call([&] {
               DestroyWindow(exec_zorder_a);
               DestroyWindow(exec_zorder_b);
             })
             .ok());

  // window.redraw round-trips the unchanged snapshot; window.kill delivers
  // WM_CLOSE through the executor and the id goes stale immediately after.
  HWND exec_kill = nullptr;
  assert(service.ui()
             .call([&] {
               exec_kill = CreateWindowExW(0, L"STATIC", L"Rime Executor Kill",
                                           WS_OVERLAPPED | WS_VISIBLE, 120, 120, 200, 140,
                                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
               assert(exec_kill != nullptr);
             })
             .ok());
  std::vector<WindowInfo> exec_kill_match;
  rime::win32::WindowQuery exec_kill_query;
  exec_kill_query.title = "Rime Executor Kill";
  assert(service.query(exec_kill_query, exec_kill_match).ok());
  assert(exec_kill_match.size() == 1);
  rime::action::Action redraw_action = move_action;
  redraw_action.id = 50;
  redraw_action.type = "window.redraw";
  redraw_action.target.id = std::to_string(exec_kill_match.front().id);
  redraw_action.payload = "{}";
  const auto redraw_result = kernel.execute(redraw_action);
  assert(redraw_result.succeeded);
  assert(redraw_result.value.find("title") != nullptr);
  rime::action::Action kill_action = move_action;
  kill_action.id = 51;
  kill_action.type = "window.kill";
  kill_action.target.id = std::to_string(exec_kill_match.front().id);
  kill_action.payload = "{}";
  const auto kill_result = kernel.execute(kill_action);
  assert(kill_result.succeeded);
  assert(kill_result.value.is_object());
  assert(kill_result.value.find("title") != nullptr);
  WindowInfo kill_after;
  assert(service.info(exec_kill_match.front().id, kill_after).code ==
         rime::core::Error::Code::TargetGone);

  // window.minimizeall / window.minimizeall.undo post the shell tray
  // command through the kernel; the effect is asynchronous, so both halves
  // poll a dedicated full-overlapped victim (the shell skips borderless
  // windows, and the main test window is borderless). The undo always runs
  // before the assertions so a failure never leaves the desktop minimized.
  HWND ma_victim = nullptr;
  assert(service.ui()
             .call([&] {
               ma_victim = CreateWindowExW(0, L"STATIC", L"Rime Executor MiniAll",
                                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 340, 240, 160,
                                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
               assert(ma_victim != nullptr);
             })
             .ok());
  rime::win32::WindowQuery ma_query;
  ma_query.title = "Rime Executor MiniAll";
  std::vector<WindowInfo> ma_match;
  assert(service.query(ma_query, ma_match).ok());
  assert(ma_match.size() == 1);
  const std::uint64_t ma_id = ma_match.front().id;
  // Freshly created windows need a beat before the shell's minimize pass.
  std::this_thread::sleep_for(200ms);
  rime::action::Action minimizeall_action = move_action;
  minimizeall_action.id = 52;
  minimizeall_action.type = "window.minimizeall";
  minimizeall_action.target = {"desktop", "all"};
  minimizeall_action.payload = "{}";
  const auto minimizeall_result = kernel.execute(minimizeall_action);
  bool ma_executor_minimized = false;
  if (minimizeall_result.succeeded) {
    for (int waited = 0; waited < 300 && !ma_executor_minimized; ++waited) {
      WindowInfo ma_snapshot;
      if (service.info(ma_id, ma_snapshot).ok() && ma_snapshot.minimized) {
        ma_executor_minimized = true;
      } else {
        std::this_thread::sleep_for(10ms);
      }
    }
  }
  minimizeall_action.id = 53;
  minimizeall_action.type = "window.minimizeall.undo";
  const auto minimizeall_undo_result = kernel.execute(minimizeall_action);
  bool ma_executor_restored = false;
  if (minimizeall_undo_result.succeeded) {
    for (int waited = 0; waited < 300 && !ma_executor_restored; ++waited) {
      WindowInfo ma_snapshot;
      if (service.info(ma_id, ma_snapshot).ok() && !ma_snapshot.minimized) {
        ma_executor_restored = true;
      } else {
        std::this_thread::sleep_for(10ms);
      }
    }
  }
  assert(minimizeall_result.succeeded);
  assert(minimizeall_result.value.is_object());
  assert(minimizeall_result.detail.find("minimized") != std::string::npos);
  assert(ma_executor_minimized);
  assert(minimizeall_undo_result.succeeded);
  assert(minimizeall_undo_result.detail.find("restored") != std::string::npos);
  assert(ma_executor_restored);
  // A wrong target kind or id refuses before any Win32 call.
  minimizeall_action.id = 54;
  minimizeall_action.type = "window.minimizeall";
  minimizeall_action.target = {"window", "all"};
  const auto wrong_kind_result = kernel.execute(minimizeall_action);
  assert(!wrong_kind_result.succeeded);
  assert(wrong_kind_result.error.code == rime::core::Error::Code::InvalidContract);
  minimizeall_action.target = {"desktop", "everything"};
  const auto wrong_id_result = kernel.execute(minimizeall_action);
  assert(!wrong_id_result.succeeded);
  assert(wrong_id_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(service.ui().call([&] { DestroyWindow(ma_victim); }).ok());

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
