// M6 batch 2: the JS object family behind gui-menu.md §4 - `ui.createGui`,
// the `Gui` object and the `GuiControl` object it hands back from `Add`.
//
// Shape rules this file implements:
//
//  * Construction is synchronous and never crosses threads (§4.1). HWND
//    materializes on the first async member call through the usual
//    start_async -> UiThread::call envelope.
//  * Every member that touches the window is a Promise; pure JS-side state
//    (Name, __Item, __Enum, ClassNN, Type, Gui) stays synchronous. Setters
//    validate synchronously, write the mirror, then fire-and-forget the pump
//    write - its rejection is observed by the host's rejection tracker, which
//    is where §4.2 wants it.
//  * State never holds a strong reference to a Gui or GuiControl object: the
//    entry keeps a WeakRef, and a native FinalizationRegistry whose cleanup
//    is `uiGuiGC` tears the window down when the object becomes unreachable
//    (§4.5 GC path). Identity is preserved while the script still holds the
//    object; a deref miss rebuilds a fresh shell for the same entry.
//  * Every Gui registers exactly one host callback channel in `__New`; the
//    pump pushes `{gui, target, event, args}` and this file routes it into
//    the per-route handler table. An undestroyed Gui therefore keeps a
//    callback registered, which is what makes §4.5's unload gate fail.
//
// Members listed as contract-only in docs/api/objects.json are deliberately
// NOT defined here: calling them yields `TypeError: not a function`, the
// explicit "not implemented" signal (gui-menu.md §4.8).

#include "rime/win32/js_ui.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"

#include "async_task.hpp"
#include "input_util.hpp"
#include "quickjs.h"

// Only for the SW_* show commands GuiService::gui_window_cmd takes.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;
using Error = rime::core::Error;
using Code = Error::Code;
using ControlKind = GuiService::GuiControlKind;

// Error text shared with the pump so a sync throw and its async backstop
// read the same way (gui-window.cpp carries the same literals).
constexpr const char* kDestroyed = "gui window is destroyed";
constexpr const char* kNoControl = "control does not exist";

// ---- wiring -----------------------------------------------------------------

GuiModuleBinding* binding_of(JSContext* context) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return nullptr;
  return static_cast<GuiModuleBinding*>(host->module_data("rime:ui"));
}

// Non-throwing variant for paths that must not raise during teardown: the
// event dispatch closure and the FinalizationRegistry cleanup.
GuiJsState* state_or_null(GuiModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel) return nullptr;
  GuiJsState* state = binding->gui.get();
  if (!state || state->closed.load()) return nullptr;
  return state;
}

GuiJsState* state_of(JSContext* context, GuiModuleBinding* binding) {
  GuiJsState* state = state_or_null(binding);
  if (state) return state;
  if (!binding || !binding->service || !binding->kernel) {
    JS_ThrowInternalError(context, "rime:ui is not wired");
  } else {
    JS_ThrowInternalError(context, "rime:ui gui state is shut down");
  }
  return nullptr;
}

// ---- errors -----------------------------------------------------------------
//
//承载方式：ValueError -> RangeError 带解析器原文；InvalidState ->
// Error 带 code="invalid_state"；capability -> Error 带
// code="capability_denied"。缺失读 undefined（现代语义，无 UnsetItemError）。

