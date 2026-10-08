// gui_window.cpp - M6 batch 2: the Gui/GuiControl object family.
//
// gui.cpp keeps batch 1 (MsgBox/InputBox/ToolTip/Tray); this TU owns every
// script-created Gui window on the runtime's single UI pump. The split is
// ownership, not threading: everything below runs either on the worker
// (the parse_* statics are pure string -> struct and touch no OS state) or
// inside GuiService::run_pump's UiThread::call (all HWND work). Semantics
// mirror AutoHotkey v2's script_gui.cpp with citations; deviations are
// called out where they exist (gui-menu.md §4 is the delivery contract).
//
// Threading invariants (AGENTS):
//  - GuiRecord, GuiChildRecord and every HWND they name are touched only on
//    the pump; the free helpers below take GuiMap + the event sink rather
//    than GuiService::Impl (whose type is private to GuiService members).
//  - Nothing crosses threads but stable ids, plain-value specs and JSON
//    payloads; the event pipe is a one-way push into the host queue
//    (EventSink), which is safe to call from the pump.
//  - No raw HANDLE/HWND ever reaches JS (gui-menu.md §0.4).

#include "gui_impl.hpp"

#include "utf.hpp"

#include <windows.h>

#include <commctrl.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace rime::win32 {

// Window procedure (namespace-scope free function; forward-declared before
// any helper that takes its address). It only ever touches GuiRecord via
// GWLP_USERDATA.
LRESULT CALLBACK gui_wnd_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);

namespace {

using Error = rime::core::Error;
using Code = rime::core::Error::Code;
using Json = rime::core::json::Value;
using EventSinkFn = std::function<void(std::uint64_t, std::string)>;

// ---- constants -----------------------------------------------------------

constexpr wchar_t kGuiWindowClass[] = L"RimeGuiClass";

// AHK's default Gui window style (script_gui.h:519): sysmenu + minimize box
// but no maximize button, no size box, and no WS_VISIBLE (Show adds it).
constexpr DWORD kDefaultGuiStyle =
    WS_POPUP | WS_CLIPSIBLINGS | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr DWORD kDefaultGuiExStyle = 0;

// The option bits parse_gui_options may ever touch (used as the delta mask).
constexpr DWORD kGuiOptionStyleMask =
    WS_SIZEBOX | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_CAPTION | WS_BORDER | WS_SYSMENU;
constexpr DWORD kGuiOptionExMask = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;

// AHK MAX_CONTROLS_PER_GUI (defines.h:709) is 11 * 1000, deliberately below
// 0xFFFF so a control's id fits a 16-bit HMENU. We keep a per-window
// 1-based id in the HMENU slot (GuiChildRecord::hmenu_id), which never wraps.
constexpr int kMaxControlsPerGui = 11000;

// GUI_CTL_VERTICAL_DEADSPACE = DPIScale(8) (script_gui.h:371); batch 2 does
// no DPI rescale, so the 96-DPI pixel value stands.
constexpr int kCtlVerticalDeadspace = 8;

constexpr const char* kDestroyed = "gui window is destroyed";
constexpr const char* kNoControl = "control does not exist";
constexpr const char* kGuiCreateFailed = "gui window could not be created";
constexpr const char* kCtrlCreateFailed = "control could not be created";

bool is_space_tab(char c) { return c == ' ' || c == '\t'; }

std::string lower_ascii(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

// ---- class registration (process lifetime, once) -------------------------

void ensure_gui_class() {
  static std::once_flag once;
  std::call_once(once, [] {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpszClassName = kGuiWindowClass;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = &gui_wnd_proc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.style = CS_DBLCLKS;  // script_gui.cpp:2457: accepted good default
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);  // already-registered is fine (process lifetime)
  });
}

// ---- event payloads ------------------------------------------------------

Json target_gui() { return Json::string("gui"); }

Json target_ctrl(std::uint64_t ctrl_id) { return Json::number(static_cast<double>(ctrl_id)); }

Json make_args() { return Json::array(); }

void push_arg(Json& args, double value) { args.push(Json::number(value)); }

// Builds {gui, target, event, args} and pushes it through the record's sink.
void push_event(GuiRecord& rec, Json target, const char* event_name, Json args) {
  if (rec.channel == 0 || !rec.event_sink) return;  // no host queue attached
  Json payload = Json::object();
  payload.set("gui", Json::number(static_cast<double>(rec.id_hint)));
  payload.set("target", std::move(target));
  payload.set("event", Json::string(event_name));
  payload.set("args", std::move(args));
  rec.event_sink(rec.channel, rime::core::json::stringify(payload));
}

// ---- record lifecycle (pump-only) ---------------------------------------

Error destroyed_error() { return {Code::InvalidState, kDestroyed}; }

// Find an existing record or materialize one from the carried spec. Dead
// records stay in the map (destroyed ids must not resurrect a window), so
// lookups distinguish "never created" (create) from "destroyed"
// (InvalidState). The record takes a copy of the current event sink.
Error ensure_live_record(GuiMap& guis, const EventSinkFn& sink,
                         const GuiService::GuiSpec& spec, GuiRecord*& out) {
  auto it = guis.find(spec.id);
  GuiRecord* rec = nullptr;
  if (it == guis.end()) {
    ensure_gui_class();
    auto entry = guis.emplace(spec.id, std::make_unique<GuiRecord>());
    rec = entry.first->second.get();
    rec->id_hint = spec.id;
    rec->channel = spec.channel;
    rec->event_sink = sink;
    rec->interest = spec.interest;  // handlers registered before the window
    GetObjectW(static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)), sizeof(LOGFONTW),
               &rec->font_log);
    rec->font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    rec->owns_font = false;  // stock object: never DeleteObject'd
    const std::wstring title = from_utf8(spec.title);
    rec->hwnd = CreateWindowExW(spec.ex_style, kGuiWindowClass, title.c_str(), spec.style,
                                CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, nullptr, nullptr,
                                GetModuleHandleW(nullptr), rec);
    if (!rec->hwnd) {
      guis.erase(spec.id);
      return {Code::InvalidState, kGuiCreateFailed};
    }
    SetWindowLongPtrW(rec->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(rec));
  } else {
    rec = it->second.get();
  }
  if (rec->dead) return destroyed_error();
  if (rec->channel == 0 && spec.channel != 0) rec->channel = spec.channel;
  if (!rec->event_sink && sink) rec->event_sink = sink;
  out = rec;
  return Error::none();
}

// Frees everything a record owns except the record itself (fonts, brushes,
// picture handles, child state). Idempotent; used by WM_DESTROY and
// stop_pump_sweep.
void free_record_state(GuiRecord& rec) {
  if (rec.owns_font && rec.font) DeleteObject(rec.font);
  rec.font = nullptr;
  rec.owns_font = false;
  if (rec.back_brush) DeleteObject(rec.back_brush);
  rec.back_brush = nullptr;
  rec.has_back_color = false;
  for (auto& [id, child] : rec.children) {
    if (child.owns_font && child.font) DeleteObject(child.font);
    child.font = nullptr;
    if (child.picture_bitmap) {
      DeleteObject(child.picture_bitmap);
      child.picture_bitmap = nullptr;
    }
    if (child.picture_icon) {
      DestroyIcon(child.picture_icon);
      child.picture_icon = nullptr;
    }
  }
  rec.children.clear();
  rec.by_hwnd.clear();
  rec.by_hmenu.clear();
}

Error find_ctrl(GuiMap& guis, const EventSinkFn& sink, const GuiService::GuiSpec& spec,
                std::uint64_t ctrl_id, GuiRecord*& rec_out, GuiChildRecord*& child_out) {
  GuiRecord* rec = nullptr;
  const Error err = ensure_live_record(guis, sink, spec, rec);
  if (!err.ok()) return err;
  auto it = rec->children.find(ctrl_id);
  if (it == rec->children.end()) return {Code::InvalidContract, kNoControl};
  rec_out = rec;
  child_out = &it->second;
  return Error::none();
}

// ---- layout --------------------------------------------------------------

void ensure_margins(GuiRecord& rec) {
  // script_gui.cpp:11179-11182: marginX = MulDiv(lfHeight, -90, 96),
  // marginY = MulDiv(lfHeight, -54, 96). lfHeight is negative for typical
  // fonts, so both come out positive. Each axis is resolved on its own: the
  // script may have written only one of them through gui_set_margins, whose
  // negative argument means "keep the other axis".
  const int default_x = MulDiv(rec.font_log.lfHeight, -90, 96);
  const int default_y = MulDiv(rec.font_log.lfHeight, -54, 96);
  if (rec.margin_x < 0) rec.margin_x = default_x > 0 ? default_x : 1;
  if (rec.margin_y < 0) rec.margin_y = default_y > 0 ? default_y : 1;
}

