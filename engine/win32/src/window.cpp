#include "rime/win32/window.hpp"

#include "rime/core/lane.hpp"

#include "utf.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <limits>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

namespace lane = rime::core;

std::wstring window_text(HWND window) {
  const int length = GetWindowTextLengthW(window);
  if (length <= 0) return {};
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(window, text.data(), length + 1);
  if (copied <= 0) return {};
  text.resize(static_cast<std::size_t>(copied));
  return text;
}

Rect to_rect(const RECT& rectangle) {
  return {rectangle.left, rectangle.top, rectangle.right, rectangle.bottom};
}

bool work_area_for(HWND window, RECT& work) {
  const HMONITOR monitor =
      MonitorFromWindow(window != nullptr ? window : GetDesktopWindow(),
                        MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return false;
  work = info.rcWork;
  return true;
}

bool resolve_placement(const RECT& work, const std::string_view placement, Rect& out) {
  const Rect area = to_rect(work);
  const long half_width = area.width() / 2;
  const long half_height = area.height() / 2;
  if (placement == "left") {
    out = {area.left, area.top, area.left + half_width, area.bottom};
  } else if (placement == "right") {
    out = {area.left + half_width, area.top, area.right, area.bottom};
  } else if (placement == "top") {
    out = {area.left, area.top, area.right, area.top + half_height};
  } else if (placement == "bottom") {
    out = {area.left, area.top + half_height, area.right, area.bottom};
  } else if (placement == "full") {
    out = area;
  } else {
    return false;
  }
  return true;
}

// Case-insensitive ASCII folding for ahk_exe basename compares. The rule is
// ASCII-only by design: bytes >= 0x80 compare unchanged, so non-ASCII image
// names fall back to exact-byte equality after folding. This documents the
// matching behavior; it does not normalize Unicode case.
[[nodiscard]] std::string fold_ascii(std::string_view text) {
  std::string folded(text);
  for (char& character : folded) {
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  }
  return folded;
}

// File-local RAII for process handles (mirrors process.cpp HandleGuard style;
// kept local so window.cpp owns its lifetime explicitly, no cross-file reuse).
struct ProcessHandleGuard {
  explicit ProcessHandleGuard(HANDLE raw) : handle(raw) {}
  ProcessHandleGuard(const ProcessHandleGuard&) = delete;
  ProcessHandleGuard& operator=(const ProcessHandleGuard&) = delete;
  ~ProcessHandleGuard() {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
  [[nodiscard]] HANDLE get() const { return handle; }
  HANDLE handle = nullptr;
};

// Image identity behind `pid`: one OpenProcess + QueryFullProcessImageNameW
// yields both the full path (WinGetProcessPath) and the final path segment
// (processName / ahk_exe matching). Both parts are empty when the process
// cannot be opened (protected/system processes stay readable as windows).
struct ProcessImage {
  std::wstring path;     // full image path, empty when unreadable
  std::wstring basename;  // final path segment (extension kept)
};

[[nodiscard]] ProcessImage process_image(const DWORD process_id) {
  ProcessImage image;
  if (process_id == 0) return image;
  ProcessHandleGuard process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
  if (!process.get()) return image;
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &size) || size == 0) {
    return image;
  }
  path.resize(size);
  const std::size_t slash = path.find_last_of(L'\\');
  image.basename = slash == std::wstring::npos ? path : path.substr(slash + 1);
  image.path = std::move(path);
  return image;
}

// Per-enumeration pid -> image cache. A single query() enum touches every
// top-level window; without this each window pays OpenProcess plus a 64KB path
// buffer even when dozens share one pid. The snapshot reads processName and
// processPath per window, so the cache only dedupes repeated pids; callers
// without a shared cache (single-window probes and reads) pass nullptr and go
// straight to process_image.
using PidImageCache = std::unordered_map<std::uint32_t, ProcessImage>;

