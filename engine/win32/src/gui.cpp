#include "gui_impl.hpp"

#include "utf.hpp"

#include "rime/win32/ui_thread.hpp"

#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace rime::win32 {
namespace {

using Error = rime::core::Error;
using Code = rime::core::Error::Code;

// Open modal dialogs (MsgBox/InputBox run DialogBoxIndirectParamW on the
// pump and block it until dismissed). stop() refuses while any is open
// instead of queueing behind it: a modal the user never closes would
// otherwise turn shutdown into an opaque queue Timeout. TU-local: dialogs
// are pump-global and a single GuiService owns the pump.
std::atomic<int>& open_dialog_count() {
  static std::atomic<int> count{0};
  return count;
}
std::mutex& open_dialog_mutex() {
  static std::mutex mutex;
  return mutex;
}
std::string& open_dialog_title() {
  static std::string title;
  return title;
}

struct ModalGuard {
  explicit ModalGuard(const std::string& title) {
    open_dialog_count().fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard lock(open_dialog_mutex());
    open_dialog_title() = title;
  }
  ~ModalGuard() { open_dialog_count().fetch_sub(1, std::memory_order_acq_rel); }
};

// using Error/Code: declared below; the shared failure texts and budget
// helpers live in gui_impl.hpp (shared with gui_window.cpp).

// EndDialog result AHK uses for a timed-out box (defines.h:699). MessageBox
// sometimes drops it on the floor (window.cpp:1076-1085), but this dialog
// procedure owns its dialog, so the value arrives intact - the known AHK
// OK-only timeout limitation (docs InputBox) does not exist here.
constexpr INT_PTR kTimeoutResult = -2;

// AHK allows ToolTip indices 1..20 (script2.cpp:1089-1091 maps the check to
// MAX_TOOLTIPS; docs ToolTip documents the same range).
constexpr int kMaxTooltips = 20;

// Layout constants, pixels at 96 DPI (the dialog is not DPI-scaled in
// batch 1; AHK's DPIScale equivalent is a documented follow-up).
constexpr int kMargin = 10;
constexpr int kGap = 6;
constexpr int kButtonH = 24;
constexpr int kButtonMinW = 75;
constexpr int kButtonPad = 18;
constexpr int kIconSize = 32;
constexpr int kEditH = 24;
constexpr int kMinMsgBoxW = 160;
constexpr int kDefaultPromptMaxW = 360;

// AHK's T option clamp (window.cpp:1063-1066): a negative timeout becomes
// 0.1s so a bad value still closes quickly, a huge one saturates at the
// SetTimer ceiling, 0 means no timer.
double clamp_timeout_seconds(double seconds) {
  if (seconds < 0) return 0.1;
  if (seconds > 2147483.0) return 2147483.0;
  return seconds;
}

DWORD timeout_timer_ms(double seconds) {
  const double clamped = clamp_timeout_seconds(seconds);
  if (clamped <= 0) return 0;
  return static_cast<DWORD>(clamped * 1000.0);
}

// ---- button sets ----------------------------------------------------------

// One button of an AHK MsgBox set: the Win32 control id EndDialog returns
// and the visible label ("Try Again" shows with a space; the return word is
// "TryAgain", script2.cpp:1042).
struct ButtonDef {
  int id;
  const wchar_t* label;
};

// script2.cpp:1004-1008 sButtonString order: type N is the Nth set.
const ButtonDef* buttons_for(int type, int& count) {
  static const ButtonDef ok[] = {{IDOK, L"OK"}};
  static const ButtonDef ok_cancel[] = {{IDOK, L"OK"}, {IDCANCEL, L"Cancel"}};
  static const ButtonDef ari[] = {{IDABORT, L"Abort"}, {IDRETRY, L"Retry"}, {IDIGNORE, L"Ignore"}};
  static const ButtonDef ync[] = {{IDYES, L"Yes"}, {IDNO, L"No"}, {IDCANCEL, L"Cancel"}};
  static const ButtonDef yn[] = {{IDYES, L"Yes"}, {IDNO, L"No"}};
  static const ButtonDef rc[] = {{IDRETRY, L"Retry"}, {IDCANCEL, L"Cancel"}};
  static const ButtonDef ctc[] = {{IDCANCEL, L"Cancel"},
                                  {IDTRYAGAIN, L"Try Again"},
                                  {IDCONTINUE, L"Continue"}};
  switch (type) {
    case 0:
      count = 1;
      return ok;
    case 1:
      count = 2;
      return ok_cancel;
    case 2:
      count = 3;
      return ari;
    case 3:
      count = 3;
      return ync;
    case 4:
      count = 2;
      return yn;
    case 5:
      count = 2;
      return rc;
    case 6:
      count = 3;
      return ctc;
    default:
      count = 0;
      return nullptr;
  }
}

bool type_has_cancel(int type) {
  int count = 0;
  const ButtonDef* buttons = buttons_for(type, count);
  for (int i = 0; i < count; ++i) {
    if (buttons[i].id == IDCANCEL) return true;
  }
  return false;
}

bool type_has_id(int type, int id) {
  int count = 0;
  const ButtonDef* buttons = buttons_for(type, count);
  for (int i = 0; i < count; ++i) {
    if (buttons[i].id == id) return true;
  }
  return false;
}

// The pressed-button word MsgBoxResultString returns (script2.cpp:1027-1045).
const char* button_result_word(INT_PTR result) {
  switch (result) {
    case IDOK:
      return "OK";
    case IDCANCEL:
      return "Cancel";
    case IDABORT:
      return "Abort";
    case IDRETRY:
      return "Retry";
    case IDIGNORE:
      return "Ignore";
    case IDYES:
      return "Yes";
    case IDNO:
      return "No";
    case IDTRYAGAIN:
      return "TryAgain";
    case IDCONTINUE:
      return "Continue";
    case kTimeoutResult:
      return "Timeout";
    default:
      return nullptr;
  }
}

// The X/ESC routing the OS documents for MessageBox (docs MsgBox, "The
// Close button"): OK-only -> X is OK; a Cancel button present -> X is
// Cancel; otherwise the close button is disabled and ESC does nothing.
// Returns 0 when the dismissal must be ignored.
int route_cancel_like(int type) {
  if (type_has_cancel(type)) return IDCANCEL;
  if (type == 0) return IDOK;
  return 0;
}

// ---- shared dialog helpers -----------------------------------------------

HFONT dialog_font() { return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)); }

