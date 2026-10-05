#include "rime/win32/window.hpp"

#include "rime/core/lane.hpp"

#include "window_match.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// AHK WinGroup: named lists of query specs plus the shared visited-window
// cycle behind GroupAdd / GroupActivate / GroupDeactivate / GroupClose. The
// state itself is WindowGroups (window_match.hpp), owned by WindowService's
// Impl and reachable through WindowService::group_state; every function here
// runs on the UI lane through ui.call, so the state needs no lock.

namespace rime::win32 {

using detail::PidImageCache;
using detail::ResolvedQuery;
using detail::WindowRegistry;
using detail::expired_deadline;
using detail::matches_query;
using detail::resolve_query;

namespace {

namespace lane = rime::core;

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
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        std::vector<WindowQuery>& specs = group_state().groups[name];
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
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        // AHK GroupActivate creates the group when it does not exist yet;
        // an empty group simply resolves nullopt.
        auto group = group_state().groups.try_emplace(name).first;
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
            foreground != nullptr && group_is_member(registry(), foreground, specs, &cache);
        if (!group_is_active) group_state().visited.clear();
        // Default cycle starts at the oldest (bottom) member; `reverse`
        // starts at the most recent (top) one - unless the group already
        // owns the foreground, where AHK keeps walking oldest-first.
        const bool find_last = !reverse || group_is_active;
        for (bool retry_needed = !group_state().visited.empty();; retry_needed = false) {
          if (group_is_active) mark_visited(group_state().visited, foreground);
          std::vector<HWND> members;
          MemberEnumContext context{&registry(), &specs, &cache, &members};
          EnumWindows(collect_members, reinterpret_cast<LPARAM>(&context));
          std::vector<HWND> candidates;
          candidates.reserve(members.size());
          for (const HWND member : members) {
            if (std::find(group_state().visited.begin(), group_state().visited.end(), member) ==
                group_state().visited.end()) {
              candidates.push_back(member);
            }
          }
          if (!candidates.empty()) {
            const HWND target = find_last ? candidates.back() : candidates.front();
            const std::uint64_t target_id = registry().id_for(target);
            mark_visited(group_state().visited, target);
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
          group_state().visited.clear();
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
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        // Unlike GroupActivate this requires a group that already exists
        // (AHK's argument error when FindGroup fails).
        const auto group = group_state().groups.find(name);
        if (group == group_state().groups.end()) {
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
            foreground != nullptr && group_is_member(registry(), foreground, specs, &cache);
        if (was_member) group_state().visited.clear();
        for (int attempt = 0; attempt < 2; ++attempt) {
          const bool find_last = !reverse || !group_state().visited.empty();
          DeactivateEnumContext context{&registry(), &specs, &cache, &group_state().visited,
                                         find_last};
          EnumWindows(collect_deactivate_target, reinterpret_cast<LPARAM>(&context));
          if (context.eligible != nullptr) {
            HWND activated = GetLastActivePopup(context.eligible);
            if (activated == nullptr) activated = context.eligible;
            const std::uint64_t activated_id = registry().id_for(activated);
            mark_visited(group_state().visited, context.eligible);
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
          if (group_state().visited.empty()) break;
          const bool wrap_around = group_state().visited.size() > 1;
          group_state().visited.clear();
          if (!wrap_around) break;
          if (foreground != nullptr) mark_visited(group_state().visited, foreground);
        }
        // Nothing left to review: fall back to the taskbar (AHK's last
        // resort). A locked-down session may refuse focus; that error
        // surfaces to the caller.
        const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (taskbar == nullptr) return;
        const std::uint64_t taskbar_id = registry().id_for(taskbar);
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
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const auto group = group_state().groups.find(name);
        if (group == group_state().groups.end()) {
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
          MemberEnumContext context{&registry(), &specs, nullptr, &members};
          EnumWindows(collect_members, reinterpret_cast<LPARAM>(&context));
          for (const HWND member : members) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
              result = expired_deadline();
              return;
            }
            const std::uint64_t member_id = registry().id_for(member);
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
        if (foreground != nullptr && group_is_member(registry(), foreground, specs, &cache)) {
          const auto now = std::chrono::steady_clock::now();
          if (now >= deadline) {
            result = expired_deadline();
            return;
          }
          const std::uint64_t foreground_id = registry().id_for(foreground);
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
              group_is_member(registry(), next_foreground, specs, &cache)) {
            const std::uint64_t next_id = registry().id_for(next_foreground);
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

}  // namespace rime::win32
