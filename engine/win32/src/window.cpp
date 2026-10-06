#include "rime/win32/window.hpp"

#include "rime/core/lane.hpp"

#include "handle_guard.hpp"
#include "utf.hpp"
#include "window_foreground.hpp"
#include "window_geometry.hpp"
#include "window_match.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rime::win32 {

using detail::ClassNNCounter;
using detail::ClassNNInstance;
using detail::EnumContext;
using detail::PidImageCache;
using detail::ResolvedQuery;
using detail::WindowGroups;
using detail::WindowRegistry;
using detail::build_info;
using detail::collect_matching;
using detail::expired_deadline;
using detail::has_selectors;
using detail::kWindowGoneMessage;
using detail::matches_query;
using detail::next_class_nn;
using detail::resolve_placement;
using detail::resolve_query;
using detail::work_area_for;

namespace {

namespace lane = rime::core;

}  // namespace

struct WindowService::Impl {
  UiThread ui;
  WindowRegistry registry;  // UI-thread only
  // Global window options written from the JS thread and read from any
  // caller; relaxed atomics are enough because every consumer posts through
  // a synchronized queue after the write (or is the writing thread).
  std::atomic<TitleMatchMode> title_match_mode{TitleMatchMode::Contains};
  std::atomic<bool> title_match_mode_slow{false};
  std::atomic<bool> detect_hidden_windows{false};
  std::atomic<bool> detect_hidden_text{false};
  // Window groups (AHK WinGroup): named query-spec lists plus the shared
  // visited-window cycle state driving activate/deactivate/close. UI-thread
  // only - every group method runs inside ui.call, so no lock is needed.
  // The complete type lives in window_match.hpp (it needs HWND), the four
  // group operations live in window_groups.cpp; unique_ptr keeps window.hpp
  // from including the HWND-dependent header.
  std::unique_ptr<WindowGroups> groups = std::make_unique<WindowGroups>();
};

WindowService::WindowService() : impl_(std::make_unique<Impl>()) {}
WindowService::~WindowService() { stop(); }

WindowSettings WindowService::settings() const {
  WindowSettings current;
  current.title_match_mode = impl_->title_match_mode.load(std::memory_order_relaxed);
  current.title_match_mode_slow = impl_->title_match_mode_slow.load(std::memory_order_relaxed);
  current.detect_hidden_windows = impl_->detect_hidden_windows.load(std::memory_order_relaxed);
  current.detect_hidden_text = impl_->detect_hidden_text.load(std::memory_order_relaxed);
  return current;
}

WindowSettings WindowService::set_settings(const WindowSettingsPatch& patch) {
  const WindowSettings previous = settings();
  if (patch.title_match_mode.has_value()) {
    impl_->title_match_mode.store(*patch.title_match_mode, std::memory_order_relaxed);
  }
  if (patch.title_match_mode_slow.has_value()) {
    impl_->title_match_mode_slow.store(*patch.title_match_mode_slow, std::memory_order_relaxed);
  }
  if (patch.detect_hidden_windows.has_value()) {
    impl_->detect_hidden_windows.store(*patch.detect_hidden_windows, std::memory_order_relaxed);
  }
  if (patch.detect_hidden_text.has_value()) {
    impl_->detect_hidden_text.store(*patch.detect_hidden_text, std::memory_order_relaxed);
  }
  return previous;
}