JSValue make_coded_error(JSContext* context, const char* code, const std::string& message) {
  JSValue error = JS_NewError(context);
  if (JS_IsException(error)) return JS_EXCEPTION;
  JSValue text = JS_NewStringLen(context, message.data(), message.size());
  if (JS_IsException(text)) {
    JS_FreeValue(context, error);
    return JS_EXCEPTION;
  }
  // JS_DefinePropertyValueStr consumes `text` on both outcomes.
  if (JS_DefinePropertyValueStr(context, error, "message", text,
                                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
    JS_FreeValue(context, error);
    return JS_EXCEPTION;
  }
  JSValue code_value = JS_NewString(context, code);
  if (JS_IsException(code_value)) {
    JS_FreeValue(context, error);
    return JS_EXCEPTION;
  }
  if (JS_DefinePropertyValueStr(context, error, "code", code_value,
                                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
    JS_FreeValue(context, error);
    return JS_EXCEPTION;
  }
  return error;
}

JSValue throw_coded(JSContext* context, const char* code, const std::string& message) {
  JSValue error = make_coded_error(context, code, message);
  if (JS_IsException(error)) return JS_EXCEPTION;
  return JS_Throw(context, error);
}

// ValueError: the parser's own message is the RangeError text, so a sync
// throw from a pure parser and a later native rejection carry one wording.
void throw_value_error(JSContext* context, const std::string& detail) {
  JS_ThrowRangeError(context, "%s", detail.c_str());
}

JSValue throw_invalid_state(JSContext* context, const char* message) {
  return throw_coded(context, "invalid_state", message);
}

// The three rows of gui-menu.md §4.7: a member that exists only to say no.
JSValue throw_policy_refusal(JSContext* context) {
  return throw_coded(context, "unsupported_by_policy",
                     "unsupported-by-policy: raw window and font handles are not part of "
                     "the API; use stable ids");
}

JSValue throw_capability(JSContext* context) {
  return throw_coded(context, "capability_denied",
                     std::string("required capability was not granted: ") +
                         kUiCreateCapability);
}

// ---- small value helpers ----------------------------------------------------

JSValue resolved_promise(JSContext* context, JSValue value) {
  JSValue funcs[2] = {JS_UNDEFINED, JS_UNDEFINED};
  JSValue promise = JS_NewPromiseCapability(context, funcs);
  if (JS_IsException(promise)) {
    JS_FreeValue(context, value);
    return JS_EXCEPTION;
  }
  JSValue settled = JS_Call(context, funcs[0], JS_UNDEFINED, 1, &value);
  JS_FreeValue(context, value);
  JS_FreeValue(context, settled);
  JS_FreeValue(context, funcs[0]);
  JS_FreeValue(context, funcs[1]);
  return promise;
}

JSValue rejected_promise(JSContext* context, JSValue reason) {
  JSValue funcs[2] = {JS_UNDEFINED, JS_UNDEFINED};
  JSValue promise = JS_NewPromiseCapability(context, funcs);
  if (JS_IsException(promise)) {
    JS_FreeValue(context, reason);
    return JS_EXCEPTION;
  }
  // The only caller hands this Promise straight to the script, which attaches
  // its handlers after this call returns. quickjs-ng notifies the host's
  // rejection tracker the instant a settled promise has no reactions, and
  // Host::eval_module reads that same tracker as its module-failure signal -
  // so an unmarked reject would fail the module before the caller ever saw
  // the value. Marking it handled first keeps the reject silent here and
  // still delivers it to whoever awaits it.
  JS_PromiseMarkAsHandled(context, promise);
  JSValue settled = JS_Call(context, funcs[1], JS_UNDEFINED, 1, &reason);
  JS_FreeValue(context, reason);
  JS_FreeValue(context, settled);
  JS_FreeValue(context, funcs[0]);
  JS_FreeValue(context, funcs[1]);
  return promise;
}

// The setter write path (window.ts minimizeAll precedent): the promise is
// dropped, so an unhandled rejection surfaces through the host's rejection
// tracker and lands in host record() instead of nowhere.
void fire_and_forget(JSContext* context, JSValue promise) { JS_FreeValue(context, promise); }

std::string rect_json(const GuiService::GuiRect& rect) {
  json::Value value = json::Value::object();
  value.set("x", json::Value::number(rect.x));
  value.set("y", json::Value::number(rect.y));
  value.set("width", json::Value::number(rect.width));
  value.set("height", json::Value::number(rect.height));
  return json::stringify(value);
}

std::string control_type_name(ControlKind kind) {
  switch (kind) {
    case ControlKind::Button:
      return "Button";
    case ControlKind::CheckBox:
      return "CheckBox";
    case ControlKind::Edit:
      return "Edit";
    case ControlKind::GroupBox:
      return "GroupBox";
    case ControlKind::Picture:
      return "Picture";
    case ControlKind::Progress:
      return "Progress";
    case ControlKind::Radio:
      return "Radio";
    case ControlKind::Text:
      return "Text";
    case ControlKind::ListBox:
      return "ListBox";
    case ControlKind::ComboBox:
      return "ComboBox";
    case ControlKind::DropDownList:
      return "DropDownList";
    case ControlKind::DateTime:
      return "DateTime";
    case ControlKind::MonthCal:
      return "MonthCal";
    case ControlKind::Slider:
      return "Slider";
    case ControlKind::UpDown:
      return "UpDown";
    case ControlKind::Tab:
      return "Tab";
    case ControlKind::Link:
      return "Link";
    case ControlKind::Hotkey:
      return "Hotkey";
    case ControlKind::ListView:
      return "ListView";
    case ControlKind::TreeView:
      return "TreeView";
    case ControlKind::StatusBar:
      return "StatusBar";
  }
  return "Text";
}

// The batch-2 constructors (eight) plus batch-3a list family (four
// magics, three kinds: AddDDL aliases AddDropDownList) and date/slider
// family (four magics, four kinds), fixed by sugar name.
std::optional<ControlKind> fixed_add_kind(const int magic);

// ---- argument parsing -------------------------------------------------------

// A trailing plain object is the ActionOptions: it is always the last
// argument, and no batch-2 member accepts an object in its final position
// otherwise (`__New`'s eventObj takes no action, and it is not parsed here).
// Functions and arrays are excluded so `OnEvent(name, fn)` keeps its
// callback and `Add(..., ["a"])` still reports the §4.6 TypeError.
bool is_action_object(JSContext* context, JSValueConst value) {
  if (!JS_IsObject(value) || JS_IsArray(value)) return false;
  return !JS_IsFunction(context, value);
}

// Shortens `end` past a trailing action object and returns its index (-1 =
// none). Everything before `end` is a feature argument.
int split_action(JSContext* context, int argc, JSValueConst* argv, int& end) {
  end = argc;
  if (end > 0 && is_action_object(context, argv[end - 1])) return end - 1;
  return -1;
}

bool optional_string_arg(JSContext* context, int argc, JSValueConst* argv, const int index,
                         const char* signature, const char* name, std::string& out) {
  if (index >= argc || JS_IsUndefined(argv[index]) || JS_IsNull(argv[index])) return true;
  if (!JS_IsString(argv[index])) {
    JS_ThrowTypeError(context, "%s: %s must be a string", signature, name);
    return false;
  }
  const char* text = JS_ToCString(context, argv[index]);
  if (!text) return false;
  out = text;
  JS_FreeCString(context, text);
  return true;
}

bool read_string(JSContext* context, JSValueConst value, const char* what, std::string& out) {
  if (!JS_IsString(value)) {
    JS_ThrowTypeError(context, "%s must be a string", what);
    return false;
  }
  const char* text = JS_ToCString(context, value);
  if (!text) return false;
  out = text;
  JS_FreeCString(context, text);
  return true;
}

// The optional ActionOptions at `action_index` (-1 = absent).
bool parse_action_at(JSContext* context, JSValueConst* argv, const int action_index,
                     ActionOptions& out) {
  if (action_index < 0) return true;
  return parse_action_options(context, argv[action_index], out);
}

// Optional positional int in [0, end). Missing/undefined leaves `out` alone.
bool optional_int_arg(JSContext* context, JSValueConst* argv, const int index,
                      const int end, const char* name, std::optional<int>& out) {
  if (index >= end || JS_IsUndefined(argv[index]) || JS_IsNull(argv[index])) return true;
  std::int64_t raw = 0;
  const std::string label = std::string(name) + " must be an integer";
  if (!js_int64_strict(context, argv[index], raw, label.c_str())) return false;
  if (raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max()) {
    JS_ThrowRangeError(context, "%s is out of range", name);
    return false;
  }
  out = static_cast<int>(raw);
  return true;
}

bool optional_bool_arg(JSContext* context, JSValueConst* argv, const int index,
                       const int end, const char* name, bool& out) {
  if (index >= end || JS_IsUndefined(argv[index]) || JS_IsNull(argv[index])) return true;
  if (!JS_IsBool(argv[index])) {
    JS_ThrowTypeError(context, "%s must be a boolean", name);
    return false;
  }
  out = JS_ToBool(context, argv[index]) > 0;
  return true;
}

// Optional positional boolean with a third state: missing/undefined/null
// leaves `out` empty (leave unchanged); a present value must be a boolean.
// Batch-3b row/node modifiers ride positionals (never a trailing object -
// that slot is the ActionOptions, see is_action_object).
bool optional_tri_arg(JSContext* context, JSValueConst* argv, const int index, const int end,
                      const char* name, std::optional<bool>& out) {
  if (index >= end || JS_IsUndefined(argv[index]) || JS_IsNull(argv[index])) return true;
  if (!JS_IsBool(argv[index])) {
    JS_ThrowTypeError(context, "%s must be a boolean", name);
    return false;
  }
  out = JS_ToBool(context, argv[index]) > 0;
  return true;
}

// Optional positional item id (TreeView stable ids, parent ids): an integer
// >= 0. Missing/undefined/null leaves `out` empty.
bool optional_id_arg(JSContext* context, JSValueConst* argv, const int index, const int end,
                     const char* name, std::optional<std::uint64_t>& out) {
  if (index >= end || JS_IsUndefined(argv[index]) || JS_IsNull(argv[index])) return true;
  std::int64_t raw = 0;
  const std::string label = std::string(name) + " must be a non-negative integer";
  if (!js_int64_strict(context, argv[index], raw, label.c_str())) return false;
  if (raw < 0) {
    JS_ThrowRangeError(context, "%s is out of range", name);
    return false;
  }
  out = static_cast<std::uint64_t>(raw);
  return true;
}

// Required positional 0-based index (row, column, part, tab): an integer
// >= 0. Negative values fail here, synchronously; absurdly large values
// fail too (a wrapped int could address the wrong row on the pump).
bool required_index_arg(JSContext* context, JSValueConst* argv, const int index, const int end,
                        const char* signature, const char* name, std::int64_t& out) {
  if (index >= end || JS_IsUndefined(argv[index]) || JS_IsNull(argv[index])) {
    JS_ThrowTypeError(context, "%s: %s is required", signature, name);
    return false;
  }
  const std::string label = std::string(signature) + ": " + name + " must be a non-negative integer";
  if (!js_int64_strict(context, argv[index], out, label.c_str())) return false;
  if (out < 0 || out > std::numeric_limits<int>::max()) {
    JS_ThrowRangeError(context, "%s: %s is out of range", signature, name);
    return false;
  }
  return true;
}

// Optional positional 0-based index: missing/undefined/null leaves `out`
// empty (Delete() clears, GetNext() starts at the top).
bool optional_index_arg(JSContext* context, JSValueConst* argv, const int index, const int end,
                        const char* signature, const char* name, std::optional<std::int64_t>& out) {
  if (index >= end || JS_IsUndefined(argv[index]) || JS_IsNull(argv[index])) return true;
  std::int64_t raw = 0;
  const std::string label = std::string(signature) + ": " + name + " must be a non-negative integer";
  if (!js_int64_strict(context, argv[index], raw, label.c_str())) return false;
  if (raw < 0 || raw > std::numeric_limits<int>::max()) {
    JS_ThrowRangeError(context, "%s: %s is out of range", signature, name);
    return false;
  }
  out = raw;
  return true;
}

// Required positional node id (TreeView stable id): an integer >= 0.
bool required_id_arg(JSContext* context, JSValueConst* argv, const int index, const int end,
                     const char* signature, std::uint64_t& out) {
  std::optional<std::uint64_t> maybe;
  if (!optional_id_arg(context, argv, index, end, "node id", maybe)) return false;
  if (!maybe.has_value()) {
    JS_ThrowTypeError(context, "%s: node id is required", signature);
    return false;
  }
  out = *maybe;
  return true;
}

// Array of strings (row cells, option values, modifier names). Missing slot
// is a TypeError - callers decide which positions may be omitted.
bool read_string_array(JSContext* context, JSValueConst value, const char* what,
                       std::vector<std::string>& out) {
  if (!JS_IsArray(value)) {
    JS_ThrowTypeError(context, "%s must be an array of strings", what);
    return false;
  }
  std::int64_t length = 0;
  if (JS_GetLength(context, value, &length) || length < 0) return false;
  for (std::int64_t i = 0; i < length; ++i) {
    JSValue item = JS_GetPropertyUint32(context, value, static_cast<std::uint32_t>(i));
    bool ok = JS_IsString(item);
    std::string text;
    if (ok) {
      const char* raw = JS_ToCString(context, item);
      ok = raw != nullptr;
      if (ok) {
        text = raw;
        JS_FreeCString(context, raw);
      }
    }
    JS_FreeValue(context, item);
    if (!ok) {
      JS_ThrowTypeError(context, "%s must be an array of strings", what);
      return false;
    }
    out.push_back(std::move(text));
  }
  return true;
}

// Array of integers (StatusBar part widths). Elements must be integral.
bool read_int_array(JSContext* context, JSValueConst value, const char* what,
                    std::vector<int>& out) {
  if (!JS_IsArray(value)) {
    JS_ThrowTypeError(context, "%s must be an array of integers", what);
    return false;
  }
  std::int64_t length = 0;
  if (JS_GetLength(context, value, &length) || length < 0) return false;
  for (std::int64_t i = 0; i < length; ++i) {
    JSValue item = JS_GetPropertyUint32(context, value, static_cast<std::uint32_t>(i));
    std::int64_t raw = 0;
    bool ok = js_int64_strict(context, item, raw, what);
    JS_FreeValue(context, item);
    if (!ok) return false;
    if (raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max()) {
      JS_ThrowRangeError(context, "%s is out of range", what);
      return false;
    }
    out.push_back(static_cast<int>(raw));
  }
  return true;
}

// `Add(type, options?, content?)` content: §4.6 - string for the text kinds,
// a path for Picture, a number for Progress; an array of strings is rows
// for the list kinds (ListBox/ComboBox/DropDownList) and stays a TypeError
// everywhere else.
bool read_control_content(JSContext* context, JSValueConst value, const ControlKind kind,
                          std::string& text_out, bool& has_number_out, double& number_out,
                          std::vector<std::string>& items_out, bool& has_items_out) {
  if (JS_IsUndefined(value) || JS_IsNull(value)) return true;
  const bool list_kind = kind == ControlKind::ListBox || kind == ControlKind::ComboBox ||
                         kind == ControlKind::DropDownList;
  const bool table_kind = kind == ControlKind::ListView || kind == ControlKind::TreeView ||
                          kind == ControlKind::StatusBar || kind == ControlKind::Tab;
  if (JS_IsArray(value)) {
    if (!list_kind && !table_kind) {
      JS_ThrowTypeError(context, "content must not be an array");
      return false;
    }
    std::int64_t length = 0;
    if (JS_GetLength(context, value, &length) || length < 0) return false;
    for (std::int64_t i = 0; i < length; ++i) {
      JSValue item = JS_GetPropertyUint32(context, value, static_cast<std::uint32_t>(i));
      if (JS_IsException(item)) return false;
      const char* text = JS_ToCString(context, item);
      JS_FreeValue(context, item);
      if (!text) return false;
      if (text[0] == '\0') {
        JS_FreeCString(context, text);
        JS_ThrowTypeError(context, "list rows must be non-empty strings");
        return false;
      }
      items_out.emplace_back(text);
      JS_FreeCString(context, text);
    }
    has_items_out = true;
    return true;
  }
  if (list_kind && !JS_IsUndefined(value) && !JS_IsNull(value) && !JS_IsString(value)) {
    JS_ThrowTypeError(context, "list content must be a string or an array of strings");
    return false;
  }
  if (kind == ControlKind::Progress) {
    if (!JS_IsNumber(value)) {
      JS_ThrowTypeError(context, "Progress content must be a number");
      return false;
    }
    if (JS_ToFloat64(context, &number_out, value)) return false;
    has_number_out = true;
    return true;
  }
  if (kind == ControlKind::Slider || kind == ControlKind::UpDown) {
    if (!JS_IsNumber(value)) {
      JS_ThrowTypeError(context, "Slider/UpDown content must be a number");
      return false;
    }
    if (JS_ToFloat64(context, &number_out, value)) return false;
    has_number_out = true;
    return true;
  }
  if (!JS_IsString(value)) {
    JS_ThrowTypeError(context, "content must be a string");
    return false;
  }
  const char* text = JS_ToCString(context, value);
  if (!text) return false;
  if (list_kind) {
    // A single string is one row; the pump ignores text_out for lists.
    if (text[0] != '\0') {
      items_out.emplace_back(text);
      has_items_out = true;
    }
    JS_FreeCString(context, text);
    return true;
  }
  text_out = text;
  JS_FreeCString(context, text);
  return true;
}

// Gui.BackColor accepts AHK's color spelling: 6 (or fewer) hex digits with
// an optional `#` or `0x` prefix, or a JS number. The stored mirror keeps
// AHK's bare uppercase "RRGGBB" form (script_gui.cpp:get_BackColor), while
// the pump wants a COLORREF, so the red and blue bytes are swapped here the
// way AHK's ColorToBGR does it.
bool parse_color(JSContext* context, JSValueConst value, std::string& mirror_out,
                 std::uint32_t& color_ref_out) {
  std::uint32_t rgb = 0;
  if (JS_IsNumber(value)) {
    double number = 0;
    if (JS_ToFloat64(context, &number, value)) return false;
    if (!std::isfinite(number) || std::trunc(number) != number || number < 0 ||
        number > 0xFFFFFF) {
      throw_value_error(context, "invalid color");
      return false;
    }
    rgb = static_cast<std::uint32_t>(number);
  } else if (JS_IsString(value)) {
    const char* raw = JS_ToCString(context, value);
    if (!raw) return false;
    std::string text(raw);
    JS_FreeCString(context, raw);
    std::size_t start = 0;
    if (!text.empty() && text[0] == '#') {
      start = 1;
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      start = 2;
    }
    const std::string digits = text.substr(start);
    if (digits.empty() || digits.size() > 6) {
      throw_value_error(context, "invalid color: " + text);
      return false;
    }
    for (const char character : digits) {
      const bool hex = (character >= '0' && character <= '9') ||
                       (character >= 'a' && character <= 'f') ||
                       (character >= 'A' && character <= 'F');
      if (!hex) {
        throw_value_error(context, "invalid color: " + text);
        return false;
      }
      rgb <<= 4;
      if (character >= '0' && character <= '9') {
        rgb |= static_cast<std::uint32_t>(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        rgb |= static_cast<std::uint32_t>(character - 'a' + 10);
      } else {
        rgb |= static_cast<std::uint32_t>(character - 'A' + 10);
      }
    }
  } else {
    JS_ThrowTypeError(context, "BackColor must be a string or a number");
    return false;
  }
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%06X", rgb);
  mirror_out = buffer;
  color_ref_out = ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu);
  return true;
}

// Value setter pre-check (§4.2: type mismatch is a synchronous ValueError).
bool js_to_control_value(JSContext* context, JSValueConst raw, const ControlKind kind,
                         json::Value& out) {
  switch (kind) {
    case ControlKind::Edit: {
      if (!JS_IsString(raw)) {
        throw_value_error(context, "Edit value must be a string");
        return false;
      }
      const char* text = JS_ToCString(context, raw);
      if (!text) return false;
      out = json::Value::string(text);
      JS_FreeCString(context, text);
      return true;
    }
    case ControlKind::CheckBox:
    case ControlKind::Radio: {
      if (JS_IsBool(raw)) {
        out = json::Value::number(JS_ToBool(context, raw) ? 1.0 : 0.0);
        return true;
      }
      double number = 0;
      if (!JS_IsNumber(raw) || JS_ToFloat64(context, &number, raw)) {
        throw_value_error(context, "CheckBox/Radio value must be 0 or 1");
        return false;
      }
      if (number != 0.0 && number != 1.0) {
        throw_value_error(context, "CheckBox/Radio value must be 0 or 1");
        return false;
      }
      out = json::Value::number(number);
      return true;
    }
    case ControlKind::Progress: {
      double number = 0;
      if (!JS_IsNumber(raw) || JS_ToFloat64(context, &number, raw)) {
        throw_value_error(context, "Progress value must be a number");
        return false;
      }
      if (!std::isfinite(number) || std::trunc(number) != number) {
        throw_value_error(context, "Progress value must be an integer");
        return false;
      }
      if (number < 0 || number > 100) {
        throw_value_error(context, "progress value must be 0..100");
        return false;
      }
      out = json::Value::number(number);
      return true;
    }
    default:
      throw_value_error(context, "this control type has no value");
      return false;
  }
}

// ---- receiver identity ------------------------------------------------------
//
// Every member closure is a per-object function whose `id` is non-enumerable
// (InputHook precedent, events_module.cpp:3547-3567). Gui and control ids
// both start at 1, so the family is disambiguated by a second non-enumerable
// brand: an extracted method called on the wrong object cannot resolve.

bool define_identity(JSContext* context, JSValue object, const std::uint64_t id,
                     const char* brand) {
  // JS_DefinePropertyValueStr consumes the value on both outcomes; only
  // CONFIGURABLE means non-enumerable and non-writable.
  if (JS_DefinePropertyValueStr(context, object, "id",
                                JS_NewInt64(context, static_cast<std::int64_t>(id)),
                                JS_PROP_CONFIGURABLE) < 0) {
    return false;
  }
  return JS_DefinePropertyValueStr(context, object, "__brand", JS_NewString(context, brand),
                                   JS_PROP_CONFIGURABLE) >= 0;
}

std::uint64_t identity_of(JSContext* context, JSValueConst this_val, const char* brand,
                          bool& ok) {
  ok = false;
  if (!JS_IsObject(this_val)) {
    JS_ThrowTypeError(context, "%s receiver expected", brand);
    return 0;
  }
  JSValue found_brand = JS_GetPropertyStr(context, this_val, "__brand");
  if (JS_IsException(found_brand)) return 0;
  bool matches = false;
  if (JS_IsString(found_brand)) {
    const char* text = JS_ToCString(context, found_brand);
    if (!text) {
      JS_FreeValue(context, found_brand);
      return 0;
    }
    matches = std::string(text) == brand;
    JS_FreeCString(context, text);
  }
  JS_FreeValue(context, found_brand);
  if (!matches) {
    JS_ThrowTypeError(context, "%s receiver expected", brand);
    return 0;
  }
  JSValue id_value = JS_GetPropertyStr(context, this_val, "id");
  if (JS_IsException(id_value)) return 0;
  std::int64_t id = 0;
  const bool number = JS_IsNumber(id_value) && JS_ToInt64(context, &id, id_value) == 0;
  JS_FreeValue(context, id_value);
  if (!number || id <= 0) {
    JS_ThrowTypeError(context, "%s receiver expected", brand);
    return 0;
  }
  ok = true;
  return static_cast<std::uint64_t>(id);
}

GuiJs* gui_of(JSContext* context, JSValueConst this_val, GuiJsState* state) {
  bool ok = false;
  const std::uint64_t id = identity_of(context, this_val, "gui", ok);
  if (!ok) return nullptr;
  const auto found = state->guis.find(id);
  if (found == state->guis.end()) {
    JS_ThrowTypeError(context, "Gui receiver expected");
    return nullptr;
  }
  return &found->second;
}

GuiJsControl* ctrl_of(JSContext* context, JSValueConst this_val, GuiJsState* state) {
  bool ok = false;
  const std::uint64_t id = identity_of(context, this_val, "GuiControl", ok);
  if (!ok) return nullptr;
  const auto found = state->controls.find(id);
  if (found == state->controls.end()) {
    JS_ThrowTypeError(context, "GuiControl receiver expected");
    return nullptr;
  }
  return &found->second;
}

// Synchronous pre-check every member runs (§4.5: after Destroy, members
// throw InvalidState; §4.2 setters throw it synchronously too).
bool require_live(JSContext* context, const GuiJs& gui) {
  if (gui.destroyed) {
    throw_invalid_state(context, kDestroyed);
    return false;
  }
  return true;
}

bool require_live_ctrl(JSContext* context, const GuiJsState& state, const GuiJsControl& control) {
  if (control.destroyed) {
    throw_invalid_state(context, kNoControl);
    return false;
  }
  const auto gui = state.guis.find(control.gui_id);
  if (gui == state.guis.end() || gui->second.destroyed) {
    throw_invalid_state(context, kNoControl);
    return false;
  }
  return true;
}

// ---- WeakRef / FinalizationRegistry ----------------------------------------

JSValue invoke_named(JSContext* context, JSValueConst target, const char* name, const int argc,
                     JSValueConst* argv) {
  const JSAtom atom = JS_NewAtom(context, name);
  if (atom == JS_ATOM_NULL) return JS_EXCEPTION;
  JSValue result = JS_Invoke(context, target, atom, argc, argv);
  JS_FreeAtom(context, atom);
  return result;
}

// Takes ownership of `target` (a duplicate of the real object) and returns
// the WeakRef, or JS_EXCEPTION.
JSValue make_weak(JSContext* context, JSValue target) {
  JSValue global = JS_GetGlobalObject(context);
  if (JS_IsException(global)) {
    JS_FreeValue(context, target);
    return JS_EXCEPTION;
  }
  JSValue ctor = JS_GetPropertyStr(context, global, "WeakRef");
  JS_FreeValue(context, global);
  if (JS_IsException(ctor)) {
    JS_FreeValue(context, target);
    return JS_EXCEPTION;
  }
  if (JS_IsUndefined(ctor)) {
    JS_FreeValue(context, target);
    return JS_ThrowInternalError(context, "WeakRef is unavailable");
  }
  JSValue ref = JS_CallConstructor(context, ctor, 1, &target);
  JS_FreeValue(context, ctor);
  JS_FreeValue(context, target);
  return ref;
}

// undefined when the object has already been collected (the registry cleanup
// for it has either run or is queued as a job).
JSValue weak_deref(JSContext* context, JSValueConst weak) {
  if (!JS_IsObject(weak)) return JS_UNDEFINED;
  return invoke_named(context, weak, "deref", 0, nullptr);
}

void registry_register(JSContext* context, GuiJsState* state, const std::uint64_t gui_id,
                       JSValueConst object) {
  if (JS_IsUndefined(state->registry)) return;
  JSValue held = JS_NewInt64(context, static_cast<std::int64_t>(gui_id));
  if (JS_IsException(held)) return;
  JSValueConst args[2] = {object, held};
  JSValue ignored = invoke_named(context, state->registry, "register", 2, args);
  JS_FreeValue(context, held);
  JS_FreeValue(context, ignored);
}

void registry_unregister(JSContext* context, GuiJsState* state, JSValueConst object) {
  if (JS_IsUndefined(state->registry) || !JS_IsObject(object)) return;
  JSValue ignored = invoke_named(context, state->registry, "unregister", 1, &object);
  JS_FreeValue(context, ignored);
}

// ---- object construction ----------------------------------------------------

// Releases the JS-side half of a Gui: handlers, event object, WeakRef, the
// host channel and every control entry. Idempotent - Destroy, the GC path
// and teardown all funnel through it.
void release_gui(JSContext* context, GuiJsState* state, GuiJs& gui) {
  if (gui.destroyed) return;
  gui.destroyed = true;
  // Unregister while the object may still be reachable (Destroy path); on a
  // GC path deref is empty and there is nothing left to unregister.
  JSValue live = weak_deref(context, gui.weak);
  if (!JS_IsException(live)) registry_unregister(context, state, live);
  JS_FreeValue(context, live);
  for (auto& [route, handlers] : gui.handlers) {
    for (auto& handler : handlers) JS_FreeValue(context, handler.fn);
    handlers.clear();
  }
  gui.handlers.clear();
  JS_FreeValue(context, gui.event_obj);
  gui.event_obj = JS_UNDEFINED;
  JS_FreeValue(context, gui.weak);
  gui.weak = JS_UNDEFINED;
  if (gui.channel != 0 && state->host) {
    (void)state->host->remove_callback(gui.channel);
    state->channels.erase(gui.channel);
    gui.channel = 0;
  }
  for (const std::uint64_t control_id : gui.controls) {
    const auto found = state->controls.find(control_id);
    if (found == state->controls.end()) continue;
    JS_FreeValue(context, found->second.weak);
    found->second.weak = JS_UNDEFINED;
    found->second.destroyed = true;
  }
  gui.controls.clear();
}

bool define_gui_members(JSContext* context, JSValue object, GuiModuleBinding* binding);
bool define_ctrl_members(JSContext* context, JSValue object, GuiModuleBinding* binding);

JSValue build_gui_object(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                         const std::uint64_t gui_id);
JSValue build_ctrl_object(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                          const std::uint64_t ctrl_id);

JSValue make_weak(JSContext* context, JSValue target);

// Resolves an entry to its JS object. Identity is preserved while the script
// still holds it; a deref miss (collected but the entry survives because the
// Win32 child/window is still alive) rebuilds a fresh shell for the same
// entry and re-registers it.
JSValue gui_object_for(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                       const std::uint64_t gui_id) {
  const auto found = state->guis.find(gui_id);
  if (found == state->guis.end() || found->second.destroyed) {
    throw_invalid_state(context, kDestroyed);
    return JS_EXCEPTION;
  }
  JSValue live = weak_deref(context, found->second.weak);
  if (JS_IsException(live)) return JS_EXCEPTION;
  if (!JS_IsUndefined(live)) return live;
  return build_gui_object(context, state, binding, gui_id);
}

JSValue ctrl_object_for(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                        const std::uint64_t ctrl_id) {
  const auto found = state->controls.find(ctrl_id);
  if (found == state->controls.end()) {
    throw_invalid_state(context, kNoControl);
    return JS_EXCEPTION;
  }
  if (found->second.destroyed) {
    throw_invalid_state(context, kNoControl);
    return JS_EXCEPTION;
  }
  JSValue live = weak_deref(context, found->second.weak);
  if (JS_IsException(live)) return JS_EXCEPTION;
  if (!JS_IsUndefined(live)) return live;
  return build_ctrl_object(context, state, binding, ctrl_id);
}

// ---- pump operations --------------------------------------------------------

// Everything an async member body needs, captured on the JS thread before
// the frame goes away.
struct GuiCall {
  GuiService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  std::int64_t deadline{0};
  rime::core::CancellationToken cancel;
};

AsyncOutcome null_out(const Error& error) {
  return error.ok() ? async_success("null") : async_failure(error);
}

// Marks the Gui materialized, captures the wiring and runs `body` on the
// worker lane behind the standard capability gate. `body` must capture every
// value it uses by value - it outlives this frame.
template <typename Body>
JSValue start_gui_op(JSContext* context, GuiJsState* state, GuiJs& gui,
                     const ActionOptions& action, Body body) {
  gui.materialized = true;
  GuiCall call;
  call.service = state->service;
  call.kernel = state->kernel;
  call.deadline = wait_deadline_unix_ms(action.deadline_ms);
  if (action.cancellation_id != 0) {
    call.cancel = state->host->cancellation_token(action.cancellation_id);
  }
  return start_async(
      context,
      [call, body = std::move(body)]() mutable -> AsyncOutcome {
        if (!call.kernel->allows(kUiCreateCapability)) {
          return capability_denied(kUiCreateCapability);
        }
        return body(*call.service, call.deadline, call.cancel);
      },
      action.cancellation_id);
}

// Event interest the pump should push for this Gui, derived from the same
// handler table the dispatch reads (gui-menu.md §4.3: mirrored wholesale).
void build_interest(const GuiJs& gui, GuiService::GuiEventInterest& out) {
  for (const auto& [route, handlers] : gui.handlers) {
    if (handlers.empty()) continue;
    switch (route.kind) {
      case 'e':
        if (route.target == 0) {
          if (route.event == "Close") out.gui_bits |= GuiService::kGuiEventBitClose;
          else if (route.event == "Resize") out.gui_bits |= GuiService::kGuiEventBitResize;
          else if (route.event == "Escape") out.gui_bits |= GuiService::kGuiEventBitEscape;
          else if (route.event == "ContextMenu") {
            out.gui_bits |= GuiService::kGuiEventBitContextMenu;
          } else if (route.event == "DropFiles") {
            out.gui_bits |= GuiService::kGuiEventBitDropFiles;
          }
        } else if (route.event == "Click" || route.event == "Change" ||
                   route.event == "ItemSelect") {
          const auto found = out.ctrl_bits.find(route.target);
          std::uint32_t bits = found == out.ctrl_bits.end() ? 0u : found->second;
          bits |= route.event == "Click"      ? GuiService::kCtrlEventBitClick
                  : route.event == "Change"   ? GuiService::kCtrlEventBitChange
                                              : GuiService::kCtrlEventBitItemSelect;
          out.ctrl_bits[route.target] = bits;
        }
        break;
      case 'm':
        out.messages[static_cast<std::uint32_t>(route.selector)].insert(route.target);
        break;
      case 'c':
        out.commands[static_cast<int>(route.selector)].insert(route.target);
        break;
      case 'n':
        out.notifies[static_cast<int>(route.selector)].insert(route.target);
        break;
      default:
        break;
    }
  }
}

// Snapshot carried on every pump call so record creation applies whatever
// the script had registered at build time.
GuiService::GuiSpec make_spec(const GuiJs& gui) {
  GuiService::GuiSpec spec;
  spec.id = gui.id;
  spec.title = gui.title;
  spec.style = gui.style.style;
  spec.ex_style = gui.style.ex_style;
  spec.dpi_scale = gui.style.dpi_scale.value_or(true);
  spec.channel = gui.channel;
  build_interest(gui, spec.interest);
  return spec;
}

// ---- the dispatch channel ---------------------------------------------------

// Resolves a handler entry to something callable: a function as registered,
// or a method name looked up on the Gui's eventObj (§4.3).
JSValue resolve_handler(JSContext* context, const GuiJs& gui, const GuiJsHandler& handler,
                        bool& ok) {
  ok = false;
  if (JS_IsFunction(context, handler.fn)) {
    ok = true;
    return JS_DupValue(context, handler.fn);
  }
  if (!JS_IsString(handler.fn)) return JS_UNDEFINED;
  if (!JS_IsObject(gui.event_obj)) {
    JS_ThrowTypeError(context, "eventObj is required for string callbacks");
    return JS_EXCEPTION;
  }
  JSValue resolved = JS_GetPropertyStr(context, gui.event_obj, handler.name.c_str());
  if (JS_IsException(resolved)) return JS_EXCEPTION;
  if (!JS_IsFunction(context, resolved)) {
    JS_FreeValue(context, resolved);
    JS_ThrowTypeError(context, "eventObj.%s is not a function", handler.name.c_str());
    return JS_EXCEPTION;
  }
  ok = true;
  return resolved;
}

JSValue gui_channel_dispatch(JSContext* context, JSValueConst this_val, int argc,
                             JSValueConst* argv, int, void* opaque) {
  (void)this_val;
  GuiModuleBinding* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_or_null(binding);
  if (!state) return JS_UNDEFINED;
  if (argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

  JSValue payload = argv[0];
  JSValue gui_value = JS_GetPropertyStr(context, payload, "gui");
  JSValue target_value = JS_GetPropertyStr(context, payload, "target");
  JSValue event_value = JS_GetPropertyStr(context, payload, "event");
  JSValue args_value = JS_GetPropertyStr(context, payload, "args");
  if (JS_IsException(gui_value) || JS_IsException(target_value) || JS_IsException(event_value) ||
      JS_IsException(args_value)) {
    JS_FreeValue(context, gui_value);
    JS_FreeValue(context, target_value);
    JS_FreeValue(context, event_value);
    JS_FreeValue(context, args_value);
    return JS_EXCEPTION;
  }

  GuiJsRoute route;
  std::uint64_t gui_id = 0;
  std::int64_t raw_gui = 0;
  bool usable = JS_IsNumber(gui_value) && JS_ToInt64(context, &raw_gui, gui_value) == 0 &&
                raw_gui > 0;
  if (usable) gui_id = static_cast<std::uint64_t>(raw_gui);

  if (usable && JS_IsString(target_value)) {
    const char* target_text = JS_ToCString(context, target_value);
    if (!target_text) usable = false;
    else {
      usable = std::string(target_text) == "gui";
      JS_FreeCString(context, target_text);
      route.target = 0;
    }
  } else if (usable) {
    std::int64_t raw_target = 0;
    usable = JS_IsNumber(target_value) && JS_ToInt64(context, &raw_target, target_value) == 0 &&
             raw_target >= 0;
    if (usable) route.target = static_cast<std::uint64_t>(raw_target);
  }

  std::string event;
  if (usable) {
    if (JS_IsString(event_value)) {
      const char* text = JS_ToCString(context, event_value);
      if (!text) {
        usable = false;
      } else {
        event = text;
        JS_FreeCString(context, text);
      }
    } else {
      usable = false;
    }
  }

  std::int64_t args_length = 0;
  if (usable) {
    JSValue length = JS_GetPropertyStr(context, args_value, "length");
    if (JS_IsException(length)) usable = false;
    else {
      if (JS_IsNumber(length)) (void)JS_ToInt64(context, &args_length, length);
      JS_FreeValue(context, length);
      if (args_length < 0) args_length = 0;
      if (args_length > 8) args_length = 8;
    }
  }

  std::vector<JSValue> args;
  if (usable) {
    if (event == "Message") {
      route.kind = 'm';
      std::int64_t message = -1;
      if (args_length >= 3) {
        JSValue message_value = JS_GetPropertyInt64(context, args_value, 2);
        if (!JS_IsException(message_value)) {
          usable = JS_IsNumber(message_value) &&
                   JS_ToInt64(context, &message, message_value) == 0 && message >= 0;
          JS_FreeValue(context, message_value);
        } else {
          usable = false;
        }
      } else {
        usable = false;
      }
      if (usable) route.selector = message;
    } else if (event == "Command" || event == "Notify") {
      route.kind = event == "Command" ? 'c' : 'n';
      std::int64_t code = 0;
      if (args_length >= 1) {
        JSValue code_value = JS_GetPropertyInt64(context, args_value, 0);
        if (!JS_IsException(code_value)) {
          usable = JS_IsNumber(code_value) && JS_ToInt64(context, &code, code_value) == 0;
          JS_FreeValue(context, code_value);
        } else {
          usable = false;
        }
      } else {
        usable = false;
      }
      if (usable) route.selector = code;
    } else {
      route.kind = 'e';
      route.event = event;
    }
  }

  JS_FreeValue(context, gui_value);
  JS_FreeValue(context, target_value);
  JS_FreeValue(context, event_value);

  if (!usable) {
    JS_FreeValue(context, args_value);
    return JS_UNDEFINED;
  }
  // `__closed` is internal bookkeeping (the window is gone); it never
  // reaches a script handler. Instead it releases the JS-side half through
  // the idempotent release path, so an X-closed window stops pinning the
  // unload gate (AGENTS shutdown rule) and later verbs fail InvalidState.
  if (event == "__closed") {
    JS_FreeValue(context, args_value);
    const auto closed_it = state->guis.find(gui_id);
    if (closed_it != state->guis.end()) release_gui(context, state, closed_it->second);
    return JS_UNDEFINED;
  }

  const auto gui_it = state->guis.find(gui_id);
  if (gui_it == state->guis.end() || gui_it->second.destroyed) {
    JS_FreeValue(context, args_value);
    return JS_UNDEFINED;
  }
  const GuiJs& gui = gui_it->second;
  const auto route_it = gui.handlers.find(route);
  if (route_it == gui.handlers.end() || route_it->second.empty()) {
    JS_FreeValue(context, args_value);
    return JS_UNDEFINED;
  }

  JSValue target_object = JS_UNDEFINED;
  if (route.target == 0) target_object = gui_object_for(context, state, binding, gui.id);
  else target_object = ctrl_object_for(context, state, binding, route.target);
  if (JS_IsException(target_object)) {
    JS_FreeValue(context, args_value);
    return JS_EXCEPTION;
  }

  // Copy: a handler that destroys the Gui mutates the table under us.
  const std::vector<GuiJsHandler> handlers = route_it->second;
  JSValue result = JS_UNDEFINED;
  for (const GuiJsHandler& handler : handlers) {
    bool ok = false;
    JSValue resolved = resolve_handler(context, gui, handler, ok);
    if (JS_IsException(resolved)) {
      JSValue exception = JS_GetException(context);
      const char* text = JS_ToCString(context, exception);
      if (text && state->host) (void)state->host->record("gui", text);
      if (text) JS_FreeCString(context, text);
      JS_FreeValue(context, exception);
      continue;
    }
    if (!ok) continue;
    std::vector<JSValueConst> call_args;
    call_args.push_back(target_object);
    for (std::int64_t index = 0; index < args_length; ++index) {
      JSValue item = JS_GetPropertyInt64(context, args_value, index);
      if (JS_IsException(item)) {
        item = JS_UNDEFINED;
      }
      call_args.push_back(item);
    }
    JSValue ignored =
        JS_Call(context, resolved, JS_UNDEFINED, static_cast<int>(call_args.size()),
                call_args.data());
    for (std::size_t index = 1; index < call_args.size(); ++index) {
      JS_FreeValue(context, call_args[index]);
    }
    JS_FreeValue(context, resolved);
    if (JS_IsException(ignored)) {
      JSValue exception = JS_GetException(context);
      const char* text = JS_ToCString(context, exception);
      if (text && state->host) (void)state->host->record("gui", text);
      if (text) JS_FreeCString(context, text);
      JS_FreeValue(context, exception);
      continue;
    }
    JS_FreeValue(context, ignored);
    result = JS_UNDEFINED;
  }

  JS_FreeValue(context, target_object);
  JS_FreeValue(context, args_value);
  return result;
}

// Menu channel dispatch (batch 3c): each Menu owns a host channel like each
// Gui owns one, but the payload target names the menu ("menu:<id>") and the
// only event is MenuSelect with args [command, name]. Runs on the JS thread.
void invoke_menu_callback(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                          GuiJsMenu& menu, const std::string& name);
void release_menu(JSContext* context, GuiJsState* state, GuiJsMenu& menu);
void release_image_list(JSContext* context, GuiJsState* state, GuiJsImageList& list);
GuiJsState* ensure_gui_state(JSContext* context, GuiModuleBinding* binding);
template <typename Body>
JSValue start_menu_op(JSContext* context, GuiJsState* state, const ActionOptions& action,
                      Body body);
JSValue menu_channel_dispatch(JSContext* context, JSValueConst this_val, int argc,
                              JSValueConst* argv, int, void* opaque) {
  (void)this_val;
  GuiModuleBinding* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_or_null(binding);
  if (!state) return JS_UNDEFINED;
  if (argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

  JSValue payload = argv[0];
  JSValue target_value = JS_GetPropertyStr(context, payload, "target");
  JSValue event_value = JS_GetPropertyStr(context, payload, "event");
  JSValue args_value = JS_GetPropertyStr(context, payload, "args");
  if (JS_IsException(target_value) || JS_IsException(event_value) || JS_IsException(args_value)) {
    JS_FreeValue(context, target_value);
    JS_FreeValue(context, event_value);
    JS_FreeValue(context, args_value);
    return JS_EXCEPTION;
  }
  std::uint64_t menu_id = 0;
  bool usable = false;
  if (JS_IsString(target_value)) {
    const char* text = JS_ToCString(context, target_value);
    if (text) {
      const std::string target(text);
      JS_FreeCString(context, text);
      constexpr const char* kPrefix = "menu:";
      if (target.rfind(kPrefix, 0) == 0) {
        const std::string digits = target.substr(std::strlen(kPrefix));
        bool numeric = !digits.empty() && digits.size() <= 19;
        for (const char c : digits) numeric = numeric && c >= '0' && c <= '9';
        if (numeric) {
          const std::int64_t raw = std::stoll(digits);
          if (raw > 0) {
            menu_id = static_cast<std::uint64_t>(raw);
            usable = true;
          }
        }
      }
    }
  }
  std::string event;
  if (usable && JS_IsString(event_value)) {
    const char* text = JS_ToCString(context, event_value);
    if (text) {
      event = text;
      JS_FreeCString(context, text);
      usable = event == "MenuSelect";
    } else {
      usable = false;
    }
  } else {
    usable = false;
  }
  std::string name;
  if (usable) {
    JSValue length = JS_GetPropertyStr(context, args_value, "length");
    std::int64_t args_length = 0;
    if (!JS_IsException(length)) {
      if (JS_IsNumber(length)) (void)JS_ToInt64(context, &args_length, length);
      JS_FreeValue(context, length);
    }
    if (args_length >= 2) {
      JSValue name_value = JS_GetPropertyInt64(context, args_value, 1);
      if (!JS_IsException(name_value)) {
        if (JS_IsString(name_value)) {
          const char* text = JS_ToCString(context, name_value);
          if (text) {
            name = text;
            JS_FreeCString(context, text);
          } else {
            usable = false;
          }
        } else {
          usable = false;
        }
        JS_FreeValue(context, name_value);
      } else {
        usable = false;
      }
    } else {
      usable = false;
    }
  }
  JS_FreeValue(context, target_value);
  JS_FreeValue(context, event_value);
  JS_FreeValue(context, args_value);
  if (!usable) return JS_UNDEFINED;
  const auto found = state->menus.find(menu_id);
  if (found == state->menus.end() || found->second.destroyed) return JS_UNDEFINED;
  invoke_menu_callback(context, state, binding, found->second, name);
  return JS_UNDEFINED;
}

// The FinalizationRegistry cleanup: the Gui object is gone, so tear the
// window down (§4.5 GC path). Runs as a host job on the JS thread, never
// inside the GC itself.
JSValue ui_gui_gc(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv, int,
                  void* opaque) {
  (void)this_val;
  GuiModuleBinding* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_or_null(binding);
  if (!state || argc < 1) return JS_UNDEFINED;
  std::int64_t held_id = 0;
  if (!JS_IsNumber(argv[0]) || JS_ToInt64(context, &held_id, argv[0]) != 0 || held_id == 0) {
    return JS_UNDEFINED;
  }
  if (held_id < 0) {
    // Menu holding (negative menu id shares the Gui registry).
    const auto found = state->menus.find(static_cast<std::uint64_t>(-held_id));
    if (found == state->menus.end() || found->second.destroyed) return JS_UNDEFINED;
    GuiJsMenu& menu = found->second;
    release_menu(context, state, menu);
    const std::uint64_t menu_id = menu.id;
    ActionOptions action;
    fire_and_forget(context,
                    start_menu_op(context, state, action,
                                  [menu_id](GuiService& service, const std::int64_t deadline,
                                            rime::core::CancellationToken cancel) {
                                    return null_out(service.menu_destroy(menu_id, deadline,
                                                                         cancel));
                                  }));
    return JS_UNDEFINED;
  }
  std::int64_t gui_id = held_id;
  const auto found = state->guis.find(static_cast<std::uint64_t>(gui_id));
  if (found == state->guis.end() || found->second.destroyed) return JS_UNDEFINED;
  GuiJs& gui = found->second;
  const GuiService::GuiSpec spec = make_spec(gui);
  release_gui(context, state, gui);
  ActionOptions action;
  fire_and_forget(context,
                  start_gui_op(context, state, gui, action,
                               [spec](GuiService& service, const std::int64_t deadline,
                                      rime::core::CancellationToken cancel) {
                                 return null_out(service.gui_destroy(spec.id, deadline, cancel));
                               }));
  return JS_UNDEFINED;
}

}  // namespace

// ---- GuiJsState -------------------------------------------------------------

void GuiJsState::teardown() {
  if (closed.exchange(true)) return;
  JSContext* context = js_context;
  if (!context) return;
  for (auto& [id, gui] : guis) (void)release_gui(context, this, gui);
  guis.clear();
  channels.clear();
  for (auto& [id, menu] : menus) (void)release_menu(context, this, menu);
  menus.clear();
  for (auto& [id, list] : image_lists) (void)release_image_list(context, this, list);
  image_lists.clear();
  JS_FreeValue(context, image_registry);
  image_registry = JS_UNDEFINED;
  for (auto& [id, control] : controls) {
    JS_FreeValue(context, control.weak);
    control.weak = JS_UNDEFINED;
    control.destroyed = true;
  }
  controls.clear();
  JS_FreeValue(context, registry);
  registry = JS_UNDEFINED;
}

namespace {

// ---- Gui construction -------------------------------------------------------

struct GuiInit {
  GuiService::GuiStyleOptions style;
  std::string title;
  JSValue event_obj{JS_UNDEFINED};

  void free(JSContext* context) {
    JS_FreeValue(context, event_obj);
    event_obj = JS_UNDEFINED;
  }
};

// Validation only - runs before an entry is allocated so a bad option string
// never leaves a half-built Gui behind (objects.json `__New` error column).
bool parse_gui_init(JSContext* context, const int argc, JSValueConst* argv, GuiInit& out,
                    const char* signature) {
  std::string options;
  if (!optional_string_arg(context, argc, argv, 0, signature, "options", options)) return false;
  if (!optional_string_arg(context, argc, argv, 1, signature, "title", out.title)) return false;
  if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
    if (!JS_IsObject(argv[2])) {
      JS_ThrowTypeError(context, "%s: eventObj must be an object", signature);
      return false;
    }
    out.event_obj = JS_DupValue(context, argv[2]);
  }
  if (const Error error = GuiService::parse_gui_options(options, out.style); !error.ok()) {
    throw_value_error(context, error.message);
    out.free(context);
    return false;
  }
  return true;
}

// Applies a validated init: style, title, eventObj plus the one dispatch
// channel every Gui owns (§4.3).
bool apply_gui_init(JSContext* context, GuiJsState* state, GuiModuleBinding* binding, GuiJs& gui,
                    GuiInit& init) {
  gui.style = init.style;
  gui.title = init.title;
  gui.event_obj = init.event_obj;
  init.event_obj = JS_UNDEFINED;
  JSValue closure =
      JS_NewCClosure(context, gui_channel_dispatch, "guiChannel", nullptr, 1, 0, binding);
  if (JS_IsException(closure)) return false;
  std::uint64_t channel = 0;
  if (const Error error = state->host->add_callback(closure, channel); !error.ok()) {
    JS_ThrowInternalError(context, "%s", error.message.c_str());
    return false;
  }
  gui.channel = channel;
  state->channels[channel] = gui.id;
  return true;
}

// ---- shared member plumbing -------------------------------------------------

// Registers (or unregisters) one handler on the JS thread, then returns a
// Promise that mirrors the resulting interest into the pump (§4.3).
JSValue register_handler(JSContext* context, GuiJsState* state, GuiJs& gui, GuiJsRoute route,
                         JSValueConst fn_value,
                         const int add_remove, const char* signature, const ActionOptions& action) {
  if (add_remove < -1 || add_remove > 1) {
    throw_value_error(context, std::string(signature) + ": addRemove must be -1, 0 or 1");
    return JS_EXCEPTION;
  }
  GuiJsHandler handler;
  handler.add_remove = add_remove;
  if (JS_IsFunction(context, fn_value)) {
    handler.fn = JS_DupValue(context, fn_value);
  } else if (JS_IsString(fn_value)) {
    const char* text = JS_ToCString(context, fn_value);
    if (!text) return JS_EXCEPTION;
    handler.name = text;
    JS_FreeCString(context, text);
    if (handler.name.empty()) {
      JS_ThrowTypeError(context, "%s: callback name must not be empty", signature);
      return JS_EXCEPTION;
    }
    if (!JS_IsObject(gui.event_obj)) {
      JS_ThrowTypeError(context, "%s: a string callback needs eventObj", signature);
      return JS_EXCEPTION;
    }
    handler.fn = JS_DupValue(context, fn_value);
  } else {
    JS_ThrowTypeError(context, "%s: callback must be a function or a string", signature);
    return JS_EXCEPTION;
  }

  auto& handlers = gui.handlers[route];
  if (add_remove == 0) {
    // Removal form: the first entry whose callback matches (script_gui.cpp
    // :2561-2568 removes one matching instance per call).
    bool removed = false;
    for (auto iterator = handlers.begin(); iterator != handlers.end(); ++iterator) {
          const bool matches =
              JS_IsString(handler.fn)
                  ? (JS_IsString(iterator->fn) && iterator->name == handler.name)
                  : (JS_IsFunction(context, iterator->fn) &&
                     JS_IsStrictEqual(context, iterator->fn, handler.fn));
      if (matches) {
        JS_FreeValue(context, iterator->fn);
        handlers.erase(iterator);
        removed = true;
        break;
      }
    }
    JS_FreeValue(context, handler.fn);
    if (!removed && handlers.empty()) gui.handlers.erase(route);
    if (!removed) {
      // Not an error in AHK either - an unmatched removal is a no-op - but the
      // pump still needs the (possibly now empty) snapshot.
    }
  } else if (add_remove > 0) {
    // 1 = "call it first" (script_gui.cpp:2579-2581).
    handlers.insert(handlers.begin(), std::move(handler));
  } else {
    handlers.push_back(std::move(handler));
  }

  GuiService::GuiSpec spec = make_spec(gui);
  // Stamp the mirror so concurrent registrations converge pump-side: the
  // JS thread runs them in order, so the last stamp carries the full table
  // and stale arrivals drop (batch 3e).
  spec.interest.seq = state->next_interest_seq++;
  return start_gui_op(context, state, gui, action,
                      [spec](GuiService& service, const std::int64_t deadline,
                             rime::core::CancellationToken cancel) {
                        return null_out(service.gui_set_event_interest(spec, spec.interest,
                                                                      deadline, cancel));
                      });
}

// `promise.then(onFulfilled, onRejected)` through the public API only:
// quickjs.h does not export the atom enum (JS_ATOM_then lives in
// quickjs-atom.h, which only expands under a caller's DEF macro), so the
// `then` method is read by name. Consumes `promise`, `on_ok` and `on_error`.
JSValue then_chain(JSContext* context, JSValue promise, JSValue on_ok, JSValue on_error) {
  JSValue then_fn = JS_GetPropertyStr(context, promise, "then");
  if (JS_IsException(then_fn)) {
    JS_FreeValue(context, on_ok);
    JS_FreeValue(context, on_error);
    JS_FreeValue(context, promise);
    return JS_EXCEPTION;
  }
  JSValueConst handlers[2] = {on_ok, on_error};
  JSValue derived = JS_Call(context, then_fn, promise, 2, handlers);
  JS_FreeValue(context, then_fn);
  JS_FreeValue(context, on_ok);
  JS_FreeValue(context, on_error);
  JS_FreeValue(context, promise);
  return derived;
}

// The well-known Symbol.iterator atom, read off the global object for the
// same reason `then` is read by name (see then_chain).
JSAtom iterator_atom(JSContext* context) {
  JSValue global = JS_GetGlobalObject(context);
  if (JS_IsException(global)) return JS_ATOM_NULL;
  JSValue symbol_ns = JS_GetPropertyStr(context, global, "Symbol");
  JS_FreeValue(context, global);
  if (JS_IsException(symbol_ns)) return JS_ATOM_NULL;
  JSValue iterator = JS_GetPropertyStr(context, symbol_ns, "iterator");
  JS_FreeValue(context, symbol_ns);
  if (JS_IsException(iterator)) return JS_ATOM_NULL;
  const JSAtom atom = JS_ValueToAtom(context, iterator);
  JS_FreeValue(context, iterator);
  return atom;
}

// ---- Add --------------------------------------------------------------------

JSValue gui_add_fulfilled(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                          int magic, JSValueConst* func_data);
JSValue gui_add_rejected(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                         int magic, JSValueConst* func_data);

// `generic_add` selects the `Add(controlType, ...)` form; otherwise `kind` is
// the fixed kind of the AddXxx sugar. The magic enum lives further down next
// to the name tables, so the sugar already resolved its kind before entry.
JSValue gui_add_impl(JSContext* context, GuiJsState* state, GuiJs& gui,
                     const bool generic_add, const ControlKind kind_in, const int argc,
                     JSValueConst* argv) {
  int end = 0;
  const int action_index = split_action(context, argc, argv, end);
  ActionOptions action;
  if (!parse_action_at(context, argv, action_index, action)) return JS_EXCEPTION;

  ControlKind kind = kind_in;
  if (generic_add) {
    if (end < 1) return JS_ThrowTypeError(context, "Add(controlType, options?, content?)");
    std::string type;
    if (!read_string(context, argv[0], "controlType", type)) return JS_EXCEPTION;
    if (const Error error = GuiService::parse_control_type(type, kind); !error.ok()) {
      throw_value_error(context, error.message);
      return JS_EXCEPTION;
    }
  }

  GuiService::GuiControlOptions options;
  // `Add(controlType, options?, content?)` shifts both by one; the AddXxx
  // sugars start at argv[0] (objects.json parameters column).
  const int base = generic_add ? 1 : 0;
  if (end > base && !JS_IsUndefined(argv[base]) && !JS_IsNull(argv[base])) {
    std::string text;
    if (!read_string(context, argv[base], "options", text)) return JS_EXCEPTION;
    if (const Error error = GuiService::parse_control_options(text, options); !error.ok()) {
      throw_value_error(context, error.message);
      return JS_EXCEPTION;
    }
  }

  GuiService::GuiControlSpec spec;
  spec.gui = make_spec(gui);
  spec.ctrl_id = state->next_ctrl_id++;
  spec.kind = kind;
  spec.options = options;
  if (end > base + 1) {
    if (!read_control_content(context, argv[base + 1], kind, spec.text, spec.has_number,
                              spec.number, spec.items, spec.has_items)) {
      return JS_EXCEPTION;
    }
  }

  const std::uint64_t gui_id = gui.id;
  const std::uint64_t ctrl_id = spec.ctrl_id;
  const std::string vname = options.vname;
  const int kind_value = static_cast<int>(kind);
  JSValue promise =
      start_gui_op(context, state, gui, action,
                   [spec](GuiService& service, const std::int64_t deadline,
                          rime::core::CancellationToken cancel) {
                     return null_out(service.gui_add(spec, deadline, cancel));
                   });
  if (JS_IsException(promise)) return JS_EXCEPTION;

  JSValue data[4];
  data[0] = JS_NewInt64(context, static_cast<std::int64_t>(gui_id));
  data[1] = JS_NewInt64(context, static_cast<std::int64_t>(ctrl_id));
  data[2] = JS_NewInt32(context, kind_value);
  data[3] = JS_NewStringLen(context, vname.data(), vname.size());
  for (JSValue& item : data) {
    if (JS_IsException(item)) {
      for (JSValue& other : data) JS_FreeValue(context, other);
      JS_FreeValue(context, promise);
      return JS_EXCEPTION;
    }
  }
  JSValue on_ok =
      JS_NewCFunctionData2(context, gui_add_fulfilled, "onGuiControl", 1, 0, 4, data);
  JSValue on_error =
      JS_NewCFunctionData2(context, gui_add_rejected, "onGuiControlError", 1, 0, 4, data);
  for (JSValue& item : data) JS_FreeValue(context, item);
  if (JS_IsException(on_ok) || JS_IsException(on_error)) {
    JS_FreeValue(context, on_ok);
    JS_FreeValue(context, on_error);
    JS_FreeValue(context, promise);
    return JS_EXCEPTION;
  }
  return then_chain(context, promise, on_ok, on_error);
}

// Publishes the control entry only once the pump has actually created the
// Win32 child, so a rejected Add never leaves an orphan object behind.
JSValue gui_add_fulfilled(JSContext* context, JSValueConst, int, JSValueConst* argv, int,
                          JSValueConst* func_data) {
  (void)argv;
  GuiModuleBinding* binding = binding_of(context);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  std::int64_t gui_id = 0;
  std::int64_t ctrl_id = 0;
  std::int64_t kind_value = 0;
  if (JS_ToInt64(context, &gui_id, func_data[0]) ||
      JS_ToInt64(context, &ctrl_id, func_data[1]) ||
      JS_ToInt64(context, &kind_value, func_data[2])) {
    return JS_EXCEPTION;
  }
  const char* name_text = JS_ToCString(context, func_data[3]);
  if (!name_text) return JS_EXCEPTION;
  std::string name(name_text);
  JS_FreeCString(context, name_text);

  const auto gui_it = state->guis.find(static_cast<std::uint64_t>(gui_id));
  if (gui_it == state->guis.end() || gui_it->second.destroyed) {
    throw_invalid_state(context, kDestroyed);
    return JS_EXCEPTION;
  }
  GuiJs& gui = gui_it->second;

  GuiJsControl entry;
  entry.id = static_cast<std::uint64_t>(ctrl_id);
  entry.gui_id = gui.id;
  entry.kind = static_cast<ControlKind>(kind_value);
  entry.name = name;
  int ordinal = 1;
  for (const std::uint64_t sibling : gui.controls) {
    const auto found = state->controls.find(sibling);
    if (found != state->controls.end() && !found->second.destroyed &&
        found->second.kind == entry.kind) {
      ++ordinal;
    }
  }
  entry.ordinal = ordinal;
  const std::uint64_t id = entry.id;
  state->controls[id] = entry;
  gui.controls.push_back(id);
  JSValue object = build_ctrl_object(context, state, binding, id);
  if (JS_IsException(object)) {
    JS_FreeValue(context, state->controls[id].weak);
    state->controls[id].weak = JS_UNDEFINED;
    state->controls.erase(id);
    gui.controls.pop_back();
    return JS_EXCEPTION;
  }
  return object;
}

// Rejects the derived promise with exactly the reason the pump produced, so
// the Error keeps its kernel `code` and message.
JSValue gui_add_rejected(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         JSValueConst*) {
  if (argc < 1) return JS_UNDEFINED;
  return JS_Throw(context, JS_DupValue(context, argv[0]));
}

// ---- Gui members ------------------------------------------------------------

enum GuiMethodMagic {
  kGuiNew = 0,
  kGuiAdd,
  kGuiAddButton,
  kGuiAddCheckBox,
  kGuiAddEdit,
  kGuiAddGroupBox,
  kGuiAddPicture,
  kGuiAddProgress,
  kGuiAddRadio,
  kGuiAddText,
  kGuiAddListBox,
  kGuiAddComboBox,
  kGuiAddDDL,
  kGuiAddDropDownList,
  kGuiAddDateTime,
  kGuiAddMonthCal,
  kGuiAddSlider,
  kGuiAddUpDown,
  kGuiAddTab,
  kGuiAddTab2,
  kGuiAddPic,
  kGuiAddLink,
  kGuiAddHotkey,
  kGuiAddListView,
  kGuiAddTreeView,
  kGuiAddStatusBar,
  kGuiDestroy,
  kGuiFlash,
  kGuiGetClientPos,
  kGuiGetPos,
  kGuiHide,
  kGuiMaximize,
  kGuiMinimize,
  kGuiMove,
  kGuiOnEvent,
  kGuiOnMessage,
  kGuiOpt,
  kGuiRestore,
  kGuiSetFont,
  kGuiShow,
  kGuiSubmit,
  kGuiItem,
  kGuiEnum,
  kGuiIterator,
  kGuiMethodCount
};

constexpr const char* kGuiMethodNames[] = {
    "__New",     "Add",       "AddButton",  "AddCheckBox", "AddEdit",       "AddGroupBox",
    "AddPicture", "AddProgress", "AddRadio", "AddText",    "AddListBox",    "AddComboBox",
    "AddDDL",    "AddDropDownList", "AddDateTime", "AddMonthCal", "AddSlider", "AddUpDown",
    "AddTab", "AddTab2", "AddPic", "AddLink", "AddHotkey", "AddListView", "AddTreeView",
    "AddStatusBar",
    "Destroy", "Flash",
    "GetClientPos", "GetPos", "Hide",       "Maximize",    "Minimize",      "Move",
    "OnEvent",   "OnMessage", "Opt",        "Restore",     "SetFont",       "Show",
    "Submit",    "__Item",    "__Enum",     "[Symbol.iterator]"};

constexpr int kGuiMethodLengths[] = {
    3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    0, 1, 0, 0, 0, 0, 0, 4, 3, 3, 1, 0, 2, 1, 1, 1, 1, 0};

static_assert(sizeof(kGuiMethodNames) / sizeof(kGuiMethodNames[0]) == kGuiMethodCount);
static_assert(sizeof(kGuiMethodLengths) / sizeof(kGuiMethodLengths[0]) == kGuiMethodCount);

std::optional<ControlKind> fixed_add_kind(const int magic) {
  switch (magic) {
    case kGuiAddButton:
      return ControlKind::Button;
    case kGuiAddCheckBox:
      return ControlKind::CheckBox;
    case kGuiAddEdit:
      return ControlKind::Edit;
    case kGuiAddGroupBox:
      return ControlKind::GroupBox;
    case kGuiAddPicture:
      return ControlKind::Picture;
    case kGuiAddProgress:
      return ControlKind::Progress;
    case kGuiAddRadio:
      return ControlKind::Radio;
    case kGuiAddText:
      return ControlKind::Text;
    case kGuiAddListBox:
      return ControlKind::ListBox;
    case kGuiAddComboBox:
      return ControlKind::ComboBox;
    case kGuiAddDDL:
    case kGuiAddDropDownList:
      // AddDDL is a ledger alias for the DropDownList kind (same control,
      // two registry rows); there is no fourth list kind.
      return ControlKind::DropDownList;
    case kGuiAddDateTime:
      return ControlKind::DateTime;
    case kGuiAddMonthCal:
      return ControlKind::MonthCal;
    case kGuiAddSlider:
      return ControlKind::Slider;
    case kGuiAddUpDown:
      return ControlKind::UpDown;
    case kGuiAddTab:
    case kGuiAddTab2:
      // AddTab2 aliases Tab (AHK resolves it early); one kind, two rows.
      return ControlKind::Tab;
    case kGuiAddPic:
      // AddPic aliases Picture (the "pic" spelling parses to it already).
      return ControlKind::Picture;
    case kGuiAddLink:
      return ControlKind::Link;
    case kGuiAddHotkey:
      return ControlKind::Hotkey;
    case kGuiAddListView:
      return ControlKind::ListView;
    case kGuiAddTreeView:
      return ControlKind::TreeView;
    case kGuiAddStatusBar:
      return ControlKind::StatusBar;
    default:
      return std::nullopt;
  }
}

enum GuiPropMagic {
  kGuiPropTitle = 0,
  kGuiPropBackColor,
  kGuiPropMarginX,
  kGuiPropMarginY,
  kGuiPropFocusedCtrl,
  kGuiPropName,
  kGuiPropHwnd,
  kGuiPropFontHandle,
  kGuiPropMenuBar,
  kGuiPropDpi,
  kGuiPropCount
};

constexpr const char* kGuiPropNames[] = {"Title", "BackColor", "MarginX", "MarginY",
                                         "FocusedCtrl", "Name", "Hwnd", "FontHandle",
                                         "MenuBar", "Dpi"};
constexpr bool kGuiPropWritable[] = {true, true, true, true, false,
                                     true, true, true, true, false};

static_assert(sizeof(kGuiPropNames) / sizeof(kGuiPropNames[0]) == kGuiPropCount);
static_assert(sizeof(kGuiPropWritable) / sizeof(kGuiPropWritable[0]) == kGuiPropCount);

// Batch-2 event names (§4.3: the set closes with this section; outside it is
// a ValueError and that is the "not implemented" signal of §4.8). Batch 3e
// adds Escape/ContextMenu/DropFiles (Gui) and ItemSelect (ListView/TreeView);
// DblClick stays on the OnNotify transport (raw notify codes, documented),
// it is not a separate event.
bool gui_event_name(const std::string& name) {
  return name == "Close" || name == "Resize" || name == "Escape" || name == "ContextMenu" ||
         name == "DropFiles";
}
bool ctrl_event_name(const std::string& name) {
  return name == "Click" || name == "Change" || name == "ItemSelect";
}

// ---- object builders (forward: they reference the member tables) -----------

bool define_gui_members(JSContext* context, JSValue object, GuiModuleBinding* binding);
bool define_ctrl_members(JSContext* context, JSValue object, GuiModuleBinding* binding);
bool define_menu_members(JSContext* context, JSValue object, GuiModuleBinding* binding);
bool define_image_members(JSContext* context, JSValue object, GuiModuleBinding* binding);
JSValue build_gui_object(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                         const std::uint64_t gui_id);
JSValue build_ctrl_object(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                          const std::uint64_t ctrl_id);
JSValue build_menu_object(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                          const std::uint64_t menu_id);
JSValue build_image_object(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                           const std::uint64_t list_id);
void release_image_list(JSContext* context, GuiJsState* state, GuiJsImageList& list);
JSValue image_list_then(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        JSValueConst*);
JSValue menu_object_for(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                        const std::uint64_t menu_id);
void release_menu(JSContext* context, GuiJsState* state, GuiJsMenu& menu);
void invoke_menu_callback(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                          GuiJsMenu& menu, const std::string& name);
bool ensure_registry(JSContext* context, GuiModuleBinding* binding, GuiJsState* state);

// ---- shared member plumbing -------------------------------------------------

// OnEvent / OnMessage on the Gui and OnEvent / OnMessage / OnCommand /
// OnNotify on a control share one shape: (selector, callback, addRemove?).
// `kind` is the GuiJsRoute kind, `target` 0 = the Gui window else a control
// id (gui-menu.md §4.3). Synchronous section validates; the async section
// mirrors the whole interest table onto the pump and returns its Promise.
JSValue register_event(JSContext* context, GuiJsState* state, GuiJs& gui, const char kind,
                       const std::uint64_t target, const char* signature, const int argc,
                       JSValueConst* argv) {
  if (!require_live(context, gui)) return JS_EXCEPTION;
  int end = 0;
  const int action_index = split_action(context, argc, argv, end);
  ActionOptions action;
  if (!parse_action_at(context, argv, action_index, action)) return JS_EXCEPTION;
  if (end < 2) return JS_ThrowTypeError(context, "%s(selector, callback, addRemove?)", signature);

  GuiJsRoute route;
  route.kind = kind;
  route.target = target;
  if (kind == 'e') {
    std::string name;
    if (!read_string(context, argv[0], "eventName", name)) return JS_EXCEPTION;
    const bool known = target == 0 ? gui_event_name(name) : ctrl_event_name(name);
    if (!known) {
      throw_value_error(context, "unknown event: " + name);
      return JS_EXCEPTION;
    }
    route.event = std::move(name);
  } else {
    std::int64_t selector = 0;
    if (!js_int64_strict(context, argv[0], selector, "selector")) return JS_EXCEPTION;
    if (kind == 'm' && (selector < 0 || selector > 0xFFFFFFFFll)) {
      throw_value_error(context, "message number must be between 0 and 4294967295");
      return JS_EXCEPTION;
    }
    route.selector = selector;
  }

  int add_remove = 1;
  if (end > 2) {
    if (!JS_IsNumber(argv[2])) {
      JS_ThrowTypeError(context, "addRemove must be a number");
      return JS_EXCEPTION;
    }
    double raw = 0;
    if (JS_ToFloat64(context, &raw, argv[2])) return JS_EXCEPTION;
    if (!std::isfinite(raw) || std::trunc(raw) != raw || raw < -1 || raw > 1) {
      throw_value_error(context, "addRemove must be -1, 0 or 1");
      return JS_EXCEPTION;
    }
    add_remove = static_cast<int>(raw);
  }
  return register_handler(context, state, gui, std::move(route), argv[1], add_remove,
                          signature, action);
}

// __Enum(varCount?) and the implicit [Symbol.iterator]: the JS-side ordered
// control list (gui-menu.md §4.2 - a list, not an AHK Enumerator object).
JSValue build_control_list(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                           const GuiJs& gui, const bool has_limit, const std::int64_t limit) {
  JSValue array = JS_NewArray(context);
  if (JS_IsException(array)) return JS_EXCEPTION;
  std::int64_t emitted = 0;
  for (const std::uint64_t ctrl_id : gui.controls) {
    if (has_limit && emitted >= limit) break;
    JSValue object = ctrl_object_for(context, state, binding, ctrl_id);
    if (JS_IsException(object)) {
      JS_FreeValue(context, array);
      return JS_EXCEPTION;
    }
    // JS_SetPropertyUint32 consumes `object` on both outcomes.
    if (JS_SetPropertyUint32(context, array, static_cast<std::uint32_t>(emitted), object) < 0) {
      JS_FreeValue(context, array);
      return JS_EXCEPTION;
    }
    ++emitted;
  }
  return array;
}

// The owning Gui entry of a control - every control pump call carries its
// GuiSpec, and require_live_ctrl has already proven the entry exists.
GuiJs* parent_of(JSContext* context, GuiJsState* state, const GuiJsControl& control) {
  const auto found = state->guis.find(control.gui_id);
  if (found == state->guis.end()) {
    JS_ThrowInternalError(context, "gui state is gone");
    return nullptr;
  }
  return &found->second;
}

// Enabled/Visible: a boolean, or the 0/1 number AHK also accepts.
bool read_flag(JSContext* context, JSValueConst value, const char* what, bool& out) {
  if (JS_IsBool(value)) {
    out = JS_ToBool(context, value) > 0;
    return true;
  }
  if (JS_IsNumber(value)) {
    double number = 0;
    if (JS_ToFloat64(context, &number, value)) return false;
    if (number == 0.0 || number == 1.0) {
      out = number == 1.0;
      return true;
    }
  }
  JS_ThrowTypeError(context, "%s must be a boolean", what);
  return false;
}

// A Value read on a kind that has none: the read itself is async, but the
// shape error is known synchronously, so it is a rejected Promise carrying
// the same ValueError a write would throw (gui-menu.md §4.2).
JSValue rejected_value_error(JSContext* context, const std::string& detail) {
  JS_ThrowRangeError(context, "%s", detail.c_str());
  return rejected_promise(context, JS_GetException(context));
}

// FocusedCtrl resolves with the focused control id (or null); this
// continuation maps it to the JS control object, the shape Add uses.
JSValue focused_ctrl_then(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          JSValueConst*) {
  if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) return JS_NULL;
  std::int64_t id = 0;
  if (!JS_IsNumber(argv[0]) || JS_ToInt64(context, &id, argv[0]) != 0 || id <= 0) return JS_NULL;
  GuiModuleBinding* binding = binding_of(context);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  return ctrl_object_for(context, state, binding, static_cast<std::uint64_t>(id));
}

// MenuBar resolves with the attached menu id (or null); this continuation
// maps it to the JS menu object, the shape FocusedCtrl uses for controls.
JSValue menubar_then(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     JSValueConst*) {
  if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) return JS_NULL;
  std::int64_t id = 0;
  if (!JS_IsNumber(argv[0]) || JS_ToInt64(context, &id, argv[0]) != 0 || id <= 0) return JS_NULL;
  GuiModuleBinding* binding = binding_of(context);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  return menu_object_for(context, state, binding, static_cast<std::uint64_t>(id));
}

// ---- Gui members ------------------------------------------------------------

JSValue gui_method(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                   int magic, void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  GuiJs* gui_ptr = gui_of(context, this_val, state);
  if (!gui_ptr) return JS_EXCEPTION;
  GuiJs& gui = *gui_ptr;

  int end = 0;
  ActionOptions action;
  const auto split = [&]() {
    const int action_index = split_action(context, argc, argv, end);
    return parse_action_at(context, argv, action_index, action);
  };

  switch (magic) {
    case kGuiNew:
      // The factory (ui.createGui) validates and builds; reaching __New on a
      // live object is the objects.json "already constructed or disposed" row.
      return throw_coded(context, "already_constructed",
                         "Gui is already constructed or disposed");

    case kGuiAdd:
    case kGuiAddButton:
    case kGuiAddCheckBox:
    case kGuiAddEdit:
    case kGuiAddGroupBox:
    case kGuiAddPicture:
    case kGuiAddProgress:
    case kGuiAddRadio:
    case kGuiAddText:
    case kGuiAddListBox:
    case kGuiAddComboBox:
    case kGuiAddDDL:
    case kGuiAddDropDownList:
    case kGuiAddDateTime:
    case kGuiAddMonthCal:
    case kGuiAddSlider:
    case kGuiAddUpDown:
    case kGuiAddTab:
    case kGuiAddTab2:
    case kGuiAddPic:
    case kGuiAddLink:
    case kGuiAddHotkey:
    case kGuiAddListView:
    case kGuiAddTreeView:
    case kGuiAddStatusBar: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      if (!split()) return JS_EXCEPTION;
      const auto fixed = fixed_add_kind(magic);
      const ControlKind kind = fixed.value_or(ControlKind::Text);
      return gui_add_impl(context, state, gui, magic == kGuiAdd, kind, argc, argv);
    }

    case kGuiDestroy: {
      // Destroy has no live pre-check by design (objects.json: destroying an
      // already destroyed Gui is a no-op) and never validates its arguments
      // as a shape - only a trailing ActionOptions.
      if (!split()) return JS_EXCEPTION;
      if (end > 0) return JS_ThrowTypeError(context, "Destroy()");
      const std::uint64_t gui_id = gui.id;
      release_gui(context, state, gui);
      return start_gui_op(context, state, gui, action,
                          [gui_id](GuiService& service, const std::int64_t deadline,
                                   rime::core::CancellationToken cancel) {
                            return null_out(service.gui_destroy(gui_id, deadline, cancel));
                          });
    }

    case kGuiFlash: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      if (!split()) return JS_EXCEPTION;
      if (end > 1) return JS_ThrowTypeError(context, "Flash(blink?)");
      bool blink = true;
      if (!optional_bool_arg(context, argv, 0, end, "blink", blink)) return JS_EXCEPTION;
      const GuiService::GuiSpec spec = make_spec(gui);
      return start_gui_op(context, state, gui, action,
                          [spec, blink](GuiService& service, const std::int64_t deadline,
                                        rime::core::CancellationToken cancel) {
                            return null_out(service.gui_flash(spec, blink, deadline, cancel));
                          });
    }

    case kGuiGetClientPos:
    case kGuiGetPos: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      if (!split()) return JS_EXCEPTION;
      if (end > 0) {
        return JS_ThrowTypeError(context, magic == kGuiGetPos ? "GetPos()" : "GetClientPos()");
      }
      const GuiService::GuiSpec spec = make_spec(gui);
      const bool client = magic == kGuiGetClientPos;
      return start_gui_op(
          context, state, gui, action,
          [spec, client](GuiService& service, const std::int64_t deadline,
                         rime::core::CancellationToken cancel) -> AsyncOutcome {
            GuiService::GuiRect rect;
            if (const Error error =
                    service.gui_get_pos(spec, client, rect, deadline, cancel);
                !error.ok()) {
              return async_failure(error);
            }
            return async_success(rect_json(rect));
          });
    }

    case kGuiHide:
    case kGuiMaximize:
    case kGuiMinimize:
    case kGuiRestore: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      if (!split()) return JS_EXCEPTION;
      if (end > 0) {
        return JS_ThrowTypeError(context,
                                 magic == kGuiHide        ? "Hide()"
                                 : magic == kGuiMaximize  ? "Maximize()"
                                 : magic == kGuiMinimize  ? "Minimize()"
                                                          : "Restore()");
      }
      const GuiService::GuiSpec spec = make_spec(gui);
      // Hide goes through gui_hide, not a ShowWindow code: the default
      // SW_SHOWNORMAL would have made Hide() show the window.
      if (magic == kGuiHide) {
        return start_gui_op(context, state, gui, action,
                            [spec](GuiService& service, const std::int64_t deadline,
                                   rime::core::CancellationToken cancel) {
                              return null_out(service.gui_hide(spec, deadline, cancel));
                            });
      }
      int show_cmd = SW_SHOWNORMAL;
      if (magic == kGuiMaximize) show_cmd = SW_MAXIMIZE;
      else if (magic == kGuiMinimize) show_cmd = SW_MINIMIZE;
      else if (magic == kGuiRestore) show_cmd = SW_RESTORE;
      return start_gui_op(context, state, gui, action,
                          [spec, show_cmd](GuiService& service, const std::int64_t deadline,
                                           rime::core::CancellationToken cancel) {
                            return null_out(service.gui_window_cmd(spec, show_cmd, deadline,
                                                                   cancel));
                          });
    }

    case kGuiMove: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      if (!split()) return JS_EXCEPTION;
      if (end > 4) return JS_ThrowTypeError(context, "Move(x?, y?, width?, height?)");
      GuiService::GuiMoveSpec move;
      if (!optional_int_arg(context, argv, 0, end, "x", move.x) ||
          !optional_int_arg(context, argv, 1, end, "y", move.y) ||
          !optional_int_arg(context, argv, 2, end, "width", move.width) ||
          !optional_int_arg(context, argv, 3, end, "height", move.height)) {
        return JS_EXCEPTION;
      }
      const GuiService::GuiSpec spec = make_spec(gui);
      return start_gui_op(context, state, gui, action,
                          [spec, move](GuiService& service, const std::int64_t deadline,
                                       rime::core::CancellationToken cancel) {
                            return null_out(service.gui_move(spec, move, deadline, cancel));
                          });
    }

    case kGuiOnEvent:
      return register_event(context, state, gui, 'e', 0, "OnEvent", argc, argv);
    case kGuiOnMessage:
      return register_event(context, state, gui, 'm', 0, "OnMessage", argc, argv);

    case kGuiOpt: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      if (!split()) return JS_EXCEPTION;
      if (end < 1) return JS_ThrowTypeError(context, "Opt(options)");
      std::string text;
      if (!read_string(context, argv[0], "options", text)) return JS_EXCEPTION;
      GuiService::GuiStyleOptions parsed;
      if (const Error error = GuiService::parse_gui_options(text, parsed); !error.ok()) {
        throw_value_error(context, error.message);
        return JS_EXCEPTION;
      }
      // Snapshot before the mirror moves: gui_opt applies the mention masks
      // to whatever style the record has, so the spec must describe the
      // record's current state, not the post-mask result.
      const GuiService::GuiSpec spec = make_spec(gui);
      gui.style.style = (gui.style.style | parsed.style_add) & ~parsed.style_remove;
      gui.style.ex_style = (gui.style.ex_style | parsed.ex_add) & ~parsed.ex_remove;
      return start_gui_op(context, state, gui, action,
                          [spec, parsed](GuiService& service, const std::int64_t deadline,
                                         rime::core::CancellationToken cancel) {
                            return null_out(service.gui_opt(spec, parsed, deadline, cancel));
                          });
    }

    case kGuiSetFont: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      if (!split()) return JS_EXCEPTION;
      if (end > 2) return JS_ThrowTypeError(context, "SetFont(options?, fontName?)");
      GuiService::GuiFontSpec font;
      if (end > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        std::string text;
        if (!read_string(context, argv[0], "options", text)) return JS_EXCEPTION;
        if (const Error error = GuiService::parse_font_options(text, font); !error.ok()) {
          throw_value_error(context, error.message);
          return JS_EXCEPTION;
        }
      }
      if (!optional_string_arg(context, argc, argv, 1, "SetFont", "fontName", font.name)) {
        return JS_EXCEPTION;
      }
      const GuiService::GuiSpec spec = make_spec(gui);
      return start_gui_op(context, state, gui, action,
                          [spec, font](GuiService& service, const std::int64_t deadline,
                                       rime::core::CancellationToken cancel) {
                            return null_out(service.gui_set_font(spec, font, deadline, cancel));
                          });
    }

    case kGuiShow: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      if (!split()) return JS_EXCEPTION;
      if (end > 1) return JS_ThrowTypeError(context, "Show(options?)");
      std::string text;
      if (!optional_string_arg(context, argc, argv, 0, "Show", "options", text))
        return JS_EXCEPTION;
      GuiService::GuiShowSpec show;
      if (const Error error = GuiService::parse_show_options(text, show); !error.ok()) {
        throw_value_error(context, error.message);
        return JS_EXCEPTION;
      }
      const GuiService::GuiSpec spec = make_spec(gui);
      return start_gui_op(context, state, gui, action,
                          [spec, show](GuiService& service, const std::int64_t deadline,
                                       rime::core::CancellationToken cancel) {
                            return null_out(service.gui_show(spec, show, deadline, cancel));
                          });
    }

    case kGuiSubmit: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      if (!split()) return JS_EXCEPTION;
      if (end > 1) return JS_ThrowTypeError(context, "Submit(hide?)");
      bool hide = false;
      if (!optional_bool_arg(context, argv, 0, end, "hide", hide)) return JS_EXCEPTION;
      const GuiService::GuiSpec spec = make_spec(gui);
      return start_gui_op(
          context, state, gui, action,
          [spec, hide](GuiService& service, const std::int64_t deadline,
                       rime::core::CancellationToken cancel) -> AsyncOutcome {
            json::Value out = json::Value::object();
            if (const Error error = service.gui_submit(spec, hide, out, deadline, cancel);
                !error.ok()) {
              return async_failure(error);
            }
            return async_success(json::stringify(out));
          });
    }

    case kGuiItem: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      if (argc < 1) return JS_ThrowTypeError(context, "__Item(index)");
      std::uint64_t matched = 0;
      if (JS_IsNumber(argv[0])) {
        std::int64_t position = 0;
        if (!js_int64_strict(context, argv[0], position, "index")) return JS_EXCEPTION;
        // 0-based positions (modern doctrine): out of range reads undefined
        // like JS indexing instead of throwing.
        if (position >= 0 && static_cast<std::size_t>(position) < gui.controls.size()) {
          matched = gui.controls[static_cast<std::size_t>(position)];
        } else {
          return JS_UNDEFINED;
        }
      } else if (JS_IsString(argv[0])) {
        std::string key;
        if (!read_string(context, argv[0], "index", key)) return JS_EXCEPTION;
        for (const std::uint64_t ctrl_id : gui.controls) {
          const auto found = state->controls.find(ctrl_id);
          if (found != state->controls.end() && !found->second.destroyed &&
              found->second.name == key) {
            matched = ctrl_id;
            break;
          }
        }
        // Unknown names read undefined, same rule as positions.
        if (matched == 0) return JS_UNDEFINED;
      } else {
        return JS_ThrowTypeError(context, "__Item(index) expects a name or a position");
      }
      return ctrl_object_for(context, state, binding, matched);
    }

    case kGuiEnum: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      if (argc > 1) return JS_ThrowTypeError(context, "__Enum(varCount?)");
      bool has_limit = false;
      std::int64_t limit = 0;
      if (argc == 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (!js_int64_strict(context, argv[0], limit, "varCount")) return JS_EXCEPTION;
        if (limit < 0) {
          throw_value_error(context, "varCount must not be negative");
          return JS_EXCEPTION;
        }
        has_limit = true;
      }
      return build_control_list(context, state, binding, gui, has_limit, limit);
    }

    case kGuiIterator: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      JSValue array = build_control_list(context, state, binding, gui, false, 0);
      if (JS_IsException(array)) return JS_EXCEPTION;
      const JSAtom atom = iterator_atom(context);
      if (atom == JS_ATOM_NULL) {
        JS_FreeValue(context, array);
        return JS_EXCEPTION;
      }
      JSValue iterator = JS_Invoke(context, array, atom, 0, nullptr);
      JS_FreeAtom(context, atom);
      JS_FreeValue(context, array);
      return iterator;
    }

    default:
      return JS_ThrowTypeError(context, "Gui: unknown method");
  }
}