// A dialog container template (cdit 0): children are created in WM_INITDIALOG
// where the text can be measured in pixels first. The in-memory DLGTEMPLATE
// is the shape the Phase 0 probe validated with DialogBoxIndirectParamW.
std::vector<BYTE> make_container_template(const std::wstring& title) {
  std::vector<BYTE> data;
  data.reserve(64 + title.size() * sizeof(wchar_t));
  auto put_raw = [&data](const void* bytes, std::size_t size) {
    const BYTE* begin = static_cast<const BYTE*>(bytes);
    data.insert(data.end(), begin, begin + size);
  };
  auto put_word = [&put_raw](WORD value) { put_raw(&value, sizeof(value)); };
  auto put_wstring = [&put_raw](const std::wstring& text) {
    put_raw(text.c_str(), (text.size() + 1) * sizeof(wchar_t));
  };

  DLGTEMPLATE dlg{};
  dlg.style = DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU;
  dlg.dwExtendedStyle = 0;
  dlg.cdit = 0;
  dlg.x = 20;
  dlg.y = 20;
  dlg.cx = 100;
  dlg.cy = 50;
  put_raw(&dlg, sizeof(dlg));
  put_word(0);  // menu: none
  put_word(0);  // class: dialog class
  put_wstring(title);
  put_word(8);           // point size (DEFAULT_GUI_FONT's)
  put_wstring(L"MS Shell Dlg");
  return data;
}

void apply_font(HWND control) {
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialog_font()), TRUE);
}

HWND make_child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
                int width, int height, int id) {
  HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, x, y,
                                 width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
  if (control) apply_font(control);
  return control;
}

RECT measure_text(HDC dc, const std::wstring& text, int max_width, bool single_line) {
  RECT rect{0, 0, max_width, 0};
  HGDIOBJ previous = SelectObject(dc, dialog_font());
  DrawTextW(dc, text.c_str(), -1, &rect,
            DT_CALCRECT | (single_line ? DT_SINGLELINE : DT_WORDBREAK));
  SelectObject(dc, previous);
  return rect;
}

RECT work_area_from_point(POINT point) {
  HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(monitor, &info);
  return info.rcWork;
}

int button_label_width(HDC dc, const wchar_t* label) {
  const RECT rect = measure_text(dc, label, 400, true);
  const int text_width = static_cast<int>(rect.right - rect.left);
  return (std::max)(kButtonMinW, text_width + kButtonPad);
}

// Sizes the dialog client from the computed content, grows the frame with
// AdjustWindowRectEx, then centers it on the cursor's monitor the way the
// InputBox dialog does (InputBox.cpp:148-170).
void size_and_center(HWND dialog, int client_width, int client_height) {
  RECT window{0, 0, client_width, client_height};
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE));
  const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE));
  AdjustWindowRectEx(&window, style, FALSE, ex_style);
  const int outer_width = window.right - window.left;
  const int outer_height = window.bottom - window.top;

  POINT cursor{};
  GetCursorPos(&cursor);
  const RECT work = work_area_from_point(cursor);
  const int x = work.left + ((work.right - work.left) - outer_width) / 2;
  const int y = work.top + ((work.bottom - work.top) - outer_height) / 2;
  SetWindowPos(dialog, HWND_TOP, x, y, outer_width, outer_height, SWP_NOACTIVATE);
}