[[nodiscard]] ProcessImage process_image_for(const DWORD process_id, PidImageCache* cache) {
  if (process_id == 0) return {};
  if (cache == nullptr) return process_image(process_id);
  const auto found = cache->find(process_id);
  if (found != cache->end()) return found->second;
  return cache->emplace(process_id, process_image(process_id)).first->second;
}

// UTF-16 class name for direct ordinal compares; callers convert once via
// to_utf8 instead of round-tripping through UTF-8.
[[nodiscard]] std::wstring window_class_name_w(HWND window) {
  wchar_t class_name[256] = {};
  const int copied = GetClassNameW(window, class_name, 256);
  if (copied <= 0) return {};
  return std::wstring(class_name, static_cast<std::size_t>(copied));
}

// Process-wide generation handed out once per registry, so ids issued by one
// WindowService can never resolve inside a newer one: an id is
// [generation:32][sequence:32], generation only grows, and lookups reject
// foreign generations before scanning. That turns a stale id held across a
// service restart into target_gone instead of a different window with the
// same sequence number.
std::atomic<std::uint32_t> next_window_generation{1};

// UI-thread-only mapping from stable ids to live HWNDs. Stale entries are
// dropped on lookup; ids are never recycled inside one service (the
// sequence is masked to its 32 bits, which cannot wrap in a service's
// lifetime).
class WindowRegistry final {
 public:
  std::uint64_t id_for(HWND window) {
    prune();
    for (const auto& [id, hwnd] : entries_) {
      if (hwnd == window) return id;
    }
    const std::uint64_t id = (static_cast<std::uint64_t>(generation_) << 32) |
                             (++next_id_ & 0xFFFFFFFFull);
    entries_.emplace_back(id, window);
    return id;
  }

  HWND hwnd_for(const std::uint64_t id) {
    if (static_cast<std::uint32_t>(id >> 32) != generation_) return nullptr;
    prune();
    for (const auto& [entry_id, hwnd] : entries_) {
      if (entry_id == id) return hwnd;
    }
    return nullptr;
  }

