#include "rime/win32/window.hpp"

#include "rime/core/lane.hpp"

#include "utf.hpp"
#include "window_geometry.hpp"
#include "window_match.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rime::win32 {

using detail::EnumContext;
using detail::PidImageCache;
using detail::ResolvedQuery;
using detail::WindowRegistry;
using detail::build_info;
using detail::collect_matching;
using detail::has_selectors;
using detail::matches_query;
using detail::resolve_placement;
using detail::resolve_query;
using detail::window_text;
using detail::work_area_for;

namespace {

namespace lane = rime::core;

lane::Error expired_deadline() {
  return {lane::Error::Code::Timeout, "operation deadline exceeded"};
}

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
  std::unordered_map<std::string, std::vector<WindowQuery>> groups;
  std::string last_group;
  std::vector<HWND> visited;
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

rime::core::Error WindowService::start() { return impl_->ui.start(); }

rime::core::Error WindowService::stop() { return impl_->ui.stop(); }

rime::core::Error WindowService::list(std::vector<WindowInfo>& out,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  std::vector<WindowInfo> windows;
  // DetectHiddenWindows decides the baseline visibility filter (AHK WinGetList).
  const bool include_hidden = settings().detect_hidden_windows;
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        EnumContext context{&impl_->registry, nullptr, include_hidden, false, &windows, nullptr};
        EnumWindows(
            [](HWND window, LPARAM parameter) -> BOOL {
              auto* enum_context = reinterpret_cast<EnumContext*>(parameter);
              if (!enum_context->include_hidden && !IsWindowVisible(window)) return TRUE;
              if (window_text(window).empty()) return TRUE;
              WindowInfo info;
              if (!build_info(*enum_context->registry, window, info).ok()) return TRUE;
              enum_context->windows->push_back(std::move(info));
              return TRUE;
            },
            reinterpret_cast<LPARAM>(&context));
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
        EnumContext context{&impl_->registry, &resolved, false, selectors, &windows, &names};
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

namespace {

// AHK WinGroup bounds its static visited list at MAX_ALREADY_VISITED; a
// full list stops recording (no eviction, no wrap).
constexpr std::size_t kMaxVisitedWindows = 500;

// Resolves every spec of a group against the current settings; the first
// bad spec fails the whole call. ResolvedQuery borrows its source spec, so
// the spec vector must outlive the evaluation.
rime::core::Error resolve_group_specs(const std::vector<WindowQuery>& specs,
                                      const WindowSettings& settings,
                                      std::vector<ResolvedQuery>& out) {
  out.clear();
  out.reserve(specs.size());
  for (const WindowQuery& spec : specs) {
    ResolvedQuery resolved;
    if (const auto error = resolve_query(spec, settings, resolved); !error.ok()) return error;
    out.push_back(resolved);
  }
  return rime::core::Error::none();
}

// True when `window` matches any resolved spec (AHK WinGroup::IsMember).
bool group_is_member(WindowRegistry& registry, HWND window,
                     const std::vector<ResolvedQuery>& specs, PidImageCache* cache) {
  for (const ResolvedQuery& resolved : specs) {
    if (matches_query(registry, window, resolved, cache)) return true;
  }
  return false;
}

struct MemberEnumContext {
  WindowRegistry* registry;
  const std::vector<ResolvedQuery>* specs;
  PidImageCache* cache;
  std::vector<HWND>* out;
};

BOOL CALLBACK collect_members(HWND window, LPARAM parameter) {
  auto* context = reinterpret_cast<MemberEnumContext*>(parameter);
  if (group_is_member(*context->registry, window, *context->specs, context->cache)) {
    context->out->push_back(window);
  }
  return TRUE;
}

// AHK's MarkAsVisited: de-duplicate, and drop silently once the cap is hit.
void mark_visited(std::vector<HWND>& visited, HWND window) {
  if (window == nullptr) return;
  if (std::find(visited.begin(), visited.end(), window) != visited.end()) return;
  if (visited.size() >= kMaxVisitedWindows) return;
  visited.push_back(window);
}

// Eligibility filter for GroupDeactivate's non-member targets (AHK
// EnumParentFindAnyExcept minus the cloaked check, which would drag in
// dwmapi for a heuristic Rime does not need yet): visible, not
// topmost/no-activate, not a bare tool window, unowned, never the desktop.
bool eligible_deactivate_target(HWND window) {
  if (!IsWindowVisible(window)) return false;
  const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if ((ex_style & (WS_EX_TOPMOST | WS_EX_NOACTIVATE)) != 0) return false;
  if ((ex_style & (WS_EX_TOOLWINDOW | WS_EX_APPWINDOW)) == WS_EX_TOOLWINDOW) return false;
  if (GetWindow(window, GW_OWNER) != nullptr) return false;
  if (window == GetShellWindow()) return false;
  return true;
}

struct DeactivateEnumContext {
  WindowRegistry* registry;
  const std::vector<ResolvedQuery>* specs;
  PidImageCache* cache;
  const std::vector<HWND>* visited;
  bool find_last;
  HWND eligible{nullptr};
};

BOOL CALLBACK collect_deactivate_target(HWND window, LPARAM parameter) {
  auto* context = reinterpret_cast<DeactivateEnumContext*>(parameter);
  if (!eligible_deactivate_target(window)) return TRUE;
  if (group_is_member(*context->registry, window, *context->specs, context->cache)) return TRUE;
  if (std::find(context->visited->begin(), context->visited->end(), window) !=
      context->visited->end()) {
    return TRUE;
  }
  context->eligible = window;
  // First match wins unless the most recent one was requested; then the
  // walk continues so the last match wins.
  return context->find_last ? TRUE : FALSE;
}

}  // namespace

rime::core::Error WindowService::group_add(const std::string& name, const WindowQuery& spec,
                                            std::size_t& spec_count,
                                            const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  if (name.empty()) {
    return {rime::core::Error::Code::InvalidContract, "group name must not be empty"};
  }
  spec_count = 0;
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        std::vector<WindowQuery>& specs = impl_->groups[name];
        if (std::find(specs.begin(), specs.end(), spec) == specs.end()) specs.push_back(spec);
        spec_count = specs.size();
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::group_activate(const std::string& name, const bool reverse,
                                                std::optional<WindowInfo>& out,
                                                const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  if (name.empty()) {
    return {rime::core::Error::Code::InvalidContract, "group name must not be empty"};
  }
  out = std::nullopt;
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        // AHK GroupActivate creates the group when it does not exist yet;
        // an empty group simply resolves nullopt.
        auto group = impl_->groups.try_emplace(name).first;
        std::vector<ResolvedQuery> specs;
        if (const auto error = resolve_group_specs(group->second, settings(), specs);
            !error.ok()) {
          result = error;
          return;
        }
        if (specs.empty()) return;
        PidImageCache cache;
        const HWND foreground = GetForegroundWindow();
        const bool group_is_active =
            foreground != nullptr && group_is_member(impl_->registry, foreground, specs, &cache);
        if (!group_is_active) impl_->visited.clear();
        // Default cycle starts at the oldest (bottom) member; `reverse`
        // starts at the most recent (top) one - unless the group already
        // owns the foreground, where AHK keeps walking oldest-first.
        const bool find_last = !reverse || group_is_active;
        for (bool retry_needed = !impl_->visited.empty();; retry_needed = false) {
          if (group_is_active) mark_visited(impl_->visited, foreground);
          std::vector<HWND> members;
          MemberEnumContext context{&impl_->registry, &specs, &cache, &members};
          EnumWindows(collect_members, reinterpret_cast<LPARAM>(&context));
          std::vector<HWND> candidates;
          candidates.reserve(members.size());
          for (const HWND member : members) {
            if (std::find(impl_->visited.begin(), impl_->visited.end(), member) ==
                impl_->visited.end()) {
              candidates.push_back(member);
            }
          }
          if (!candidates.empty()) {
            const HWND target = find_last ? candidates.back() : candidates.front();
            const std::uint64_t target_id = impl_->registry.id_for(target);
            mark_visited(impl_->visited, target);
            // Focus denial (foreground lock) propagates like window.focus.
            if (const auto error = focus(target_id, timeout); !error.ok()) {
              result = error;
              return;
            }
            WindowInfo info;
            if (const auto error = this->info(target_id, info, timeout); !error.ok()) {
              result = error;
              return;
            }
            out = std::move(info);
            return;
          }
          if (!retry_needed) break;
          impl_->visited.clear();
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::group_deactivate(const std::string& name, const bool reverse,
                                                  std::optional<WindowInfo>& out,
                                                  const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  if (name.empty()) {
    return {rime::core::Error::Code::InvalidContract, "group name must not be empty"};
  }
  out = std::nullopt;
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        // Unlike GroupActivate this requires a group that already exists
        // (AHK's argument error when FindGroup fails).
        const auto group = impl_->groups.find(name);
        if (group == impl_->groups.end()) {
          result = {rime::core::Error::Code::InvalidContract, "window group not found"};
          return;
        }
        std::vector<ResolvedQuery> specs;
        if (const auto error = resolve_group_specs(group->second, settings(), specs);
            !error.ok()) {
          result = error;
          return;
        }
        PidImageCache cache;
        HWND foreground = GetForegroundWindow();
        while (foreground != nullptr && GetWindow(foreground, GW_OWNER) != nullptr) {
          foreground = GetWindow(foreground, GW_OWNER);
        }
        const bool was_member =
            foreground != nullptr && group_is_member(impl_->registry, foreground, specs, &cache);
        if (was_member) impl_->visited.clear();
        for (int attempt = 0; attempt < 2; ++attempt) {
          const bool find_last = !reverse || !impl_->visited.empty();
          DeactivateEnumContext context{&impl_->registry, &specs, &cache, &impl_->visited,
                                         find_last};
          EnumWindows(collect_deactivate_target, reinterpret_cast<LPARAM>(&context));
          if (context.eligible != nullptr) {
            HWND activated = GetLastActivePopup(context.eligible);
            if (activated == nullptr) activated = context.eligible;
            const std::uint64_t activated_id = impl_->registry.id_for(activated);
            mark_visited(impl_->visited, context.eligible);
            if (const auto error = focus(activated_id, timeout); !error.ok()) {
              result = error;
              return;
            }
            WindowInfo info;
            if (const auto error = this->info(activated_id, info, timeout); !error.ok()) {
              result = error;
              return;
            }
            out = std::move(info);
            return;
          }
          if (impl_->visited.empty()) break;
          const bool wrap_around = impl_->visited.size() > 1;
          impl_->visited.clear();
          if (!wrap_around) break;
          if (foreground != nullptr) mark_visited(impl_->visited, foreground);
        }
        // Nothing left to review: fall back to the taskbar (AHK's last
        // resort). A locked-down session may refuse focus; that error
        // surfaces to the caller.
        const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (taskbar == nullptr) return;
        const std::uint64_t taskbar_id = impl_->registry.id_for(taskbar);
        if (const auto error = focus(taskbar_id, timeout); !error.ok()) {
          result = error;
          return;
        }
        WindowInfo info;
        if (const auto error = this->info(taskbar_id, info, timeout); !error.ok()) {
          result = error;
          return;
        }
        out = std::move(info);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::group_close(const std::string& name,
                                              const std::string_view mode,
                                              std::uint64_t& closed,
                                              std::optional<WindowInfo>& activated,
                                              const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  if (name.empty()) {
    return {rime::core::Error::Code::InvalidContract, "group name must not be empty"};
  }
  if (mode != "" && mode != "reverse" && mode != "all") {
    return {rime::core::Error::Code::InvalidContract, "unknown group close mode"};
  }
  closed = 0;
  activated = std::nullopt;
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const auto group = impl_->groups.find(name);
        if (group == impl_->groups.end()) {
          result = {rime::core::Error::Code::InvalidContract, "window group not found"};
          return;
        }
        std::vector<ResolvedQuery> specs;
        if (const auto error = resolve_group_specs(group->second, settings(), specs);
            !error.ok()) {
          result = error;
          return;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (mode == "all") {
          // Close every member up front and activate nothing (AHK's
          // CloseAllWinsInGroup ignores mIsModeActivate).
          std::vector<HWND> members;
          MemberEnumContext context{&impl_->registry, &specs, nullptr, &members};
          EnumWindows(collect_members, reinterpret_cast<LPARAM>(&context));
          for (const HWND member : members) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
              result = expired_deadline();
              return;
            }
            const std::uint64_t member_id = impl_->registry.id_for(member);
            const auto error = close(
                member_id, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            if (!error.ok()) {
              if (error.code == rime::core::Error::Code::TargetGone) continue;  // raced away
              result = error;
              return;
            }
            ++closed;
          }
          return;
        }
        const bool reverse = mode == "reverse";
        const HWND foreground = GetForegroundWindow();
        PidImageCache cache;
        if (foreground != nullptr && group_is_member(impl_->registry, foreground, specs, &cache)) {
          const auto now = std::chrono::steady_clock::now();
          if (now >= deadline) {
            result = expired_deadline();
            return;
          }
          const std::uint64_t foreground_id = impl_->registry.id_for(foreground);
          const auto error = close(
              foreground_id,
              std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
          if (!error.ok() && error.code != rime::core::Error::Code::TargetGone) {
            result = error;
            return;
          }
          if (error.ok()) ++closed;
          // When Windows already promoted the next member into the
          // foreground, the cycle stops there (AHK's IsMember check after
          // the close); otherwise activate the next member explicitly.
          const HWND next_foreground = GetForegroundWindow();
          if (next_foreground != nullptr && next_foreground != foreground &&
              group_is_member(impl_->registry, next_foreground, specs, &cache)) {
            const std::uint64_t next_id = impl_->registry.id_for(next_foreground);
            WindowInfo info;
            if (const auto info_error = this->info(next_id, info, timeout); !info_error.ok()) {
              result = info_error;
              return;
            }
            activated = std::move(info);
            return;
          }
        }
        // Advance the cycle (also the whole operation when the foreground
        // was not a member: nothing closed, focus just moves on).
        if (const auto error = group_activate(name, reverse, activated, timeout); !error.ok()) {
          result = error;
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        // AHK EnumChildWindows semantics: z-order, descendants (not just
        // direct children), hidden controls included so the ClassNN numbers
        // stay stable when an app hides and re-shows controls.
        struct EnumState {
          WindowRegistry* registry;
          std::vector<ControlInfo>* controls;
          // (class, instances seen so far); AHK numbers per class across the
          // whole enumeration with a case-insensitive compare and a 99999 cap.
          std::vector<std::pair<std::wstring, int>> counts;
        } state{&impl_->registry, &controls, {}};
        EnumChildWindows(
            window,
            [](HWND control, LPARAM parameter) -> BOOL {
              auto* enum_state = reinterpret_cast<EnumState*>(parameter);
              wchar_t class_name[256] = {};
              const int length = GetClassNameW(control, class_name, 256);
              if (length <= 0) return TRUE;  // AHK skips unnameable controls
              const std::wstring key(class_name, static_cast<std::size_t>(length));
              int* count = nullptr;
              for (auto& [known, occurrences] : enum_state->counts) {
                if (CompareStringOrdinal(known.c_str(), -1, key.c_str(), -1, TRUE) ==
                    CSTR_EQUAL) {
                  count = &occurrences;
                  break;
                }
              }
              if (count == nullptr) {
                enum_state->counts.emplace_back(key, 1);
                count = &enum_state->counts.back().second;
              } else {
                ++*count;
              }
              if (*count > 99999) return TRUE;  // AHK's numbering cap
              ControlInfo info;
              info.id = enum_state->registry->id_for(control);
              info.class_name = to_utf8(key);
              info.class_nn = info.class_name + std::to_string(*count);
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
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

rime::core::Error WindowService::window_at(const std::int32_t x, const std::int32_t y,
                                           WindowAtInfo& out,
                                           const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  WindowAtInfo info;
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const POINT point{x, y};
        const HWND child_under = WindowFromPoint(point);
        // No window under the point (desktop, or WindowFromPoint refused):
        // a successful read of "nothing is here", like AHK's blank outputs.
        if (!child_under) return;
        // AHK GetNonChildParent (source/window.cpp:1136): first ancestor
        // without WS_CHILD; the window itself when it is not a child.
        HWND parent = child_under;
        for (;;) {
          if ((static_cast<DWORD>(GetWindowLongPtrW(parent, GWL_STYLE)) & WS_CHILD) == 0) break;
          const HWND next = GetParent(parent);
          if (!next) break;
          parent = next;
        }
        WindowInfo window_info;
        if (!build_info(impl_->registry, parent, window_info).ok()) {
          return;  // parent vanished between the point read and the snapshot
        }
        info.window = std::move(window_info);

        // AHK EnumChildFindPoint (source/script2.cpp:1380): the topmost
        // visible descendant whose rect contains the point; a rect entirely
        // enclosed by the incumbent wins outright, otherwise the closer
        // center wins unless the candidate entirely encloses the incumbent.
        // Enumeration continues through every descendant so equal candidates
        // keep z-order precedence.
        HWND control_hwnd = child_under;
        if (child_under != parent) {
          struct PointState {
            POINT point;
            HWND found{nullptr};
            RECT found_rect{};
            double distance{0.0};
          } find{point, nullptr, {}, 0.0};
          EnumChildWindows(
              parent,
              [](HWND control, LPARAM parameter) -> BOOL {
                auto* state = reinterpret_cast<PointState*>(parameter);
                if (!IsWindowVisible(control)) return TRUE;  // Window Spy rule
                RECT rect{};
                if (!GetWindowRect(control, &rect)) return TRUE;
                // right/bottom are exclusive (MSDN): use < on both edges.
                if (!(state->point.x >= rect.left && state->point.x < rect.right &&
                      state->point.y >= rect.top && state->point.y < rect.bottom)) {
                  return TRUE;
                }
                const double center_x = rect.left + (rect.right - rect.left) / 2.0;
                const double center_y = rect.top + (rect.bottom - rect.top) / 2.0;
                const double distance =
                    std::hypot(static_cast<double>(state->point.x) - center_x,
                               static_cast<double>(state->point.y) - center_y);
                bool update = state->found == nullptr;
                if (!update) {
                  const RECT& old = state->found_rect;
                  if (rect.left >= old.left && rect.right <= old.right &&
                      rect.top >= old.top && rect.bottom <= old.bottom) {
                    update = true;  // new is entirely enclosed by old
                  } else if (distance < state->distance &&
                             (old.left < rect.left || old.right > rect.right ||
                              old.top < rect.top || old.bottom > rect.bottom)) {
                    update = true;  // closer center, and new does not enclose old
                  }
                }
                if (update) {
                  state->found = control;
                  state->found_rect = rect;
                  state->distance = distance;
                }
                return TRUE;
              },
              reinterpret_cast<LPARAM>(&find));
          if (find.found) control_hwnd = find.found;
        }
        // The parent itself (no control per se): window only, blank control.
        if (control_hwnd == parent) return;
        // ClassNN with the exact numbering controls() emits: same case-
        // insensitive per-class count across the descendant walk, stopping at
        // the target (AHK ControlGetClassNN counts the same way but is case-
        // sensitive; we stay consistent with our own WinGetControls).
        struct ClassNNState {
          std::vector<std::pair<std::wstring, int>> counts;
          HWND target{nullptr};
          std::wstring class_name;
          int instance{0};
        } state{{}, control_hwnd, {}, 0};
        EnumChildWindows(
            parent,
            [](HWND control, LPARAM parameter) -> BOOL {
              auto* cn = reinterpret_cast<ClassNNState*>(parameter);
              wchar_t class_name[256] = {};
              const int length = GetClassNameW(control, class_name, 256);
              if (length <= 0) return TRUE;  // AHK skips unnameable controls
              const std::wstring key(class_name, static_cast<std::size_t>(length));
              int* count = nullptr;
              for (auto& [known, occurrences] : cn->counts) {
                if (CompareStringOrdinal(known.c_str(), -1, key.c_str(), -1, TRUE) ==
                    CSTR_EQUAL) {
                  count = &occurrences;
                  break;
                }
              }
              if (count == nullptr) {
                cn->counts.emplace_back(key, 1);
                count = &cn->counts.back().second;
              } else {
                ++*count;
              }
              if (*count > 99999) return TRUE;  // AHK's numbering cap
              if (control == cn->target) {
                cn->instance = *count;
                cn->class_name = key;
                return FALSE;  // target numbered; stop the walk
              }
              return TRUE;
            },
            reinterpret_cast<LPARAM>(&state));
        if (state.instance == 0) return;  // vanished or unnameable: window only
        ControlInfo control_info;
        control_info.id = impl_->registry.id_for(control_hwnd);
        control_info.class_name = to_utf8(state.class_name);
        control_info.class_nn = control_info.class_name + std::to_string(state.instance);
        info.control = std::move(control_info);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = std::move(info);
  return rime::core::Error::none();
}

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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        RECT work{};
        if (!work_area_for(window, work)) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "cannot read the monitor work area"};
          return;
        }
        Rect target{};
        if (!resolve_placement(work, placement_text, target)) {
          result = {rime::core::Error::Code::InvalidContract,
                    "unknown window placement: " + placement_text};
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        if (!SetForegroundWindow(window)) {
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
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
  if (!window) return {lane::Error::Code::TargetGone, "window no longer exists"};
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
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
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!process) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "OpenProcess failed (win32 error " + std::to_string(failure) + ")"};
          return;
        }
        const BOOL terminated = TerminateProcess(process, 0);
        const DWORD failure = terminated ? 0u : GetLastError();
        CloseHandle(process);
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
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

rime::core::Error WindowService::set_title(const std::uint64_t id, const std::string& title,
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        // WinSetTitle: SetWindowText fails for windows that refuse the
        // change; that is a hard error, matching AHK's FR_E_WIN32.
        if (!SetWindowTextW(window, from_utf8(title).c_str())) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetWindowText failed (win32 error " + std::to_string(failure) + ")"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::set_enabled(const std::uint64_t id, const int value,
                                             const std::chrono::milliseconds timeout) {
  if (value < -1 || value > 1) {
    return {rime::core::Error::Code::InvalidContract, "enabled value must be -1, 0 or 1"};
  }
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        // WinSetEnabled: -1 toggles the current state; EnableWindow's
        // return value is unreliable, so verify through IsWindowEnabled.
        const BOOL want = value == -1 ? (IsWindowEnabled(window) ? 0 : 1) : value;
        EnableWindow(window, want);
        if ((IsWindowEnabled(window) ? 1 : 0) != (want ? 1 : 0)) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "EnableWindow did not take effect"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::set_always_on_top(const std::uint64_t id, const int value,
                                                   const std::chrono::milliseconds timeout) {
  if (value < -1 || value > 1) {
    return {rime::core::Error::Code::InvalidContract, "always-on-top value must be -1, 0 or 1"};
  }
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        // WinSetAlwaysOnTop: SetWindowPos with the topmost handle; -1
        // resolves against the current WS_EX_TOPMOST bit (SetWindowLong
        // does not take on some windows, so the z-order call is required).
        // Windows silently ignores the z-order change unless the calling
        // process holds SetForegroundWindow permission (MSDN SetWindowPos),
        // so the result is read back and, when it did not take, the
        // foreground is acquired (bare Alt tap first, like AHK's
        // WinActivate) and restored before one final attempt.
        const bool topmost =
            value == -1 ? (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0
                        : value != 0;
        const auto apply = [&]() {
          if (!SetWindowPos(window, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
            return false;
          }
          return ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) ==
                 topmost;
        };
        bool took = apply();
        if (!took) {
          const HWND previous_foreground = GetForegroundWindow();
          if (!SetForegroundWindow(window)) {
            INPUT tap[2] = {};
            tap[0].type = INPUT_KEYBOARD;
            tap[0].ki.wVk = VK_MENU;
            tap[1].type = INPUT_KEYBOARD;
            tap[1].ki.wVk = VK_MENU;
            tap[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, tap, sizeof(INPUT));
            SetForegroundWindow(window);
          }
          took = apply();
          if (previous_foreground && previous_foreground != window) {
            SetForegroundWindow(previous_foreground);
          }
        }
        if (!took) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "topmost state did not take (SetForegroundWindow permission denied)"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::set_style_bits(const std::uint64_t id,
                                                const std::string_view value, const int index,
                                                const std::chrono::milliseconds timeout) {
  StyleChange change;
  if (!parse_style_change(value, change)) {
    return {rime::core::Error::Code::InvalidContract,
            "style value must be '+N', '-N', '^N' or a plain decimal/0x-hex number"};
  }
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        // AHK WinSetStyle: work in unsigned 32-bit (sign-extension of
        // WS_POPUP-style bits would make same-value comparisons lie), treat
        // "no change needed" as success, then SetWindowLong with the MSDN
        // precise error check plus a read-back (AHK: even a partial change
        // counts as a success).
        const auto original = static_cast<std::uint32_t>(GetWindowLongPtrW(window, index));
        std::uint32_t updated = original;
        switch (change.op) {
          case StyleChangeOp::Add:
            updated = original | change.mask;
            break;
          case StyleChangeOp::Remove:
            updated = original & ~change.mask;
            break;
          case StyleChangeOp::Toggle:
            updated = original ^ change.mask;
            break;
          case StyleChangeOp::Replace:
            updated = change.mask;
            break;
        }
        if (updated == original) return;
        SetLastError(ERROR_SUCCESS);
        SetWindowLongPtrW(window, index, static_cast<LONG_PTR>(updated));
        if (GetLastError() != ERROR_SUCCESS ||
            static_cast<std::uint32_t>(GetWindowLongPtrW(window, index)) == original) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetWindowLong did not take effect"};
          return;
        }
        // AHK pairs the style change with a frame refresh; without
        // SWP_FRAMECHANGED only parts of the border repaint.
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(window, nullptr, TRUE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::set_style(const std::uint64_t id, const std::string_view value,
                                            const std::chrono::milliseconds timeout) {
  return set_style_bits(id, value, GWL_STYLE, timeout);
}

rime::core::Error WindowService::set_ex_style(const std::uint64_t id,
                                              const std::string_view value,
                                              const std::chrono::milliseconds timeout) {
  return set_style_bits(id, value, GWL_EXSTYLE, timeout);
}

rime::core::Error WindowService::set_transparent(const std::uint64_t id, const int value,
                                                  const std::chrono::milliseconds timeout) {
  if (value < -1 || value > 255) {
    return {rime::core::Error::Code::InvalidContract, "transparent value must be -1 or 0..255"};
  }
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (value == -1) {
          // AHK WinSetTrans with no flags: drop WS_EX_LAYERED; the OS
          // forgets the alpha and the color key along with it.
          SetLastError(ERROR_SUCCESS);
          SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style & ~WS_EX_LAYERED);
          if (GetLastError() != ERROR_SUCCESS) {
            result = {rime::core::Error::Code::ExecutionFailed,
                      "SetWindowLong failed to clear WS_EX_LAYERED"};
          }
          return;
        }
        SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
        if (!SetLayeredWindowAttributes(window, 0, static_cast<BYTE>(value), LWA_ALPHA)) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetLayeredWindowAttributes failed (win32 error " +
                        std::to_string(failure) + ")"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::set_trans_color(const std::uint64_t id,
                                                  const std::string_view value,
                                                  const std::chrono::milliseconds timeout) {
  TransColorChange change;
  if (!parse_trans_color_change(value, change)) {
    return {rime::core::Error::Code::InvalidContract,
            "trans-color value must be 'off', '', 'RRGGBB'/'0xRRGGBB' "
            "and an optional 0..255 alpha suffix"};
  }
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (change.off) {
          // Same clear path as WinSetTransparent("Off"): no flags left, so
          // AHK drops WS_EX_LAYERED instead of leaving it set with no key.
          SetLastError(ERROR_SUCCESS);
          SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style & ~WS_EX_LAYERED);
          if (GetLastError() != ERROR_SUCCESS) {
            result = {rime::core::Error::Code::ExecutionFailed,
                      "SetWindowLong failed to clear WS_EX_LAYERED"};
          }
          return;
        }
        SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
        // 0xRRGGBB (our wire order) to Win32's 0x00BBGGRR color key.
        const COLORREF color =
            static_cast<COLORREF>(((change.rgb >> 16) & 0xFF) | (change.rgb & 0xFF00) |
                                  ((change.rgb & 0xFF) << 16));
        const DWORD flags =
            (change.color_key ? LWA_COLORKEY : 0) | (change.with_alpha ? LWA_ALPHA : 0);
        if (!SetLayeredWindowAttributes(window, color, static_cast<BYTE>(change.alpha), flags)) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetLayeredWindowAttributes failed (win32 error " +
                        std::to_string(failure) + ")"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::set_region(const std::uint64_t id, const std::string_view value,
                                             const std::chrono::milliseconds timeout) {
  RegionSpec spec;
  if (!parse_region_options(value, spec)) {
    return {rime::core::Error::Code::InvalidContract,
            "region value must be '<x>-<y>' coordinate pairs with optional E, "
            "R[<rrw>-<rrh>], W[<width>]/Wind and H[<height>] options"};
  }
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        if (spec.kind == RegionKind::Restore) {
          // AHK: setting the region to NULL restores the window's proper
          // region (GetWindowRect-based hacks leave maximized windows
          // clipped, per the AHK v1.0.31.07 note).
          if (!SetWindowRgn(window, nullptr, TRUE)) {
            const DWORD failure = GetLastError();
            result = {rime::core::Error::Code::ExecutionFailed,
                      "SetWindowRgn failed to clear the region (win32 error " +
                          std::to_string(failure) + ")"};
          }
          return;
        }
        // Width and height are relative sizes; AHK converts them to the
        // right and bottom edges by adding the anchor point.
        const std::int64_t right =
            static_cast<std::int64_t>(spec.coords[0]) + spec.width;
        const std::int64_t bottom =
            static_cast<std::int64_t>(spec.coords[1]) + spec.height;
        HRGN region = nullptr;
        switch (spec.kind) {
          case RegionKind::Ellipse:
            region = CreateEllipticRgn(spec.coords[0], spec.coords[1],
                                       static_cast<int>(right), static_cast<int>(bottom));
            break;
          case RegionKind::RoundRect:
            region = CreateRoundRectRgn(spec.coords[0], spec.coords[1],
                                        static_cast<int>(right), static_cast<int>(bottom),
                                        spec.round_width, spec.round_height);
            break;
          case RegionKind::Rect:
            region = CreateRectRgn(spec.coords[0], spec.coords[1], static_cast<int>(right),
                                   static_cast<int>(bottom));
            break;
          case RegionKind::Polygon: {
            std::vector<POINT> points(spec.coords.size() / 2);
            for (std::size_t index = 0; index < points.size(); ++index) {
              points[index].x = spec.coords[index * 2];
              points[index].y = spec.coords[index * 2 + 1];
            }
            region = CreatePolygonRgn(points.data(), static_cast<int>(points.size()),
                                      spec.winding ? WINDING : ALTERNATE);
            break;
          }
          case RegionKind::Restore:
            return;  // handled above; keeps the switch exhaustive
        }
        if (!region) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "region creation failed (win32 error " + std::to_string(failure) + ")"};
          return;
        }
        // On success the OS owns the HRGN and frees the previous region;
        // on failure we still own it and must release it (AHK same).
        if (!SetWindowRgn(window, region, TRUE)) {
          DeleteObject(region);
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetWindowRgn failed (win32 error " + std::to_string(failure) + ")"};
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
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
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
        RECT work{};
        if (!work_area_for(nullptr, work)) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "cannot read the monitor work area"};
          return;
        }
        if (!resolve_placement(work, placement_text, resolved)) {
          result = {rime::core::Error::Code::InvalidContract,
                    "unknown window placement: " + placement_text};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = resolved;
  return rime::core::Error::none();
}

}  // namespace rime::win32