// First-control baseline (script_gui.cpp:2884): mPrevX = mMarginX with
// mPrevY/mPrevHeight at 0, so a first control without x/y lands on the
// margins through the same flow formula as every later control.
void prime_first_control_baseline(GuiRecord& rec) {
  if (rec.have_prev) return;
  ensure_margins(rec);
  rec.prev_x = rec.margin_x;
  rec.prev_y = 0;
  rec.prev_w = 0;
  rec.prev_h = 0;
}

// GUI_STANDARD_WIDTH = 15 * MulDiv(lfHeight, -72, 96) (script_gui.h:366-367).
int gui_standard_width(const GuiRecord& rec) {
  return 15 * MulDiv(rec.font_log.lfHeight, -72, 96);
}

// PROGRESS_DEFAULT_THICKNESS = MulDiv(lfHeight, -2 * 72, 96) (script_gui.h:372).
int progress_default_thickness(const GuiRecord& rec) {
  return MulDiv(rec.font_log.lfHeight, -2 * 72, 96);
}

// Auto-size extents over visible children (script_gui.cpp:7424-7438 body).
// Returns the raw max edges; the caller adds the trailing margin.
void content_extent(const GuiRecord& rec, int& right, int& bottom) {
  right = 0;
  bottom = 0;
  if (!rec.hwnd) return;
  for (const auto& entry : rec.children) {
    const GuiChildRecord& child = entry.second;
    if (!child.hwnd || !IsWindow(child.hwnd)) continue;
    if (!(GetWindowLongPtrW(child.hwnd, GWL_STYLE) & WS_VISIBLE)) continue;
    RECT rc{};
    GetWindowRect(child.hwnd, &rc);
    MapWindowPoints(nullptr, rec.hwnd, reinterpret_cast<POINT*>(&rc), 2);
    right = (std::max)(right, static_cast<int>(rc.right));
    bottom = (std::max)(bottom, static_cast<int>(rc.bottom));
  }
}

struct FontScope {
  HDC hdc{nullptr};
  HFONT previous{nullptr};
  TEXTMETRICW tm{};

  explicit FontScope(const GuiRecord& rec) : owner(rec.hwnd) {
    hdc = GetDC(owner);
    if (hdc) {
      previous = static_cast<HFONT>(SelectObject(hdc, rec.font));
      GetTextMetricsW(hdc, &tm);
    }
  }
  ~FontScope() {
    if (hdc) {
      SelectObject(hdc, previous);
      ReleaseDC(owner, hdc);
    }
  }
  FontScope(const FontScope&) = delete;
  FontScope& operator=(const FontScope&) = delete;

 private:
  HWND owner{nullptr};
};

// Merges an AHK SetFont spec onto a base LOGFONT (script_gui.cpp:8172-8240:
// face override, s<n> size, bold/italic/underline/strike/norm). Batch 2 does
// no DPI rescale (gui-menu.md §4.8), so 96 DPI is the point->pixel factor.
LOGFONTW merge_font_spec(const LOGFONTW& base, const GuiService::GuiFontSpec& spec) {
  LOGFONTW lf = base;
  if (!spec.name.empty()) {
    const std::wstring face = from_utf8(spec.name);
    wcsncpy_s(lf.lfFaceName, face.c_str(), _TRUNCATE);
  }
  if (spec.size_pt > 0) lf.lfHeight = -MulDiv(spec.size_pt, 96, 72);
  lf.lfWeight = spec.bold ? FW_BOLD : FW_NORMAL;
  lf.lfItalic = spec.italic ? TRUE : FALSE;
  lf.lfUnderline = spec.underline ? TRUE : FALSE;
  lf.lfStrikeOut = spec.strike ? TRUE : FALSE;
  return lf;
}

// ---- control sizing ------------------------------------------------------

// Resolves the flow position (script_gui.cpp:3229-3243 for the default flow,
// 6497-6500 for x-without-y, 6551-6554 for y-without-x) for the next control.
void resolve_position(const GuiRecord& rec, GuiService::GuiControlKind kind,
                      const GuiService::GuiControlOptions& opts, int& x, int& y, int& width,
                      int& height) {
  if (opts.pos.width) width = *opts.pos.width;
  if (opts.pos.height) height = *opts.pos.height;
  const bool has_x = opts.pos.x.has_value();
  const bool has_y = opts.pos.y.has_value();
  if (has_x && has_y) {
    x = *opts.pos.x;
    y = *opts.pos.y;
    return;
  }
  if (!has_x && !has_y) {
    x = rec.prev_x;
    y = rec.prev_y + rec.prev_h + rec.margin_y;
    if (kind == GuiService::GuiControlKind::Text && rec.prev_was_text && rec.have_prev) {
      y += kCtlVerticalDeadspace;  // text-after-text spacing (3238-3243)
    }
    return;
  }
  if (has_x) {
    x = *opts.pos.x;
    y = x != rec.prev_x ? rec.prev_y : rec.prev_y + rec.prev_h + rec.margin_y;
    return;
  }
  y = *opts.pos.y;
  // Source quirk: this fallback adds mMarginY, not mMarginX (script_gui.cpp:6554).
  x = y != rec.prev_y ? rec.prev_x : rec.prev_x + rec.prev_w + rec.margin_y;
}

// Default styles per kind (script_gui.cpp:2897-3170): WS_CHILD + WS_VISIBLE
// are forced for all, per-type bits as in the source switch. Radio groups:
// the first radio of a run gets WS_GROUP, and the next non-radio control
// closes an open group with WS_GROUP (2911-2919).
DWORD control_style_for(GuiService::GuiControlKind kind, const GuiRecord& rec,
                        const GuiService::GuiControlSpec& spec, bool explicit_size) {
  DWORD style = WS_CHILD | WS_VISIBLE;
  const bool newline_text = spec.text.find('\n') != std::string::npos;
  const bool multiline_hint = explicit_size || newline_text;  // script_gui.cpp:3095-3101
  switch (kind) {
    case GuiService::GuiControlKind::Button:
      style |= WS_TABSTOP | BS_PUSHBUTTON;
      if (multiline_hint) style |= BS_MULTILINE;
      break;
    case GuiService::GuiControlKind::CheckBox:
      style |= WS_TABSTOP | BS_AUTOCHECKBOX;
      if (multiline_hint) style |= BS_MULTILINE;
      break;
    case GuiService::GuiControlKind::Radio:
      style |= BS_AUTORADIOBUTTON;
      if (!rec.in_radio_group) style |= WS_GROUP | WS_TABSTOP;
      if (multiline_hint) style |= BS_MULTILINE;
      break;
    case GuiService::GuiControlKind::GroupBox:
      style |= BS_GROUPBOX;
      if (rec.in_radio_group) style |= WS_GROUP;
      break;
    case GuiService::GuiControlKind::Edit:
      style |= WS_TABSTOP;
      if (newline_text) style |= ES_MULTILINE | WS_VSCROLL | ES_WANTRETURN | ES_AUTOVSCROLL;
      else style |= ES_AUTOHSCROLL;
      break;
    case GuiService::GuiControlKind::Picture:
      style |= SS_NOTIFY;  // Click works without a later style retrofit
      break;
    case GuiService::GuiControlKind::Progress:
    case GuiService::GuiControlKind::Text:
      break;
  }
  return style;
}