 private:
  void prune() {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (!IsWindow(it->second)) {
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::vector<std::pair<std::uint64_t, HWND>> entries_;
  const std::uint32_t generation_{next_window_generation.fetch_add(1)};
  std::uint64_t next_id_{0};
};

lane::Error build_info_impl(WindowRegistry& registry, HWND window, WindowInfo& out,
                            PidImageCache* names) {
  if (!window || !IsWindow(window)) {
    return {lane::Error::Code::TargetGone, "window no longer exists"};
  }
  out = WindowInfo{};
  out.id = registry.id_for(window);
  out.title = to_utf8(window_text(window));
  out.class_name = to_utf8(window_class_name_w(window));
  RECT rectangle{};
  if (GetWindowRect(window, &rectangle)) out.rect = to_rect(rectangle);
  out.visible = IsWindowVisible(window) != FALSE;
  out.minimized = IsIconic(window) != FALSE;
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  out.process_id = process_id;
  const ProcessImage image = process_image_for(process_id, names);
  out.process_name = to_utf8(image.basename);
  out.process_path = to_utf8(image.path);

  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  const bool maximized =
      GetWindowPlacement(window, &placement) && placement.showCmd == SW_SHOWMAXIMIZED;
  if (out.minimized) {
    out.state = "minimized";
  } else if (!out.visible) {
    out.state = "hidden";
  } else if (maximized) {
    out.state = "maximized";
  } else {
    out.state = "normal";
  }
  out.min_max = IsZoomed(window) ? 1 : (IsIconic(window) ? -1 : 0);
  // Styles are read as unsigned 32-bit (AHK WinGetStyle returns UINT): bits
  // like WS_POPUP (0x80000000) must not come out negative.
  out.style = static_cast<std::uint32_t>(GetWindowLongPtrW(window, GWL_STYLE));
  out.ex_style = static_cast<std::uint32_t>(GetWindowLongPtrW(window, GWL_EXSTYLE));
  out.enabled = IsWindowEnabled(window) != FALSE;
  out.always_on_top = (out.ex_style & WS_EX_TOPMOST) != 0;
  RECT client{};
  if (GetClientRect(window, &client)) {
    POINT top_left{client.left, client.top};
    POINT bottom_right{client.right, client.bottom};
    if (ClientToScreen(window, &top_left) && ClientToScreen(window, &bottom_right)) {
      out.client_rect = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    }
  }
  // Layered attributes are opt-in per window; failures stay at the "none"
  // defaults (-1 / ""), matching AHK's empty return for WinGetTransparent
  // and WinGetTransColor.
  BYTE alpha = 0;
  DWORD flags = 0;
  COLORREF color_key = 0;
  if (GetLayeredWindowAttributes(window, &color_key, &alpha, &flags)) {
    if ((flags & LWA_ALPHA) != 0) out.transparent = static_cast<int>(alpha);
    if ((flags & LWA_COLORKEY) != 0) {
      // COLORREF stores 0x00BBGGRR; AHK prints WinGetTransColor as "0xRRGGBB".
      char hex[16];
      std::snprintf(hex, sizeof(hex), "0x%06X",
                    static_cast<unsigned int>((GetRValue(color_key) << 16) |
                                              (GetGValue(color_key) << 8) |
                                              GetBValue(color_key)));
      out.trans_color = hex;
    }
  }
  return lane::Error::none();
}

lane::Error build_info(WindowRegistry& registry, HWND window, WindowInfo& out) {
  return build_info_impl(registry, window, out, nullptr);
}

// Compiles one AHK-style regex pattern: an optional run of option letters
// followed by ')' (AHK's `i)`/`m)`/`s)` prefix) selects case/line/dot
// semantics, everything else is the pattern body. AHK's regex default is
// case-sensitive, which std::wregex already gives us. Only `i)` maps onto a
// std::wregex flag (icase); `m)` and `s)` would need multiline/dotall, which
// MSVC's std::regex does not provide, so they fail loudly (Unsupported)
// instead of silently changing match semantics. An unknown option letter or
// an invalid pattern fails with a typed error, never silently ignored.
lane::Error compile_window_regex(const std::string& utf8, std::optional<std::wregex>& out) {
  out.reset();
  if (utf8.empty()) {
    return {lane::Error::Code::InvalidContract, "regex pattern must not be empty"};
  }
  const std::wstring pattern = from_utf8(utf8);
  if (pattern.empty()) {
    return {lane::Error::Code::InvalidContract, "regex pattern is not valid UTF-8"};
  }
  std::wstring body = pattern;
  std::regex_constants::syntax_option_type flags = std::regex_constants::ECMAScript;
  std::size_t index = 0;
  while (index < body.size() && iswalpha(body[index])) ++index;
  if (index > 0 && index < body.size() && body[index] == L')') {
    for (std::size_t option = 0; option < index; ++option) {
      switch (body[option]) {
        case L'i':
          flags |= std::regex_constants::icase;
          break;
        case L'm':
          return {lane::Error::Code::Unsupported,
                  "regex option m) (multiline) is not supported by the runtime regex engine"};
        case L's':
          return {lane::Error::Code::Unsupported,
                  "regex option s) (dotall) is not supported by the runtime regex engine"};
        default:
          return {lane::Error::Code::Unsupported,
                  "unsupported regex option: " + to_utf8(body.substr(option, 1))};
      }
    }
    body = body.substr(index + 1);
  }
  if (body.empty()) {
    return {lane::Error::Code::InvalidContract, "regex pattern must not be empty"};
  }
  try {
    out.emplace(body, flags);
  } catch (const std::regex_error&) {
    return {lane::Error::Code::InvalidContract, "invalid regular expression pattern"};
  }
  return lane::Error::none();
}

// A query with the global defaults resolved and any regex compiled once for
// the whole request (never per window). Built on the caller thread before
// the UI hop: std::wregex touches no HWND, and an invalid pattern then
// fails the service call without queueing UI work.
struct ResolvedQuery {
  const WindowQuery* query{nullptr};
  TitleMatchMode mode{TitleMatchMode::Contains};
  bool include_hidden{false};
  std::optional<std::wregex> title_regex;
  std::optional<std::wregex> class_regex;
  std::optional<std::wregex> exe_regex;
};

lane::Error resolve_query(const WindowQuery& query, const WindowSettings& settings,
                          ResolvedQuery& out) {
  out = ResolvedQuery{};
  out.query = &query;
  out.mode = query.title_match_mode.value_or(settings.title_match_mode);
  out.include_hidden = query.include_hidden.value_or(settings.detect_hidden_windows);
  if (out.mode != TitleMatchMode::Regex) return lane::Error::none();
  if (!query.title.empty()) {
    if (const auto error = compile_window_regex(query.title, out.title_regex); !error.ok()) {
      return error;
    }
  }
  if (!query.class_name.empty()) {
    if (const auto error = compile_window_regex(query.class_name, out.class_regex); !error.ok()) {
      return error;
    }
  }
  if (!query.process_name.empty()) {
    if (const auto error = compile_window_regex(query.process_name, out.exe_regex); !error.ok()) {
      return error;
    }
  }
  return lane::Error::none();
}

bool has_selectors(const WindowQuery& query) {
  return query.active || !query.title.empty() || !query.class_name.empty() ||
         !query.process_name.empty() || query.id != 0;
}

// WinTitle-style matching evaluated on the UI lane only. `cache` dedupes
// pid -> basename lookups within one enumeration; pass nullptr for single
// foreground lookups. Title matching is case-sensitive in every mode
// (AHK WinTitle rule; only RegEx `i)` opts back in), class stays
// case-insensitive (AHK v2.1) and exe keeps the ASCII-fold rule.
bool matches_query(WindowRegistry& registry, HWND window, const ResolvedQuery& resolved,
                   PidImageCache* cache) {
  const WindowQuery& query = *resolved.query;
  if (!resolved.include_hidden && !IsWindowVisible(window)) return false;
  if (query.id != 0 && registry.id_for(window) != query.id) return false;
  if (!query.title.empty()) {
    const std::wstring text = window_text(window);
    const std::wstring needle = from_utf8(query.title);
    // from_utf8 returns empty on invalid UTF-8; an empty needle would match
    // everything via FindStringOrdinal, so fail closed instead.
    if (needle.empty()) return false;
    constexpr std::size_t kIntMax = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    bool matched = false;
    switch (resolved.mode) {
      case TitleMatchMode::Regex:
        matched = resolved.title_regex.has_value() &&
                  std::regex_search(text, *resolved.title_regex);
        break;
      case TitleMatchMode::Exact:
        matched = text.size() <= kIntMax && needle.size() <= kIntMax &&
                  CompareStringOrdinal(text.c_str(), -1, needle.c_str(), -1, FALSE) ==
                      CSTR_EQUAL;
        break;
      case TitleMatchMode::StartsWith:
        matched = text.size() <= kIntMax && needle.size() <= kIntMax &&
                  FindStringOrdinal(FIND_FROMSTART, text.c_str(), static_cast<int>(text.size()),
                                    needle.c_str(), static_cast<int>(needle.size()),
                                    FALSE) == 0;
        break;
      case TitleMatchMode::Contains:
        matched = text.size() <= kIntMax && needle.size() <= kIntMax &&
                  FindStringOrdinal(FIND_FROMSTART, text.c_str(), static_cast<int>(text.size()),
                                    needle.c_str(), static_cast<int>(needle.size()),
                                    FALSE) >= 0;
        break;
    }
    if (!matched) return false;
  }
  if (!query.class_name.empty()) {
    const std::wstring actual = window_class_name_w(window);
    if (resolved.mode == TitleMatchMode::Regex) {
      // RegEx mode reinterprets ahk_class as a pattern (AHK WinTitle rule).
      if (!resolved.class_regex || actual.empty() ||
          !std::regex_search(actual, *resolved.class_regex)) {
        return false;
      }
    } else {
      // Same ordinal rule as before: case-insensitive CompareStringOrdinal
      // on UTF-16, no ASCII-fold round-trip through UTF-8.
      const std::wstring expected = from_utf8(query.class_name);
      if (expected.empty() || actual.empty()) return false;
      if (CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL) {
        return false;
      }
    }
  }
  if (!query.process_name.empty()) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    const std::string actual = to_utf8(process_image_for(process_id, cache).basename);
    if (resolved.mode == TitleMatchMode::Regex) {
      // RegEx mode also reinterprets ahk_exe; the pattern runs against the
      // image basename (the same string the non-regex rule compares).
      if (!resolved.exe_regex || !std::regex_search(from_utf8(actual), *resolved.exe_regex)) {
        return false;
      }
    } else if (fold_ascii(actual) != fold_ascii(query.process_name)) {
      return false;
    }
  }
  // A selector-less query keeps the list() convention of skipping untitled
  // windows so an empty query never dumps the whole desktop enum.
  if (!has_selectors(query) && window_text(window).empty()) return false;
  return true;
}

struct EnumContext {
  WindowRegistry* registry;
  const ResolvedQuery* resolved;   // null for the list() baseline enum
  bool include_hidden;             // list() baseline only (DetectHiddenWindows)
  bool selectors;
  std::vector<WindowInfo>* windows;
  PidImageCache* images;  // per-query pid cache; null on the list() baseline
};

BOOL CALLBACK collect_matching(HWND window, LPARAM parameter) {
  auto* context = reinterpret_cast<EnumContext*>(parameter);
  // EnumWindows contract: parameter always carries the query context above.
  assert(context != nullptr);
  assert(context->registry != nullptr);
  assert(context->resolved != nullptr);
  assert(context->windows != nullptr);
  assert(context->images != nullptr);
  if (!matches_query(*context->registry, window, *context->resolved, context->images)) {
    return TRUE;
  }
  WindowInfo info;
  if (!build_info_impl(*context->registry, window, info, context->images).ok()) return TRUE;
  context->windows->push_back(std::move(info));
  return TRUE;
}

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

std::optional<TitleMatchMode> parse_title_match_mode(const std::string_view text) {
  if (text == "1") return TitleMatchMode::StartsWith;
  if (text == "2") return TitleMatchMode::Contains;
  if (text == "3") return TitleMatchMode::Exact;
  // AHK's ConvertTitleMatchMode compares case-insensitively; "regex" is the
  // lowercase spelling the wire/query side also uses.
  if (text.size() == 5) {
    std::string lowered(text);
    for (char& character : lowered) {
      if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    if (lowered == "regex") return TitleMatchMode::Regex;
  }
  return std::nullopt;
}

std::string title_match_mode_text(const TitleMatchMode mode) {
  switch (mode) {
    case TitleMatchMode::StartsWith:
      return "1";
    case TitleMatchMode::Contains:
      return "2";
    case TitleMatchMode::Exact:
      return "3";
    case TitleMatchMode::Regex:
      return "RegEx";
  }
  return "2";
}

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

rime::core::Error validate_window_regex(const std::string& utf8_pattern) {
  std::optional<std::wregex> compiled;
  return compile_window_regex(utf8_pattern, compiled);
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

rime::core::Error WindowService::move_rect(const std::uint64_t id, const Rect& rect,
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
        if (!SetWindowPos(window, nullptr, rect.left, rect.top, rect.width(), rect.height(),
                          SWP_NOZORDER | SWP_NOACTIVATE)) {
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
