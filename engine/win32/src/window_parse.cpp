#include "rime/win32/window.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rime::win32 {
namespace {
// Strict ASCII digit parser: the whole text must be decimal and fit the
// limit (AHK's istrtoi64 consumes the entire string or fails).
bool parse_decimal(const std::string_view text, const std::uint32_t limit,
                   std::uint32_t& out) {
  if (text.empty() || text.size() > 10) return false;
  std::uint32_t parsed = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') return false;
    parsed = parsed * 10 + static_cast<std::uint32_t>(character - '0');
    if (parsed > limit) return false;
  }
  out = parsed;
  return true;
}

bool equals_ignore_case(const std::string_view text, const std::string_view other) {
  if (text.size() != other.size()) return false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    char left = text[index];
    char right = other[index];
    if (left >= 'A' && left <= 'Z') left = static_cast<char>(left - 'A' + 'a');
    if (right >= 'A' && right <= 'Z') right = static_cast<char>(right - 'A' + 'a');
    if (left != right) return false;
  }
  return true;
}

int hex_digit(const char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

// Leading signed decimal with at least one digit (AHK's ATOI prefix
// convention, tightened: an empty number is an argument error). Consumes
// the number from `text`; overflow beyond 32 bits fails like an argument
// error rather than wrapping.
bool parse_region_int(std::string_view& text, std::int32_t& out) {
  if (text.empty()) return false;
  const bool negative = text.front() == '-';
  const bool positive = text.front() == '+';
  if (negative || positive) text.remove_prefix(1);
  if (text.empty() || text.front() < '0' || text.front() > '9') return false;
  std::int64_t parsed = 0;
  while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
    parsed = parsed * 10 + (text.front() - '0');
    if (parsed > 2147483648LL) return false;
    text.remove_prefix(1);
  }
  out = negative ? -static_cast<std::int32_t>(parsed) : static_cast<std::int32_t>(parsed);
  return true;
}

char ascii_upper(const char character) {
  if (character >= 'a' && character <= 'z') return static_cast<char>(character - 'a' + 'A');
  return character;
}
}  // namespace

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

bool parse_style_change(std::string_view text, StyleChange& out) {
  out = StyleChange{};
  if (text.empty()) return false;
  switch (text.front()) {
    case '+':
      out.op = StyleChangeOp::Add;
      text.remove_prefix(1);
      break;
    case '-':
      out.op = StyleChangeOp::Remove;
      text.remove_prefix(1);
      break;
    case '^':
      out.op = StyleChangeOp::Toggle;
      text.remove_prefix(1);
      break;
    default:
      break;
  }
  if (text.empty()) return false;
  unsigned int base = 10;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    text.remove_prefix(2);
    if (text.empty()) return false;
  }
  std::uint64_t parsed = 0;
  for (const char character : text) {
    const int digit = hex_digit(character);
    if (digit < 0 || static_cast<unsigned int>(digit) >= base) return false;
    parsed = parsed * base + static_cast<std::uint64_t>(digit);
    if (parsed > 0xFFFFFFFFull) return false;
  }
  out.mask = static_cast<std::uint32_t>(parsed);
  return true;
}

bool parse_trans_color_change(const std::string_view text, TransColorChange& out) {
  out = TransColorChange{};
  // AHK compares the whole value against "Off" before splitting; '' clears
  // everything for us too (WinSetTrans with no flags drops WS_EX_LAYERED).
  if (text.empty() || equals_ignore_case(text, "off")) {
    out.off = true;
    return true;
  }
  std::string_view color_part = text;
  std::string_view alpha_part;
  if (const std::size_t split = text.find_first_of(" \t"); split != std::string_view::npos) {
    color_part = text.substr(0, split);
    const std::string_view tail = text.substr(split + 1);
    if (const std::size_t start = tail.find_first_not_of(" \t"); start != std::string_view::npos) {
      alpha_part = tail.substr(start);
    }
  }
  std::uint32_t alpha = 0;
  if (!alpha_part.empty() && !parse_decimal(alpha_part, 255, alpha)) return false;
  out.with_alpha = !alpha_part.empty();
  out.alpha = static_cast<int>(alpha);
  if (color_part.empty()) {
    // A leading space omits the color key (AHK); with no alpha left this is
    // a pure clear, otherwise layered alpha only.
    if (!out.with_alpha) out.off = true;
    return true;
  }
  std::string_view digits = color_part;
  if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
    digits.remove_prefix(2);
  }
  if (digits.size() != 6) return false;
  std::uint32_t rgb = 0;
  for (const char character : digits) {
    const int digit = hex_digit(character);
    if (digit < 0) return false;
    rgb = (rgb << 4) | static_cast<std::uint32_t>(digit);
  }
  out.color_key = true;
  out.rgb = rgb;
  return true;
}