JSValue gui_prop_get(JSContext* context, JSValueConst this_val, int, JSValueConst*, int magic,
                     void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  GuiJs* gui_ptr = gui_of(context, this_val, state);
  if (!gui_ptr) return JS_EXCEPTION;
  GuiJs& gui = *gui_ptr;

  switch (magic) {
    case kGuiPropHwnd:
    case kGuiPropFontHandle:
      // §4.7: the row exists to refuse, so it refuses before any state check
      // and does so for reads and writes alike.
      return throw_policy_refusal(context);

    case kGuiPropName:
      return JS_NewStringLen(context, gui.name.data(), gui.name.size());

    case kGuiPropBackColor:
      if (!require_live(context, gui)) return JS_EXCEPTION;
      return resolved_promise(
          context, JS_NewStringLen(context, gui.back_color.data(), gui.back_color.size()));

    case kGuiPropMarginX:
      if (!require_live(context, gui)) return JS_EXCEPTION;
      return gui.margin_x >= 0 ? resolved_promise(context, JS_NewInt32(context, gui.margin_x))
                               : resolved_promise(context, JS_NULL);

    case kGuiPropMarginY:
      if (!require_live(context, gui)) return JS_EXCEPTION;
      return gui.margin_y >= 0 ? resolved_promise(context, JS_NewInt32(context, gui.margin_y))
                               : resolved_promise(context, JS_NULL);

    case kGuiPropTitle: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      // Before the first async member the window does not exist, so the
      // mirror is the whole truth (gui-menu.md §4.2).
      if (!gui.materialized) {
        return resolved_promise(context,
                                JS_NewStringLen(context, gui.title.data(), gui.title.size()));
      }
      const GuiService::GuiSpec spec = make_spec(gui);
      return start_gui_op(
          context, state, gui, ActionOptions{},
          [spec](GuiService& service, const std::int64_t deadline,
                 rime::core::CancellationToken cancel) -> AsyncOutcome {
            std::string title;
            if (const Error error = service.gui_get_title(spec, title, deadline, cancel);
                !error.ok()) {
              return async_failure(error);
            }
            return async_success(json::stringify(json::Value::string(title)));
          });
    }

    case kGuiPropFocusedCtrl: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      const GuiService::GuiSpec spec = make_spec(gui);
      JSValue promise = start_gui_op(
          context, state, gui, ActionOptions{},
          [spec](GuiService& service, const std::int64_t deadline,
                 rime::core::CancellationToken cancel) -> AsyncOutcome {
            std::uint64_t ctrl_id = 0;
            if (const Error error =
                    service.gui_focused_control(spec, ctrl_id, deadline, cancel);
                !error.ok()) {
              return async_failure(error);
            }
            if (ctrl_id == 0) return async_success("null");
            return async_success(
                json::stringify(json::Value::number(static_cast<double>(ctrl_id))));
          });
      if (JS_IsException(promise)) return JS_EXCEPTION;
      JSValue on_ok = JS_NewCFunctionData2(context, focused_ctrl_then, "onFocusedCtrl", 1, 0, 0,
                                           nullptr);
      if (JS_IsException(on_ok)) {
        JS_FreeValue(context, promise);
        return JS_EXCEPTION;
      }
      return then_chain(context, promise, on_ok, JS_UNDEFINED);
    }

    case kGuiPropMenuBar: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      const GuiService::GuiSpec spec = make_spec(gui);
      JSValue promise = start_gui_op(
          context, state, gui, ActionOptions{},
          [spec](GuiService& service, const std::int64_t deadline,
                 rime::core::CancellationToken cancel) -> AsyncOutcome {
            std::uint64_t menu_id = 0;
            if (const Error error =
                    service.gui_menu_bar_of(spec, menu_id, deadline, cancel);
                !error.ok()) {
              return async_failure(error);
            }
            if (menu_id == 0) return async_success("null");
            return async_success(
                json::stringify(json::Value::number(static_cast<double>(menu_id))));
          });
      if (JS_IsException(promise)) return JS_EXCEPTION;
      JSValue on_ok = JS_NewCFunctionData2(context, menubar_then, "onMenuBar", 1, 0, 0, nullptr);
      if (JS_IsException(on_ok)) {
        JS_FreeValue(context, promise);
        return JS_EXCEPTION;
      }
      return then_chain(context, promise, on_ok, JS_UNDEFINED);
    }

    case kGuiPropDpi: {
      if (!require_live(context, gui)) return JS_EXCEPTION;
      const GuiService::GuiSpec spec = make_spec(gui);
      return start_gui_op(
          context, state, gui, ActionOptions{},
          [spec](GuiService& service, const std::int64_t deadline,
                 rime::core::CancellationToken cancel) -> AsyncOutcome {
            std::uint64_t dpi = 0;
            if (const Error error = service.gui_get_dpi(spec, dpi, deadline, cancel);
                !error.ok()) {
              return async_failure(error);
            }
            return async_success(std::to_string(dpi));
          });
    }

    default:
      return JS_ThrowTypeError(context, "Gui: unknown property");
  }
}