// Grays SC_CLOSE when the box may not be dismissed with X (docs MsgBox:
// the X button is disabled unless OK-only or a Cancel button is present).
void update_close_enable(HWND dialog, int type) {
  const bool close_dismisses = type == 0 || type_has_cancel(type);
  HMENU system_menu = GetSystemMenu(dialog, FALSE);
  if (!system_menu) return;
  EnableMenuItem(system_menu, SC_CLOSE, MF_BYCOMMAND | (close_dismisses ? MF_ENABLED : MF_GRAYED));
}

// ---- MsgBox dialog ---------------------------------------------------------

struct DialogCtx {
  const GuiService::MsgBoxSpec* spec{nullptr};
};

// Creates the icon/text/button children from the measured text, arms the
// timeout timer and sizes the window - all inside WM_INITDIALOG, before the
// dialog manager shows it.
void build_msgbox_children(HWND dialog, const GuiService::MsgBoxSpec& spec) {
  POINT cursor{};
  GetCursorPos(&cursor);
  const RECT work = work_area_from_point(cursor);
  const int max_text_width =
      (std::max)(200, (std::min)(640, static_cast<int>(work.right - work.left) / 2 - 2 * kMargin));

  HDC dc = GetDC(dialog);
  const std::wstring text =
      spec.text.empty() && spec.buttons == 0 ? L"Press OK to continue." : from_utf8(spec.text);
  const RECT text_rect = measure_text(dc, text, max_text_width, false);
  ReleaseDC(dialog, dc);

  const int text_x = kMargin + (spec.icon != 0 ? kIconSize + kGap : 0);
  const int text_height = text_rect.bottom - text_rect.top;
  const int block_height = (std::max)(text_height, spec.icon != 0 ? kIconSize : 0);

  int button_count = 0;
  const ButtonDef* buttons = buttons_for(spec.buttons, button_count);

  dc = GetDC(dialog);
  int buttons_width = 0;
  int button_widths[4] = {0, 0, 0, 0};
  for (int index = 0; index < button_count; ++index) {
    button_widths[index] = button_label_width(dc, buttons[index].label);
    buttons_width += button_widths[index];
  }
  ReleaseDC(dialog, dc);
  buttons_width += kGap * (button_count - 1);

  const int text_block_width = text_x + static_cast<int>(text_rect.right) + kMargin;
  int client_width = (std::max)(kMinMsgBoxW, (std::max)(text_block_width, buttons_width + 2 * kMargin));
  const int client_height = kMargin + block_height + kGap + kButtonH + kMargin;

  if (spec.icon != 0) {
    const wchar_t* icon_id = IDI_APPLICATION;
    switch (spec.icon) {
      case 0x10:
        icon_id = IDI_HAND;
        break;
      case 0x20:
        icon_id = IDI_QUESTION;
        break;
      case 0x30:
        icon_id = IDI_EXCLAMATION;
        break;
      case 0x40:
        icon_id = IDI_ASTERISK;
        break;
      default:
        break;
    }
    HWND icon = make_child(dialog, L"STATIC", nullptr, SS_ICON, kMargin, kMargin, kIconSize,
                           kIconSize, 0);
    if (icon) {
      SendMessageW(icon, STM_SETICON, reinterpret_cast<WPARAM>(LoadIconW(nullptr, icon_id)), 0);
    }
  }

  make_child(dialog, L"STATIC", text.c_str(), SS_LEFT, text_x, kMargin,
             client_width - text_x - kMargin, text_height, 0);

  const int buttons_y = kMargin + block_height + kGap;
  int button_x = client_width - kMargin - buttons_width;
  HWND default_control = nullptr;
  int default_id = IDOK;
  for (int index = 0; index < button_count; ++index) {
    const ButtonDef& button = buttons[index];
    // The default is positional: DefaultN selects the Nth button (by index,
    // not id) - script2.cpp:979-986 shifts MB_DEFBITS by N-1.
    const bool positional_default = index + 1 == spec.default_index;
    DWORD style = positional_default ? BS_DEFPUSHBUTTON | BS_PUSHBUTTON : BS_PUSHBUTTON;
    HWND control =
        make_child(dialog, L"BUTTON", button.label, style, button_x, buttons_y,
                   button_widths[index], kButtonH, button.id);
    if (positional_default) {
      default_control = control;
      default_id = button.id;
    }
    button_x += button_widths[index] + kGap;
  }

  if (default_control) {
    SendMessageW(dialog, DM_SETDEFID, static_cast<WPARAM>(default_id), 0);
    SendMessageW(dialog, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(default_control), TRUE);
  } else if (button_count > 0) {
    // No positional default matched (e.g. an index past the set was clamped
    // earlier): focus the first button so the dialog is still keyboard-legal.
    HWND first = GetNextDlgTabItem(dialog, nullptr, FALSE);
    if (first) SendMessageW(dialog, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(first), TRUE);
  }

  size_and_center(dialog, client_width, client_height);
  update_close_enable(dialog, spec.buttons);

  const DWORD timer_ms = timeout_timer_ms(spec.timeout_seconds);
  if (timer_ms != 0) SetTimer(dialog, 1, timer_ms, nullptr);
}