// Content-driven sizing (script_gui.cpp:3461-3777); x/y are already final.
void resolve_size(GuiRecord& rec, const GuiService::GuiControlSpec& spec, int& width,
                  int& height) {
  const auto kind = spec.kind;
  const bool has_w = width >= 0;
  const bool has_h = height >= 0;
  FontScope font(rec);
  if (!font.hdc) {
    if (!has_w) width = gui_standard_width(rec);
    if (!has_h) height = 30;
    return;
  }

  switch (kind) {
    case GuiService::GuiControlKind::Text:
    case GuiService::GuiControlKind::Button:
    case GuiService::GuiControlKind::CheckBox:
    case GuiService::GuiControlKind::Radio:
    case GuiService::GuiControlKind::Edit: {
      UINT format = DT_CALCRECT;
      int extra_w = 0;
      int extra_h = 0;
      if (kind == GuiService::GuiControlKind::Text) format |= DT_EXPANDTABS;
      if (kind == GuiService::GuiControlKind::Edit) {
        format |= DT_EXPANDTABS | DT_EDITCONTROL | DT_NOPREFIX;
        extra_w = 4 + font.tm.tmAveCharWidth;  // script_gui.cpp:3497
      } else if (kind == GuiService::GuiControlKind::CheckBox ||
                 kind == GuiService::GuiControlKind::Radio) {
        extra_w = GetSystemMetrics(SM_CXMENUCHECK) + font.tm.tmAveCharWidth +
                  2;  // script_gui.cpp:3534
      }
      const bool word_break =
          has_w && !(kind == GuiService::GuiControlKind::Button ||
                     kind == GuiService::GuiControlKind::CheckBox ||
                     kind == GuiService::GuiControlKind::Radio);
      if (word_break) format |= DT_WORDBREAK;
      RECT rc{0, 0, 0, 0};
      if (has_w) rc.right = (std::max)(1, width - extra_w);
      std::wstring text = from_utf8(spec.text);
      if (text.empty()) text = L"H";  // script_gui.cpp:3580: empty still measures one row
      DrawTextW(font.hdc, text.c_str(), -1, &rc, format);
      int draw_w = rc.right - rc.left;
      const int draw_h = rc.bottom - rc.top;
      if (!spec.text.empty() && kind != GuiService::GuiControlKind::CheckBox &&
          kind != GuiService::GuiControlKind::Radio) {
        // Negative right-side ABC overhang adjustment (script_gui.cpp:3597-3615).
        const wchar_t last = text.back();
        ABC abc{};
        if (GetCharABCWidthsW(font.hdc, last, last, &abc) && abc.abcC < 0) draw_w -= abc.abcC;
      }
      if (!has_h || (draw_h + extra_h > height && kind != GuiService::GuiControlKind::Edit)) {
        height = draw_h + extra_h;
        if (kind == GuiService::GuiControlKind::Edit) {
          height += kCtlVerticalDeadspace;  // script_gui.cpp:3634
        } else if (kind == GuiService::GuiControlKind::Button) {
          height += -rec.font_log.lfHeight - 1;  // script_gui.cpp:3639
        }
      }
      if (!has_w || draw_w > width) {
        width = draw_w + extra_w;
        if (kind == GuiService::GuiControlKind::Button) {
          width += 2 * GetSystemMetrics(SM_CXEDGE) + -rec.font_log.lfHeight;
        }
      }
      break;
    }
    case GuiService::GuiControlKind::GroupBox: {
      // row_count = 2 plus one for the title row, then the row-count height
      // formula (script_gui.cpp:3344-3346, 3408-3413) + margins (3434-3436).
      if (!has_h) {
        const int rows = 3;
        height = static_cast<int>(font.tm.tmHeight * rows +
                                  font.tm.tmExternalLeading * (static_cast<int>(rows + 0.5f) - 1) +
                                  0.5f);
        height += rec.margin_y * (2 + (static_cast<int>(rows + 0.5f) - 2));
      }
      if (!has_w) width = gui_standard_width(rec) + 2 * rec.margin_x;  // script_gui.cpp:3764
      break;
    }
    case GuiService::GuiControlKind::Progress: {
      if (!has_h) height = progress_default_thickness(rec);
      if (!has_w) width = gui_standard_width(rec);
      break;
    }
    case GuiService::GuiControlKind::Picture:
      // Sizes come from the loaded image when unspecified (script_gui.cpp:
      // 3881-3884: w/h of 0 = keep original size); the caller fills them in.
      break;
  }
  if (width < 0) width = gui_standard_width(rec);
  if (height < 0) height = 30;  // script_gui.cpp:3459-3460 fallback
}

// ---- picture loading -----------------------------------------------------

struct LoadedPicture {
  bool ok{false};
  bool is_icon{false};
  HBITMAP bitmap{nullptr};
  HICON icon{nullptr};
  int width{0};
  int height{0};
};