JSValue gui_prop_set(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                     int magic, void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  GuiJs* gui_ptr = gui_of(context, this_val, state);
  if (!gui_ptr) return JS_EXCEPTION;
  GuiJs& gui = *gui_ptr;

  if (magic == kGuiPropHwnd || magic == kGuiPropFontHandle) {
    return throw_policy_refusal(context);
  }
  if (argc < 1) return JS_ThrowTypeError(context, "setter requires a value");
  if (!require_live(context, gui)) return JS_EXCEPTION;
  ActionOptions action;

  switch (magic) {
    case kGuiPropTitle: {
      std::string title;
      if (!read_string(context, argv[0], "Title", title)) return JS_EXCEPTION;
      gui.title = std::move(title);
      const GuiService::GuiSpec spec = make_spec(gui);
      fire_and_forget(context,
                      start_gui_op(context, state, gui, action,
                                   [spec](GuiService& service, const std::int64_t deadline,
                                          rime::core::CancellationToken cancel) {
                                     return null_out(service.gui_set_title(
                                         spec, spec.title, deadline, cancel));
                                   }));
      return JS_UNDEFINED;
    }

    case kGuiPropBackColor: {
      std::string mirror;
      std::uint32_t color_ref = 0;
      if (!parse_color(context, argv[0], mirror, color_ref)) return JS_EXCEPTION;
      gui.back_color = std::move(mirror);
      const GuiService::GuiSpec spec = make_spec(gui);
      fire_and_forget(
          context, start_gui_op(context, state, gui, action,
                                [spec, color_ref](GuiService& service,
                                                  const std::int64_t deadline,
                                                  rime::core::CancellationToken cancel) {
                                  return null_out(service.gui_set_back_color(spec, color_ref,
                                                                             deadline, cancel));
                                }));
      return JS_UNDEFINED;
    }

    case kGuiPropMarginX:
    case kGuiPropMarginY: {
      const char* label = magic == kGuiPropMarginX ? "MarginX" : "MarginY";
      std::int64_t raw = 0;
      if (!js_int64_strict(context, argv[0], raw, label)) return JS_EXCEPTION;
      // The JS surface never asks to "keep" an axis (-1 is the internal
      // unset marker), so a negative here is an error, not a sentinel.
      if (raw < 0 || raw > std::numeric_limits<int>::max()) {
        throw_value_error(context, std::string(label) + " must not be negative");
        return JS_EXCEPTION;
      }
      if (magic == kGuiPropMarginX) {
        gui.margin_x = static_cast<int>(raw);
      } else {
        gui.margin_y = static_cast<int>(raw);
      }
      const GuiService::GuiSpec spec = make_spec(gui);
      const int margin_x = gui.margin_x;
      const int margin_y = gui.margin_y;
      fire_and_forget(
          context, start_gui_op(context, state, gui, action,
                                [spec, margin_x, margin_y](GuiService& service,
                                                           const std::int64_t deadline,
                                                           rime::core::CancellationToken cancel) {
                                  return null_out(service.gui_set_margins(
                                      spec, margin_x, margin_y, deadline, cancel));
                                }));
      return JS_UNDEFINED;
    }

    case kGuiPropName: {
      std::string name;
      if (!read_string(context, argv[0], "Name", name)) return JS_EXCEPTION;
      gui.name = std::move(name);
      return JS_UNDEFINED;
    }

    case kGuiPropMenuBar: {
      // A Menu object attaches, null/undefined detaches. Fire-and-forget
      // like the other setters; the getter reads back through the pump.
      std::uint64_t menu_id = 0;
      if (!JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        bool ok = false;
        menu_id = identity_of(context, argv[0], "Menu", ok);
        if (!ok) return JS_EXCEPTION;
        const auto found = state->menus.find(menu_id);
        if (found == state->menus.end() || found->second.destroyed) {
          throw_invalid_state(context, kDestroyed);
          return JS_EXCEPTION;
        }
      }
      const GuiService::GuiSpec spec = make_spec(gui);
      fire_and_forget(context,
                      start_gui_op(context, state, gui, action,
                                   [spec, menu_id](GuiService& service,
                                                   const std::int64_t deadline,
                                                   rime::core::CancellationToken cancel) {
                                     return null_out(service.gui_menu_bar(spec, menu_id, deadline,
                                                                          cancel));
                                   }));
      return JS_UNDEFINED;
    }

    default:
      return JS_ThrowTypeError(context, "Gui: unknown property");
  }
}

// ---- GuiControl members -----------------------------------------------------

enum CtrlMethodMagic {
  kCtrlFocus = 0,
  kCtrlGetPos,
  kCtrlMove,
  kCtrlOnCommand,
  kCtrlOnEvent,
  kCtrlOnMessage,
  kCtrlOnNotify,
  kCtrlOpt,
  kCtrlRedraw,
  kCtrlSetFont,
  kCtrlSetCue,
  // Batch 3b: per-kind control methods. Add/Choose/Delete/Insert/Modify/
  // GetCount/GetText/GetNext are shared names dispatched by kind (list
  // kinds, ListView rows, TreeView nodes); the rest are kind-specific.
  // SetImageList stays out (needs the 3c ImageList model).
  kCtrlAdd,
  kCtrlChoose,
  kCtrlDelete,
  kCtrlInsert,
  kCtrlModify,
  kCtrlGetCount,
  kCtrlGetText,
  kCtrlGetNext,
  kCtrlInsertCol,
  kCtrlModifyCol,
  kCtrlDeleteCol,
  kCtrlGet,
  kCtrlGetChild,
  kCtrlGetParent,
  kCtrlGetPrev,
  kCtrlGetSelection,
  kCtrlSetParts,
  kCtrlSetText,
  kCtrlSetIcon,
  kCtrlUseTab,
  kCtrlSetFormat,
  kCtrlSetHotkey,
  kCtrlSetImageList,
  kCtrlMethodCount
};