rime::core::json::Value window_info_json(const WindowInfo& info) {
  namespace json = rime::core::json;
  json::Value rect = json::Value::object();
  rect.set("left", json::Value::number(info.rect.left));
  rect.set("top", json::Value::number(info.rect.top));
  rect.set("right", json::Value::number(info.rect.right));
  rect.set("bottom", json::Value::number(info.rect.bottom));
  json::Value client_rect = json::Value::object();
  client_rect.set("left", json::Value::number(info.client_rect.left));
  client_rect.set("top", json::Value::number(info.client_rect.top));
  client_rect.set("right", json::Value::number(info.client_rect.right));
  client_rect.set("bottom", json::Value::number(info.client_rect.bottom));
  json::Value value = json::Value::object();
  value.set("id", json::Value::number(static_cast<double>(info.id)));
  value.set("title", json::Value::string(info.title));
  value.set("className", json::Value::string(info.class_name));
  value.set("processName", json::Value::string(info.process_name));
  value.set("processPath", json::Value::string(info.process_path));
  value.set("rect", std::move(rect));
  value.set("clientRect", std::move(client_rect));
  value.set("visible", json::Value::boolean(info.visible));
  value.set("minimized", json::Value::boolean(info.minimized));
  value.set("state", json::Value::string(info.state));
  value.set("processId", json::Value::number(static_cast<double>(info.process_id)));
  value.set("style", json::Value::number(static_cast<double>(info.style)));
  value.set("exStyle", json::Value::number(static_cast<double>(info.ex_style)));
  value.set("enabled", json::Value::boolean(info.enabled));
  value.set("alwaysOnTop", json::Value::boolean(info.always_on_top));
  value.set("minMax", json::Value::number(info.min_max));
  value.set("transparent", json::Value::number(info.transparent));
  value.set("transColor", json::Value::string(info.trans_color));
  value.set("region", json::Value::string(info.region));
  return value;
}

UiThread& WindowService::ui() { return impl_->ui; }
UiThreadState WindowService::state() const { return impl_->ui.state(); }

detail::WindowRegistry& WindowService::registry() { return impl_->registry; }
detail::WindowGroups& WindowService::group_state() { return *impl_->groups; }

rime::core::Error WindowService::start() { return impl_->ui.start(); }

rime::core::Error WindowService::stop() { return impl_->ui.stop(); }

rime::core::Error WindowService::list(std::vector<WindowInfo>& out,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  std::vector<WindowInfo> windows;
  // The list() baseline has no WinTitle selectors: DetectHiddenWindows picks
  // the visibility filter (AHK WinGetList) and matches_query adds the
  // untitled-window skip, so list() shares query()'s enum callback.
  ResolvedQuery baseline;
  baseline.include_hidden = settings().detect_hidden_windows;
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        EnumContext context{&impl_->registry, &baseline, false, &windows, nullptr};
        EnumWindows(collect_matching, reinterpret_cast<LPARAM>(&context));
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = std::move(windows);
  return rime::core::Error::none();
}

rime::core::Error WindowService::query(const WindowQuery& query, std::vector<WindowInfo>& out,
                                       const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  // Resolved on the caller thread: regex compilation touches no HWND, and a
  // bad pattern fails the call without queueing UI work.
  ResolvedQuery resolved;
  if (const auto error = resolve_query(query, settings(), resolved); !error.ok()) return error;
  rime::core::Error result = rime::core::Error::none();
  std::vector<WindowInfo> windows;
  const bool selectors = has_selectors(query);
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const bool foreground_selection = query.active;
        if (foreground_selection) {
          const HWND foreground = GetForegroundWindow();
          if (foreground != nullptr && matches_query(impl_->registry, foreground, resolved,
                                                     nullptr)) {
            WindowInfo info;
            if (build_info(impl_->registry, foreground, info).ok()) {
              windows.push_back(std::move(info));
            }
          }
          return;
        }
        PidImageCache names;
        EnumContext context{&impl_->registry, &resolved, selectors, &windows, &names};
        EnumWindows(collect_matching, reinterpret_cast<LPARAM>(&context));
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = std::move(windows);
  return rime::core::Error::none();
}

rime::core::Error WindowService::active(std::optional<WindowInfo>& out,
                                        const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  std::optional<WindowInfo> found;
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND foreground = GetForegroundWindow();
        if (!foreground) return;
        WindowInfo info;
        if (build_info(impl_->registry, foreground, info).ok()) found = std::move(info);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = found;
  return rime::core::Error::none();
}