LoadedPicture load_picture(const std::string& path) {
  LoadedPicture out;
  const std::wstring wide = from_utf8(path);
  if (wide.empty()) return out;
  const bool wants_icon =
      wide.size() >= 4 && _wcsicmp(wide.c_str() + wide.size() - 4, L".ico") == 0;
  if (wants_icon) {
    auto* icon = reinterpret_cast<HICON>(
        LoadImageW(nullptr, wide.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
    if (!icon) return out;
    ICONINFO info{};
    if (GetIconInfo(icon, &info)) {
      BITMAP bm{};
      HBITMAP source = info.hbmColor ? info.hbmColor : info.hbmMask;
      if (source && GetObjectW(source, sizeof(bm), &bm)) {
        out.width = bm.bmWidth;
        out.height = bm.bmHeight;
      }
      if (info.hbmColor) DeleteObject(info.hbmColor);
      if (info.hbmMask) DeleteObject(info.hbmMask);
    }
    out.ok = true;
    out.is_icon = true;
    out.icon = icon;
    return out;
  }
  auto* bitmap = static_cast<HBITMAP>(
      LoadImageW(nullptr, wide.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
  if (!bitmap) return out;
  BITMAP bm{};
  if (GetObjectW(bitmap, sizeof(bm), &bm)) {
    out.width = bm.bmWidth;
    out.height = bm.bmHeight;
  }
  out.ok = true;
  out.bitmap = bitmap;
  return out;
}

// ---- control creation ----------------------------------------------------

Error add_control_pump(GuiMap& guis, const EventSinkFn& sink,
                       const GuiService::GuiControlSpec& spec) {
  GuiRecord* rec = nullptr;
  if (const Error err = ensure_live_record(guis, sink, spec.gui, rec); !err.ok()) return err;
  if (rec->children.size() >= static_cast<std::size_t>(kMaxControlsPerGui)) {
    return {Code::InvalidContract, "too many controls in one gui"};
  }
  ensure_margins(*rec);
  prime_first_control_baseline(*rec);

  LoadedPicture picture{};
  if (spec.kind == GuiService::GuiControlKind::Picture) {
    if (spec.text.empty()) return {Code::InvalidContract, "picture content path must not be empty"};
    picture = load_picture(spec.text);
    if (!picture.ok) return {Code::InvalidContract, "picture could not be loaded"};
  }

  int x = 0;
  int y = 0;
  int width = -1;
  int height = -1;
  resolve_position(*rec, spec.kind, spec.options, x, y, width, height);
  resolve_size(*rec, spec, width, height);
  if (spec.kind == GuiService::GuiControlKind::Picture && picture.ok) {
    if (spec.options.pos.width.value_or(0) <= 0 && picture.width > 0) width = picture.width;
    if (spec.options.pos.height.value_or(0) <= 0 && picture.height > 0) height = picture.height;
  }

  const bool explicit_size =
      spec.options.pos.width.has_value() || spec.options.pos.height.has_value();
  const DWORD style = control_style_for(spec.kind, *rec, spec, explicit_size);
  DWORD ex_style = 0;
  if (spec.kind == GuiService::GuiControlKind::Edit) ex_style = WS_EX_CLIENTEDGE;

  const std::uint16_t hmenu_id = rec->next_hmenu++;
  const std::wstring text = from_utf8(spec.text);
  const wchar_t* class_name = L"STATIC";
  switch (spec.kind) {
    case GuiService::GuiControlKind::Button:
    case GuiService::GuiControlKind::CheckBox:
    case GuiService::GuiControlKind::Radio:
    case GuiService::GuiControlKind::GroupBox:
      class_name = L"BUTTON";
      break;
    case GuiService::GuiControlKind::Edit:
      class_name = L"EDIT";
      break;
    case GuiService::GuiControlKind::Picture:
    case GuiService::GuiControlKind::Text:
      class_name = L"STATIC";
      break;
    case GuiService::GuiControlKind::Progress:
      class_name = PROGRESS_CLASSW;
      break;
  }

  HWND child = CreateWindowExW(ex_style, class_name, text.c_str(), style, x, y, width, height,
                               rec->hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(hmenu_id)),
                               GetModuleHandleW(nullptr), nullptr);
  if (!child) {
    if (picture.bitmap) DeleteObject(picture.bitmap);
    if (picture.icon) DestroyIcon(picture.icon);
    return {Code::InvalidState, kCtrlCreateFailed};
  }

  if (spec.kind == GuiService::GuiControlKind::Picture) {
    // SS_BITMAP/SS_ICON are applied after creation so the static does not try
    // to load a resource itself (script_gui.cpp:3896-3898 note).
    const LONG_PTR style_now = GetWindowLongPtrW(child, GWL_STYLE);
    SetWindowLongPtrW(child, GWL_STYLE, style_now | (picture.is_icon ? SS_ICON : SS_BITMAP));
    if (picture.icon) {
      SendMessageW(child, STM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(picture.icon));
    } else if (picture.bitmap) {
      SendMessageW(child, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(picture.bitmap));
    }
  } else if (spec.kind == GuiService::GuiControlKind::Progress) {
    SendMessageW(child, PBM_SETRANGE32, 0, 100);
    int value = 0;
    if (spec.has_number) {
      value = static_cast<int>(spec.number + 0.5);
      value = (std::max)(0, (std::min)(100, value));
    }
    SendMessageW(child, PBM_SETPOS, static_cast<WPARAM>(value), 0);
  }

  const bool parent_visible = IsWindowVisible(rec->hwnd);
  SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(rec->font),
               parent_visible ? TRUE : FALSE);
  if (spec.options.hidden.value_or(false)) ShowWindow(child, SW_HIDE);
  if (spec.options.disabled.value_or(false)) EnableWindow(child, FALSE);

  GuiChildRecord record;
  record.hwnd = child;
  record.kind = spec.kind;
  record.vname = spec.options.vname;
  record.hmenu_id = hmenu_id;
  record.hidden = spec.options.hidden.value_or(false);
  record.disabled = spec.options.disabled.value_or(false);
  record.font_log = rec->font_log;  // ctrl.SetFont merges onto the GUI font
  record.picture_bitmap = picture.bitmap;
  record.picture_icon = picture.icon;
  rec->by_hwnd.emplace(reinterpret_cast<std::uintptr_t>(child), spec.ctrl_id);
  rec->by_hmenu.emplace(hmenu_id, spec.ctrl_id);
  rec->children.emplace(spec.ctrl_id, record);

  rec->prev_x = x;
  rec->prev_y = y;
  rec->prev_w = width;
  rec->prev_h = height;
  rec->have_prev = true;
  rec->prev_was_text = spec.kind == GuiService::GuiControlKind::Text;
  rec->in_radio_group = spec.kind == GuiService::GuiControlKind::Radio;
  return Error::none();
}

// ---- value access --------------------------------------------------------

Error read_window_text(HWND hwnd, std::string& out) {
  const int len = GetWindowTextLengthW(hwnd);
  if (len <= 0) {
    out.clear();
    return Error::none();
  }
  std::wstring buffer(static_cast<std::size_t>(len) + 1, L'\0');
  GetWindowTextW(hwnd, buffer.data(), len + 1);
  buffer.resize(static_cast<std::size_t>(len));
  out = to_utf8(buffer);
  return Error::none();
}

Error read_value(const GuiChildRecord& child, Json& out) {
  switch (child.kind) {
    case GuiService::GuiControlKind::Edit: {
      std::string text;
      const Error err = read_window_text(child.hwnd, text);
      if (!err.ok()) return err;
      out = Json::string(std::move(text));
      return Error::none();
    }
    case GuiService::GuiControlKind::CheckBox:
    case GuiService::GuiControlKind::Radio:
      out = Json::number(SendMessageW(child.hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1.0 : 0.0);
      return Error::none();
    case GuiService::GuiControlKind::Progress:
      out = Json::number(
          static_cast<double>(static_cast<int>(SendMessageW(child.hwnd, PBM_GETPOS, 0, 0))));
      return Error::none();
    default:
      return {Code::InvalidContract, "this control type has no value"};
  }
}

Error write_value(GuiChildRecord& child, const Json& value) {
  switch (child.kind) {
    case GuiService::GuiControlKind::Edit: {
      if (!value.is_string()) return {Code::InvalidContract, "Edit value must be a string"};
      const std::wstring wide = from_utf8(value.as_string());
      SetWindowTextW(child.hwnd, wide.c_str());
      return Error::none();
    }
    case GuiService::GuiControlKind::CheckBox:
    case GuiService::GuiControlKind::Radio: {
      if (!value.is_number()) return {Code::InvalidContract, "CheckBox/Radio value must be 0 or 1"};
      SendMessageW(child.hwnd, BM_SETCHECK,
                   value.as_number() != 0 ? BST_CHECKED : BST_UNCHECKED, 0);
      return Error::none();
    }
    case GuiService::GuiControlKind::Progress: {
      if (!value.is_number()) return {Code::InvalidContract, "Progress value must be a number"};
      const int pos = static_cast<int>(value.as_number() + 0.5);
      if (pos < 0 || pos > 100) return {Code::InvalidContract, "progress value must be 0..100"};
      SendMessageW(child.hwnd, PBM_SETPOS, static_cast<WPARAM>(pos), 0);
      return Error::none();
    }
    default:
      return {Code::InvalidContract, "this control type has no value"};
  }
}

// ---- show helpers --------------------------------------------------------

// Client size -> outer size including frame (script_gui.cpp:7505-7528).
void client_to_outer(HWND hwnd, int& width, int& height) {
  RECT rc{0, 0, width, height};
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
  const DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  AdjustWindowRectEx(&rc, style, FALSE, ex);
  if (style & WS_HSCROLL) rc.bottom += GetSystemMetrics(SM_CYHSCROLL);
  if (style & WS_VSCROLL) rc.right += GetSystemMetrics(SM_CXVSCROLL);
  width = rc.right - rc.left;
  height = rc.bottom - rc.top;
}

}  // namespace

// ---- window procedure ----------------------------------------------------

LRESULT CALLBACK gui_wnd_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  auto* rec = reinterpret_cast<GuiRecord*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!rec) return DefWindowProcW(hwnd, msg, w, l);

  switch (msg) {
    case WM_CLOSE: {
      if (rec->interest.gui_bits & GuiService::kGuiEventBitClose) {
        // A Close handler exists: deliver and let it decide (AHK keeps the
        // window alive until the handler calls Destroy; gui-menu.md §4.3).
        push_event(*rec, target_gui(), "Close", make_args());
        return 0;
      }
      DestroyWindow(hwnd);  // AHK default: closing destroys.
      return 0;
    }
    case WM_DESTROY: {
      // Window + children are going away; the record stays so later calls
      // fail with InvalidState instead of recreating (gui-menu.md §4.5).
      push_event(*rec, target_gui(), "__closed", make_args());
      rec->dead = true;
      free_record_state(*rec);
      rec->hwnd = nullptr;
      rec->channel = 0;
      return 0;
    }
    case WM_SIZE: {
      if (rec->interest.gui_bits & GuiService::kGuiEventBitResize) {
        Json args = make_args();
        push_arg(args, static_cast<double>(w));  // SIZE_RESTORED/MINIMIZED/MAXIMIZED
        push_arg(args, static_cast<double>(LOWORD(l)));
        push_arg(args, static_cast<double>(HIWORD(l)));
        push_event(*rec, target_gui(), "Resize", std::move(args));
      }
      break;  // DefWindowProc keeps frame bookkeeping.
    }
    case WM_COMMAND: {
      const int code = HIWORD(w);
      auto* child = reinterpret_cast<HWND>(l);
      if (!child || !IsChild(hwnd, child)) break;  // menu/accelerator, not ours
      const auto id_it = rec->by_hwnd.find(reinterpret_cast<std::uintptr_t>(child));
      if (id_it == rec->by_hwnd.end()) break;
      const std::uint64_t ctrl_id = id_it->second;
      const auto kind_it = rec->children.find(ctrl_id);
      const GuiService::GuiControlKind kind =
          kind_it != rec->children.end() ? kind_it->second.kind : GuiService::GuiControlKind::Text;
      const auto bit_it = rec->interest.ctrl_bits.find(ctrl_id);
      const std::uint32_t bits = bit_it != rec->interest.ctrl_bits.end() ? bit_it->second : 0u;

      if (code == 0 /* BN_CLICKED / STN_CLICKED */) {
        const bool clickable = kind == GuiService::GuiControlKind::Button ||
                               kind == GuiService::GuiControlKind::CheckBox ||
                               kind == GuiService::GuiControlKind::Radio ||
                               kind == GuiService::GuiControlKind::Picture;
        if (clickable && (bits & GuiService::kCtrlEventBitClick)) {
          Json args = make_args();
          push_arg(args, 0);  // AHK info: batch 2 attaches none
          push_event(*rec, target_ctrl(ctrl_id), "Click", std::move(args));
        }
      } else if (code == EN_CHANGE && kind == GuiService::GuiControlKind::Edit &&
                 (bits & GuiService::kCtrlEventBitChange)) {
        Json args = make_args();
        push_arg(args, 0);
        push_event(*rec, target_ctrl(ctrl_id), "Change", std::move(args));
      }
      const auto cmd_it = rec->interest.commands.find(code);
      if (cmd_it != rec->interest.commands.end() && cmd_it->second.count(ctrl_id)) {
        Json args = make_args();
        push_arg(args, static_cast<double>(code));
        push_event(*rec, target_ctrl(ctrl_id), "Command", std::move(args));
      }
      break;
    }
    case WM_NOTIFY: {
      auto* header = reinterpret_cast<NMHDR*>(l);
      if (!header || !IsChild(hwnd, header->hwndFrom)) break;
      const auto id_it = rec->by_hwnd.find(reinterpret_cast<std::uintptr_t>(header->hwndFrom));
      if (id_it == rec->by_hwnd.end()) break;
      const int code = static_cast<int>(header->code);
      const auto it = rec->interest.notifies.find(code);
      if (it != rec->interest.notifies.end() && it->second.count(id_it->second)) {
        Json args = make_args();
        push_arg(args, static_cast<double>(code));
        push_event(*rec, target_ctrl(id_it->second), "Notify", std::move(args));
      }
      break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
      if (rec->has_back_color && rec->back_brush) {
        SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
        return reinterpret_cast<LRESULT>(rec->back_brush);
      }
      break;
    }
    case WM_ERASEBKGND: {
      if (rec->has_back_color && rec->back_brush) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(w), &rc, rec->back_brush);
        return 1;
      }
      break;
    }
    case WM_NEXTDLGCTL: {
      // DefWindowProc drops WM_NEXTDLGCTL (only DefDlgProc acts on it) and
      // this window is a plain class, so without this case GuiControl.Focus()
      // would resolve successfully while the focus never moved. Owning the
      // message here is also what makes the AHK-style send, and script code
      // that sends it directly, do what it says.
      HWND target = nullptr;
      if (l != 0) {
        target = reinterpret_cast<HWND>(w);
        if (target == nullptr || IsChild(hwnd, target) == 0) target = nullptr;
      } else {
        target = GetNextDlgTabItem(hwnd, GetFocus(), w != FALSE);
      }
      if (target != nullptr) SetFocus(target);
      return 0;
    }
    default: {
      // OnMessage: observer-only, never swallow (same rule as
      // UiThread::MessageObserver). The payload has no hwnd (gui-menu.md
      // §0.4); w/l arrive as JSON numbers, so integers beyond 2^53 lose
      // precision - documented batch-2 limitation.
      const auto it = rec->interest.messages.find(static_cast<std::uint32_t>(msg));
      if (it != rec->interest.messages.end()) {
        for (const std::uint64_t target : it->second) {
          Json args = make_args();
          push_arg(args, static_cast<double>(static_cast<std::int64_t>(w)));
          push_arg(args, static_cast<double>(static_cast<std::int64_t>(l)));
          push_arg(args, static_cast<double>(msg));
          if (target == 0) {
            push_event(*rec, target_gui(), "Message", std::move(args));
          } else {
            push_event(*rec, target_ctrl(target), "Message", std::move(args));
          }
        }
      }
      break;
    }
  }
  return DefWindowProcW(hwnd, msg, w, l);
}