INT_PTR CALLBACK msgbox_dlg_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_INITDIALOG: {
      SetWindowLongPtrW(dialog, DWLP_USER, lparam);
      auto* ctx = reinterpret_cast<DialogCtx*>(lparam);
      build_msgbox_children(dialog, *ctx->spec);
      SetForegroundWindow(dialog);
      return TRUE;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      const int notify = HIWORD(wparam);
      if (notify == BN_CLICKED || notify == 1) {
        auto* ctx = reinterpret_cast<DialogCtx*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (ctx && type_has_id(ctx->spec->buttons, id)) {
          EndDialog(dialog, id);
          return TRUE;
        }
        if (id == IDCANCEL && ctx) {
          const int routed = route_cancel_like(ctx->spec->buttons);
          if (routed != 0) {
            EndDialog(dialog, routed);
            return TRUE;
          }
          return TRUE;  // swallowed: no Cancel-like button exists
        }
      }
      return FALSE;
    }
    case WM_CLOSE: {
      auto* ctx = reinterpret_cast<DialogCtx*>(GetWindowLongPtrW(dialog, DWLP_USER));
      if (!ctx) return TRUE;
      const int routed = route_cancel_like(ctx->spec->buttons);
      if (routed != 0) EndDialog(dialog, routed);
      return TRUE;
    }
    case WM_TIMER: {
      if (wparam == 1) {
        KillTimer(dialog, 1);
        EndDialog(dialog, kTimeoutResult);
        return TRUE;
      }
      return FALSE;
    }
  }
  return FALSE;
}

Error run_msgbox_dialog(const GuiService::MsgBoxSpec& spec, std::string& result_out) {
  const ModalGuard guard(spec.title);
  const std::vector<BYTE> template_data = make_container_template(from_utf8(spec.title));
  DialogCtx ctx{&spec};
  const INT_PTR result = DialogBoxIndirectParamW(
      GetModuleHandleW(nullptr), reinterpret_cast<const DLGTEMPLATE*>(template_data.data()),
      nullptr, msgbox_dlg_proc, reinterpret_cast<LPARAM>(&ctx));
  if (result == -1) {
    return {Code::ExecutionFailed,
            "msgbox dialog failed (win32 error " + std::to_string(GetLastError()) + ")"};
  }
  const char* word = button_result_word(result);
  if (!word) {
    return {Code::ExecutionFailed, "msgbox dialog returned an unexpected result"};
  }
  result_out = word;
  return Error::none();
}

// ---- InputBox dialog -------------------------------------------------------

struct InputDialogCtx {
  const GuiService::InputBoxSpec* spec{nullptr};
  HWND edit{nullptr};
  std::wstring value;
};

void capture_value(InputDialogCtx* ctx) {
  if (!ctx || !ctx->edit) return;
  const int length = GetWindowTextLengthW(ctx->edit);
  ctx->value.assign(static_cast<std::size_t>(length) + 1, L'\0');
  const int written = GetWindowTextW(ctx->edit, ctx->value.data(), length + 1);
  ctx->value.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
}