rime::core::Error WindowService::exists(const WindowQuery& query, bool& out,
                                        const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  ResolvedQuery resolved;
  if (const auto error = resolve_query(query, settings(), resolved); !error.ok()) return error;
  rime::core::Error result = rime::core::Error::none();
  bool found = false;
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        if (query.active) {
          const HWND foreground = GetForegroundWindow();
          found = foreground != nullptr &&
                  matches_query(impl_->registry, foreground, resolved, nullptr);
          return;
        }
        PidImageCache names;
        struct Probe {
          WindowRegistry* registry;
          const ResolvedQuery* resolved;
          PidImageCache* names;
          bool* found;
        };
        Probe probe{&impl_->registry, &resolved, &names, &found};
        EnumWindows(
            [](HWND window, LPARAM parameter) -> BOOL {
              auto* probe_context = reinterpret_cast<Probe*>(parameter);
              if (!matches_query(*probe_context->registry, window, *probe_context->resolved,
                                 probe_context->names)) {
                return TRUE;
              }
              *probe_context->found = true;
              return FALSE;  // first match wins; the enum can stop here
            },
            reinterpret_cast<LPARAM>(&probe));
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = found;
  return rime::core::Error::none();
}

rime::core::Error WindowService::matches_active(const WindowQuery& query, bool& out,
                                                const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  ResolvedQuery resolved;
  if (const auto error = resolve_query(query, settings(), resolved); !error.ok()) return error;
  rime::core::Error result = rime::core::Error::none();
  bool found = false;
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND foreground = GetForegroundWindow();
        found = foreground != nullptr && matches_query(impl_->registry, foreground, resolved,
                                                       nullptr);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = found;
  return rime::core::Error::none();
}

rime::core::Error WindowService::evaluate_wait(const WindowQuery& query, WaitCondition until,
                                                WaitEvaluation& out,
                                                const std::chrono::milliseconds timeout) {
  out = WaitEvaluation{};
  switch (until) {
    case WaitCondition::Exists: {
      std::vector<WindowInfo> windows;
      if (const auto error = this->query(query, windows, timeout); !error.ok()) return error;
      if (windows.empty()) return rime::core::Error::none();
      out.met = true;
      out.target = std::move(windows.front());
      return rime::core::Error::none();
    }
    case WaitCondition::Active: {
      bool matches = false;
      if (const auto error = matches_active(query, matches, timeout); !error.ok()) return error;
      if (!matches) return rime::core::Error::none();
      out.met = true;
      std::optional<WindowInfo> foreground;
      if (const auto error = active(foreground, timeout); !error.ok()) return error;
      out.target = std::move(foreground);
      return rime::core::Error::none();
    }
    case WaitCondition::Closed: {
      std::vector<WindowInfo> windows;
      if (const auto error = this->query(query, windows, timeout); !error.ok()) return error;
      out.met = windows.empty();
      return rime::core::Error::none();
    }
    case WaitCondition::NotActive: {
      bool matches = false;
      if (const auto error = matches_active(query, matches, timeout); !error.ok()) return error;
      out.met = !matches;
      return rime::core::Error::none();
    }
  }
  return rime::core::Error::none();
}

rime::core::Error WindowService::info(const std::uint64_t id, WindowInfo& out,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  WindowInfo info;
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        result = build_info(impl_->registry, impl_->registry.hwnd_for(id), info);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = std::move(info);
  return rime::core::Error::none();
}