// ---- run envelope --------------------------------------------------------

Error GuiService::run_pump(const std::int64_t deadline_unix_ms,
                           const rime::core::CancellationToken cancel,
                           const PumpBody& pump_body) {
  UiThread* ui = impl_->ui;
  if (!ui) return {Code::InvalidState, kNoUiThread};
  if (cancel.cancelled()) return {Code::Cancelled, kCancelledBeforeStart};
  bool expired = false;
  const std::chrono::milliseconds budget = queue_budget(deadline_unix_ms, expired);
  if (expired) return expired_error();
  Error task_error = Error::none();
  const Error call_error = ui->call(
      [&] {
        if (cancel.cancelled()) {
          task_error = {Code::Cancelled, kCancelledBeforeStart};
          return;
        }
        task_error = pump_body();
      },
      budget, cancel);
  if (!call_error.ok()) return call_error;
  return task_error;
}

// ---- parsers (JS thread, pure) -------------------------------------------

Error GuiService::parse_gui_options(const std::string& options, GuiStyleOptions& out) {
  out = GuiStyleOptions{};
  out.style = kDefaultGuiStyle;
  out.ex_style = kDefaultGuiExStyle;
  std::size_t i = 0;
  while (i < options.size()) {
    while (i < options.size() && is_space_tab(options[i])) ++i;
    if (i >= options.size()) break;
    bool adding = true;
    if (options[i] == '+' || options[i] == '-') {
      adding = options[i] == '+';
      ++i;
      if (i >= options.size() || is_space_tab(options[i])) continue;
    }
    const std::size_t start = i;
    // Gui option words end at whitespace or an embedded +/- sign (the Show/
    // ParseOptions scanners accept "Resize+Caption" without a space).
    while (i < options.size() && !is_space_tab(options[i]) && options[i] != '+' &&
           options[i] != '-') {
      ++i;
    }
    const std::string word = lower_ascii(options.substr(start, i - start));
    if (word.empty()) continue;
    if (word == "resize") {
      const DWORD bits = WS_SIZEBOX | WS_MAXIMIZEBOX;
      if (adding) {
        out.style |= bits;
        out.style_add |= bits;
      } else {
        out.style &= ~bits;
        out.style_remove |= bits;
      }
    } else if (word == "minimizebox") {
      const DWORD bits = WS_MINIMIZEBOX | WS_SYSMENU;
      if (adding) {
        out.style |= bits;
        out.style_add |= WS_MINIMIZEBOX;
      } else {
        out.style &= ~WS_MINIMIZEBOX;
        out.style_remove |= WS_MINIMIZEBOX;
      }
    } else if (word == "maximizebox") {
      const DWORD bits = WS_MAXIMIZEBOX | WS_SYSMENU;
      if (adding) {
        out.style |= bits;
        out.style_add |= WS_MAXIMIZEBOX;
      } else {
        out.style &= ~WS_MAXIMIZEBOX;
        out.style_remove |= WS_MAXIMIZEBOX;
      }
    } else if (word == "caption") {
      if (adding) {
        out.style |= WS_CAPTION;
        out.style_add |= WS_CAPTION;
      } else {
        out.style &= ~WS_CAPTION;
        out.style_remove |= WS_CAPTION;
      }
    } else if (word == "border") {
      if (adding) {
        out.style |= WS_BORDER;
        out.style_add |= WS_BORDER;
      } else {
        out.style &= ~WS_BORDER;
        out.style_remove |= WS_BORDER;
      }
    } else if (word == "alwaysontop") {
      const DWORD bit = WS_EX_TOPMOST;
      if (adding) {
        out.ex_style |= bit;
        out.ex_add |= bit;
      } else {
        out.ex_style &= ~bit;
        out.ex_remove |= bit;
      }
    } else if (word == "toolwindow") {
      const DWORD bit = WS_EX_TOOLWINDOW;
      if (adding) {
        out.ex_style |= bit;
        out.ex_add |= bit;
      } else {
        out.ex_style &= ~bit;
        out.ex_remove |= bit;
      }
    } else if (word == "owndialogs") {
      out.own_dialogs = adding;
    } else {
      return {Code::InvalidContract, "unknown gui option: " + word};
    }
  }
  return Error::none();
}

Error GuiService::parse_show_options(const std::string& options, GuiShowSpec& out) {
  out = GuiShowSpec{};
  std::size_t i = 0;
  auto parse_number = [&](std::size_t& cursor, int& value) -> bool {
    const char* begin = options.c_str() + cursor;
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end == begin) return false;
    cursor += static_cast<std::size_t>(end - begin);
    value = static_cast<int>(parsed);  // AHK casts _tcstod results (7248-7250)
    return true;
  };
  while (i < options.size()) {
    while (i < options.size() && is_space_tab(options[i])) ++i;
    if (i >= options.size()) break;
    const std::size_t start = i;
    while (i < options.size() && !is_space_tab(options[i])) ++i;
    const std::string token = options.substr(start, i - start);
    const std::string word = lower_ascii(token);
    if (word == "center") {
      out.center = true;
    } else if (word == "autosize") {
      out.auto_size = true;
    } else if (word == "hide") {
      out.hide = true;
    } else if (word == "minimize") {
      out.minimize = true;
    } else if (word == "maximize") {
      out.maximize = true;
    } else if (word == "noactivate") {
      out.no_activate = true;
    } else if (token.size() >= 2 && (token[0] == 'w' || token[0] == 'h' || token[0] == 'x' ||
                                     token[0] == 'y')) {
      std::size_t cursor = 1;
      int value = 0;
      if (!parse_number(cursor, value) || cursor != token.size()) {
        return {Code::InvalidContract, "unknown show option: " + token};
      }
      switch (token[0]) {
        case 'w': out.width = value; break;
        case 'h': out.height = value; break;
        case 'x': out.x = value; break;
        case 'y': out.y = value; break;
      }
    } else {
      return {Code::InvalidContract, "unknown show option: " + token};
    }
  }
  return Error::none();
}