constexpr const char* kCtrlMethodNames[] = {"Focus",   "GetPos",   "Move",    "OnCommand",
                                            "OnEvent", "OnMessage", "OnNotify", "Opt",
                                            "Redraw",  "SetFont",  "SetCue",  "Add",
                                            "Choose",  "Delete",   "Insert",  "Modify",
                                            "GetCount", "GetText", "GetNext", "InsertCol",
                                            "ModifyCol", "DeleteCol", "Get",  "GetChild",
                                            "GetParent", "GetPrev", "GetSelection", "SetParts",
                                            "SetText", "SetIcon",  "UseTab",  "SetFormat",
                                            "SetHotkey", "SetImageList"};
constexpr int kCtrlMethodLengths[] = {0, 0, 4, 3, 3, 3, 3, 1, 0, 2, 2,
                                      6, 1, 1, 6, 6, 1, 2, 2, 4,
                                      4, 1, 2, 1, 1, 1, 0, 1,
                                      3, 3, 2, 1, 2, 2};

static_assert(sizeof(kCtrlMethodNames) / sizeof(kCtrlMethodNames[0]) == kCtrlMethodCount);
static_assert(sizeof(kCtrlMethodLengths) / sizeof(kCtrlMethodLengths[0]) == kCtrlMethodCount);

enum CtrlPropMagic {
  kCtrlPropClassNN = 0,
  kCtrlPropEnabled,
  kCtrlPropFocused,
  kCtrlPropGui,
  kCtrlPropHwnd,
  kCtrlPropName,
  kCtrlPropText,
  kCtrlPropType,
  kCtrlPropValue,
  kCtrlPropVisible,
  kCtrlPropCount
};

constexpr const char* kCtrlPropNames[] = {"ClassNN", "Enabled", "Focused", "Gui",   "Hwnd",
                                          "Name",    "Text",    "Type",    "Value", "Visible"};
// Hwnd is writable only so the assignment path can carry the §4.7 refusal
// too; Focused/ClassNN/Type/Gui are derived reads.
constexpr bool kCtrlPropWritable[] = {false, true, false, false, true,
                                      true,  true,  false, true,  true};

static_assert(sizeof(kCtrlPropNames) / sizeof(kCtrlPropNames[0]) == kCtrlPropCount);
static_assert(sizeof(kCtrlPropWritable) / sizeof(kCtrlPropWritable[0]) == kCtrlPropCount);

bool control_has_value(const ControlKind kind) {
  return kind == ControlKind::Edit || kind == ControlKind::CheckBox ||
         kind == ControlKind::Radio || kind == ControlKind::Progress ||
         kind == ControlKind::DateTime || kind == ControlKind::MonthCal ||
         kind == ControlKind::Slider || kind == ControlKind::UpDown;
}

// Batch-3b result faces: the service speaks one GuiControlResult; each
// method declares which face it shows. OrNull faces map the service's
// empty sentinel (-1 row, 0 node id) to null - never AHK's 0.
enum class CtrlResultFace { Null, Number, Item, Text, Bool, NumberOrNull, ItemOrNull };

JSValue start_ctrl_invoke(JSContext* context, GuiJsState* state, GuiJs& parent,
                          const GuiService::GuiSpec& spec, const std::uint64_t ctrl_id,
                          const ActionOptions& action, GuiService::GuiControlCall call,
                          const CtrlResultFace face) {
  return start_gui_op(
      context, state, parent, action,
      [spec, ctrl_id, call, face](GuiService& service, const std::int64_t deadline,
                                  rime::core::CancellationToken cancel) -> AsyncOutcome {
        GuiService::GuiControlResult result;
        if (const Error error =
                service.ctrl_invoke(spec, ctrl_id, call, result, deadline, cancel);
            !error.ok()) {
          return async_failure(error);
        }
        switch (face) {
          case CtrlResultFace::Number:
            return async_success(std::to_string(result.number));
          case CtrlResultFace::Item:
            return async_success(std::to_string(result.item));
          case CtrlResultFace::Text:
            return async_success(json::stringify(json::Value::string(result.text)));
          case CtrlResultFace::Bool:
            return async_success(result.boolean ? "true" : "false");
          case CtrlResultFace::NumberOrNull:
            return result.number < 0 ? async_success("null")
                                     : async_success(std::to_string(result.number));
          case CtrlResultFace::ItemOrNull:
            return result.item == 0 ? async_success("null")
                                    : async_success(std::to_string(result.item));
          case CtrlResultFace::Null:
          default:
            return async_success("null");
        }
      });
}

JSValue ctrl_method(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                    int magic, void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  GuiJsControl* control = ctrl_of(context, this_val, state);
  if (!control) return JS_EXCEPTION;
  if (!require_live_ctrl(context, *state, *control)) return JS_EXCEPTION;
  GuiJs* parent = parent_of(context, state, *control);
  if (!parent) return JS_EXCEPTION;

  const GuiService::GuiSpec spec = make_spec(*parent);
  const std::uint64_t ctrl_id = control->id;

  int end = 0;
  ActionOptions action;
  const auto split = [&]() {
    const int action_index = split_action(context, argc, argv, end);
    return parse_action_at(context, argv, action_index, action);
  };

  switch (magic) {
    case kCtrlFocus:
    case kCtrlRedraw: {
      if (!split()) return JS_EXCEPTION;
      if (end > 0) {
        return JS_ThrowTypeError(context, magic == kCtrlFocus ? "Focus()" : "Redraw()");
      }
      return start_gui_op(context, state, *parent, action,
                          [spec, ctrl_id, magic](GuiService& service,
                                                 const std::int64_t deadline,
                                                 rime::core::CancellationToken cancel) {
                            return magic == kCtrlFocus
                                       ? null_out(service.ctrl_focus(spec, ctrl_id, deadline,
                                                                     cancel))
                                       : null_out(service.ctrl_redraw(spec, ctrl_id, deadline,
                                                                      cancel));
                          });
    }

    case kCtrlGetPos: {
      if (!split()) return JS_EXCEPTION;
      if (end > 0) return JS_ThrowTypeError(context, "GetPos()");
      return start_gui_op(
          context, state, *parent, action,
          [spec, ctrl_id](GuiService& service, const std::int64_t deadline,
                          rime::core::CancellationToken cancel) -> AsyncOutcome {
            GuiService::GuiRect rect;
            if (const Error error = service.ctrl_get_pos(spec, ctrl_id, rect, deadline, cancel);
                !error.ok()) {
              return async_failure(error);
            }
            return async_success(rect_json(rect));
          });
    }

    case kCtrlMove: {
      if (!split()) return JS_EXCEPTION;
      if (end > 4) return JS_ThrowTypeError(context, "Move(x?, y?, width?, height?)");
      GuiService::GuiMoveSpec move;
      if (!optional_int_arg(context, argv, 0, end, "x", move.x) ||
          !optional_int_arg(context, argv, 1, end, "y", move.y) ||
          !optional_int_arg(context, argv, 2, end, "width", move.width) ||
          !optional_int_arg(context, argv, 3, end, "height", move.height)) {
        return JS_EXCEPTION;
      }
      return start_gui_op(context, state, *parent, action,
                          [spec, ctrl_id, move](GuiService& service,
                                                const std::int64_t deadline,
                                                rime::core::CancellationToken cancel) {
                            return null_out(service.ctrl_move(spec, ctrl_id, move, deadline,
                                                              cancel));
                          });
    }

    case kCtrlOnEvent:
      return register_event(context, state, *parent, 'e', ctrl_id, "OnEvent", argc, argv);
    case kCtrlOnMessage:
      return register_event(context, state, *parent, 'm', ctrl_id, "OnMessage", argc,
                            argv);
    case kCtrlOnCommand:
      return register_event(context, state, *parent, 'c', ctrl_id, "OnCommand", argc,
                            argv);
    case kCtrlOnNotify:
      return register_event(context, state, *parent, 'n', ctrl_id, "OnNotify", argc,
                            argv);

    case kCtrlOpt: {
      if (!split()) return JS_EXCEPTION;
      if (end < 1) return JS_ThrowTypeError(context, "Opt(options)");
      std::string text;
      if (!read_string(context, argv[0], "options", text)) return JS_EXCEPTION;
      GuiService::GuiControlOptions options;
      if (const Error error = GuiService::parse_control_options(text, options); !error.ok()) {
        throw_value_error(context, error.message);
        return JS_EXCEPTION;
      }
      return start_gui_op(context, state, *parent, action,
                          [spec, ctrl_id, options](GuiService& service,
                                                   const std::int64_t deadline,
                                                   rime::core::CancellationToken cancel) {
                            return null_out(service.ctrl_opt(spec, ctrl_id, options, deadline,
                                                             cancel));
                          });
    }

    case kCtrlSetFont: {
      if (!split()) return JS_EXCEPTION;
      if (end > 2) return JS_ThrowTypeError(context, "SetFont(options?, fontName?)");
      GuiService::GuiFontSpec font;
      if (end > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        std::string text;
        if (!read_string(context, argv[0], "options", text)) return JS_EXCEPTION;
        if (const Error error = GuiService::parse_font_options(text, font); !error.ok()) {
          throw_value_error(context, error.message);
          return JS_EXCEPTION;
        }
      }
      if (!optional_string_arg(context, argc, argv, 1, "SetFont", "fontName", font.name)) {
        return JS_EXCEPTION;
      }
      return start_gui_op(context, state, *parent, action,
                          [spec, ctrl_id, font](GuiService& service, const std::int64_t deadline,
                                                rime::core::CancellationToken cancel) {
                            return null_out(service.ctrl_set_font(spec, ctrl_id, font, deadline,
                                                                  cancel));
                          });
    }

    case kCtrlSetCue: {
      if (!split()) return JS_EXCEPTION;
      if (end < 1 || end > 2) {
        return JS_ThrowTypeError(context, "SetCue(cueText, activate?)");
      }
      std::string cue;
      if (!read_string(context, argv[0], "cueText", cue)) return JS_EXCEPTION;
      bool activate = true;
      if (!optional_bool_arg(context, argv, 1, end, "activate", activate)) {
        return JS_EXCEPTION;
      }
      return start_gui_op(context, state, *parent, action,
                          [spec, ctrl_id, cue, activate](GuiService& service,
                                                         const std::int64_t deadline,
                                                         rime::core::CancellationToken cancel) {
                            return null_out(service.ctrl_set_cue(spec, ctrl_id, cue, activate,
                                                                 deadline, cancel));
                          });
    }

    case kCtrlAdd: {
      if (!split()) return JS_EXCEPTION;
      GuiService::GuiControlCall call;
      CtrlResultFace face = CtrlResultFace::Number;
      const ControlKind kind = control->kind;
      if (kind == ControlKind::ListBox || kind == ControlKind::ComboBox ||
          kind == ControlKind::DropDownList) {
        if (end != 1) return JS_ThrowTypeError(context, "Add(value)");
        if (JS_IsArray(argv[0])) {
          if (!read_string_array(context, argv[0], "value", call.columns)) return JS_EXCEPTION;
        } else {
          std::string value;
          if (!read_string(context, argv[0], "value", value)) return JS_EXCEPTION;
          call.columns.push_back(std::move(value));
        }
        call.op = GuiService::GuiControlCallOp::ListAdd;
      } else if (kind == ControlKind::ListView) {
        if (end < 1 || end > 5) {
          return JS_ThrowTypeError(context, "Add(columns, selected?, focused?, checked?, imageIndex?)");
        }
        if (!read_string_array(context, argv[0], "columns", call.columns)) return JS_EXCEPTION;
        if (!optional_tri_arg(context, argv, 1, end, "selected", call.selected) ||
            !optional_tri_arg(context, argv, 2, end, "focused", call.focused) ||
            !optional_tri_arg(context, argv, 3, end, "checked", call.checked)) {
          return JS_EXCEPTION;
        }
        std::optional<int> image_index;
        if (!optional_int_arg(context, argv, 4, end, "imageIndex", image_index)) {
          return JS_EXCEPTION;
        }
        if (image_index.has_value()) {
          if (*image_index < 0) {
            return JS_ThrowRangeError(context, "Add: imageIndex is out of range");
          }
          call.image_index = *image_index;
        }
        call.op = GuiService::GuiControlCallOp::LvAddRow;
      } else if (kind == ControlKind::TreeView) {
        if (end < 1 || end > 6) {
          return JS_ThrowTypeError(context,
                                    "Add(text, parentId?, selected?, expanded?, bold?, imageIndex?)");
        }
        if (!read_string(context, argv[0], "text", call.text)) return JS_EXCEPTION;
        std::optional<std::uint64_t> parent;
        if (!optional_id_arg(context, argv, 1, end, "parentId", parent)) return JS_EXCEPTION;
        if (parent.has_value()) {
          call.item = *parent;
          call.has_item = true;
        }
        if (!optional_tri_arg(context, argv, 2, end, "selected", call.selected) ||
            !optional_tri_arg(context, argv, 3, end, "expanded", call.expanded) ||
            !optional_tri_arg(context, argv, 4, end, "bold", call.bold)) {
          return JS_EXCEPTION;
        }
        std::optional<int> image_index;
        if (!optional_int_arg(context, argv, 5, end, "imageIndex", image_index)) {
          return JS_EXCEPTION;
        }
        if (image_index.has_value()) {
          if (*image_index < 0) {
            return JS_ThrowRangeError(context, "Add: imageIndex is out of range");
          }
          call.image_index = *image_index;
        }
        call.op = GuiService::GuiControlCallOp::TvAdd;
        face = CtrlResultFace::Item;
      } else {
        return JS_ThrowTypeError(context, "Add() is not supported by this control");
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call, face);
    }

    case kCtrlChoose: {
      if (!split()) return JS_EXCEPTION;
      const ControlKind kind = control->kind;
      if (kind != ControlKind::ListBox && kind != ControlKind::ComboBox &&
          kind != ControlKind::DropDownList) {
        return JS_ThrowTypeError(context, "Choose() needs a list control");
      }
      if (end != 1) return JS_ThrowTypeError(context, "Choose(index | text)");
      GuiService::GuiControlCall call;
      call.op = GuiService::GuiControlCallOp::ListChoose;
      if (JS_IsString(argv[0])) {
        if (!read_string(context, argv[0], "text", call.text)) return JS_EXCEPTION;
      } else {
        std::optional<std::int64_t> at;
        if (!optional_index_arg(context, argv, 0, end, "Choose", "index", at)) return JS_EXCEPTION;
        if (!at.has_value()) return JS_ThrowTypeError(context, "Choose(index | text)");
        call.index = *at;
        call.has_index = true;
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Number);
    }

    case kCtrlDelete: {
      if (!split()) return JS_EXCEPTION;
      GuiService::GuiControlCall call;
      const ControlKind kind = control->kind;
      if (kind == ControlKind::ListBox || kind == ControlKind::ComboBox ||
          kind == ControlKind::DropDownList) {
        if (end > 1) return JS_ThrowTypeError(context, "Delete(index?)");
        call.op = GuiService::GuiControlCallOp::ListDelete;
        std::optional<std::int64_t> at;
        if (!optional_index_arg(context, argv, 0, end, "Delete", "index", at)) return JS_EXCEPTION;
        if (at.has_value()) {
          call.index = *at;
          call.has_index = true;
        }
      } else if (kind == ControlKind::ListView) {
        if (end > 1) return JS_ThrowTypeError(context, "Delete(index?)");
        call.op = GuiService::GuiControlCallOp::LvDeleteRows;
        std::optional<std::int64_t> at;
        if (!optional_index_arg(context, argv, 0, end, "Delete", "index", at)) return JS_EXCEPTION;
        if (at.has_value()) {
          call.index = *at;
          call.has_index = true;
        }
      } else if (kind == ControlKind::TreeView) {
        if (end > 1) return JS_ThrowTypeError(context, "Delete(nodeId?)");
        call.op = GuiService::GuiControlCallOp::TvDelete;
        std::optional<std::uint64_t> id;
        if (!optional_id_arg(context, argv, 0, end, "node id", id)) return JS_EXCEPTION;
        if (id.has_value()) {
          call.item = *id;
          call.has_item = true;
        }
      } else {
        return JS_ThrowTypeError(context, "Delete() is not supported by this control");
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Number);
    }

    case kCtrlInsert: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::ListView) {
        return JS_ThrowTypeError(context, "Insert() needs a ListView");
      }
      if (end < 2 || end > 6) {
        return JS_ThrowTypeError(context, "Insert(index, columns, selected?, focused?, checked?, imageIndex?)");
      }
      GuiService::GuiControlCall call;
      call.op = GuiService::GuiControlCallOp::LvInsertRow;
      if (!required_index_arg(context, argv, 0, end, "Insert", "index", call.index)) {
        return JS_EXCEPTION;
      }
      call.has_index = true;
      if (!read_string_array(context, argv[1], "columns", call.columns)) return JS_EXCEPTION;
      if (!optional_tri_arg(context, argv, 2, end, "selected", call.selected) ||
          !optional_tri_arg(context, argv, 3, end, "focused", call.focused) ||
          !optional_tri_arg(context, argv, 4, end, "checked", call.checked)) {
        return JS_EXCEPTION;
      }
      std::optional<int> insert_image;
      if (!optional_int_arg(context, argv, 5, end, "imageIndex", insert_image)) {
        return JS_EXCEPTION;
      }
      if (insert_image.has_value()) {
        if (*insert_image < 0) {
          return JS_ThrowRangeError(context, "Insert: imageIndex is out of range");
        }
        call.image_index = *insert_image;
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Number);
    }

    case kCtrlModify: {
      if (!split()) return JS_EXCEPTION;
      GuiService::GuiControlCall call;
      CtrlResultFace face = CtrlResultFace::Number;
      if (control->kind == ControlKind::ListView) {
        if (end < 1 || end > 6) {
          return JS_ThrowTypeError(context,
                                    "Modify(index, columns?, selected?, focused?, checked?, imageIndex?)");
        }
        call.op = GuiService::GuiControlCallOp::LvModifyRow;
        if (!required_index_arg(context, argv, 0, end, "Modify", "index", call.index)) {
          return JS_EXCEPTION;
        }
        call.has_index = true;
        if (end > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
          if (!read_string_array(context, argv[1], "columns", call.columns)) return JS_EXCEPTION;
        }
        if (!optional_tri_arg(context, argv, 2, end, "selected", call.selected) ||
            !optional_tri_arg(context, argv, 3, end, "focused", call.focused) ||
            !optional_tri_arg(context, argv, 4, end, "checked", call.checked)) {
          return JS_EXCEPTION;
        }
        std::optional<int> modify_image;
        if (!optional_int_arg(context, argv, 5, end, "imageIndex", modify_image)) {
          return JS_EXCEPTION;
        }
        if (modify_image.has_value()) {
          if (*modify_image < 0) {
            return JS_ThrowRangeError(context, "Modify: imageIndex is out of range");
          }
          call.image_index = *modify_image;
        }
      } else if (control->kind == ControlKind::TreeView) {
        if (end < 1 || end > 6) {
          return JS_ThrowTypeError(context,
                                    "Modify(nodeId, text?, selected?, expanded?, bold?, imageIndex?)");
        }
        call.op = GuiService::GuiControlCallOp::TvModify;
        if (!required_id_arg(context, argv, 0, end, "Modify", call.item)) return JS_EXCEPTION;
        call.has_item = true;
        if (!optional_string_arg(context, argc, argv, 1, "Modify", "text", call.text)) {
          return JS_EXCEPTION;
        }
        if (!optional_tri_arg(context, argv, 2, end, "selected", call.selected) ||
            !optional_tri_arg(context, argv, 3, end, "expanded", call.expanded) ||
            !optional_tri_arg(context, argv, 4, end, "bold", call.bold)) {
          return JS_EXCEPTION;
        }
        std::optional<int> modify_node_image;
        if (!optional_int_arg(context, argv, 5, end, "imageIndex", modify_node_image)) {
          return JS_EXCEPTION;
        }
        if (modify_node_image.has_value()) {
          if (*modify_node_image < 0) {
            return JS_ThrowRangeError(context, "Modify: imageIndex is out of range");
          }
          call.image_index = *modify_node_image;
        }
        face = CtrlResultFace::Item;
      } else {
        return JS_ThrowTypeError(context, "Modify() needs a ListView or TreeView");
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call, face);
    }

    case kCtrlGetCount: {
      if (!split()) return JS_EXCEPTION;
      GuiService::GuiControlCall call;
      if (control->kind == ControlKind::ListView) {
        if (end > 1) return JS_ThrowTypeError(context, "GetCount(mode?)");
        call.op = GuiService::GuiControlCallOp::LvGetCount;
        if (!optional_string_arg(context, argc, argv, 0, "GetCount", "mode", call.mode)) {
          return JS_EXCEPTION;
        }
        call.mode = ascii_lower(call.mode);
      } else if (control->kind == ControlKind::TreeView) {
        if (end > 0) return JS_ThrowTypeError(context, "GetCount()");
        call.op = GuiService::GuiControlCallOp::TvGetCount;
      } else {
        return JS_ThrowTypeError(context, "GetCount() needs a ListView or TreeView");
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Number);
    }

    case kCtrlGetText: {
      if (!split()) return JS_EXCEPTION;
      GuiService::GuiControlCall call;
      if (control->kind == ControlKind::ListView) {
        if (end < 1 || end > 2) return JS_ThrowTypeError(context, "GetText(row, column?)");
        call.op = GuiService::GuiControlCallOp::LvGetText;
        if (!required_index_arg(context, argv, 0, end, "GetText", "row", call.index)) {
          return JS_EXCEPTION;
        }
        call.has_index = true;
        std::optional<std::int64_t> column;
        if (!optional_index_arg(context, argv, 1, end, "GetText", "column", column)) {
          return JS_EXCEPTION;
        }
        if (column.has_value()) {
          call.sub = *column;
          call.has_sub = true;
        }
      } else if (control->kind == ControlKind::TreeView) {
        if (end != 1) return JS_ThrowTypeError(context, "GetText(nodeId)");
        call.op = GuiService::GuiControlCallOp::TvGetText;
        if (!required_id_arg(context, argv, 0, end, "GetText", call.item)) return JS_EXCEPTION;
        call.has_item = true;
      } else {
        return JS_ThrowTypeError(context, "GetText() needs a ListView or TreeView");
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Text);
    }

    case kCtrlGetNext: {
      if (!split()) return JS_EXCEPTION;
      GuiService::GuiControlCall call;
      CtrlResultFace face = CtrlResultFace::NumberOrNull;
      if (control->kind == ControlKind::ListView) {
        if (end > 2) return JS_ThrowTypeError(context, "GetNext(start?, filter?)");
        call.op = GuiService::GuiControlCallOp::LvGetNext;
        if (end > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
          std::int64_t start = -1;
          const std::string label("GetNext: start must be an integer >= -1");
          if (!js_int64_strict(context, argv[0], start, label.c_str())) return JS_EXCEPTION;
          if (start < -1 || start > std::numeric_limits<int>::max()) {
            return JS_ThrowRangeError(context, "GetNext: start is out of range");
          }
          call.index = start;
          call.has_index = true;
        }
        if (!optional_string_arg(context, argc, argv, 1, "GetNext", "filter", call.mode)) {
          return JS_EXCEPTION;
        }
        call.mode = ascii_lower(call.mode);
        if (!call.mode.empty() && call.mode != "selected" && call.mode != "focused" &&
            call.mode != "checked") {
          return JS_ThrowTypeError(context, "GetNext: filter is selected, focused or checked");
        }
      } else if (control->kind == ControlKind::TreeView) {
        if (end > 2) return JS_ThrowTypeError(context, "GetNext(nodeId?, traversal?)");
        call.op = GuiService::GuiControlCallOp::TvGetNext;
        std::optional<std::uint64_t> id;
        if (!optional_id_arg(context, argv, 0, end, "node id", id)) return JS_EXCEPTION;
        if (id.has_value()) {
          call.item = *id;
          call.has_item = true;
        }
        if (!optional_string_arg(context, argc, argv, 1, "GetNext", "traversal", call.mode)) {
          return JS_EXCEPTION;
        }
        call.mode = ascii_lower(call.mode);
        if (call.mode.empty()) call.mode = "sibling";
        if (call.mode != "sibling" && call.mode != "document") {
          return JS_ThrowTypeError(context, "GetNext: traversal is sibling or document");
        }
        face = CtrlResultFace::ItemOrNull;
      } else {
        return JS_ThrowTypeError(context, "GetNext() needs a ListView or TreeView");
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call, face);
    }

    case kCtrlInsertCol:
    case kCtrlModifyCol: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::ListView) {
        return JS_ThrowTypeError(context, "column methods need a ListView");
      }
      const bool insert = magic == kCtrlInsertCol;
      const char* signature =
          insert ? "InsertCol(index?, title?, width?, align?)" : "ModifyCol(index, title?, width?, align?)";
      if ((!insert && end < 1) || end > 4) return JS_ThrowTypeError(context, "%s", signature);
      GuiService::GuiControlCall call;
      call.op = insert ? GuiService::GuiControlCallOp::LvInsertCol
                       : GuiService::GuiControlCallOp::LvModifyCol;
      if (insert) {
        std::optional<std::int64_t> at;
        if (!optional_index_arg(context, argv, 0, end, signature, "index", at)) return JS_EXCEPTION;
        if (at.has_value()) {
          call.index = *at;
          call.has_index = true;
        }
      } else {
        if (!required_index_arg(context, argv, 0, end, signature, "index", call.index)) {
          return JS_EXCEPTION;
        }
        call.has_index = true;
      }
      if (end > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        std::string title;
        if (!read_string(context, argv[1], "title", title)) return JS_EXCEPTION;
        call.title = std::move(title);
      }
      std::optional<int> width;
      if (!optional_int_arg(context, argv, 2, end, "width", width)) return JS_EXCEPTION;
      if (width.has_value()) call.width = *width;
      if (end > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
        std::string align;
        if (!read_string(context, argv[3], "align", align)) return JS_EXCEPTION;
        align = ascii_lower(align);
        if (align != "left" && align != "center" && align != "right") {
          return JS_ThrowTypeError(context, "%s: align is left, center or right", signature);
        }
        call.align = std::move(align);
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Number);
    }

    case kCtrlDeleteCol: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::ListView) {
        return JS_ThrowTypeError(context, "DeleteCol() needs a ListView");
      }
      if (end != 1) return JS_ThrowTypeError(context, "DeleteCol(index)");
      GuiService::GuiControlCall call;
      call.op = GuiService::GuiControlCallOp::LvDeleteCol;
      if (!required_index_arg(context, argv, 0, end, "DeleteCol", "index", call.index)) {
        return JS_EXCEPTION;
      }
      call.has_index = true;
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Number);
    }

    case kCtrlGet: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::TreeView) {
        return JS_ThrowTypeError(context, "Get() needs a TreeView");
      }
      if (end != 2) return JS_ThrowTypeError(context, "Get(nodeId, attribute)");
      GuiService::GuiControlCall call;
      call.op = GuiService::GuiControlCallOp::TvGet;
      if (!required_id_arg(context, argv, 0, end, "Get", call.item)) return JS_EXCEPTION;
      call.has_item = true;
      if (!read_string(context, argv[1], "attribute", call.mode)) return JS_EXCEPTION;
      call.mode = ascii_lower(call.mode);
      if (call.mode != "expanded" && call.mode != "checked" && call.mode != "bold") {
        return JS_ThrowTypeError(context, "Get: attribute is expanded, checked or bold");
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Bool);
    }

    case kCtrlGetChild:
    case kCtrlGetParent:
    case kCtrlGetPrev: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::TreeView) {
        return JS_ThrowTypeError(context, "tree navigation needs a TreeView");
      }
      const char* signature =
          magic == kCtrlGetChild ? "GetChild(nodeId)" : magic == kCtrlGetParent ? "GetParent(nodeId)"
                                                                                : "GetPrev(nodeId)";
      if (end != 1) return JS_ThrowTypeError(context, "%s", signature);
      GuiService::GuiControlCall call;
      call.op = magic == kCtrlGetChild ? GuiService::GuiControlCallOp::TvGetChild
                : magic == kCtrlGetParent ? GuiService::GuiControlCallOp::TvGetParent
                                          : GuiService::GuiControlCallOp::TvGetPrev;
      if (!required_id_arg(context, argv, 0, end, signature, call.item)) return JS_EXCEPTION;
      call.has_item = true;
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::ItemOrNull);
    }

    case kCtrlGetSelection: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::TreeView) {
        return JS_ThrowTypeError(context, "GetSelection() needs a TreeView");
      }
      if (end > 0) return JS_ThrowTypeError(context, "GetSelection()");
      GuiService::GuiControlCall call;
      call.op = GuiService::GuiControlCallOp::TvGetSelection;
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::ItemOrNull);
    }

    case kCtrlSetParts: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::StatusBar) {
        return JS_ThrowTypeError(context, "SetParts() needs a StatusBar");
      }
      if (end != 1) return JS_ThrowTypeError(context, "SetParts(widths)");
      GuiService::GuiControlCall call;
      call.op = GuiService::GuiControlCallOp::SbSetParts;
      if (!read_int_array(context, argv[0], "widths", call.widths)) return JS_EXCEPTION;
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Number);
    }

    case kCtrlSetText: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::StatusBar) {
        return JS_ThrowTypeError(context, "SetText() needs a StatusBar");
      }
      if (end < 1 || end > 3) {
        return JS_ThrowTypeError(context, "SetText(text, part?, borders?)");
      }
      GuiService::GuiControlCall call;
      call.op = GuiService::GuiControlCallOp::SbSetText;
      if (!read_string(context, argv[0], "text", call.text)) return JS_EXCEPTION;
      std::optional<std::int64_t> part;
      if (!optional_index_arg(context, argv, 1, end, "SetText", "part", part)) return JS_EXCEPTION;
      if (part.has_value()) {
        call.index = *part;
        call.has_index = true;
      }
      if (!optional_tri_arg(context, argv, 2, end, "borders", call.borders)) {
        return JS_EXCEPTION;
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Number);
    }

    case kCtrlSetIcon: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::StatusBar) {
        return JS_ThrowTypeError(context, "SetIcon() needs a StatusBar");
      }
      if (end < 1 || end > 3) {
        return JS_ThrowTypeError(context, "SetIcon(file, iconIndex?, part?)");
      }
      GuiService::GuiControlCall call;
      call.op = GuiService::GuiControlCallOp::SbSetIcon;
      if (!read_string(context, argv[0], "file", call.text)) return JS_EXCEPTION;
      std::optional<int> icon_index;
      if (!optional_int_arg(context, argv, 1, end, "iconIndex", icon_index)) return JS_EXCEPTION;
      if (icon_index.has_value()) {
        if (*icon_index < 0) return JS_ThrowRangeError(context, "SetIcon: iconIndex is out of range");
        call.icon_index = *icon_index;
      }
      std::optional<std::int64_t> part;
      if (!optional_index_arg(context, argv, 2, end, "SetIcon", "part", part)) return JS_EXCEPTION;
      if (part.has_value()) {
        call.index = *part;
        call.has_index = true;
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Number);
    }

    case kCtrlUseTab: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::Tab) {
        return JS_ThrowTypeError(context, "UseTab() needs a Tab control");
      }
      if (end < 1 || end > 2) return JS_ThrowTypeError(context, "UseTab(index | label, exactMatch?)");
      GuiService::GuiControlCall call;
      call.op = GuiService::GuiControlCallOp::TabUseTab;
      if (JS_IsString(argv[0])) {
        if (!read_string(context, argv[0], "label", call.text)) return JS_EXCEPTION;
      } else {
        std::optional<std::int64_t> at;
        if (!optional_index_arg(context, argv, 0, end, "UseTab", "index", at)) {
          return JS_EXCEPTION;
        }
        if (!at.has_value()) return JS_ThrowTypeError(context, "UseTab(index | label, exactMatch?)");
        call.index = *at;
        call.has_index = true;
      }
      bool exact_match = true;
      if (!optional_bool_arg(context, argv, 1, end, "exactMatch", exact_match)) {
        return JS_EXCEPTION;
      }
      call.mode = exact_match ? "exact" : "prefix";
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Number);
    }

    case kCtrlSetFormat: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::DateTime) {
        return JS_ThrowTypeError(context, "SetFormat() needs a DateTime control");
      }
      if (end != 1) return JS_ThrowTypeError(context, "SetFormat(picture)");
      GuiService::GuiControlCall call;
      call.op = GuiService::GuiControlCallOp::DtSetFormat;
      if (!read_string(context, argv[0], "picture", call.text)) return JS_EXCEPTION;
      if (call.text.empty()) return JS_ThrowTypeError(context, "SetFormat: picture must not be empty");
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Null);
    }

    case kCtrlSetHotkey: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::Hotkey) {
        return JS_ThrowTypeError(context, "SetHotkey() needs a Hotkey control");
      }
      if (end != 2) return JS_ThrowTypeError(context, "SetHotkey(modifiers, key)");
      std::vector<std::string> modifiers;
      if (!read_string_array(context, argv[0], "modifiers", modifiers)) return JS_EXCEPTION;
      GuiService::GuiControlCall call;
      call.op = GuiService::GuiControlCallOp::HotkeySet;
      for (const std::string& name : modifiers) {
        const std::string lower = ascii_lower(name);
        if (lower == "shift" || lower == "lshift" || lower == "rshift") {
          call.hot_modifiers |= HOTKEYF_SHIFT;
        } else if (lower == "ctrl" || lower == "control" || lower == "lctrl" ||
                   lower == "rctrl" || lower == "lcontrol" || lower == "rcontrol") {
          call.hot_modifiers |= HOTKEYF_CONTROL;
        } else if (lower == "alt" || lower == "lalt" || lower == "ralt") {
          call.hot_modifiers |= HOTKEYF_ALT;
        } else {
          return JS_ThrowTypeError(context, "SetHotkey: modifier is shift, ctrl or alt");
        }
      }
      std::string key;
      if (!read_string(context, argv[1], "key", key)) return JS_EXCEPTION;
      std::uint32_t vk = 0;
      std::string key_error;
      if (!chord_key(ascii_lower(key), vk, key_error)) {
        JS_ThrowTypeError(context, "SetHotkey: %s", key_error.c_str());
        return JS_EXCEPTION;
      }
      if (vk == 0 || vk > 0xFF) {
        return JS_ThrowTypeError(context, "SetHotkey: key has no hotkey virtual key");
      }
      call.hot_vk = static_cast<std::uint8_t>(vk);
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Null);
    }

    case kCtrlSetImageList: {
      if (!split()) return JS_EXCEPTION;
      if (control->kind != ControlKind::ListView && control->kind != ControlKind::TreeView) {
        return JS_ThrowTypeError(context, "SetImageList() needs a ListView or TreeView");
      }
      if (end < 1 || end > 2) {
        return JS_ThrowTypeError(context, "SetImageList(imageList, size?)");
      }
      bool ok = false;
      const std::uint64_t list_id = identity_of(context, argv[0], "ImageList", ok);
      if (!ok) return JS_EXCEPTION;
      const auto found = state->image_lists.find(list_id);
      if (found == state->image_lists.end() || found->second.destroyed) {
        throw_invalid_state(context, kDestroyed);
        return JS_EXCEPTION;
      }
      GuiService::GuiControlCall call;
      call.op = GuiService::GuiControlCallOp::SetImageList;
      call.list = list_id;
      if (!optional_string_arg(context, argc, argv, 1, "SetImageList", "size", call.mode)) {
        return JS_EXCEPTION;
      }
      call.mode = ascii_lower(call.mode);
      if (!call.mode.empty() && call.mode != "normal" && call.mode != "small" &&
          call.mode != "state") {
        return JS_ThrowTypeError(context, "SetImageList: size is normal, small or state");
      }
      return start_ctrl_invoke(context, state, *parent, spec, ctrl_id, action, call,
                               CtrlResultFace::Null);
    }

    default:
      return JS_ThrowTypeError(context, "GuiControl: unknown method");
  }
}