rime::core::Error WindowService::controls(const std::uint64_t id,
                                          std::vector<ControlInfo>& out,
                                          const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  std::vector<ControlInfo> controls;
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        // AHK EnumChildWindows semantics: z-order, descendants (not just
        // direct children), hidden controls included so the ClassNN numbers
        // stay stable when an app hides and re-shows controls.
        struct EnumState {
          WindowRegistry* registry;
          std::vector<ControlInfo>* controls;
          // AHK numbers per class across the whole enumeration; the shared
          // counter owns the case-insensitive compare and the 99999 cap.
          ClassNNCounter counts;
        } state{&impl_->registry, &controls, {}};
        EnumChildWindows(
            window,
            [](HWND control, LPARAM parameter) -> BOOL {
              auto* enum_state = reinterpret_cast<EnumState*>(parameter);
              const ClassNNInstance instance = next_class_nn(enum_state->counts, control);
              if (instance.number == 0) return TRUE;  // unnameable or past the cap
              ControlInfo info;
              info.id = enum_state->registry->id_for(control);
              info.class_name = to_utf8(instance.class_name);
              info.class_nn = info.class_name + std::to_string(instance.number);
              enum_state->controls->push_back(std::move(info));
              return TRUE;
            },
            reinterpret_cast<LPARAM>(&state));
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = std::move(controls);
  return rime::core::Error::none();
}