Error GuiService::parse_control_options(const std::string& options, GuiControlOptions& out) {
  out = GuiControlOptions{};
  std::size_t i = 0;
  while (i < options.size()) {
    while (i < options.size() && is_space_tab(options[i])) ++i;
    if (i >= options.size()) break;
    bool adding = true;
    if (options[i] == '+' || options[i] == '-') {
      adding = options[i] == '+';
      ++i;
      if (i >= options.size() || is_space_tab(options[i])) continue;
    }
    // Control option words end at whitespace only (script_gui.cpp:5390-5393)
    // so values like "x-50" stay in one word.
    const std::size_t start = i;
    while (i < options.size() && !is_space_tab(options[i])) ++i;
    const std::string word = options.substr(start, i - start);
    if (word.empty()) continue;
    const std::string lowered = lower_ascii(word);
    if (lowered == "hidden" || lowered == "hide") {
      out.hidden = adding;
      continue;
    }
    if (lowered == "disabled") {
      out.disabled = adding;
      continue;
    }
    if (word.size() >= 2 && (word[0] == 'v' || word[0] == 'V')) {
      out.vname = word.substr(1);
      continue;
    }
    if (word.size() >= 2 && (word[0] == 'g' || word[0] == 'G')) {
      return {Code::InvalidContract, "control label options are not supported; use OnEvent"};
    }
    if (word.size() >= 2 && (word[0] == 'x' || word[0] == 'X' || word[0] == 'y' ||
                             word[0] == 'Y' || word[0] == 'w' || word[0] == 'W' ||
                             word[0] == 'h' || word[0] == 'H')) {
      const char* begin = word.c_str() + 1;
      char* end = nullptr;
      const double parsed = std::strtod(begin, &end);
      if (end == begin || *end != '\0') {
        return {Code::InvalidContract, "unknown control option: " + word};
      }
      const int value = static_cast<int>(parsed);
      switch (word[0]) {
        case 'x': case 'X': out.pos.x = value; break;
        case 'y': case 'Y': out.pos.y = value; break;
        case 'w': case 'W': out.pos.width = value; break;
        case 'h': case 'H': out.pos.height = value; break;
      }
      continue;
    }
    return {Code::InvalidContract, "unknown control option: " + word};
  }
  return Error::none();
}

Error GuiService::parse_control_type(const std::string& type, GuiControlKind& out) {
  const std::string word = lower_ascii(type);
  if (word == "button") out = GuiControlKind::Button;
  else if (word == "checkbox") out = GuiControlKind::CheckBox;
  else if (word == "edit") out = GuiControlKind::Edit;
  else if (word == "groupbox" || word == "group box") out = GuiControlKind::GroupBox;
  else if (word == "picture" || word == "pic") out = GuiControlKind::Picture;
  else if (word == "progress") out = GuiControlKind::Progress;
  else if (word == "radio") out = GuiControlKind::Radio;
  else if (word == "text") out = GuiControlKind::Text;
  else return {Code::InvalidContract, "unsupported control type: " + type};
  return Error::none();
}

Error GuiService::parse_font_options(const std::string& options, GuiFontSpec& out) {
  out = GuiFontSpec{};
  std::size_t i = 0;
  while (i < options.size()) {
    while (i < options.size() && is_space_tab(options[i])) ++i;
    if (i >= options.size()) break;
    const std::size_t start = i;
    while (i < options.size() && !is_space_tab(options[i])) ++i;
    const std::string token = options.substr(start, i - start);
    if (token.empty()) continue;
    const std::string word = lower_ascii(token);
    if (word == "bold") out.bold = true;
    else if (word == "italic") out.italic = true;
    else if (word == "underline") out.underline = true;
    else if (word == "strike") out.strike = true;
    else if (word == "norm") {
      out.bold = false;
      out.italic = false;
      out.underline = false;
      out.strike = false;
    } else if (token.size() >= 2 && (token[0] == 's' || token[0] == 'S')) {
      const char* begin = token.c_str() + 1;
      char* end = nullptr;
      const double parsed = std::strtod(begin, &end);
      if (end == begin || *end != '\0' || parsed <= 0) {
        return {Code::InvalidContract, "unknown font option: " + token};
      }
      out.size_pt = static_cast<int>(parsed);
    } else {
      return {Code::InvalidContract, "unknown font option: " + token};
    }
  }
  return Error::none();
}

// ---- Gui lifecycle -------------------------------------------------------

Error GuiService::gui_show(const GuiSpec& gui, const GuiShowSpec& show,
                           const std::int64_t deadline_unix_ms,
                           const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    ensure_margins(*rec);
    prime_first_control_baseline(*rec);

    const BOOL was_iconic = IsIconic(rec->hwnd);
    const BOOL was_zoomed = IsZoomed(rec->hwnd);
    int mode;
    if (show.hide) mode = SW_HIDE;
    else if (show.maximize) mode = SW_MAXIMIZE;
    else if (show.minimize) mode = SW_MINIMIZE;
    else if (show.no_activate) mode = SW_SHOWNOACTIVATE;
    else mode = was_iconic ? SW_RESTORE : (was_zoomed ? SW_SHOW : SW_SHOWNORMAL);

    // Early show for hide / restore-of-odd-state (script_gui.cpp:7460-7469):
    // sizing must see the restored window, and hide happens before the move
    // to reduce flicker.
    bool show_done = false;
    if (mode == SW_HIDE ||
        ((mode == SW_RESTORE || mode == SW_SHOWNOACTIVATE) && (was_zoomed || was_iconic))) {
      ShowWindow(rec->hwnd, mode);
      show_done = true;
    }

    const bool first_show = !rec->shown_once;
    if (!was_iconic) {
      int width = show.width.value_or(-1);
      int height = show.height.value_or(-1);
      int extent_right = 0;
      int extent_bottom = 0;
      content_extent(*rec, extent_right, extent_bottom);
      bool clamp_to_work_area = false;
      if (show.auto_size) {
        // AutoSize wins over given sizes (script_gui.cpp:7423-7438).
        width = extent_right;
        height = extent_bottom;
        if (width > 0) width += rec->margin_x;
        if (height > 0) height += rec->margin_y;
      } else if (width < 0 || height < 0) {
        if (first_show) {
          if (width < 0) width = extent_right + rec->margin_x;
          if (height < 0) height = extent_bottom + rec->margin_y;
          clamp_to_work_area = true;  // first-show restriction (7545-7553)
        } else {
          RECT client{};
          GetClientRect(rec->hwnd, &client);
          if (width < 0) width = client.right - client.left;
          if (height < 0) height = client.bottom - client.top;
        }
      }
      width = (std::max)(0, width);
      height = (std::max)(0, height);
      if (clamp_to_work_area) {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int work_w = work.right - work.left;
        const int work_h = work.bottom - work.top;
        if (width > work_w) width = work_w;
        if (height > work_h) height = work_h;
      }

      int outer_w = width;
      int outer_h = height;
      client_to_outer(rec->hwnd, outer_w, outer_h);

      RECT old{};
      GetWindowRect(rec->hwnd, &old);
      int x = old.left;
      int y = old.top;
      // First Show centers by default when x/y are omitted (7470-7477).
      const bool center_x = !show.x.has_value() && (show.center || first_show);
      const bool center_y = !show.y.has_value() && (show.center || first_show);
      if (show.x) x = *show.x;
      if (show.y) y = *show.y;
      if (center_x || center_y) {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        if (center_x) x = work.left + ((work.right - work.left - outer_w) / 2);
        if (center_y) y = work.top + ((work.bottom - work.top - outer_h) / 2);
      }

      const int old_w = old.right - old.left;
      const int old_h = old.bottom - old.top;
      if (outer_w != old_w || outer_h != old_h || x != old.left || y != old.top) {
        if (was_zoomed) ShowWindow(rec->hwnd, SW_RESTORE);
        MoveWindow(rec->hwnd, x, y, outer_w, outer_h, IsWindowVisible(rec->hwnd));
      }
    }

    if (!show_done) ShowWindow(rec->hwnd, mode);
    rec->shown_once = true;
    return Error::none();
  });
}

Error GuiService::gui_hide(const GuiSpec& gui, const std::int64_t deadline_unix_ms,
                           const rime::core::CancellationToken cancel) {
  GuiShowSpec show;
  show.hide = true;
  return gui_show(gui, show, deadline_unix_ms, cancel);
}