bool parse_region_options(const std::string_view text, RegionSpec& out) {
  out = RegionSpec{};
  std::string_view cursor = text;
  const auto skip_spaces = [&] {
    while (!cursor.empty() && (cursor.front() == ' ' || cursor.front() == '\t')) {
      cursor.remove_prefix(1);
    }
  };
  skip_spaces();
  if (cursor.empty()) {
    // AHK: a blank option string restores the window's normal region.
    out.kind = RegionKind::Restore;
    return true;
  }
  bool has_width = false;
  bool has_height = false;
  bool has_round = false;
  bool ellipse = false;
  while (true) {
    skip_spaces();
    if (cursor.empty()) break;
    const std::size_t token_length = cursor.find_first_of(" \t");
    const std::string_view token = cursor.substr(0, token_length);
    cursor.remove_prefix(token_length == std::string_view::npos ? cursor.size() : token_length);
    const char first = token.front();
    if ((first >= '0' && first <= '9') || first == '-' || first == '+') {
      // A coordinate pair '<x>-<y>'; the first pair anchors rect, ellipse
      // and rounded shapes, extra pairs are polygon vertices.
      std::string_view part = token;
      std::int32_t x = 0;
      std::int32_t y = 0;
      if (!parse_region_int(part, x) || part.empty() || part.front() != '-') return false;
      part.remove_prefix(1);
      if (!parse_region_int(part, y) || !part.empty()) return false;
      // AHK caps regions at MAX_REGION_POINTS (2000) coordinate pairs.
      if (out.coords.size() >= 4000) return false;
      out.coords.push_back(x);
      out.coords.push_back(y);
      continue;
    }
    if (first < 'A' || (first > 'Z' && first < 'a') || first > 'z') return false;
    std::string_view rest = token.substr(1);
    switch (ascii_upper(first)) {
      case 'E':
        if (!rest.empty()) return false;
        ellipse = true;
        break;
      case 'R':
        if (rest.empty()) {
          has_round = true;  // AHK default corner size 30x30
          break;
        }
        if (!parse_region_int(rest, out.round_width) || rest.empty() || rest.front() != '-') {
          return false;
        }
        rest.remove_prefix(1);
        if (!parse_region_int(rest, out.round_height) || !rest.empty()) return false;
        has_round = true;
        break;
      case 'W':
        if (equals_ignore_case(rest, "ind")) {
          out.winding = true;
          break;
        }
        if (!parse_region_int(rest, out.width) || !rest.empty()) return false;
        has_width = true;
        break;
      case 'H':
        if (!parse_region_int(rest, out.height) || !rest.empty()) return false;
        has_height = true;
        break;
      default:
        return false;  // unknown letters reserve future options, like AHK
    }
  }
  // AHK: at least one coordinate pair is required even for shaped regions.
  if (out.coords.empty()) return false;
  const bool both_dimensions = has_width && has_height;
  // Width and height are relative sizes: AHK converts them to the right
  // and bottom edges by adding the anchor point.
  const std::int64_t right = static_cast<std::int64_t>(out.coords[0]) + out.width;
  const std::int64_t bottom = static_cast<std::int64_t>(out.coords[1]) + out.height;
  if (ellipse) {
    // AHK lets a shape without both dimensions fail at CreateEllipticRgn
    // (FR_E_WIN32); we refuse it at the contract layer instead.
    if (!both_dimensions) return false;
    if (right < -2147483648LL || right > 2147483647LL) return false;
    if (bottom < -2147483648LL || bottom > 2147483647LL) return false;
    out.kind = RegionKind::Ellipse;
  } else if (has_round) {
    if (!both_dimensions) return false;
    if (right < -2147483648LL || right > 2147483647LL) return false;
    if (bottom < -2147483648LL || bottom > 2147483647LL) return false;
    out.kind = RegionKind::RoundRect;
  } else if (both_dimensions) {
    if (right < -2147483648LL || right > 2147483647LL) return false;
    if (bottom < -2147483648LL || bottom > 2147483647LL) return false;
    out.kind = RegionKind::Rect;
  } else {
    // A width or height alone is ignored and the points become a polygon
    // (AHK); fewer than three points cannot form one, so refuse before the
    // Win32 call.
    if (out.coords.size() < 6) return false;
    out.kind = RegionKind::Polygon;
  }
  return true;
}

}  // namespace rime::win32