JSValue ctrl_prop_get(JSContext* context, JSValueConst this_val, int, JSValueConst*, int magic,
                      void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  GuiJsControl* control = ctrl_of(context, this_val, state);
  if (!control) return JS_EXCEPTION;

  if (magic == kCtrlPropHwnd) return throw_policy_refusal(context);
  if (!require_live_ctrl(context, *state, *control)) return JS_EXCEPTION;
  GuiJs* parent = parent_of(context, state, *control);
  if (!parent) return JS_EXCEPTION;

  switch (magic) {
    case kCtrlPropName:
      return JS_NewStringLen(context, control->name.data(), control->name.size());
    case kCtrlPropType:
      return JS_NewString(context, control_type_name(control->kind).c_str());
    case kCtrlPropClassNN:
      return JS_NewString(
          context, (control_type_name(control->kind) + std::to_string(control->ordinal)).c_str());
    case kCtrlPropGui:
      return gui_object_for(context, state, binding, control->gui_id);
    default:
      break;
  }

  const GuiService::GuiSpec spec = make_spec(*parent);
  const std::uint64_t ctrl_id = control->id;

  switch (magic) {
    case kCtrlPropEnabled:
    case kCtrlPropVisible:
    case kCtrlPropFocused: {
      const bool wanted_enabled = magic == kCtrlPropEnabled;
      const bool wanted_visible = magic == kCtrlPropVisible;
      return start_gui_op(
          context, state, *parent, ActionOptions{},
          [spec, ctrl_id, wanted_enabled, wanted_visible](
              GuiService& service, const std::int64_t deadline,
              rime::core::CancellationToken cancel) -> AsyncOutcome {
            bool value = false;
            Error error = wanted_enabled
                              ? service.ctrl_get_enabled(spec, ctrl_id, value, deadline, cancel)
                              : wanted_visible
                                    ? service.ctrl_get_visible(spec, ctrl_id, value, deadline,
                                                               cancel)
                                    : service.ctrl_focused(spec, ctrl_id, value, deadline, cancel);
            if (!error.ok()) return async_failure(error);
            return async_success(value ? "true" : "false");
          });
    }

    case kCtrlPropText:
      return start_gui_op(
          context, state, *parent, ActionOptions{},
          [spec, ctrl_id](GuiService& service, const std::int64_t deadline,
                          rime::core::CancellationToken cancel) -> AsyncOutcome {
            std::string text;
            if (const Error error = service.ctrl_get_text(spec, ctrl_id, text, deadline, cancel);
                !error.ok()) {
              return async_failure(error);
            }
            return async_success(json::stringify(json::Value::string(text)));
          });

    case kCtrlPropValue: {
      if (!control_has_value(control->kind)) {
        return rejected_value_error(context, "this control type has no value");
      }
      return start_gui_op(
          context, state, *parent, ActionOptions{},
          [spec, ctrl_id](GuiService& service, const std::int64_t deadline,
                          rime::core::CancellationToken cancel) -> AsyncOutcome {
            json::Value out = json::Value::object();
            if (const Error error =
                    service.ctrl_get_value(spec, ctrl_id, out, deadline, cancel);
                !error.ok()) {
              return async_failure(error);
            }
            return async_success(json::stringify(out));
          });
    }

    default:
      return JS_ThrowTypeError(context, "GuiControl: unknown property");
  }
}

JSValue ctrl_prop_set(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                      int magic, void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  GuiJsControl* control = ctrl_of(context, this_val, state);
  if (!control) return JS_EXCEPTION;
  if (magic == kCtrlPropHwnd) return throw_policy_refusal(context);
  if (argc < 1) return JS_ThrowTypeError(context, "setter requires a value");
  if (!require_live_ctrl(context, *state, *control)) return JS_EXCEPTION;
  GuiJs* parent = parent_of(context, state, *control);
  if (!parent) return JS_EXCEPTION;

  const GuiService::GuiSpec spec = make_spec(*parent);
  const std::uint64_t ctrl_id = control->id;
  ActionOptions action;

  switch (magic) {
    case kCtrlPropName: {
      std::string name;
      if (!read_string(context, argv[0], "Name", name)) return JS_EXCEPTION;
      control->name = std::move(name);
      return JS_UNDEFINED;
    }

    case kCtrlPropText: {
      std::string text;
      if (!read_string(context, argv[0], "Text", text)) return JS_EXCEPTION;
      fire_and_forget(context,
                      start_gui_op(context, state, *parent, action,
                                   [spec, ctrl_id, text](GuiService& service,
                                                         const std::int64_t deadline,
                                                         rime::core::CancellationToken cancel) {
                                     return null_out(service.ctrl_set_text(
                                         spec, ctrl_id, text, deadline, cancel));
                                   }));
      return JS_UNDEFINED;
    }

    case kCtrlPropValue: {
      json::Value value = json::Value::object();
      if (!js_to_control_value(context, argv[0], control->kind, value)) return JS_EXCEPTION;
      fire_and_forget(context,
                      start_gui_op(context, state, *parent, action,
                                   [spec, ctrl_id, value](GuiService& service,
                                                          const std::int64_t deadline,
                                                          rime::core::CancellationToken cancel) {
                                     return null_out(service.ctrl_set_value(
                                         spec, ctrl_id, value, deadline, cancel));
                                   }));
      return JS_UNDEFINED;
    }

    case kCtrlPropEnabled:
    case kCtrlPropVisible: {
      const bool is_enabled = magic == kCtrlPropEnabled;
      bool flag = false;
      if (!read_flag(context, argv[0], is_enabled ? "Enabled" : "Visible", flag)) {
        return JS_EXCEPTION;
      }
      fire_and_forget(context,
                      start_gui_op(context, state, *parent, action,
                                   [spec, ctrl_id, is_enabled, flag](
                                       GuiService& service, const std::int64_t deadline,
                                       rime::core::CancellationToken cancel) {
                                     return is_enabled
                                                ? null_out(service.ctrl_set_enabled(
                                                      spec, ctrl_id, flag, deadline, cancel))
                                                : null_out(service.ctrl_set_visible(
                                                      spec, ctrl_id, flag, deadline, cancel));
                                   }));
      return JS_UNDEFINED;
    }

    default:
      return JS_ThrowTypeError(context, "GuiControl: unknown property");
  }
}

// ---- member tables ----------------------------------------------------------