Error GuiService::gui_destroy(const std::uint64_t gui_id, const std::int64_t deadline_unix_ms,
                              const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    const auto it = impl_->guis.find(gui_id);
    if (it == impl_->guis.end()) return Error::none();  // never created: no-op
    GuiRecord* rec = it->second.get();
    if (rec->dead) return Error::none();                // already destroyed: no-op
    DestroyWindow(rec->hwnd);                           // WM_DESTROY marks dead + frees
    return Error::none();
  });
}

Error GuiService::gui_move(const GuiSpec& gui, const GuiMoveSpec& move,
                           const std::int64_t deadline_unix_ms,
                           const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    // AHK Gui.Move works on the outer window rect (script_gui.cpp:574-593).
    RECT rect{};
    GetWindowRect(rec->hwnd, &rect);
    const int width = move.width.value_or(rect.right - rect.left);
    const int height = move.height.value_or(rect.bottom - rect.top);
    const int x = move.x.value_or(rect.left);
    const int y = move.y.value_or(rect.top);
    if (width != rect.right - rect.left || height != rect.bottom - rect.top || x != rect.left ||
        y != rect.top) {
      MoveWindow(rec->hwnd, x, y, width, height, TRUE);
    }
    return Error::none();
  });
}

Error GuiService::gui_get_pos(const GuiSpec& gui, const bool client, GuiRect& out,
                              const std::int64_t deadline_unix_ms,
                              const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    if (client) {
      RECT rc{};
      GetClientRect(rec->hwnd, &rc);
      POINT origin{rc.left, rc.top};
      ClientToScreen(rec->hwnd, &origin);
      out.x = origin.x;
      out.y = origin.y;
      out.width = rc.right - rc.left;
      out.height = rc.bottom - rc.top;
    } else {
      RECT rc{};
      GetWindowRect(rec->hwnd, &rc);
      out.x = rc.left;
      out.y = rc.top;
      out.width = rc.right - rc.left;
      out.height = rc.bottom - rc.top;
    }
    return Error::none();
  });
}

Error GuiService::gui_window_cmd(const GuiSpec& gui, const int show_cmd,
                                 const std::int64_t deadline_unix_ms,
                                 const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    ShowWindow(rec->hwnd, show_cmd);
    return Error::none();
  });
}

Error GuiService::gui_flash(const GuiSpec& gui, const bool blink,
                            const std::int64_t deadline_unix_ms,
                            const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    FlashWindow(rec->hwnd, blink ? TRUE : FALSE);
    return Error::none();
  });
}

Error GuiService::gui_opt(const GuiSpec& gui, const GuiStyleOptions& style,
                          const std::int64_t deadline_unix_ms,
                          const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    // Apply exactly the bits the caller's options mentioned (the parser's
    // +/- mention masks), against the CURRENT window style.
    const DWORD current = static_cast<DWORD>(GetWindowLongPtrW(rec->hwnd, GWL_STYLE));
    const DWORD next = (current | style.style_add) & ~style.style_remove;
    const DWORD current_ex = static_cast<DWORD>(GetWindowLongPtrW(rec->hwnd, GWL_EXSTYLE));
    const DWORD next_ex = (current_ex | style.ex_add) & ~style.ex_remove;
    bool changed = false;
    if (next != current) {
      SetWindowLongPtrW(rec->hwnd, GWL_STYLE, next);
      changed = true;
    }
    if (next_ex != current_ex) {
      SetWindowLongPtrW(rec->hwnd, GWL_EXSTYLE, next_ex);
      changed = true;
    }
    if (changed) {
      SetWindowPos(rec->hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    if (style.ex_add & WS_EX_TOPMOST) {
      SetWindowPos(rec->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else if (style.ex_remove & WS_EX_TOPMOST) {
      SetWindowPos(rec->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    return Error::none();
  });
}

Error GuiService::gui_set_font(const GuiSpec& gui, const GuiFontSpec& font,
                               const std::int64_t deadline_unix_ms,
                               const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    LOGFONTW merged = merge_font_spec(rec->font_log, font);
    HFONT created = CreateFontIndirectW(&merged);
    if (!created) return {Code::ExecutionFailed, "font could not be created"};
    if (rec->owns_font && rec->font) DeleteObject(rec->font);
    rec->font = created;
    rec->owns_font = true;
    rec->font_log = merged;
    const bool visible = IsWindowVisible(rec->hwnd);
    for (auto& entry : rec->children) {
      GuiChildRecord& child = entry.second;
      if (!child.hwnd) continue;
      // Controls without their own font follow the GUI font.
      if (!child.owns_font) child.font_log = merged;
      SendMessageW(child.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(created),
                   visible ? TRUE : FALSE);
    }
    return Error::none();
  });
}

Error GuiService::gui_set_title(const GuiSpec& gui, const std::string& title,
                                const std::int64_t deadline_unix_ms,
                                const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    const std::wstring wide = from_utf8(title);
    SetWindowTextW(rec->hwnd, wide.c_str());
    return Error::none();
  });
}

Error GuiService::gui_get_title(const GuiSpec& gui, std::string& out,
                                const std::int64_t deadline_unix_ms,
                                const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    return read_window_text(rec->hwnd, out);
  });
}

Error GuiService::gui_set_back_color(const GuiSpec& gui, const std::uint32_t color_ref,
                                     const std::int64_t deadline_unix_ms,
                                     const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    if (rec->back_brush) DeleteObject(rec->back_brush);
    rec->back_brush = CreateSolidBrush(RGB(color_ref & 0xFF, (color_ref >> 8) & 0xFF,
                                           (color_ref >> 16) & 0xFF));
    rec->back_color = color_ref;
    rec->has_back_color = rec->back_brush != nullptr;
    InvalidateRect(rec->hwnd, nullptr, TRUE);
    return Error::none();
  });
}

Error GuiService::gui_set_margins(const GuiSpec& gui, const int margin_x, const int margin_y,
                                  const std::int64_t deadline_unix_ms,
                                  const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    const bool first_baseline = !rec->have_prev;
    // A negative axis means "keep it": the JS mirror keeps -1 as its unset
    // marker and only ever sends a value for the axis it just wrote.
    ensure_margins(*rec);
    if (margin_x >= 0) rec->margin_x = margin_x;
    if (margin_y >= 0) rec->margin_y = margin_y;
    if (first_baseline) rec->prev_x = rec->margin_x;
    return Error::none();
  });
}

Error GuiService::gui_submit(const GuiSpec& gui, const bool hide, Json& out,
                             const std::int64_t deadline_unix_ms,
                             const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    out = Json::object();
    for (const auto& entry : rec->children) {
      const GuiChildRecord& child = entry.second;
      if (child.vname.empty()) continue;
      Json value;
      switch (child.kind) {
        case GuiService::GuiControlKind::Edit:
        case GuiService::GuiControlKind::CheckBox:
        case GuiService::GuiControlKind::Radio: {
          const Error err = read_value(child, value);
          if (!err.ok()) return err;
          break;
        }
        case GuiService::GuiControlKind::Progress:
          continue;  // Progress carries no Submit value in batch 2
        default: {
          std::string text;
          const Error err = read_window_text(child.hwnd, text);
          if (!err.ok()) return err;
          value = Json::string(std::move(text));
          break;
        }
      }
      out.set(child.vname, std::move(value));
    }
    if (hide) ShowWindow(rec->hwnd, SW_HIDE);
    return Error::none();
  });
}

Error GuiService::gui_focused_control(const GuiSpec& gui, std::uint64_t& ctrl_id_out,
                                      const std::int64_t deadline_unix_ms,
                                      const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    if (const Error err = ensure_live_record(impl_->guis, impl_->event_sink, gui, rec);
        !err.ok()) {
      return err;
    }
    ctrl_id_out = 0;
    HWND focused = GetFocus();
    if (focused && IsChild(rec->hwnd, focused)) {
      const auto it = rec->by_hwnd.find(reinterpret_cast<std::uintptr_t>(focused));
      if (it != rec->by_hwnd.end()) ctrl_id_out = it->second;
    }
    return Error::none();
  });
}

Error GuiService::gui_set_event_interest(const GuiSpec& gui, const GuiEventInterest& interest,
                                         const std::int64_t deadline_unix_ms,
                                         const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    const auto it = impl_->guis.find(gui.id);
    if (it == impl_->guis.end()) return Error::none();  // applied by ensure later
    GuiRecord* rec = it->second.get();
    rec->interest = interest;
    if (gui.channel != 0) rec->channel = gui.channel;
    return Error::none();
  });
}