void build_inputbox_children(HWND dialog, InputDialogCtx* ctx) {
  const GuiService::InputBoxSpec& spec = *ctx->spec;

  POINT cursor{};
  GetCursorPos(&cursor);
  const RECT work = work_area_from_point(cursor);
  const std::wstring prompt = from_utf8(spec.prompt);

  // Natural size when the spec gives none; an explicit width/height is the
  // client size in pixels, the way InputBox.cpp:137-140 applies them.
  HDC dc = GetDC(dialog);
  const int work_width = static_cast<int>(work.right - work.left);
  const int work_height = static_cast<int>(work.bottom - work.top);
  const int natural_cap = (std::max)(200, (std::min)(kDefaultPromptMaxW, work_width / 2));
  const RECT natural_rect = measure_text(dc, prompt, natural_cap, false);
  ReleaseDC(dialog, dc);
  const int natural_text_width = static_cast<int>(natural_rect.right);
  const int natural_text_height = static_cast<int>(natural_rect.bottom);
  const int natural_width =
      (std::max)(240, (std::min)(480, natural_text_width + 2 * kMargin + 60));

  int client_width = spec.width > 0 ? (std::min)(spec.width, work_width - 20) : natural_width;
  const int natural_height =
      kMargin + natural_text_height + kGap + kEditH + kGap + kButtonH + kMargin;
  int client_height =
      spec.height > 0 ? (std::min)(spec.height, work_height - 20) : natural_height;

  // The prompt wraps at the width it will actually get, so the measured
  // height matches the static control created below.
  dc = GetDC(dialog);
  const RECT prompt_rect = measure_text(dc, prompt, client_width - 2 * kMargin, false);
  ReleaseDC(dialog, dc);
  const int prompt_height = prompt_rect.bottom - prompt_rect.top;

  make_child(dialog, L"STATIC", prompt.c_str(), SS_LEFT, kMargin, kMargin,
             client_width - 2 * kMargin, prompt_height, 0);
  HWND edit = make_child(dialog, L"EDIT", from_utf8(spec.default_value).c_str(),
                         ES_AUTOHSCROLL | (spec.password ? ES_PASSWORD : 0) | WS_BORDER, kMargin,
                         kMargin + prompt_height + kGap, client_width - 2 * kMargin, kEditH, 101);
  ctx->edit = edit;
  if (edit && spec.password) {
    SendMessageW(edit, EM_SETPASSWORDCHAR, static_cast<WPARAM>(0x25CF), 0);
  }

  HDC dc2 = GetDC(dialog);
  const int ok_width = button_label_width(dc2, L"OK");
  const int cancel_width = button_label_width(dc2, L"Cancel");
  ReleaseDC(dialog, dc2);
  const int buttons_total = ok_width + kGap + cancel_width;
  int button_x = client_width - kMargin - buttons_total;
  const int buttons_y = kMargin + prompt_height + kGap + kEditH + kGap;

  HWND ok = make_child(dialog, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | BS_PUSHBUTTON, button_x,
                       buttons_y, ok_width, kButtonH, IDOK);
  make_child(dialog, L"BUTTON", L"Cancel", BS_PUSHBUTTON, button_x + ok_width + kGap, buttons_y,
             cancel_width, kButtonH, IDCANCEL);
  SendMessageW(dialog, DM_SETDEFID, static_cast<WPARAM>(IDOK), 0);
  if (ok) SendMessageW(dialog, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(ok), TRUE);

  // Position: both coordinates given -> place there; otherwise center both
  // axes on the cursor's monitor and override the axis that was given
  // (InputBox.cpp:148-170).
  POINT place{};
  RECT window_rect{0, 0, client_width, client_height};
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE));
  const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE));
  AdjustWindowRectEx(&window_rect, style, FALSE, ex_style);
  const int outer_width = window_rect.right - window_rect.left;
  const int outer_height = window_rect.bottom - window_rect.top;
  place.x = work.left + ((work.right - work.left) - outer_width) / 2;
  place.y = work.top + ((work.bottom - work.top) - outer_height) / 2;
  if (spec.x.has_value()) place.x = *spec.x;
  if (spec.y.has_value()) place.y = *spec.y;
  SetWindowPos(dialog, HWND_TOP, place.x, place.y, outer_width, outer_height,
               SWP_NOZORDER | SWP_SHOWWINDOW);
}

INT_PTR CALLBACK inputbox_dlg_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_INITDIALOG: {
      SetWindowLongPtrW(dialog, DWLP_USER, lparam);
      auto* ctx = reinterpret_cast<InputDialogCtx*>(lparam);
      build_inputbox_children(dialog, ctx);
      const DWORD timer_ms = timeout_timer_ms(ctx->spec->timeout_seconds);
      if (timer_ms != 0) SetTimer(dialog, 1, timer_ms, nullptr);
      SetForegroundWindow(dialog);
      return TRUE;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      const int notify = HIWORD(wparam);
      auto* ctx = reinterpret_cast<InputDialogCtx*>(GetWindowLongPtrW(dialog, DWLP_USER));
      if ((notify == BN_CLICKED || notify == 1) && (id == IDOK || id == IDCANCEL)) {
        capture_value(ctx);
        EndDialog(dialog, id);
        return TRUE;
      }
      return FALSE;
    }
    case WM_CLOSE: {
      auto* ctx = reinterpret_cast<InputDialogCtx*>(GetWindowLongPtrW(dialog, DWLP_USER));
      capture_value(ctx);
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
    case WM_TIMER: {
      if (wparam == 1) {
        KillTimer(dialog, 1);
        auto* ctx = reinterpret_cast<InputDialogCtx*>(GetWindowLongPtrW(dialog, DWLP_USER));
        capture_value(ctx);
        EndDialog(dialog, kTimeoutResult);
        return TRUE;
      }
      return FALSE;
    }
  }
  return FALSE;
}

Error run_inputbox_dialog(const GuiService::InputBoxSpec& spec, GuiService::InputBoxResult& out) {
  const ModalGuard guard(spec.title);
  const std::vector<BYTE> template_data = make_container_template(from_utf8(spec.title));
  InputDialogCtx ctx{&spec, nullptr, std::wstring()};
  const INT_PTR result = DialogBoxIndirectParamW(
      GetModuleHandleW(nullptr), reinterpret_cast<const DLGTEMPLATE*>(template_data.data()),
      nullptr, inputbox_dlg_proc, reinterpret_cast<LPARAM>(&ctx));
  if (result == -1) {
    return {Code::ExecutionFailed,
            "inputbox dialog failed (win32 error " + std::to_string(GetLastError()) + ")"};
  }
  const char* word = button_result_word(result);
  if (!word) {
    return {Code::ExecutionFailed, "inputbox dialog returned an unexpected result"};
  }
  out.result = word;
  out.value = to_utf8(ctx.value);
  return Error::none();
}

// ---- icon loading ----------------------------------------------------------

constexpr const wchar_t* kShellTip = L"rime";

