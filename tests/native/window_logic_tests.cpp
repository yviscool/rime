// Realism: L2 - pure window matching, selector parsing and geometry
// resolution logic; no HWND is touched and every expectation is a literal
// in the test.

#include "rime/win32/window.hpp"

#include "window_geometry.hpp"
#include "window_match.hpp"

#include <cassert>
#include <optional>
#include <regex>
#include <string>

namespace {

using rime::core::Error;
using rime::win32::Rect;
using rime::win32::RegionKind;
using rime::win32::RegionSpec;
using rime::win32::StyleChange;
using rime::win32::StyleChangeOp;
using rime::win32::TitleMatchMode;
using rime::win32::TransColorChange;
using rime::win32::parse_region_options;
using rime::win32::parse_style_change;
using rime::win32::parse_title_match_mode;
using rime::win32::parse_trans_color_change;
using rime::win32::title_match_mode_text;
using rime::win32::validate_window_regex;
using rime::win32::detail::compile_window_regex;
using rime::win32::detail::resolve_placement;

constexpr Error::Code kInvalidContract = Error::Code::InvalidContract;
constexpr Error::Code kUnsupported = Error::Code::Unsupported;

// A work area with a real taskbar: 1920x1040 below a 80px reserved top edge.
void test_resolve_placement() {
  const RECT work{0, 0, 1920, 1040};
  Rect out{};

  assert(resolve_placement(work, "left", out));
  assert((out == Rect{0, 0, 960, 1040}));
  assert(resolve_placement(work, "right", out));
  assert((out == Rect{960, 0, 1920, 1040}));
  assert(resolve_placement(work, "top", out));
  assert((out == Rect{0, 0, 1920, 520}));
  assert(resolve_placement(work, "bottom", out));
  assert((out == Rect{0, 520, 1920, 1040}));
  assert(resolve_placement(work, "full", out));
  assert((out == Rect{0, 0, 1920, 1040}));
  assert(!resolve_placement(work, "diagonal", out));
  assert(!resolve_placement(work, "", out));

  // Odd sizes: both halves truncate toward zero (plain long division).
  const RECT odd{0, 0, 1921, 1041};
  assert(resolve_placement(odd, "left", out));
  assert((out == Rect{0, 0, 960, 1041}));
  assert(resolve_placement(odd, "right", out));
  assert((out == Rect{960, 0, 1921, 1041}));
  assert(resolve_placement(odd, "top", out));
  assert((out == Rect{0, 0, 1921, 520}));
  assert(resolve_placement(odd, "bottom", out));
  assert((out == Rect{0, 520, 1921, 1041}));
  assert(resolve_placement(odd, "full", out));
  assert((out == Rect{0, 0, 1921, 1041}));

  // Non-zero origin (secondary monitor work area).
  const RECT shifted{100, 50, 2020, 1090};
  assert(resolve_placement(shifted, "left", out));
  assert((out == Rect{100, 50, 1060, 1090}));
  assert(resolve_placement(shifted, "right", out));
  assert((out == Rect{1060, 50, 2020, 1090}));
  assert(resolve_placement(shifted, "top", out));
  assert((out == Rect{100, 50, 2020, 570}));
}

void test_title_match_mode() {
  assert(parse_title_match_mode("1") == TitleMatchMode::StartsWith);
  assert(parse_title_match_mode("2") == TitleMatchMode::Contains);
  assert(parse_title_match_mode("3") == TitleMatchMode::Exact);
  // "regex" folds ASCII case, but only at exactly five letters.
  assert(parse_title_match_mode("regex") == TitleMatchMode::Regex);
  assert(parse_title_match_mode("ReGeX") == TitleMatchMode::Regex);
  assert(parse_title_match_mode("REGEX") == TitleMatchMode::Regex);
  assert(!parse_title_match_mode("regexx").has_value());
  assert(!parse_title_match_mode("RegEx ").has_value());
  assert(!parse_title_match_mode("4").has_value());
  assert(!parse_title_match_mode("").has_value());

  assert(title_match_mode_text(TitleMatchMode::StartsWith) == "1");
  assert(title_match_mode_text(TitleMatchMode::Contains) == "2");
  assert(title_match_mode_text(TitleMatchMode::Exact) == "3");
  assert(title_match_mode_text(TitleMatchMode::Regex) == "RegEx");

  const TitleMatchMode modes[] = {TitleMatchMode::StartsWith, TitleMatchMode::Contains,
                                  TitleMatchMode::Exact, TitleMatchMode::Regex};
  for (const TitleMatchMode mode : modes) {
    assert(parse_title_match_mode(title_match_mode_text(mode)) == mode);
  }
}

void test_style_change() {
  StyleChange change{};

  assert(parse_style_change("+0x100", change));
  assert(change.op == StyleChangeOp::Add);
  assert(change.mask == 0x100u);

  assert(parse_style_change("-5", change));
  assert(change.op == StyleChangeOp::Remove);
  assert(change.mask == 5u);

  assert(parse_style_change("^0x2", change));
  assert(change.op == StyleChangeOp::Toggle);
  assert(change.mask == 2u);

  assert(parse_style_change("0x80000000", change));
  assert(change.op == StyleChangeOp::Replace);
  assert(change.mask == 0x80000000u);

  assert(parse_style_change("0X100", change));
  assert(change.op == StyleChangeOp::Replace);
  assert(change.mask == 0x100u);

  assert(parse_style_change("2048", change));
  assert(change.op == StyleChangeOp::Replace);
  assert(change.mask == 2048u);

  assert(!parse_style_change("", change));
  assert(!parse_style_change("+", change));
  assert(!parse_style_change("0x", change));
  assert(!parse_style_change("0x100000000", change));
  assert(!parse_style_change("abc", change));
  assert(!parse_style_change("12z", change));
}

void test_trans_color_change() {
  TransColorChange change{};

  assert(parse_trans_color_change("", change));
  assert(change.off);
  assert(!change.color_key);

  assert(parse_trans_color_change("off", change));
  assert(change.off);
  assert(parse_trans_color_change("OFF", change));
  assert(change.off);

  assert(parse_trans_color_change("FF0000", change));
  assert(!change.off);
  assert(change.color_key);
  assert(change.rgb == 0xFF0000u);
  assert(!change.with_alpha);

  assert(parse_trans_color_change("0x1E90FF", change));
  assert(change.color_key);
  assert(change.rgb == 0x1E90FFu);

  assert(parse_trans_color_change("FF0000 128", change));
  assert(change.color_key);
  assert(change.rgb == 0xFF0000u);
  assert(change.with_alpha);
  assert(change.alpha == 128);

  // A leading space drops the color key and keeps the layered alpha.
  assert(parse_trans_color_change(" 100", change));
  assert(!change.off);
  assert(!change.color_key);
  assert(change.with_alpha);
  assert(change.alpha == 100);

  assert(!parse_trans_color_change("FF000", change));
  assert(!parse_trans_color_change("GGGGGG", change));
  assert(!parse_trans_color_change("0x", change));
  assert(!parse_trans_color_change("FF0000 300", change));
  assert(!parse_trans_color_change("FF0000 12x", change));
}

void test_region_options() {
  RegionSpec spec{};

  assert(parse_region_options("", spec));
  assert(spec.kind == RegionKind::Restore);
  assert(spec.coords.empty());

  assert(parse_region_options("   ", spec));
  assert(spec.kind == RegionKind::Restore);

  assert(parse_region_options("0-0 W100 H50 E", spec));
  assert(spec.kind == RegionKind::Ellipse);
  assert(spec.coords.size() == 2u);
  assert(spec.coords[0] == 0 && spec.coords[1] == 0);
  assert(spec.width == 100 && spec.height == 50);

  assert(parse_region_options("10-20 W100 H50 R", spec));
  assert(spec.kind == RegionKind::RoundRect);
  assert(spec.round_width == 30 && spec.round_height == 30);

  assert(parse_region_options("0-0 W100 H50 R10-20", spec));
  assert(spec.kind == RegionKind::RoundRect);
  assert(spec.round_width == 10 && spec.round_height == 20);

  assert(parse_region_options("10-20 W100 H50", spec));
  assert(spec.kind == RegionKind::Rect);
  assert(spec.coords[0] == 10 && spec.coords[1] == 20);
  assert(spec.width == 100 && spec.height == 50);

  assert(parse_region_options("0-0 100-0 100-100", spec));
  assert(spec.kind == RegionKind::Polygon);
  assert(spec.coords.size() == 6u);

  assert(parse_region_options("0-0 100-0 100-100 Wind", spec));
  assert(spec.kind == RegionKind::Polygon);
  assert(spec.winding);

  // A lone width never turns points into a rectangle (AHK polygon rule).
  assert(parse_region_options("0-0 100-0 100-100 W50", spec));
  assert(spec.kind == RegionKind::Polygon);

  assert(parse_region_options("-10--20 30-40 50-60", spec));
  assert(spec.kind == RegionKind::Polygon);
  assert(spec.coords[0] == -10 && spec.coords[1] == -20);

  assert(!parse_region_options("E", spec));
  assert(!parse_region_options("0-0 E", spec));
  assert(!parse_region_options("0-0 R", spec));
  assert(!parse_region_options("0-0 W100 H50 X", spec));
  assert(!parse_region_options("0-0 100-100", spec));
  assert(!parse_region_options("0-0 W", spec));
  assert(!parse_region_options("-", spec));
}

void test_validate_window_regex() {
  assert(validate_window_regex("a.*").ok());
  assert(validate_window_regex("i)abc").ok());
  assert(validate_window_regex("").code == kInvalidContract);
  assert(validate_window_regex("i)").code == kInvalidContract);
  assert(validate_window_regex("[a").code == kInvalidContract);
  assert(validate_window_regex("m)...").code == kUnsupported);
  assert(validate_window_regex("s)abc").code == kUnsupported);
  assert(validate_window_regex("x)abc").code == kUnsupported);
}

void test_compile_window_regex() {
  std::optional<std::wregex> out;

  assert(compile_window_regex("a.*", out).ok());
  assert(out.has_value());
  // AHK's default is case-sensitive; only an i) prefix opts into icase.
  assert(!std::regex_search(std::wstring(L"ABC"), *out));
  assert(std::regex_search(std::wstring(L"a-b"), *out));

  assert(compile_window_regex("i)abc", out).ok());
  assert(out.has_value());
  assert(std::regex_search(std::wstring(L"ABC"), *out));
  assert(!std::regex_search(std::wstring(L"ABD"), *out));

  assert(compile_window_regex("", out).code == kInvalidContract);
  assert(compile_window_regex("[a", out).code == kInvalidContract);
  assert(compile_window_regex("m)...", out).code == kUnsupported);
  assert(compile_window_regex("s)abc", out).code == kUnsupported);
}

}  // namespace

int main() {
  test_resolve_placement();
  test_title_match_mode();
  test_style_change();
  test_trans_color_change();
  test_region_options();
  test_validate_window_regex();
  test_compile_window_regex();
  return 0;
}