bool define_gui_members(JSContext* context, JSValue object, GuiModuleBinding* binding) {
  for (int magic = 0; magic < kGuiMethodCount; ++magic) {
    const bool is_iterator = magic == kGuiIterator;
    const char* name = is_iterator ? "Symbol.iterator" : kGuiMethodNames[magic];
    JSValue closure =
        JS_NewCClosure(context, gui_method, name, nullptr, kGuiMethodLengths[magic], magic,
                       binding);
    if (JS_IsException(closure)) return false;
    if (is_iterator) {
      const JSAtom atom = iterator_atom(context);
      if (atom == JS_ATOM_NULL) {
        JS_FreeValue(context, closure);
        return false;
      }
      const int defined =
          JS_DefinePropertyValue(context, object, atom, closure, JS_PROP_ENUMERABLE);
      JS_FreeAtom(context, atom);
      if (defined < 0) return false;
      continue;
    }
    if (JS_SetPropertyStr(context, object, kGuiMethodNames[magic], closure) < 0) return false;
  }
  for (int magic = 0; magic < kGuiPropCount; ++magic) {
    const char* name = kGuiPropNames[magic];
    JSValue getter = JS_NewCClosure(context, gui_prop_get, name, nullptr, 0, magic, binding);
    if (JS_IsException(getter)) return false;
    JSValue setter = JS_UNDEFINED;
    if (kGuiPropWritable[magic]) {
      setter = JS_NewCClosure(context, gui_prop_set, name, nullptr, 1, magic, binding);
      if (JS_IsException(setter)) {
        JS_FreeValue(context, getter);
        return false;
      }
    }
    const JSAtom atom = JS_NewAtom(context, name);
    if (atom == JS_ATOM_NULL) {
      JS_FreeValue(context, getter);
      JS_FreeValue(context, setter);
      return false;
    }
    // Consumes getter and setter on both outcomes.
    const int defined =
        JS_DefinePropertyGetSet(context, object, atom, getter, setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(context, atom);
    if (defined < 0) return false;
  }
  return true;
}

bool define_ctrl_members(JSContext* context, JSValue object, GuiModuleBinding* binding) {
  for (int magic = 0; magic < kCtrlMethodCount; ++magic) {
    JSValue closure = JS_NewCClosure(context, ctrl_method, kCtrlMethodNames[magic], nullptr,
                                     kCtrlMethodLengths[magic], magic, binding);
    if (JS_IsException(closure)) return false;
    if (JS_SetPropertyStr(context, object, kCtrlMethodNames[magic], closure) < 0) return false;
  }
  for (int magic = 0; magic < kCtrlPropCount; ++magic) {
    const char* name = kCtrlPropNames[magic];
    JSValue getter = JS_NewCClosure(context, ctrl_prop_get, name, nullptr, 0, magic, binding);
    if (JS_IsException(getter)) return false;
    JSValue setter = JS_UNDEFINED;
    if (kCtrlPropWritable[magic]) {
      setter = JS_NewCClosure(context, ctrl_prop_set, name, nullptr, 1, magic, binding);
      if (JS_IsException(setter)) {
        JS_FreeValue(context, getter);
        return false;
      }
    }
    const JSAtom atom = JS_NewAtom(context, name);
    if (atom == JS_ATOM_NULL) {
      JS_FreeValue(context, getter);
      JS_FreeValue(context, setter);
      return false;
    }
    const int defined =
        JS_DefinePropertyGetSet(context, object, atom, getter, setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(context, atom);
    if (defined < 0) return false;
  }
  return true;
}

// ---- object shells ----------------------------------------------------------

JSValue build_gui_object(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                         const std::uint64_t gui_id) {
  const auto found = state->guis.find(gui_id);
  if (found == state->guis.end() || found->second.destroyed) {
    throw_invalid_state(context, kDestroyed);
    return JS_EXCEPTION;
  }
  GuiJs& gui = found->second;
  JSValue object = JS_NewObject(context);
  if (JS_IsException(object)) return JS_EXCEPTION;
  if (!define_identity(context, object, gui_id, "gui") ||
      !define_gui_members(context, object, binding)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  if (!ensure_registry(context, binding, state)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  // make_weak consumes its argument, so hand it a duplicate.
  JSValue weak = make_weak(context, JS_DupValue(context, object));
  if (JS_IsException(weak)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  JS_FreeValue(context, gui.weak);
  gui.weak = weak;
  registry_register(context, state, gui_id, object);
  return object;
}

JSValue build_ctrl_object(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                          const std::uint64_t ctrl_id) {
  const auto found = state->controls.find(ctrl_id);
  if (found == state->controls.end() || found->second.destroyed) {
    throw_invalid_state(context, kNoControl);
    return JS_EXCEPTION;
  }
  GuiJsControl& control = found->second;
  JSValue object = JS_NewObject(context);
  if (JS_IsException(object)) return JS_EXCEPTION;
  if (!define_identity(context, object, ctrl_id, "GuiControl") ||
      !define_ctrl_members(context, object, binding)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  JSValue weak = make_weak(context, JS_DupValue(context, object));
  if (JS_IsException(weak)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  JS_FreeValue(context, control.weak);
  control.weak = weak;
  return object;
}

// ---- Menu object family (batch 3c) ------------------------------------------

GuiJsMenu* menu_of(JSContext* context, JSValueConst this_val, GuiJsState* state) {
  bool ok = false;
  const std::uint64_t id = identity_of(context, this_val, "Menu", ok);
  if (!ok) return nullptr;
  const auto found = state->menus.find(id);
  if (found == state->menus.end()) {
    JS_ThrowTypeError(context, "Menu receiver expected");
    return nullptr;
  }
  return &found->second;
}

bool require_live_menu(JSContext* context, const GuiJsMenu& menu) {
  if (menu.destroyed) {
    throw_invalid_state(context, kDestroyed);
    return false;
  }
  return true;
}

void release_menu(JSContext* context, GuiJsState* state, GuiJsMenu& menu) {
  if (menu.destroyed) return;
  menu.destroyed = true;
  JSValue live = weak_deref(context, menu.weak);
  if (!JS_IsException(live)) registry_unregister(context, state, live);
  JS_FreeValue(context, live);
  for (auto& [name, fn] : menu.callbacks) JS_FreeValue(context, fn);
  menu.callbacks.clear();
  JS_FreeValue(context, menu.weak);
  menu.weak = JS_UNDEFINED;
  if (menu.channel != 0 && state->host) {
    (void)state->host->remove_callback(menu.channel);
    menu.channel = 0;
  }
}

// Menus own no host channel (popup Show is modal-RETURNCMD, menubar clicks
// ride the owner Gui's channel), so this mirrors start_gui_op minus the
// materialized flag. Same capability gate, same worker lane.
template <typename Body>
JSValue start_menu_op(JSContext* context, GuiJsState* state, const ActionOptions& action,
                      Body body) {
  GuiCall call;
  call.service = state->service;
  call.kernel = state->kernel;
  call.deadline = wait_deadline_unix_ms(action.deadline_ms);
  if (action.cancellation_id != 0) {
    call.cancel = state->host->cancellation_token(action.cancellation_id);
  }
  return start_async(
      context,
      [call, body = std::move(body)]() mutable -> AsyncOutcome {
        if (!call.kernel->allows(kUiCreateCapability)) {
          return capability_denied(kUiCreateCapability);
        }
        return body(*call.service, call.deadline, call.cancel);
      },
      action.cancellation_id);
}

JSValue menu_object_for(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                        const std::uint64_t menu_id) {
  const auto found = state->menus.find(menu_id);
  if (found == state->menus.end() || found->second.destroyed) {
    throw_invalid_state(context, kDestroyed);
    return JS_EXCEPTION;
  }
  JSValue live = weak_deref(context, found->second.weak);
  if (JS_IsException(live)) return JS_EXCEPTION;
  if (!JS_IsUndefined(live)) return live;
  return build_menu_object(context, state, binding, menu_id);
}

// Invokes the item callback for a MenuSelect channel event. Runs on the JS
// thread (channel dispatch), never in a worker body.
void invoke_menu_callback(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                          GuiJsMenu& menu, const std::string& name) {
  const auto found = menu.callbacks.find(name);
  if (found == menu.callbacks.end() || JS_IsUndefined(found->second)) return;
  JSValue target = menu_object_for(context, state, binding, menu.id);
  if (JS_IsException(target)) {
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
    return;
  }
  JSValue name_arg = JS_NewStringLen(context, name.data(), name.size());
  if (JS_IsException(name_arg)) {
    JS_FreeValue(context, target);
    return;
  }
  JSValueConst call_args[2] = {target, name_arg};
  JSValue ignored = JS_Call(context, found->second, JS_UNDEFINED, 2, call_args);
  JS_FreeValue(context, target);
  JS_FreeValue(context, name_arg);
  if (JS_IsException(ignored)) {
    JSValue exception = JS_GetException(context);
    const char* text = JS_ToCString(context, exception);
    if (text && state->host) (void)state->host->record("gui", text);
    if (text) JS_FreeCString(context, text);
    JS_FreeValue(context, exception);
  } else {
    JS_FreeValue(context, ignored);
  }
}

enum MenuMethod {
  kMenuAdd = 0,
  kMenuInsert,
  kMenuDelete,
  kMenuRename,
  kMenuCheck,
  kMenuUncheck,
  kMenuEnable,
  kMenuDisable,
  kMenuToggleCheck,
  kMenuToggleEnable,
  kMenuSetColor,
  kMenuSetIcon,
  kMenuShow,
  kMenuDestroy,
  kMenuAddStandard,
  kMenuMethodCount
};

constexpr const char* kMenuMethodNames[] = {
    "Add",         "Insert",      "Delete",  "Rename",      "Check",   "Uncheck",
    "Enable",      "Disable",     "ToggleCheck", "ToggleEnable", "SetColor", "SetIcon",
    "Show",        "Destroy",     "AddStandard"};
constexpr int kMenuMethodLengths[] = {2, 3, 1, 2, 1, 1, 1, 1, 1, 1, 2, 4, 2, 0, 0};

static_assert(sizeof(kMenuMethodNames) / sizeof(kMenuMethodNames[0]) == kMenuMethodCount);
static_assert(sizeof(kMenuMethodLengths) / sizeof(kMenuMethodLengths[0]) == kMenuMethodCount);

// Reads a menu-item selector (exact name or 0-based position) into the call.
bool read_menu_selector(JSContext* context, JSValueConst value, const char* signature,
                        GuiService::GuiMenuCall& call) {
  if (JS_IsString(value)) {
    if (!read_string(context, value, "item", call.text)) return false;
    if (call.text.empty()) {
      JS_ThrowTypeError(context, "%s: item name must not be empty", signature);
      return false;
    }
    return true;
  }
  std::int64_t raw = 0;
  const std::string label = std::string(signature) + ": item must be a name or a position";
  if (!js_int64_strict(context, value, raw, label.c_str())) return false;
  if (raw < 0) {
    JS_ThrowRangeError(context, "%s: item position is out of range", signature);
    return false;
  }
  call.index = raw;
  call.has_index = true;
  return true;
}

// Callback-map sync for mutating menu calls (batch 3c). The map lives on
// the JS thread and mutates only in then_chain continuations - worker
// bodies never touch JS state. Tags keep one closure pair for every op:
// Add/Insert commit the pending fn, Delete drops one name ("" = clear
// all), Rename moves one entry (empty new name = drop).
enum MenuSyncTag { kSyncAdd, kSyncDelete, kSyncRename };

struct MenuSync {
  GuiModuleBinding* binding{nullptr};
  MenuSyncTag tag{kSyncAdd};
  std::uint64_t menu_id{0};
  std::string name;      // Add/Insert item name
  std::string new_name;  // Rename target ("" = to separator, drop)
  bool clear_all{false};  // Delete() with no selector
  JSValue fn{JS_UNDEFINED};  // Add/Insert pending callback (else undefined)
};

JSValue menu_sync_ok(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void* opaque) {
  std::unique_ptr<MenuSync> sync(static_cast<MenuSync*>(opaque));
  GuiJsState* state = sync->binding ? state_or_null(sync->binding) : nullptr;
  if (state) {
    const auto found = state->menus.find(sync->menu_id);
    if (found != state->menus.end() && !found->second.destroyed) {
      GuiJsMenu& menu = found->second;
      if (sync->tag == kSyncAdd && !sync->name.empty() && !JS_IsUndefined(sync->fn)) {
        const auto old = menu.callbacks.find(sync->name);
        if (old != menu.callbacks.end()) JS_FreeValue(context, old->second);
        menu.callbacks[sync->name] = sync->fn;
        sync->fn = JS_UNDEFINED;  // owned by the map now
      } else if (sync->tag == kSyncDelete) {
        // The resolution is the deleted name; clear-all rode the closure
        // (a deleted separator also resolves "", which must NOT clear).
        if (sync->clear_all) {
          for (auto& [name, fn] : menu.callbacks) JS_FreeValue(context, fn);
          menu.callbacks.clear();
        } else if (argc > 0 && JS_IsString(argv[0])) {
          const char* text = JS_ToCString(context, argv[0]);
          std::string gone = text ? text : "";
          if (text) JS_FreeCString(context, text);
          if (!gone.empty()) {
            const auto old = menu.callbacks.find(gone);
            if (old != menu.callbacks.end()) {
              JS_FreeValue(context, old->second);
              menu.callbacks.erase(old);
            }
          }
        }
      } else if (sync->tag == kSyncRename) {
        // The resolution is the OLD name; the new one rode the closure.
        std::string old_name;
        if (argc > 0 && JS_IsString(argv[0])) {
          const char* text = JS_ToCString(context, argv[0]);
          if (text) {
            old_name = text;
            JS_FreeCString(context, text);
          }
        }
        if (old_name.empty()) {
          // By-position rename of a separator: nothing keyed, nothing moves.
        } else {
          const auto old = menu.callbacks.find(old_name);
          if (old != menu.callbacks.end()) {
            JSValue fn = old->second;
            menu.callbacks.erase(old);
            if (!sync->new_name.empty()) {
              const auto clash = menu.callbacks.find(sync->new_name);
              if (clash != menu.callbacks.end()) JS_FreeValue(context, clash->second);
              menu.callbacks[sync->new_name] = fn;
            } else {
              JS_FreeValue(context, fn);
            }
          }
        }
      }
    }
  }
  if (!JS_IsUndefined(sync->fn)) JS_FreeValue(context, sync->fn);
  return argc > 0 ? JS_DupValue(context, argv[0]) : JS_UNDEFINED;
}

JSValue menu_sync_err(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                      void* opaque) {
  std::unique_ptr<MenuSync> sync(static_cast<MenuSync*>(opaque));
  if (!JS_IsUndefined(sync->fn)) JS_FreeValue(context, sync->fn);
  if (argc > 0) return JS_Throw(context, JS_DupValue(context, argv[0]));
  return JS_ThrowInternalError(context, "menu sync failed");
}

// Chains a mutating menu promise through the map sync. ONE MenuSync serves
// both closures: promise semantics run exactly one of them, and the runner
// deletes it (the other closure is never invoked after registration, so its
// dangling pointer is never touched).
JSValue chain_menu_sync(JSContext* context, JSValue promise, MenuSync* sync) {
  JSValue on_ok = JS_NewCClosure(context, menu_sync_ok, "menuSync", nullptr, 1, 0, sync);
  if (JS_IsException(on_ok)) {
    delete sync;
    JS_FreeValue(context, promise);
    return JS_EXCEPTION;
  }
  JSValue on_error =
      JS_NewCClosure(context, menu_sync_err, "menuSyncErr", nullptr, 1, 0, sync);
  if (JS_IsException(on_error)) {
    // Neither closure registered: nothing will delete sync.
    // then_chain frees on_ok below; sync dies here.
    JS_FreeValue(context, on_ok);
    delete sync;
    JS_FreeValue(context, promise);
    return JS_EXCEPTION;
  }
  return then_chain(context, promise, on_ok, on_error);
}

// Menu result faces: the service speaks one GuiMenuResult; each method
// declares which face it shows. Delete resolves the deleted name (""
// = cleared all), Rename the old name, Add/Insert the 0-based position.
enum class MenuResultFace { Null, Number, String, Bool };

JSValue start_menu_invoke(JSContext* context, GuiJsState* state, std::uint64_t menu_id,
                          std::uint64_t channel, const ActionOptions& action,
                          GuiService::GuiMenuCall call, const MenuResultFace face) {
  call.channel = channel;
  return start_menu_op(
      context, state, action,
      [menu_id, call, face](GuiService& service, const std::int64_t deadline,
                            rime::core::CancellationToken cancel) mutable -> AsyncOutcome {
        GuiService::GuiMenuResult result;
        if (const Error error = service.menu_invoke(menu_id, call, result, deadline, cancel);
            !error.ok()) {
          return async_failure(error);
        }
        switch (face) {
          case MenuResultFace::Number:
            return async_success(std::to_string(result.number));
          case MenuResultFace::String:
            return async_success(json::stringify(json::Value::string(result.text)));
          case MenuResultFace::Bool:
            return async_success(result.boolean ? "true" : "false");
          case MenuResultFace::Null:
          default:
            return async_success("null");
        }
      });
}

JSValue menu_method(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                    int magic, void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  GuiJsMenu* menu = menu_of(context, this_val, state);
  if (!menu) return JS_EXCEPTION;
  if (!require_live_menu(context, *menu)) return JS_EXCEPTION;
  const std::uint64_t menu_id = menu->id;
  const std::uint64_t channel = menu->channel;

  int end = 0;
  ActionOptions action;
  const auto split = [&]() {
    const int action_index = split_action(context, argc, argv, end);
    return parse_action_at(context, argv, action_index, action);
  };

  // Add/Insert share the (name?, callback|submenu?) tail. A function is a
  // per-item callback (JS-side map, fired on select); a Menu object is a
  // submenu attachment (pump-side HMENU link); omitted is a plain item.
  auto parse_add_tail = [&](const char* signature, int name_at, GuiService::GuiMenuCall& call,
                            bool& want_callback, JSValue& callback_out) -> bool {
    want_callback = false;
    callback_out = JS_UNDEFINED;
    if (name_at < end && !JS_IsUndefined(argv[name_at]) && !JS_IsNull(argv[name_at])) {
      if (!read_string(context, argv[name_at], "name", call.text)) return false;
    }
    const int second_at = name_at + 1;
    if (second_at < end && !JS_IsUndefined(argv[second_at]) && !JS_IsNull(argv[second_at])) {
      if (JS_IsFunction(context, argv[second_at])) {
        want_callback = true;
        callback_out = JS_DupValue(context, argv[second_at]);
      } else {
        bool ok = false;
        const std::uint64_t submenu = identity_of(context, argv[second_at], "Menu", ok);
        if (!ok) return false;
        const auto found = state->menus.find(submenu);
        if (found == state->menus.end() || found->second.destroyed) {
          JS_FreeValue(context, callback_out);
          throw_invalid_state(context, kDestroyed);
          return false;
        }
        call.submenu = submenu;
        call.has_submenu = true;
      }
    }
    if (call.text.empty() && (want_callback || call.has_submenu)) {
      JS_FreeValue(context, callback_out);
      callback_out = JS_UNDEFINED;
      JS_ThrowTypeError(context, "%s: a callback or submenu item needs a name", signature);
      return false;
    }
    return true;
  };

  switch (magic) {
    case kMenuAddStandard: {
      if (!split()) return JS_EXCEPTION;
      if (end > 0) return JS_ThrowTypeError(context, "AddStandard()");
      return throw_coded(context, "unsupported_by_policy",
                         "unsupported-by-policy: AddStandard fills the tray menu, which does "
                         "not exist in this runtime; build the menu with Add");
    }

    case kMenuAdd:
    case kMenuInsert: {
      if (!split()) return JS_EXCEPTION;
      const bool insert = magic == kMenuInsert;
      const char* signature = insert ? "Insert(before?, name?, callback|submenu?)"
                                     : "Add(name?, callback|submenu?)";
      const int max_args = insert ? 3 : 2;
      if (end > max_args) return JS_ThrowTypeError(context, "%s", signature);
      GuiService::GuiMenuCall call;
      call.op = insert ? GuiService::GuiMenuCallOp::Insert : GuiService::GuiMenuCallOp::Add;
      int name_at = 0;
      if (insert && end > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_IsString(argv[0])) {
          if (!read_string(context, argv[0], "before", call.before)) return JS_EXCEPTION;
          call.has_before = true;
        } else {
          std::int64_t raw = 0;
          const std::string label("Insert: before must be a name or a position");
          if (!js_int64_strict(context, argv[0], raw, label.c_str())) return JS_EXCEPTION;
          if (raw < 0) return JS_ThrowRangeError(context, "Insert: position is out of range");
          call.index = raw;
          call.has_index = true;
        }
        name_at = 1;
      }
      bool want_callback = false;
      JSValue callback = JS_UNDEFINED;
      if (!parse_add_tail(signature, name_at, call, want_callback, callback)) {
        return JS_EXCEPTION;
      }
      JSValue promise = start_menu_invoke(context, state, menu_id, channel, action, call,
                                            MenuResultFace::Number);
      if (JS_IsException(promise)) {
        JS_FreeValue(context, callback);
        return JS_EXCEPTION;
      }
      if (!want_callback) return promise;
      // The callback commits into the name map only once the pump confirms
      // the item exists; a refused Add leaves no stale entry behind.
      MenuSync* sync = new (std::nothrow) MenuSync();
      if (!sync) {
        JS_FreeValue(context, callback);
        JS_FreeValue(context, promise);
        return JS_ThrowInternalError(context, "out of memory");
      }
      sync->binding = binding;
      sync->tag = kSyncAdd;
      sync->menu_id = menu_id;
      sync->name = call.text;
      sync->fn = callback;
      return chain_menu_sync(context, promise, sync);
    }

    case kMenuDelete: {
      if (!split()) return JS_EXCEPTION;
      if (end > 1) return JS_ThrowTypeError(context, "Delete(item?)");
      GuiService::GuiMenuCall call;
      call.op = GuiService::GuiMenuCallOp::Delete;
      const bool clear_all = end == 0 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]);
      if (!clear_all && !read_menu_selector(context, argv[0], "Delete", call)) {
        return JS_EXCEPTION;
      }
      JSValue promise = start_menu_invoke(context, state, menu_id, channel, action, call,
                                            MenuResultFace::String);
      if (JS_IsException(promise)) return JS_EXCEPTION;
      MenuSync* sync = new (std::nothrow) MenuSync();
      if (!sync) {
        JS_FreeValue(context, promise);
        return JS_ThrowInternalError(context, "out of memory");
      }
      sync->binding = binding;
      sync->tag = kSyncDelete;
      sync->menu_id = menu_id;
      sync->clear_all = clear_all;
      return chain_menu_sync(context, promise, sync);
    }

    case kMenuRename: {
      if (!split()) return JS_EXCEPTION;
      if (end < 1 || end > 2) return JS_ThrowTypeError(context, "Rename(item, newName?)");
      GuiService::GuiMenuCall call;
      call.op = GuiService::GuiMenuCallOp::Rename;
      if (!read_menu_selector(context, argv[0], "Rename", call)) return JS_EXCEPTION;
      std::string new_name;
      if (end > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (!read_string(context, argv[1], "newName", new_name)) return JS_EXCEPTION;
        call.text2 = new_name;
      }
      JSValue promise = start_menu_invoke(context, state, menu_id, channel, action, call,
                                            MenuResultFace::String);
      if (JS_IsException(promise)) return JS_EXCEPTION;
      MenuSync* sync = new (std::nothrow) MenuSync();
      if (!sync) {
        JS_FreeValue(context, promise);
        return JS_ThrowInternalError(context, "out of memory");
      }
      sync->binding = binding;
      sync->tag = kSyncRename;
      sync->menu_id = menu_id;
      sync->new_name = new_name;
      return chain_menu_sync(context, promise, sync);
    }

    case kMenuCheck:
    case kMenuUncheck:
    case kMenuEnable:
    case kMenuDisable:
    case kMenuToggleCheck:
    case kMenuToggleEnable: {
      if (!split()) return JS_EXCEPTION;
      const char* signature = magic == kMenuCheck         ? "Check(item)"
                              : magic == kMenuUncheck    ? "Uncheck(item)"
                              : magic == kMenuEnable     ? "Enable(item)"
                              : magic == kMenuDisable    ? "Disable(item)"
                              : magic == kMenuToggleCheck ? "ToggleCheck(item)"
                                                          : "ToggleEnable(item)";
      if (end != 1) return JS_ThrowTypeError(context, "%s", signature);
      GuiService::GuiMenuCall call;
      call.op = magic == kMenuCheck         ? GuiService::GuiMenuCallOp::Check
                : magic == kMenuUncheck     ? GuiService::GuiMenuCallOp::Uncheck
                : magic == kMenuEnable      ? GuiService::GuiMenuCallOp::Enable
                : magic == kMenuDisable     ? GuiService::GuiMenuCallOp::Disable
                : magic == kMenuToggleCheck ? GuiService::GuiMenuCallOp::ToggleCheck
                                            : GuiService::GuiMenuCallOp::ToggleEnable;
      if (!read_menu_selector(context, argv[0], signature, call)) return JS_EXCEPTION;
      return start_menu_invoke(context, state, menu_id, channel, action, call,
                               magic == kMenuToggleCheck || magic == kMenuToggleEnable
                                   ? MenuResultFace::Bool
                                   : MenuResultFace::Null);
    }

    case kMenuSetColor: {
      if (!split()) return JS_EXCEPTION;
      if (end > 2) return JS_ThrowTypeError(context, "SetColor(color?, applyToSubmenus?)");
      GuiService::GuiMenuCall call;
      call.op = GuiService::GuiMenuCallOp::SetColor;
      if (end > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        std::string mirror;
        std::uint32_t color_ref = 0;
        if (!parse_color(context, argv[0], mirror, color_ref)) return JS_EXCEPTION;
        call.has_color = true;
        call.color = color_ref;
      }
      call.apply_to_submenus = true;
      if (!optional_bool_arg(context, argv, 1, end, "applyToSubmenus", call.apply_to_submenus)) {
        return JS_EXCEPTION;
      }
      return start_menu_invoke(context, state, menu_id, channel, action, call,
                               MenuResultFace::Null);
    }

    case kMenuSetIcon: {
      if (!split()) return JS_EXCEPTION;
      if (end < 2 || end > 4) {
        return JS_ThrowTypeError(context, "SetIcon(item, file, iconIndex?, width?)");
      }
      GuiService::GuiMenuCall call;
      call.op = GuiService::GuiMenuCallOp::SetIcon;
      if (!read_menu_selector(context, argv[0], "SetIcon", call)) return JS_EXCEPTION;
      if (!read_string(context, argv[1], "file", call.file)) return JS_EXCEPTION;
      std::optional<int> icon_index;
      if (!optional_int_arg(context, argv, 2, end, "iconIndex", icon_index)) return JS_EXCEPTION;
      if (icon_index.has_value()) {
        if (*icon_index < 0) {
          return JS_ThrowRangeError(context, "SetIcon: iconIndex is out of range");
        }
        call.icon_index = *icon_index;
      }
      std::optional<int> width;
      if (!optional_int_arg(context, argv, 3, end, "width", width)) return JS_EXCEPTION;
      if (width.has_value()) {
        if (*width < 0) return JS_ThrowRangeError(context, "SetIcon: width is out of range");
        call.icon_width = *width;
        call.has_icon_width = true;
      }
      return start_menu_invoke(context, state, menu_id, channel, action, call,
                               MenuResultFace::Null);
    }

    case kMenuShow: {
      if (!split()) return JS_EXCEPTION;
      if (end > 2) return JS_ThrowTypeError(context, "Show(x?, y?)");
      std::optional<int> x;
      std::optional<int> y;
      if (!optional_int_arg(context, argv, 0, end, "x", x) ||
          !optional_int_arg(context, argv, 1, end, "y", y)) {
        return JS_EXCEPTION;
      }
      const bool has_position = x.has_value() && y.has_value();
      const int show_x = x.value_or(0);
      const int show_y = y.value_or(0);
      // The selection ALSO fires the item callback through the menu's own
      // channel (same MenuSelect event as a menubar click); the direct
      // result lets scripts skip callbacks. No JS runs in the worker.
      return start_menu_op(
          context, state, action,
          [menu_id, channel, has_position, show_x, show_y](
              GuiService& service, const std::int64_t deadline,
              rime::core::CancellationToken cancel) mutable -> AsyncOutcome {
            std::uint64_t command = 0;
            std::string name;
            if (const Error error =
                    service.menu_show(menu_id, channel, has_position, show_x, show_y, command,
                                      name, deadline, cancel);
                !error.ok()) {
              return async_failure(error);
            }
            if (command == 0) return async_success("null");
            return async_success(json::stringify(json::Value::string(name)));
          });
    }

    case kMenuDestroy: {
      if (!split()) return JS_EXCEPTION;
      if (end > 0) return JS_ThrowTypeError(context, "Destroy()");
      GuiJsMenu* doomed = menu;
      release_menu(context, state, *doomed);
      ActionOptions no_action;
      fire_and_forget(context,
                      start_menu_op(context, state, no_action,
                                    [menu_id](GuiService& service, const std::int64_t deadline,
                                              rime::core::CancellationToken cancel) {
                                      return null_out(service.menu_destroy(menu_id, deadline,
                                                                           cancel));
                                    }));
      return JS_UNDEFINED;
    }

    default:
      return JS_ThrowTypeError(context, "Menu: unknown method");
  }
}

enum MenuProp {
  kMenuPropClickCount = 0,
  kMenuPropDefault,
  kMenuPropHandle,
  kMenuPropCount
};

constexpr const char* kMenuPropNames[] = {"ClickCount", "Default", "Handle"};

JSValue menu_prop_get(JSContext* context, JSValueConst this_val, int, JSValueConst*, int magic,
                      void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  GuiJsMenu* menu = menu_of(context, this_val, state);
  if (!menu) return JS_EXCEPTION;
  if (!require_live_menu(context, *menu)) return JS_EXCEPTION;
  const std::uint64_t menu_id = menu->id;
  const std::uint64_t channel = menu->channel;

  switch (magic) {
    case kMenuPropClickCount: {
      ActionOptions action;
      GuiService::GuiMenuCall call;
      call.op = GuiService::GuiMenuCallOp::GetClickCount;
      return start_menu_invoke(context, state, menu_id, channel, action, call,
                               MenuResultFace::Number);
    }
    case kMenuPropDefault: {
      ActionOptions action;
      GuiService::GuiMenuCall call;
      call.op = GuiService::GuiMenuCallOp::GetDefault;
      return start_menu_invoke(context, state, menu_id, channel, action, call,
                               MenuResultFace::String);
    }
    case kMenuPropHandle:
    default:
      return throw_coded(context, "unsupported_by_policy",
                         "unsupported-by-policy: raw menu handles are not part of the API; "
                         "use stable menu ids");
  }
}

JSValue menu_prop_set(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                      int magic, void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  GuiJsMenu* menu = menu_of(context, this_val, state);
  if (!menu) return JS_EXCEPTION;
  if (!require_live_menu(context, *menu)) return JS_EXCEPTION;
  const std::uint64_t menu_id = menu->id;
  const std::uint64_t channel = menu->channel;
  if (argc < 1) {
    JS_ThrowTypeError(context, "setter needs a value");
    return JS_EXCEPTION;
  }

  switch (magic) {
    case kMenuPropClickCount: {
      std::int64_t count = 0;
      if (!js_int64_strict(context, argv[0], count, "ClickCount must be 1 or 2")) {
        return JS_EXCEPTION;
      }
      if (count < 1 || count > 2) {
        throw_value_error(context, "ClickCount must be 1 or 2");
        return JS_EXCEPTION;
      }
      ActionOptions action;
      GuiService::GuiMenuCall call;
      call.op = GuiService::GuiMenuCallOp::SetClickCount;
      call.click_count = static_cast<int>(count);
      return start_menu_invoke(context, state, menu_id, channel, action, call,
                               MenuResultFace::Number);
    }
    case kMenuPropDefault: {
      std::string name;
      if (!read_string(context, argv[0], "Default", name)) return JS_EXCEPTION;
      ActionOptions action;
      GuiService::GuiMenuCall call;
      call.op = GuiService::GuiMenuCallOp::SetDefault;
      call.text = name;
      return start_menu_invoke(context, state, menu_id, channel, action, call,
                               MenuResultFace::String);
    }
    case kMenuPropHandle:
    default:
      return throw_coded(context, "unsupported_by_policy",
                         "unsupported-by-policy: raw menu handles are not part of the API; "
                         "use stable menu ids");
  }
}

bool define_menu_members(JSContext* context, JSValue object, GuiModuleBinding* binding) {
  for (int magic = 0; magic < kMenuMethodCount; ++magic) {
    JSValue closure = JS_NewCClosure(context, menu_method, kMenuMethodNames[magic], nullptr,
                                     kMenuMethodLengths[magic], magic, binding);
    if (JS_IsException(closure)) return false;
    if (JS_SetPropertyStr(context, object, kMenuMethodNames[magic], closure) < 0) return false;
  }
  for (int magic = 0; magic < kMenuPropCount; ++magic) {
    const char* name = kMenuPropNames[magic];
    JSValue getter = JS_NewCClosure(context, menu_prop_get, name, nullptr, 0, magic, binding);
    if (JS_IsException(getter)) return false;
    JSValue setter =
        JS_NewCClosure(context, menu_prop_set, name, nullptr, 1, magic, binding);
    if (JS_IsException(setter)) {
      JS_FreeValue(context, getter);
      return false;
    }
    const JSAtom atom = JS_NewAtom(context, name);
    if (atom == JS_ATOM_NULL) {
      JS_FreeValue(context, getter);
      JS_FreeValue(context, setter);
      return false;
    }
    // Consumes getter and setter on both outcomes.
    const int defined =
        JS_DefinePropertyGetSet(context, object, atom, getter, setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(context, atom);
    if (defined < 0) return false;
  }
  return true;
}

JSValue build_menu_object(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                          const std::uint64_t menu_id) {
  const auto found = state->menus.find(menu_id);
  if (found == state->menus.end() || found->second.destroyed) {
    throw_invalid_state(context, kDestroyed);
    return JS_EXCEPTION;
  }
  GuiJsMenu& menu = found->second;
  JSValue object = JS_NewObject(context);
  if (JS_IsException(object)) return JS_EXCEPTION;
  if (!define_identity(context, object, menu_id, "Menu") ||
      !define_menu_members(context, object, binding)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  if (!ensure_registry(context, binding, state)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  JSValue weak = make_weak(context, JS_DupValue(context, object));
  if (JS_IsException(weak)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  JS_FreeValue(context, menu.weak);
  menu.weak = weak;
  // Negative holdings share the Gui registry: ui_gui_gc branches on sign.
  JSValue held = JS_NewInt64(context, -static_cast<std::int64_t>(menu_id));
  if (JS_IsException(held)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  JSValueConst args[2] = {object, held};
  JSValue ignored = invoke_named(context, state->registry, "register", 2, args);
  JS_FreeValue(context, held);
  JS_FreeValue(context, ignored);
  return object;
}

// ---- ImageList object family (batch 3d) -------------------------------------

GuiJsImageList* image_of(JSContext* context, JSValueConst this_val, GuiJsState* state) {
  bool ok = false;
  const std::uint64_t id = identity_of(context, this_val, "ImageList", ok);
  if (!ok) return nullptr;
  const auto found = state->image_lists.find(id);
  if (found == state->image_lists.end()) {
    JS_ThrowTypeError(context, "ImageList receiver expected");
    return nullptr;
  }
  return &found->second;
}

bool require_live_image(JSContext* context, const GuiJsImageList& list) {
  if (list.destroyed) {
    throw_invalid_state(context, kDestroyed);
    return false;
  }
  return true;
}

void release_image_list(JSContext* context, GuiJsState* state, GuiJsImageList& list) {
  if (list.destroyed) return;
  list.destroyed = true;
  JSValue live = weak_deref(context, list.weak);
  if (!JS_IsException(live) && JS_IsObject(live) && !JS_IsUndefined(state->image_registry)) {
    JSValue ignored = invoke_named(context, state->image_registry, "unregister", 1, &live);
    JS_FreeValue(context, ignored);
  }
  JS_FreeValue(context, live);
  JS_FreeValue(context, list.weak);
  list.weak = JS_UNDEFINED;
}

// The image-list FinalizationRegistry cleanup: the ImageList object is
// gone, so destroy the pump record (detach-first inside, like Destroy).
JSValue ui_image_gc(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv, int,
                    void* opaque) {
  (void)this_val;
  GuiModuleBinding* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_or_null(binding);
  if (!state || argc < 1) return JS_UNDEFINED;
  std::int64_t list_id = 0;
  if (!JS_IsNumber(argv[0]) || JS_ToInt64(context, &list_id, argv[0]) != 0 || list_id <= 0) {
    return JS_UNDEFINED;
  }
  const auto found = state->image_lists.find(static_cast<std::uint64_t>(list_id));
  if (found == state->image_lists.end() || found->second.destroyed) return JS_UNDEFINED;
  GuiJsImageList& list = found->second;
  release_image_list(context, state, list);
  ActionOptions action;
  fire_and_forget(context,
                  start_menu_op(context, state, action,
                                [list_id = static_cast<std::uint64_t>(list_id)](
                                    GuiService& service, const std::int64_t deadline,
                                    rime::core::CancellationToken cancel) {
                                  return null_out(service.image_list_destroy(list_id, deadline,
                                                                             cancel));
                                }));
  return JS_UNDEFINED;
}

bool ensure_image_registry(JSContext* context, GuiModuleBinding* binding, GuiJsState* state) {
  if (!JS_IsUndefined(state->image_registry)) return true;
  JSValue global = JS_GetGlobalObject(context);
  if (JS_IsException(global)) return false;
  JSValue ctor = JS_GetPropertyStr(context, global, "FinalizationRegistry");
  JS_FreeValue(context, global);
  if (JS_IsException(ctor)) return false;
  if (JS_IsUndefined(ctor)) {
    JS_ThrowInternalError(context, "FinalizationRegistry is unavailable");
    return false;
  }
  JSValue cleanup = JS_NewCClosure(context, ui_image_gc, "uiImageGC", nullptr, 1, 0, binding);
  if (JS_IsException(cleanup)) {
    JS_FreeValue(context, ctor);
    return false;
  }
  JSValue registry = JS_CallConstructor(context, ctor, 1, &cleanup);
  JS_FreeValue(context, ctor);
  JS_FreeValue(context, cleanup);
  if (JS_IsException(registry)) return false;
  state->image_registry = registry;
  return true;
}

enum ImageMethod { kImageAdd = 0, kImageDestroy, kImageMethodCount };

constexpr const char* kImageMethodNames[] = {"Add", "Destroy"};
constexpr int kImageMethodLengths[] = {2, 0};

static_assert(sizeof(kImageMethodNames) / sizeof(kImageMethodNames[0]) == kImageMethodCount);
static_assert(sizeof(kImageMethodLengths) / sizeof(kImageMethodLengths[0]) == kImageMethodCount);

JSValue image_method(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                     int magic, void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  GuiJsImageList* list = image_of(context, this_val, state);
  if (!list) return JS_EXCEPTION;
  if (!require_live_image(context, *list)) return JS_EXCEPTION;
  const std::uint64_t list_id = list->id;

  int end = 0;
  ActionOptions action;
  const auto split = [&]() {
    const int action_index = split_action(context, argc, argv, end);
    return parse_action_at(context, argv, action_index, action);
  };

  switch (magic) {
    case kImageAdd: {
      if (!split()) return JS_EXCEPTION;
      if (end < 1 || end > 2) return JS_ThrowTypeError(context, "Add(file, iconIndex?)");
      std::string file;
      if (!read_string(context, argv[0], "file", file)) return JS_EXCEPTION;
      std::optional<int> icon_index;
      if (!optional_int_arg(context, argv, 1, end, "iconIndex", icon_index)) return JS_EXCEPTION;
      if (icon_index.has_value() && *icon_index < 0) {
        return JS_ThrowRangeError(context, "Add: iconIndex is out of range");
      }
      const int index_arg = icon_index.value_or(0);
      return start_menu_op(
          context, state, action,
          [list_id, file, index_arg](GuiService& service, const std::int64_t deadline,
                                     rime::core::CancellationToken cancel) mutable
          -> AsyncOutcome {
            std::int64_t index = 0;
            if (const Error error =
                    service.image_list_add(list_id, file, index_arg, index, deadline, cancel);
                !error.ok()) {
              return async_failure(error);
            }
            return async_success(std::to_string(index));
          });
    }

    case kImageDestroy: {
      if (!split()) return JS_EXCEPTION;
      if (end > 0) return JS_ThrowTypeError(context, "Destroy()");
      GuiJsImageList* doomed = list;
      release_image_list(context, state, *doomed);
      ActionOptions no_action;
      fire_and_forget(context,
                      start_menu_op(context, state, no_action,
                                    [list_id](GuiService& service, const std::int64_t deadline,
                                              rime::core::CancellationToken cancel) {
                                      return null_out(service.image_list_destroy(list_id, deadline,
                                                                                 cancel));
                                    }));
      return JS_UNDEFINED;
    }

    default:
      return JS_ThrowTypeError(context, "ImageList: unknown method");
  }
}

bool define_image_members(JSContext* context, JSValue object, GuiModuleBinding* binding) {
  for (int magic = 0; magic < kImageMethodCount; ++magic) {
    JSValue closure = JS_NewCClosure(context, image_method, kImageMethodNames[magic], nullptr,
                                     kImageMethodLengths[magic], magic, binding);
    if (JS_IsException(closure)) return false;
    if (JS_SetPropertyStr(context, object, kImageMethodNames[magic], closure) < 0) return false;
  }
  return true;
}

JSValue build_image_object(JSContext* context, GuiJsState* state, GuiModuleBinding* binding,
                           const std::uint64_t list_id) {
  const auto found = state->image_lists.find(list_id);
  if (found == state->image_lists.end() || found->second.destroyed) {
    throw_invalid_state(context, kDestroyed);
    return JS_EXCEPTION;
  }
  GuiJsImageList& list = found->second;
  JSValue object = JS_NewObject(context);
  if (JS_IsException(object)) return JS_EXCEPTION;
  if (!define_identity(context, object, list_id, "ImageList") ||
      !define_image_members(context, object, binding)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  if (!ensure_image_registry(context, binding, state)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  JSValue weak = make_weak(context, JS_DupValue(context, object));
  if (JS_IsException(weak)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  JS_FreeValue(context, list.weak);
  list.weak = weak;
  JSValue held = JS_NewInt64(context, static_cast<std::int64_t>(list_id));
  if (JS_IsException(held)) {
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
  }
  JSValueConst args[2] = {object, held};
  JSValue ignored = invoke_named(context, state->image_registry, "register", 2, args);
  JS_FreeValue(context, held);
  JS_FreeValue(context, ignored);
  return object;
}

// ui.createImageList(width, height, initial?, grow?): awaited factory -
// geometry can refuse, so unlike menus the pump record exists before the
// object does (GuiSpec-free strict create).
JSValue ui_create_image_list(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                             void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = ensure_gui_state(context, binding);
  if (!state) return JS_EXCEPTION;
  if (!state->kernel->allows(kUiCreateCapability)) return throw_capability(context);
  if (argc < 2 || argc > 4) {
    return JS_ThrowTypeError(context, "createImageList(width, height, initial?, grow?)");
  }
  std::int64_t width = 0;
  std::int64_t height = 0;
  if (!js_int64_strict(context, argv[0], width, "width must be an integer") ||
      !js_int64_strict(context, argv[1], height, "height must be an integer")) {
    return JS_EXCEPTION;
  }
  std::optional<int> initial;
  std::optional<int> grow;
  if (!optional_int_arg(context, argv, 2, argc, "initial", initial) ||
      !optional_int_arg(context, argv, 3, argc, "grow", grow)) {
    return JS_EXCEPTION;
  }
  const std::uint64_t list_id = state->next_image_id++;
  // No JS slot yet: the service owns the only record until creation
  // resolves, so a refused create leaves nothing behind to clean.
  ActionOptions action;
  JSValue promise = start_menu_op(
      context, state, action,
      [list_id, width, height, initial_arg = initial.value_or(0), grow_arg = grow.value_or(8)](
          GuiService& service, const std::int64_t deadline,
          rime::core::CancellationToken cancel) mutable -> AsyncOutcome {
        if (const Error error =
                service.image_list_create(list_id, static_cast<int>(width),
                                          static_cast<int>(height), initial_arg, grow_arg, deadline,
                                          cancel);
            !error.ok()) {
          return async_failure(error);
        }
        return async_success(std::to_string(list_id));
      });
  if (JS_IsException(promise)) {
    return JS_EXCEPTION;
  }
  // The object (and its JS slot) builds once creation resolves.
  JSValue on_ok = JS_NewCFunctionData2(context, image_list_then, "onImageList", 1, 0, 0, nullptr);
  if (JS_IsException(on_ok)) {
    JS_FreeValue(context, promise);
    return JS_EXCEPTION;
  }
  return then_chain(context, promise, on_ok, JS_UNDEFINED);
}

// createImageList resolves with the list id; this mints the JS slot and
// maps it to the object (a refused create never reaches here, so no slot
// can leak).
JSValue image_list_then(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        JSValueConst*) {
  if (argc < 1 || !JS_IsNumber(argv[0])) return JS_NULL;
  std::int64_t id = 0;
  if (JS_ToInt64(context, &id, argv[0]) != 0 || id <= 0) return JS_NULL;
  GuiModuleBinding* binding = binding_of(context);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  const std::uint64_t list_id = static_cast<std::uint64_t>(id);
  GuiJsImageList& slot = state->image_lists[list_id];
  slot.id = list_id;
  return build_image_object(context, state, binding, list_id);
}

// fromHwnd continuations: the service resolves a Gui id (fromHwnd) or a
// [gui, control] pair (fromHwndControl); these map them to live objects,
// null stays null (TargetGone is a miss, never a throw).
JSValue from_hwnd_then(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       JSValueConst*) {
  if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) return JS_NULL;
  std::int64_t id = 0;
  if (!JS_IsNumber(argv[0]) || JS_ToInt64(context, &id, argv[0]) != 0 || id <= 0) return JS_NULL;
  GuiModuleBinding* binding = binding_of(context);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  return gui_object_for(context, state, binding, static_cast<std::uint64_t>(id));
}

JSValue from_hwnd_ctrl_then(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                            JSValueConst*) {
  if (argc < 1 || !JS_IsArray(argv[0])) return JS_NULL;
  JSValue pair_value = JS_GetPropertyUint32(context, argv[0], 1);
  if (JS_IsException(pair_value)) return JS_EXCEPTION;
  std::int64_t id = 0;
  const bool number =
      JS_IsNumber(pair_value) && JS_ToInt64(context, &id, pair_value) == 0 && id > 0;
  JS_FreeValue(context, pair_value);
  if (!number) return JS_NULL;
  GuiModuleBinding* binding = binding_of(context);
  GuiJsState* state = state_of(context, binding);
  if (!state) return JS_EXCEPTION;
  return ctrl_object_for(context, state, binding, static_cast<std::uint64_t>(id));
}

// ui.fromHwnd(windowId) / ui.fromHwndControl(controlId) (batch 3e): the
// DoD's §4.7 fork - only runtime-owned windows/controls resolve, anything
// else is null (TargetGone). windowId/controlId are automation stable ids;
// raw handles never enter the runtime.
JSValue ui_from_hwnd(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = ensure_gui_state(context, binding);
  if (!state) return JS_EXCEPTION;
  if (!state->kernel->allows(kUiCreateCapability)) return throw_capability(context);
  if (argc != 1) return JS_ThrowTypeError(context, "fromHwnd(windowId)");
  std::int64_t raw = 0;
  if (!js_int64_strict(context, argv[0], raw, "fromHwnd(windowId)")) return JS_EXCEPTION;
  if (raw <= 0) return JS_NULL;
  const std::uint64_t window_id = static_cast<std::uint64_t>(raw);
  ActionOptions action;
  JSValue promise = start_menu_op(
      context, state, action,
      [window_id](GuiService& service, const std::int64_t deadline,
                  rime::core::CancellationToken cancel) mutable -> AsyncOutcome {
        std::uint64_t gui_id = 0;
        if (const Error error = service.gui_from_window(window_id, gui_id, deadline, cancel);
            !error.ok()) {
          if (error.code == Code::TargetGone) return async_success("null");
          return async_failure(error);
        }
        if (gui_id == 0) return async_success("null");
        return async_success(std::to_string(gui_id));
      });
  if (JS_IsException(promise)) return JS_EXCEPTION;
  JSValue on_ok = JS_NewCFunctionData2(context, from_hwnd_then, "onFromHwnd", 1, 0, 0, nullptr);
  if (JS_IsException(on_ok)) {
    JS_FreeValue(context, promise);
    return JS_EXCEPTION;
  }
  return then_chain(context, promise, on_ok, JS_UNDEFINED);
}

JSValue ui_from_hwnd_control(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                             void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = ensure_gui_state(context, binding);
  if (!state) return JS_EXCEPTION;
  if (!state->kernel->allows(kUiCreateCapability)) return throw_capability(context);
  if (argc != 1) return JS_ThrowTypeError(context, "fromHwndControl(controlId)");
  std::int64_t raw = 0;
  if (!js_int64_strict(context, argv[0], raw, "fromHwndControl(controlId)")) return JS_EXCEPTION;
  if (raw <= 0) return JS_NULL;
  const std::uint64_t control_id = static_cast<std::uint64_t>(raw);
  ActionOptions action;
  JSValue promise = start_menu_op(
      context, state, action,
      [control_id](GuiService& service, const std::int64_t deadline,
                   rime::core::CancellationToken cancel) mutable -> AsyncOutcome {
        std::uint64_t gui_id = 0;
        std::uint64_t ctrl_id = 0;
        if (const Error error =
                service.gui_ctrl_from_window(control_id, gui_id, ctrl_id, deadline, cancel);
            !error.ok()) {
          if (error.code == Code::TargetGone) return async_success("null");
          return async_failure(error);
        }
        if (gui_id == 0 || ctrl_id == 0) return async_success("null");
        return async_success("[" + std::to_string(gui_id) + "," + std::to_string(ctrl_id) + "]");
      });
  if (JS_IsException(promise)) return JS_EXCEPTION;
  JSValue on_ok =
      JS_NewCFunctionData2(context, from_hwnd_ctrl_then, "onFromHwndCtrl", 1, 0, 0, nullptr);
  if (JS_IsException(on_ok)) {
    JS_FreeValue(context, promise);
    return JS_EXCEPTION;
  }
  return then_chain(context, promise, on_ok, JS_UNDEFINED);
}

// ---- construction -----------------------------------------------------------

// Builds the FinalizationRegistry whose cleanup is ui_gui_gc. Created once
// per GuiJsState on the JS thread; gui-menu.md §4.5 registers the Gui object
// during construction (ui.createGui -> build_gui_object) and re-registers it
// whenever gui_object_for rebuilds a shell after the weak deref missed. The
// registry is per state, not per object, so one shared instance covers both.
bool ensure_registry(JSContext* context, GuiModuleBinding* binding, GuiJsState* state) {
  if (!JS_IsUndefined(state->registry)) return true;
  JSValue global = JS_GetGlobalObject(context);
  if (JS_IsException(global)) return false;
  JSValue ctor = JS_GetPropertyStr(context, global, "FinalizationRegistry");
  JS_FreeValue(context, global);
  if (JS_IsException(ctor)) return false;
  if (JS_IsUndefined(ctor)) {
    JS_ThrowInternalError(context, "FinalizationRegistry is unavailable");
    return false;
  }
  JSValue cleanup = JS_NewCClosure(context, ui_gui_gc, "uiGuiGC", nullptr, 1, 0, binding);
  if (JS_IsException(cleanup)) {
    JS_FreeValue(context, ctor);
    return false;
  }
  JSValue registry = JS_CallConstructor(context, ctor, 1, &cleanup);
  JS_FreeValue(context, ctor);
  JS_FreeValue(context, cleanup);
  if (JS_IsException(registry)) return false;
  state->registry = registry;
  return true;
}

// Lazily builds (and wires) the JS-thread Gui state. Rebuilds when the
// existing one is closed or belongs to another host/context, so a second
// runtime never writes through someone else's service pointer.
GuiJsState* ensure_gui_state(JSContext* context, GuiModuleBinding* binding) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) {
    JS_ThrowInternalError(context, "runtime host is gone");
    return nullptr;
  }
  if (!binding || !binding->service || !binding->kernel) {
    JS_ThrowInternalError(context, "rime:ui is not wired");
    return nullptr;
  }
  if (binding->gui && !binding->gui->closed.load() && binding->gui->host == host &&
      binding->gui->js_context == context) {
    return binding->gui.get();
  }
  auto state = std::make_shared<GuiJsState>();
  state->host = host;
  state->service = binding->service;
  state->kernel = binding->kernel;
  state->js_context = context;
  std::weak_ptr<GuiJsState> weak = state;
  binding->gui = std::move(state);
  host->add_teardown([weak] {
    if (const auto locked = weak.lock()) locked->teardown();
  });
  // The pump only ever pushes; it never touches JS. Guarded so a push that
  // races teardown becomes a no-op instead of a use-after-free.
  binding->service->set_event_sink([weak](const std::uint64_t channel, std::string payload) {
    const auto locked = weak.lock();
    if (!locked || locked->closed.load()) return;
    (void)locked->host->event_queue()->push(channel, std::move(payload));
  });
  return binding->gui.get();
}

// ui.createGui(options?, title?, eventObj?): the factory behind `new Gui()`.
// Synchronous - construction never crosses threads (gui-menu.md §4.1) - but
// gated on ui.create like every other rime:ui entry.
JSValue ui_create_gui(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                      void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = ensure_gui_state(context, binding);
  if (!state) return JS_EXCEPTION;
  if (!state->kernel->allows(kUiCreateCapability)) return throw_capability(context);
  if (argc > 3) return JS_ThrowTypeError(context, "createGui(options?, title?, eventObj?)");

  GuiInit init;
  if (!parse_gui_init(context, argc, argv, init, "createGui(options?, title?, eventObj?)")) {
    return JS_EXCEPTION;
  }
  const std::uint64_t gui_id = state->next_gui_id++;
  GuiJs& slot = state->guis[gui_id];
  slot.id = gui_id;
  if (!apply_gui_init(context, state, binding, slot, init)) {
    init.free(context);
    state->guis.erase(gui_id);
    return JS_EXCEPTION;
  }
  init.free(context);
  slot.initialized = true;
  JSValue object = build_gui_object(context, state, binding, gui_id);
  if (JS_IsException(object)) {
    release_gui(context, state, slot);
    state->guis.erase(gui_id);
    return JS_EXCEPTION;
  }
  return object;
}

// ui.createMenu(): the Menu factory. Synchronous and pump-free: ids are
// minted JS-side and the pump materializes the record on first touch
// (ensure_menu_record adopts the carried channel order-free, the GuiSpec
// shape). Only the dispatch channel allocates here; teardown mirrors Gui.
JSValue ui_create_menu(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void* opaque) {
  auto* binding = static_cast<GuiModuleBinding*>(opaque);
  GuiJsState* state = ensure_gui_state(context, binding);
  if (!state) return JS_EXCEPTION;
  if (!state->kernel->allows(kUiCreateCapability)) return throw_capability(context);
  if (argc > 0) return JS_ThrowTypeError(context, "createMenu()");
  (void)argv;

  const std::uint64_t menu_id = state->next_menu_id++;
  GuiJsMenu& slot = state->menus[menu_id];
  slot.id = menu_id;
  JSValue closure =
      JS_NewCClosure(context, menu_channel_dispatch, "menuChannel", nullptr, 1, 0, binding);
  if (JS_IsException(closure)) {
    state->menus.erase(menu_id);
    return JS_EXCEPTION;
  }
  std::uint64_t channel = 0;
  if (const Error error = state->host->add_callback(closure, channel); !error.ok()) {
    JS_FreeValue(context, closure);
    state->menus.erase(menu_id);
    JS_ThrowInternalError(context, "%s", error.message.c_str());
    return JS_EXCEPTION;
  }
  slot.channel = channel;
  JSValue object = build_menu_object(context, state, binding, menu_id);
  if (JS_IsException(object)) {
    if (state->host) (void)state->host->remove_callback(channel);
    state->menus.erase(menu_id);
    return JS_EXCEPTION;
  }
  return object;
}

}  // namespace

// ---- rime:ui registration ---------------------------------------------------

bool install_gui_family(JSContext* context, JSValue ui_namespace, GuiModuleBinding* binding) {
  if (!ensure_gui_state(context, binding)) return false;
  JSValue factory = JS_NewCClosure(context, ui_create_gui, "createGui", nullptr, 3, 0, binding);
  if (JS_IsException(factory)) return false;
  if (JS_SetPropertyStr(context, ui_namespace, "createGui", factory) < 0) return false;
  JSValue menu_factory =
      JS_NewCClosure(context, ui_create_menu, "createMenu", nullptr, 0, 0, binding);
  if (JS_IsException(menu_factory)) return false;
  if (JS_SetPropertyStr(context, ui_namespace, "createMenu", menu_factory) < 0) return false;
  JSValue list_factory =
      JS_NewCClosure(context, ui_create_image_list, "createImageList", nullptr, 4, 0, binding);
  if (JS_IsException(list_factory)) return false;
  if (JS_SetPropertyStr(context, ui_namespace, "createImageList", list_factory) < 0) return false;
  JSValue from_hwnd =
      JS_NewCClosure(context, ui_from_hwnd, "fromHwnd", nullptr, 1, 0, binding);
  if (JS_IsException(from_hwnd)) return false;
  if (JS_SetPropertyStr(context, ui_namespace, "fromHwnd", from_hwnd) < 0) return false;
  JSValue from_hwnd_ctrl =
      JS_NewCClosure(context, ui_from_hwnd_control, "fromHwndControl", nullptr, 1, 0, binding);
  if (JS_IsException(from_hwnd_ctrl)) return false;
  if (JS_SetPropertyStr(context, ui_namespace, "fromHwndControl", from_hwnd_ctrl) < 0) return false;
  return true;
}

}  // namespace rime::win32