bool has_ico_extension(const std::wstring& file) {
  if (file.size() < 4) return false;
  return _wcsicmp(file.c_str() + file.size() - 4, L".ico") == 0;
}

// Tray icon at SM_CXSMICON/SM_CYSMICON, the size Shell_NotifyIcon displays
// (script.cpp:943-957 loads both sizes for the same quality reason: no
// intermediate scaling). Returns nullptr with GetLastError preserved.
HICON load_tray_icon(const std::wstring& file, int icon_number) {
  const int small_x = GetSystemMetrics(SM_CXSMICON);
  const int small_y = GetSystemMetrics(SM_CYSMICON);
  if (has_ico_extension(file)) {
    return static_cast<HICON>(LoadImageW(nullptr, file.c_str(), IMAGE_ICON, small_x, small_y,
                                         LR_LOADFROMFILE | LR_DEFAULTCOLOR));
  }
  HMODULE module = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
  if (!module) return nullptr;
  HICON icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(icon_number), IMAGE_ICON,
                                             small_x, small_y, LR_DEFAULTCOLOR));
  const DWORD saved_error = GetLastError();
  FreeLibrary(module);
  SetLastError(saved_error);
  return icon;
}

// ---- tray plumbing (UI-thread only) ---------------------------------------

constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kTrayId = 1;

Error tray_ensure_added(HWND owner, bool& added, HICON custom_icon) {
  if (added) return Error::none();
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = owner;
  nid.uID = kTrayId;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid.uCallbackMessage = kTrayCallback;
  nid.hIcon = custom_icon ? custom_icon : LoadIconW(nullptr, IDI_APPLICATION);
  wcsncpy_s(nid.szTip, kShellTip, _TRUNCATE);
  if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
    return {Code::ExecutionFailed,
            "tray icon add failed (win32 error " + std::to_string(GetLastError()) + ")"};
  }
  added = true;
  return Error::none();
}

Error tray_modify_icon(HWND owner, HICON icon) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = owner;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON;
  nid.hIcon = icon;
  if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
    return {Code::ExecutionFailed,
            "tray icon update failed (win32 error " + std::to_string(GetLastError()) + ")"};
  }
  return Error::none();
}

}  // namespace

// ---- service -------------------------------------------------------------

// GuiService::Impl (now with the batch-2 Gui records) lives in gui_impl.hpp
// so gui_window.cpp shares one definition.

GuiService::GuiService() : impl_(std::make_unique<Impl>()) {}

GuiService::~GuiService() { (void)stop(); }

void GuiService::set_ui_thread(UiThread* ui) { impl_->ui = ui; }

void GuiService::set_event_sink(EventSink sink) { impl_->event_sink = std::move(sink); }

Error GuiService::msg_box(const MsgBoxSpec& spec, std::string& result_out,
                          const std::int64_t deadline_unix_ms,
                          const rime::core::CancellationToken cancel) {
  UiThread* ui = impl_->ui;
  if (!ui) return no_ui_error();
  int count = 0;
  if (!buttons_for(spec.buttons, count)) {
    return {Code::InvalidContract, "msgbox buttons must be 0..6"};
  }
  if (spec.default_index < 1 || spec.default_index > count) {
    return {Code::InvalidContract, "msgbox default button index is out of range"};
  }
  if (spec.icon != 0 && spec.icon != 0x10 && spec.icon != 0x20 && spec.icon != 0x30 &&
      spec.icon != 0x40) {
    return {Code::InvalidContract, "msgbox icon must be 0x10, 0x20, 0x30 or 0x40"};
  }
  if (cancel.cancelled()) return {Code::Cancelled, kCancelledBeforeStart};
  bool expired = false;
  const std::chrono::milliseconds budget = queue_budget(deadline_unix_ms, expired);
  if (expired) return expired_error();

  Error task_error = Error::none();
  std::string result;
  const Error call_error = ui->call(
      [&] {
        if (cancel.cancelled()) {
          task_error = {Code::Cancelled, kCancelledBeforeStart};
          return;
        }
        task_error = run_msgbox_dialog(spec, result);
      },
      budget, cancel);
  if (!call_error.ok()) return call_error;
  if (!task_error.ok()) return task_error;
  result_out = std::move(result);
  return Error::none();
}

Error GuiService::input_box(const InputBoxSpec& spec, InputBoxResult& out,
                            const std::int64_t deadline_unix_ms,
                            const rime::core::CancellationToken cancel) {
  UiThread* ui = impl_->ui;
  if (!ui) return no_ui_error();
  if (cancel.cancelled()) return {Code::Cancelled, kCancelledBeforeStart};
  bool expired = false;
  const std::chrono::milliseconds budget = queue_budget(deadline_unix_ms, expired);
  if (expired) return expired_error();

  Error task_error = Error::none();
  InputBoxResult result;
  const Error call_error = ui->call(
      [&] {
        if (cancel.cancelled()) {
          task_error = {Code::Cancelled, kCancelledBeforeStart};
          return;
        }
        task_error = run_inputbox_dialog(spec, result);
      },
      budget, cancel);
  if (!call_error.ok()) return call_error;
  if (!task_error.ok()) return task_error;
  out = std::move(result);
  return Error::none();
}