rime::core::Error WindowService::text(const std::uint64_t id, std::string& out,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  // DetectHiddenText decides whether hidden controls contribute (AHK WinGetText).
  const bool detect_hidden = settings().detect_hidden_text;
  rime::core::Error result = rime::core::Error::none();
  std::wstring joined;
  const auto call_error = impl_->ui.call(
      [&, detect_hidden] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        // AHK WinGetText: per control WM_GETTEXT via SendMessageTimeout
        // (SMTO_ABORTIFHUNG, 5s like AHK's GetWindowTextTimeout), "\r\n"
        // after every non-empty text including the last one. Hidden controls
        // are skipped unless DetectHiddenText is on.
        struct TextState {
          std::wstring* joined;
          bool detect_hidden;
        } state{&joined, detect_hidden};
        EnumChildWindows(
            window,
            [](HWND control, LPARAM parameter) -> BOOL {
              auto* text_state = reinterpret_cast<TextState*>(parameter);
              if (!text_state->detect_hidden && !IsWindowVisible(control)) return TRUE;
              DWORD_PTR length = 0;
              if (!SendMessageTimeoutW(control, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG,
                                       5000, &length) ||
                  length == 0) {
                return TRUE;
              }
              std::wstring buffer(length + 1, L'\0');
              DWORD_PTR copied = 0;
              if (!SendMessageTimeoutW(control, WM_GETTEXT,
                                       static_cast<WPARAM>(buffer.size()),
                                       reinterpret_cast<LPARAM>(buffer.data()),
                                       SMTO_ABORTIFHUNG, 5000, &copied) ||
                  copied == 0) {
                return TRUE;
              }
              // Misbehaving apps report or write more than they should (the
              // same class of bugs AHK defends against): clamp and strip a
              // stray terminator.
              if (copied > buffer.size() - 1) copied = buffer.size() - 1;
              while (copied > 0 && buffer[copied - 1] == L'\0') --copied;
              text_state->joined->append(buffer.data(), copied);
              text_state->joined->append(L"\r\n");
              return TRUE;
            },
            reinterpret_cast<LPARAM>(&state));
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = to_utf8(joined);
  return rime::core::Error::none();
}

namespace {

// Shared by move() and placement_rect(): read the monitor work area of
// `window` (nullptr selects the primary one) and resolve the named placement
// against it. A failed work-area read maps to ExecutionFailed and an unknown
// placement name to InvalidContract - identically in both entry points.
rime::core::Error resolve_named_placement(const HWND window, const std::string& placement,
                                           Rect& out) {
  RECT work{};
  if (!work_area_for(window, work)) {
    return {rime::core::Error::Code::ExecutionFailed, "cannot read the monitor work area"};
  }
  if (!resolve_placement(work, placement, out)) {
    return {rime::core::Error::Code::InvalidContract, "unknown window placement: " + placement};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error WindowService::move(const std::uint64_t id, const std::string_view placement,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  const std::string placement_text(placement);
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        Rect target{};
        if (const auto error = resolve_named_placement(window, placement_text, target);
            !error.ok()) {
          result = error;
          return;
        }
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        if (!SetWindowPos(window, nullptr, target.left, target.top, target.width(),
                          target.height(), SWP_NOZORDER | SWP_NOACTIVATE)) {
          result = {rime::core::Error::Code::ExecutionFailed, "SetWindowPos failed"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::move_rect(const std::uint64_t id, const RectMove& move,
                                           const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  // Defense in depth: the executor validates the wire payload too, but a
  // direct caller must not ask for a degenerate frame or an edge that does
  // not fit the 32-bit screen coordinates SetWindowPos takes.
  const auto valid_field = [](const std::optional<std::int64_t>& field, const std::int64_t low) {
    return !field || (*field >= low && *field <= 2147483647);
  };
  if (!valid_field(move.w, 1) || !valid_field(move.h, 1) || !valid_field(move.x, -2147483648) ||
      !valid_field(move.y, -2147483648)) {
    return {rime::core::Error::Code::InvalidContract, "rect move fields are out of range"};
  }
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        // Omitted fields keep the current value (AHK WinMove rule): read the
        // committed rect first, then apply the patch as one SetWindowPos so
        // the window never visits an intermediate shape.
        RECT current{};
        if (!GetWindowRect(window, &current)) {
          result = {rime::core::Error::Code::ExecutionFailed, "GetWindowRect failed"};
          return;
        }
        const auto patched = [](const std::optional<std::int64_t>& field, const long value) {
          return field ? static_cast<long>(*field) : value;
        };
        const long x = patched(move.x, current.left);
        const long y = patched(move.y, current.top);
        const long w = patched(move.w, current.right - current.left);
        const long h = patched(move.h, current.bottom - current.top);
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        if (!SetWindowPos(window, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE)) {
          result = {rime::core::Error::Code::ExecutionFailed, "SetWindowPos failed"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::focus(const std::uint64_t id,
                                       const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        // The ladder is shared with set_always_on_top: SetForegroundWindow
        // alone is refused for a background process, which is why a plain
        // call reported "denied by the foreground lock" while the very same
        // window came forward for a process that tapped Alt or attached to
        // the foreground queue. The verdict is read back from
        // GetForegroundWindow(), not from SetForegroundWindow's return.
        if (acquire_foreground(window) == ForegroundRun::Denied) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetForegroundWindow was denied by the foreground lock"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::zorder(const std::uint64_t id, const bool bottom,
                                        const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        // AHK WinMoveTopBottom: SWP_NOACTIVATE is required, otherwise the
        // target window often fails to move.
        if (!SetWindowPos(window, bottom ? HWND_BOTTOM : HWND_TOP, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetWindowPos failed (win32 error " + std::to_string(failure) + ")"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

namespace {

// Shared body for the flag-based state mutations (hide/show/min/max/restore).
lane::Error show_window_op(WindowRegistry& registry, const std::uint64_t id, const int command) {
  const HWND window = registry.hwnd_for(id);
  if (!window) return {lane::Error::Code::TargetGone, kWindowGoneMessage};
  ShowWindow(window, command);
  return lane::Error::none();
}

}  // namespace

rime::core::Error WindowService::close(const std::uint64_t id,
                                       const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  // Absolute deadline: ui.call() already spends part of `timeout` while queued,
  // so recompute the remainder inside the lane instead of reusing the full
  // timeout for SendMessageTimeoutW (which would double-count the queue wait).
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto call_error = impl_->ui.call(
      [&, deadline] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
          result = expired_deadline();
          return;
        }
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        const auto bounded = std::clamp<std::uint64_t>(
            static_cast<std::uint64_t>(remaining_ms.count()), 1, 0xffffffffu);
        DWORD_PTR delivered = 0;
        SetLastError(0);
        const LRESULT sent = SendMessageTimeoutW(window, WM_CLOSE, 0, 0,
                                                 SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
                                                 static_cast<UINT>(bounded), &delivered);
        if (sent == 0 && GetLastError() == ERROR_TIMEOUT) {
          result = {rime::core::Error::Code::Timeout, "WM_CLOSE was not delivered in time"};
          return;
        }
        if (sent == 0 && IsWindow(window)) {
          result = {rime::core::Error::Code::ExecutionFailed, "WM_CLOSE was not delivered"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::kill(const std::uint64_t id,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  // Absolute deadline like close(): ui.call() already spends part of
  // `timeout` while queued, and the WM_CLOSE wait below must not
  // double-count it.
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto call_error = impl_->ui.call(
      [&, deadline] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
          result = expired_deadline();
          return;
        }
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        // AHK Util_WinKill waits at most 500ms for WM_CLOSE; WinKill is for
        // suspected-hung targets, so it must not sit on the deadline.
        auto budget = static_cast<DWORD>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(remaining_ms.count()), 500u));
        if (budget == 0) budget = 1;
        DWORD_PTR delivered = 0;
        SetLastError(0);
        const LRESULT sent = SendMessageTimeoutW(window, WM_CLOSE, 0, 0,
                                                 SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, budget,
                                                 &delivered);
        // Handled (destroyed or explicitly ignored) or already gone: done.
        if (sent != 0 || !IsWindow(window)) return;
        // Hung or undeliverable: fall back to TerminateProcess like AHK,
        // with one deliberate deviation - never kill our own process.
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == 0) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "cannot force-terminate: window has no process"};
          return;
        }
        if (pid == GetCurrentProcessId()) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "refusing to force-terminate the runtime's own process"};
          return;
        }
        const HandleGuard process(OpenProcess(PROCESS_TERMINATE, FALSE, pid));
        if (!process) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "OpenProcess failed (win32 error " + std::to_string(failure) + ")"};
          return;
        }
        const BOOL terminated = TerminateProcess(process.get(), 0);
        // Captured while the handle is still open; the guard closes it at
        // scope exit, and CloseHandle would overwrite the last error.
        const DWORD failure = terminated ? 0u : GetLastError();
        if (!terminated) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "TerminateProcess failed (win32 error " + std::to_string(failure) + ")"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::redraw(const std::uint64_t id,
                                        const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        // AHK WinRedraw: InvalidateRect only - UpdateWindow would force an
        // immediate WM_PAINT, which AHK deliberately avoids.
        if (!InvalidateRect(window, nullptr, TRUE)) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "InvalidateRect failed (win32 error " + std::to_string(failure) + ")"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::minimize_all(const bool undo,
                                               const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        // AHK WinMinimizeAll / WinMinimizeAllUndo: PostMessage the Shell_
        // TrayWnd taskbar with WM_COMMAND 419 (minimize all) / 416 (undo).
        const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (tray == nullptr) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "cannot find the taskbar window (Shell_TrayWnd)"};
          return;
        }
        const WPARAM command = undo ? 416u : 419u;
        if (!PostMessageW(tray, WM_COMMAND, command, 0)) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "PostMessage(WM_COMMAND) failed (win32 error " +
                        std::to_string(failure) + ")"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::hide(const std::uint64_t id,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        result = show_window_op(impl_->registry, id, SW_HIDE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::show(const std::uint64_t id,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        result = show_window_op(impl_->registry, id, SW_SHOW);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::minimize(const std::uint64_t id,
                                           const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        result = show_window_op(impl_->registry, id, SW_MINIMIZE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::maximize(const std::uint64_t id,
                                           const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        ShowWindow(window, SW_MAXIMIZE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::restore(const std::uint64_t id,
                                          const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        result = show_window_op(impl_->registry, id, SW_RESTORE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::placement_rect(const std::string_view placement, Rect& out,
                                                 const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  const std::string placement_text(placement);
  rime::core::Error result = rime::core::Error::none();
  Rect resolved{};
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        if (const auto error = resolve_named_placement(nullptr, placement_text, resolved);
            !error.ok()) {
          result = error;
          return;
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = resolved;
  return rime::core::Error::none();
}

}  // namespace rime::win32