// ---- controls ------------------------------------------------------------

Error GuiService::gui_add(const GuiControlSpec& spec, const std::int64_t deadline_unix_ms,
                          const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel,
                  [&]() -> Error {
                    return add_control_pump(impl_->guis, impl_->event_sink, spec);
                  });
}

// Pump-internal move used by ctrl_opt (a nested ui->call would re-enter the
// pump; helpers never marshal).
void move_child_on_pump(GuiRecord* rec, GuiChildRecord* child,
                        const GuiService::GuiMoveSpec& move) {
  RECT rect{};
  GetWindowRect(child->hwnd, &rect);
  MapWindowPoints(nullptr, rec->hwnd, reinterpret_cast<POINT*>(&rect), 2);
  const int x = move.x.value_or(rect.left);
  const int y = move.y.value_or(rect.top);
  const int width = move.width.value_or(rect.right - rect.left);
  const int height = move.height.value_or(rect.bottom - rect.top);
  SetWindowPos(child->hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

Error GuiService::ctrl_move(const GuiSpec& gui, const std::uint64_t ctrl_id,
                            const GuiMoveSpec& move, const std::int64_t deadline_unix_ms,
                            const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    move_child_on_pump(rec, child, move);
    return Error::none();
  });
}

Error GuiService::ctrl_get_pos(const GuiSpec& gui, const std::uint64_t ctrl_id, GuiRect& out,
                               const std::int64_t deadline_unix_ms,
                               const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    RECT rect{};
    GetWindowRect(child->hwnd, &rect);
    MapWindowPoints(nullptr, rec->hwnd, reinterpret_cast<POINT*>(&rect), 2);
    out.x = rect.left;
    out.y = rect.top;
    out.width = rect.right - rect.left;
    out.height = rect.bottom - rect.top;
    return Error::none();
  });
}

Error GuiService::ctrl_focus(const GuiSpec& gui, const std::uint64_t ctrl_id,
                             const std::int64_t deadline_unix_ms,
                             const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    // WM_NEXTDLGCTL is what AHK uses (script_gui.cpp:763-771).
    SendMessageW(rec->hwnd, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(child->hwnd), TRUE);
    return Error::none();
  });
}

Error GuiService::ctrl_focused(const GuiSpec& gui, const std::uint64_t ctrl_id, bool& out,
                               const std::int64_t deadline_unix_ms,
                               const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    out = GetFocus() == child->hwnd;
    return Error::none();
  });
}

Error GuiService::ctrl_redraw(const GuiSpec& gui, const std::uint64_t ctrl_id,
                              const std::int64_t deadline_unix_ms,
                              const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    InvalidateRect(child->hwnd, nullptr, TRUE);
    UpdateWindow(child->hwnd);
    return Error::none();
  });
}

Error GuiService::ctrl_set_font(const GuiSpec& gui, const std::uint64_t ctrl_id,
                                const GuiFontSpec& font, const std::int64_t deadline_unix_ms,
                                const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    LOGFONTW merged = merge_font_spec(child->font_log, font);
    HFONT created = CreateFontIndirectW(&merged);
    if (!created) return {Code::ExecutionFailed, "font could not be created"};
    if (child->owns_font && child->font) DeleteObject(child->font);
    child->font = created;
    child->owns_font = true;
    child->font_log = merged;
    SendMessageW(child->hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(created),
                 IsWindowVisible(rec->hwnd) ? TRUE : FALSE);
    return Error::none();
  });
}

Error GuiService::ctrl_get_text(const GuiSpec& gui, const std::uint64_t ctrl_id, std::string& out,
                                const std::int64_t deadline_unix_ms,
                                const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    return read_window_text(child->hwnd, out);
  });
}

Error GuiService::ctrl_set_text(const GuiSpec& gui, const std::uint64_t ctrl_id,
                                const std::string& text, const std::int64_t deadline_unix_ms,
                                const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    const std::wstring wide = from_utf8(text);
    SetWindowTextW(child->hwnd, wide.c_str());
    return Error::none();
  });
}

Error GuiService::ctrl_get_value(const GuiSpec& gui, const std::uint64_t ctrl_id, Json& out,
                                 const std::int64_t deadline_unix_ms,
                                 const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    return read_value(*child, out);
  });
}

Error GuiService::ctrl_set_value(const GuiSpec& gui, const std::uint64_t ctrl_id,
                                 const Json& value, const std::int64_t deadline_unix_ms,
                                 const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    return write_value(*child, value);
  });
}

Error GuiService::ctrl_set_enabled(const GuiSpec& gui, const std::uint64_t ctrl_id,
                                   const bool enabled, const std::int64_t deadline_unix_ms,
                                   const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    EnableWindow(child->hwnd, enabled ? TRUE : FALSE);
    child->disabled = !enabled;
    return Error::none();
  });
}

Error GuiService::ctrl_get_enabled(const GuiSpec& gui, const std::uint64_t ctrl_id, bool& out,
                                   const std::int64_t deadline_unix_ms,
                                   const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    out = (GetWindowLongPtrW(child->hwnd, GWL_STYLE) & WS_DISABLED) == 0;
    return Error::none();
  });
}

Error GuiService::ctrl_set_visible(const GuiSpec& gui, const std::uint64_t ctrl_id,
                                   const bool visible, const std::int64_t deadline_unix_ms,
                                   const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    // Own flag only: IsWindowVisible would fold in the parent's state. The
    // show/hide flags keep the WS_VISIBLE bit consistent with reality.
    SetWindowPos(child->hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    child->hidden = !visible;
    return Error::none();
  });
}

Error GuiService::ctrl_get_visible(const GuiSpec& gui, const std::uint64_t ctrl_id, bool& out,
                                   const std::int64_t deadline_unix_ms,
                                   const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    out = (GetWindowLongPtrW(child->hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
    return Error::none();
  });
}

Error GuiService::ctrl_opt(const GuiSpec& gui, const std::uint64_t ctrl_id,
                           const GuiControlOptions& options, const std::int64_t deadline_unix_ms,
                           const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    if (options.hidden) {
      SetWindowPos(child->hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       (*options.hidden ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
      child->hidden = !*options.hidden;
    }
    if (options.disabled) {
      EnableWindow(child->hwnd, *options.disabled ? FALSE : TRUE);
      child->disabled = *options.disabled;
    }
    if (options.pos.x || options.pos.y || options.pos.width || options.pos.height) {
      GuiMoveSpec move;
      move.x = options.pos.x;
      move.y = options.pos.y;
      move.width = options.pos.width;
      move.height = options.pos.height;
      move_child_on_pump(rec, child, move);
    }
    if (!options.vname.empty()) child->vname = options.vname;
    return Error::none();
  });
}

Error GuiService::ctrl_set_cue(const GuiSpec& gui, const std::uint64_t ctrl_id,
                               const std::string& cue, const bool activate,
                               const std::int64_t deadline_unix_ms,
                               const rime::core::CancellationToken cancel) {
  return run_pump(deadline_unix_ms, cancel, [&]() -> Error {
    GuiRecord* rec = nullptr;
    GuiChildRecord* child = nullptr;
    if (const Error err = find_ctrl(impl_->guis, impl_->event_sink, gui, ctrl_id, rec, child);
        !err.ok()) {
      return err;
    }
    if (child->kind != GuiService::GuiControlKind::Edit) {
      return {Code::InvalidContract, "only Edit controls support cues"};
    }
    const std::wstring wide = from_utf8(cue);
    SendMessageW(child->hwnd, EM_SETCUEBANNER, activate ? TRUE : FALSE,
                 reinterpret_cast<LPARAM>(wide.c_str()));
    return Error::none();
  });
}

// ---- count / sweep -------------------------------------------------------

std::size_t GuiService::gui_count() const {
  UiThread* ui = impl_->ui;
  if (!ui) return 0;
  std::size_t count = 0;
  GuiMap* guis = &impl_->guis;
  const Error err = ui->call(
      [&] {
        count = guis->size();
      },
      std::chrono::seconds(5));
  if (!err.ok()) return 0;
  return count;
}

void GuiService::stop_pump_sweep() {
  for (auto& entry : impl_->guis) {
    GuiRecord& rec = *entry.second;
    if (rec.hwnd && IsWindow(rec.hwnd)) {
      DestroyWindow(rec.hwnd);  // WM_DESTROY marks dead + frees child state
    }
    free_record_state(rec);  // idempotent for already-destroyed records
    rec.hwnd = nullptr;
  }
  impl_->guis.clear();
}

}  // namespace rime::win32