Error GuiService::tool_tip(const std::string& text, const std::optional<int> x,
                           const std::optional<int> y, const int which,
                           const std::int64_t deadline_unix_ms,
                           const rime::core::CancellationToken cancel) {
  UiThread* ui = impl_->ui;
  if (!ui) return no_ui_error();
  if (which < 1 || which > kMaxTooltips) {
    return {Code::InvalidContract, "tooltip index must be 1..20"};
  }
  if (cancel.cancelled()) return {Code::Cancelled, kCancelledBeforeStart};
  bool expired = false;
  const std::chrono::milliseconds budget = queue_budget(deadline_unix_ms, expired);
  if (expired) return expired_error();

  Impl* impl = impl_.get();
  const std::wstring wide_text = from_utf8(text);
  Error task_error = Error::none();
  const Error call_error = ui->call(
      [&] {
        if (cancel.cancelled()) {
          task_error = {Code::Cancelled, kCancelledBeforeStart};
          return;
        }
        HWND& tip = impl->tooltips[which - 1];
        if (wide_text.empty()) {
          // Blank text destroys the window (script2.cpp:1096-1105): the next
          // show recreates it at the then-current position.
          if (tip && IsWindow(tip)) DestroyWindow(tip);
          tip = nullptr;
          return;
        }
        if (!impl->comctl_ready) {
          INITCOMMONCONTROLSEX controls{};
          controls.dwSize = sizeof(controls);
          controls.dwICC = ICC_WIN95_CLASSES;
          InitCommonControlsEx(&controls);
          impl->comctl_ready = true;
        }
        const bool fresh = !tip || !IsWindow(tip);
        if (fresh) {
          tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, L"", TTS_NOPREFIX | TTS_ALWAYSTIP,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
          if (!tip) {
            task_error = {Code::ExecutionFailed,
                          "tooltip window creation failed (win32 error " +
                              std::to_string(GetLastError()) + ")"};
            return;
          }
        }

        // Position: both omitted -> cursor + 16 (script2.cpp:1115-1121);
        // otherwise start from that base and override the given axis.
        POINT point{};
        if (!x.has_value() || !y.has_value()) {
          GetCursorPos(&point);
          point.x += 16;
          point.y += 16;
        }
        if (x.has_value()) point.x = *x;
        if (y.has_value()) point.y = *y;

        const HWND owner = reinterpret_cast<HWND>(impl->ui->message_window());
        TOOLINFO info{};
        // The host has no comctl32 v6 manifest, so Windows loads v5.82, which
        // rejects the modern sizeof(TOOLINFO) (=72, includes lpReserved) at
        // TTM_ADDTOOL with FALSE. The v2 size (through lParam, 64 bytes) is
        // accepted by v5 and v6 alike; drop the trailing lpReserved slot.
        info.cbSize = static_cast<UINT>(sizeof(info) - sizeof(info.lpReserved));
        info.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        info.hwnd = owner;
        info.lpszText = const_cast<wchar_t*>(wide_text.c_str());

        if (fresh) {
          SendMessageW(tip, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&info));
        }
        RECT work = work_area_from_point(point);
        HDC screen_dc = GetDC(nullptr);
        const int dpi = GetDeviceCaps(screen_dc, LOGPIXELSX);
        ReleaseDC(nullptr, screen_dc);
        SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0,
                     static_cast<LPARAM>((work.right - work.left) * 96 / (dpi > 0 ? dpi : 96)));
        SendMessageW(tip, TTM_TRACKPOSITION, 0,
                     static_cast<LPARAM>(MAKELONG(point.x, point.y)));
        if (fresh) {
          SendMessageW(tip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
        } else {
          SendMessageW(tip, TTM_UPDATETIPTEXT, 0, reinterpret_cast<LPARAM>(&info));
        }

        // Clamp inside the work area the way AHK does (script2.cpp:1206-1214).
        RECT tip_rect{};
        GetWindowRect(tip, &tip_rect);
        const int width = tip_rect.right - tip_rect.left;
        const int height = tip_rect.bottom - tip_rect.top;
        if (point.x + width >= work.right) point.x = work.right - width - 1;
        if (point.y + height >= work.bottom) point.y = work.bottom - height - 1;
        SendMessageW(tip, TTM_TRACKPOSITION, 0,
                     static_cast<LPARAM>(MAKELONG(point.x, point.y)));
      },
      budget, cancel);
  if (!call_error.ok()) return call_error;
  return task_error;
}

