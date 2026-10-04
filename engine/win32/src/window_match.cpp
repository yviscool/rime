#include "window_match.hpp"

#include "rime/core/lane.hpp"

#include "utf.hpp"
#include "window_geometry.hpp"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <utility>

namespace rime::win32::detail {
namespace {

namespace lane = rime::core;

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
// kept local so window_match.cpp owns its lifetime explicitly, no cross-file reuse).
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
  // Region bounding box (extension beyond AHK, which has no getter):
  // GetWindowRgn copies the region into our probe; ERROR or an empty box
  // means the window has no region. Non-rectangular regions report the
  // bounding box from GetRgnBox.
  if (HRGN probe = CreateRectRgn(0, 0, 0, 0); probe) {
    const int region_state = GetWindowRgn(window, probe);
    RECT box{};
    if (region_state != ERROR && GetRgnBox(probe, &box) != ERROR &&
        (box.left != box.right || box.top != box.bottom)) {
      char region_text[64];
      std::snprintf(region_text, sizeof(region_text), "%ld,%ld,%ld,%ld", box.left, box.top,
                    box.right, box.bottom);
      out.region = region_text;
    }
    DeleteObject(probe);
  }
  return lane::Error::none();
}

}  // namespace

std::wstring window_text(HWND window) {
  const int length = GetWindowTextLengthW(window);
  if (length <= 0) return {};
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(window, text.data(), length + 1);
  if (copied <= 0) return {};
  text.resize(static_cast<std::size_t>(copied));
  return text;
}

lane::Error build_info(WindowRegistry& registry, HWND window, WindowInfo& out) {
  return build_info_impl(registry, window, out, nullptr);
}

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

}  // namespace rime::win32::detail

namespace rime::win32 {

rime::core::Error validate_window_regex(const std::string& utf8_pattern) {
  std::optional<std::wregex> compiled;
  return detail::compile_window_regex(utf8_pattern, compiled);
}

}  // namespace rime::win32