Error GuiService::tray_set_icon(const std::string& file, const int icon_number, const bool freeze,
                                const bool freeze_given, const std::int64_t deadline_unix_ms,
                                const rime::core::CancellationToken cancel) {
  UiThread* ui = impl_->ui;
  if (!ui) return no_ui_error();
  if (cancel.cancelled()) return {Code::Cancelled, kCancelledBeforeStart};
  bool expired = false;
  const std::chrono::milliseconds budget = queue_budget(deadline_unix_ms, expired);
  if (expired) return expired_error();

  Impl* impl = impl_.get();
  const std::wstring wide_file = from_utf8(file);
  Error task_error = Error::none();
  const Error call_error = ui->call(
      [&] {
        if (cancel.cancelled()) {
          task_error = {Code::Cancelled, kCancelledBeforeStart};
          return;
        }
        const HWND owner = reinterpret_cast<HWND>(impl->ui->message_window());
        task_error = tray_ensure_added(owner, impl->tray_added, impl->custom_icon);
        if (!task_error.ok()) return;
        if (freeze_given) impl->tray_frozen = freeze;

        const bool restore = wide_file.empty() || wide_file == L"*";
        HICON loaded = nullptr;
        if (!restore) {
          const int number = icon_number == 0 ? 1 : icon_number;  // script.cpp:956-957
          loaded = load_tray_icon(wide_file, number);
          if (!loaded) {
            task_error = {Code::ExecutionFailed,
                          "load icon failed: " + file + " (win32 error " +
                              std::to_string(GetLastError()) + ")"};
            return;
          }
        }
        HICON next = loaded ? loaded : LoadIconW(nullptr, IDI_APPLICATION);
        if (impl->custom_icon && impl->custom_icon != next) DestroyIcon(impl->custom_icon);
        impl->custom_icon = loaded;
        task_error = tray_modify_icon(owner, next);
      },
      budget, cancel);
  if (!call_error.ok()) return call_error;
  return task_error;
}

Error GuiService::tray_tip(const std::string& text, const std::string& title,
                           const unsigned int info_flags, const bool mute,
                           const std::int64_t deadline_unix_ms,
                           const rime::core::CancellationToken cancel) {
  UiThread* ui = impl_->ui;
  if (!ui) return no_ui_error();
  if (cancel.cancelled()) return {Code::Cancelled, kCancelledBeforeStart};
  bool expired = false;
  const std::chrono::milliseconds budget = queue_budget(deadline_unix_ms, expired);
  if (expired) return expired_error();

  Impl* impl = impl_.get();
  const std::wstring wide_text = from_utf8(text);
  const std::wstring wide_title = from_utf8(title);
  Error task_error = Error::none();
  const Error call_error = ui->call(
      [&] {
        if (cancel.cancelled()) {
          task_error = {Code::Cancelled, kCancelledBeforeStart};
          return;
        }
        const HWND owner = reinterpret_cast<HWND>(impl->ui->message_window());
        task_error = tray_ensure_added(owner, impl->tray_added, impl->custom_icon);
        if (!task_error.ok()) return;

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = owner;
        nid.uID = kTrayId;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = info_flags | (mute ? NIIF_NOSOUND : 0);
        wcsncpy_s(nid.szInfoTitle, wide_title.c_str(), _TRUNCATE);
        if (!wide_text.empty()) {
          wcsncpy_s(nid.szInfo, wide_text.c_str(), _TRUNCATE);
        } else if (!wide_title.empty()) {
          // Title without text still shows (script2.cpp:124-126).
          nid.szInfo[0] = L' ';
          nid.szInfo[1] = 0;
        } else {
          nid.szInfo[0] = 0;  // both empty: remove the notification
        }
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        // AHK never treats a balloon failure as fatal (script2.cpp:131-134);
        // the tray icon itself was already proven present above.
      },
      budget, cancel);
  if (!call_error.ok()) return call_error;
  return task_error;
}

Error GuiService::stop() {
  UiThread* ui = impl_->ui;
  if (!ui) return Error::none();
  if (ui->state() != UiThreadState::Running) return Error::none();
  if (open_dialog_count().load(std::memory_order_acquire) > 0) {
    std::string title;
    {
      std::lock_guard lock(open_dialog_mutex());
      title = open_dialog_title();
    }
    return {Code::ExecutionFailed,
            "cannot stop: modal dialog '" + title +
                "' is open; close it or wait for its timeout"};
  }
  Error task_error = Error::none();
  Impl* impl = impl_.get();
  const Error call_error = ui->call(
      [&] {
        if (impl->tray_added) {
          NOTIFYICONDATAW nid{};
          nid.cbSize = sizeof(nid);
          nid.hWnd = reinterpret_cast<HWND>(impl->ui->message_window());
          nid.uID = kTrayId;
          Shell_NotifyIconW(NIM_DELETE, &nid);
          impl->tray_added = false;
        }
        if (impl->custom_icon) {
          DestroyIcon(impl->custom_icon);
          impl->custom_icon = nullptr;
        }
        for (HWND& tip : impl->tooltips) {
          if (tip && IsWindow(tip)) DestroyWindow(tip);
          tip = nullptr;
        }
        // Batch 2: destroy every script-created Gui window and free the
        // records' fonts/brushes/pictures on the pump (gui_window.cpp).
        stop_pump_sweep();
      },
      std::chrono::seconds(5));
  if (!call_error.ok()) return call_error;
  return task_error;
}

}  // namespace rime::win32
