#include "rime/win32/js_screen.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "async_task.hpp"
#include "quickjs.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

ScreenModuleBinding* binding_of(JSContext* context) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return nullptr;
  return static_cast<ScreenModuleBinding*>(host->module_data("rime:screen"));
}

// Capability name checked by this module. Pointed at from
// contracts/registry/actions.json (capabilities.screen.capture), so the
// literal lives here at the top of the file rather than inline in the bodies.
constexpr const char* kScreenCaptureCapability = "screen.capture";

json::Value rect_value(const int left, const int top, const int right, const int bottom) {
  json::Value value = json::Value::object();
  value.set("left", json::Value::number(left));
  value.set("top", json::Value::number(top));
  value.set("right", json::Value::number(right));
  value.set("bottom", json::Value::number(bottom));
  return value;
}

json::Value monitor_value(const ScreenService::Monitor& monitor) {
  json::Value value = json::Value::object();
  value.set("index", json::Value::number(monitor.index));
  value.set("primary", json::Value::boolean(monitor.primary));
  value.set("name", json::Value::string(monitor.name));
  value.set("bounds", rect_value(monitor.left, monitor.top, monitor.right, monitor.bottom));
  value.set("work",
            rect_value(monitor.work_left, monitor.work_top, monitor.work_right, monitor.work_bottom));
  return value;
}

// Splits `screen.monitor(index?, options?)`: a leading number is the index,
// anything else must be the options object. Returns false with an exception
// pending on a shape the caller must reject synchronously. The index uses
// js_int64_strict so NaN/Infinity/fractions never silently truncate via
// JS_ToInt32; null/undefined means omitted (primary monitor).
bool parse_monitor_args(JSContext* context, int argc, JSValueConst* argv, int& index,
                        ActionOptions& options) {
  index = 0;
  int next = 0;
  if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]) && !JS_IsObject(argv[0])) {
    int64_t strict = 0;
    if (!js_int64_strict(context, argv[0], strict, "monitor(index)")) return false;
    if (strict < 0 || strict > INT32_MAX) {
      JS_ThrowRangeError(context, "monitor(index): index must be 0 or a positive number");
      return false;
    }
    index = static_cast<int>(strict);
    next = 1;
  }
  if (argc - next > 1 || (argc - next == 1 && !JS_IsObject(argv[next]))) {
    JS_ThrowTypeError(context, "monitor(index?, options?): expected monitor(index?, options?)");
    return false;
  }
  if (argc - next == 1) return parse_action_options(context, argv[next], options);
  return true;
}

// screen.monitorCount(options?): AHK MonitorGetCount. EnumDisplayMonitors
// order, same as AHK, so the count and the indexes of `monitor()` agree.
// Capability: screen.capture. Query, so no Action Trace.
JSValue screen_monitor_count(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                             void*) {
  ScreenModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:screen is not wired");
  }
  if (argc > 1) return JS_ThrowTypeError(context, "monitorCount(options?)");
  ActionOptions options;
  if (argc == 1 && !parse_action_options(context, argv[0], options)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  ScreenService* service = binding->service;
  return start_async(
      context,
      [kernel, service]() -> AsyncOutcome {
        if (!kernel->allows(kScreenCaptureCapability)) {
          return capability_denied(kScreenCaptureCapability);
        }
        int count = 0;
        if (const auto error = service->monitor_count(count); !error.ok()) {
          return async_failure(error);
        }
        json::Value value = json::Value::object();
        value.set("count", json::Value::number(count));
        return async_success(json::stringify(value));
      },
      options.cancellation_id);
}

// screen.monitor(index?, options?): one record that answers AHK's MonitorGet,
// MonitorGetWorkArea, MonitorGetName and MonitorGetPrimary together - bounds,
// work area, device name and the primary flag of a single monitor. An omitted
// index means the primary monitor, exactly as AHK's omitted argument does.
// Capability: screen.capture. Query, so no Action Trace.
JSValue screen_monitor(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void*) {
  ScreenModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:screen is not wired");
  }
  int index = 0;
  ActionOptions options;
  if (!parse_monitor_args(context, argc, argv, index, options)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  ScreenService* service = binding->service;
  return start_async(
      context,
      [kernel, service, index]() -> AsyncOutcome {
        if (!kernel->allows(kScreenCaptureCapability)) {
          return capability_denied(kScreenCaptureCapability);
        }
        ScreenService::Monitor monitor;
        if (const auto error = service->monitor_at(index, monitor); !error.ok()) {
          return async_failure(error);
        }
        return async_success(json::stringify(monitor_value(monitor)));
      },
      options.cancellation_id);
}

// ---- pixel family ---------------------------------------------------------

// Strict integer read: coordinates and colors must be whole numbers, and a
// value outside int32 range is a RangeError instead of a silent truncation.
bool parse_int32(JSContext* context, JSValueConst value, const char* where, int& out) {
  if (!JS_IsNumber(value)) {
    JS_ThrowTypeError(context, "%s must be an integer", where);
    return false;
  }
  double raw = 0;
  if (JS_ToFloat64(context, &raw, value) < 0) return false;
  if (!std::isfinite(raw)) {
    JS_ThrowRangeError(context, "%s must be a finite integer", where);
    return false;
  }
  if (raw != std::floor(raw)) {
    JS_ThrowTypeError(context, "%s must be an integer", where);
    return false;
  }
  if (raw < -2147483648.0 || raw > 2147483647.0) {
    JS_ThrowRangeError(context, "%s must be an integer in -2147483648..2147483647", where);
    return false;
  }
  out = static_cast<int>(raw);
  return true;
}

// screen.pixel(x, y, options?): one pixel as 0xRRGGBB. The rectangle comes
// from the capture seam, so the value is exact for whatever the seam (or the
// desktop) shows. Capability: screen.capture. Query, no Action Trace.
JSValue screen_pixel(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  ScreenModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:screen is not wired");
  }
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "pixel(x, y, options?)");
  int x = 0;
  int y = 0;
  if (!parse_int32(context, argv[0], "pixel(x): x", x)) return JS_EXCEPTION;
  if (!parse_int32(context, argv[1], "pixel(y): y", y)) return JS_EXCEPTION;
  ActionOptions options;
  if (argc == 3 && !parse_action_options(context, argv[2], options)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  ScreenService* service = binding->service;
  return start_async(
      context,
      [kernel, service, x, y]() -> AsyncOutcome {
        if (!kernel->allows(kScreenCaptureCapability)) {
          return capability_denied(kScreenCaptureCapability);
        }
        std::uint32_t color = 0;
        if (const auto error = service->pixel_color(x, y, color); !error.ok()) {
          return async_failure(error);
        }
        json::Value value = json::Value::object();
        value.set("color", json::Value::number(static_cast<double>(color)));
        return async_success(json::stringify(value));
      },
      options.cancellation_id);
}

// Reads the {left, top, right, bottom} rectangle the pixel and image searches
// work in. `call_shape` is the full signature so a bad area names the call it
// came from.
bool parse_area(JSContext* context, JSValueConst value, const char* call_shape, int& left,
                int& top, int& right, int& bottom) {
  if (!JS_IsObject(value)) {
    JS_ThrowTypeError(context, "%s: area must be an object", call_shape);
    return false;
  }
  static constexpr const char* kKeys[] = {"left", "top", "right", "bottom"};
  int* const outputs[] = {&left, &top, &right, &bottom};
  for (std::size_t i = 0; i < 4; ++i) {
    JSValue field = JS_GetPropertyStr(context, value, kKeys[i]);
    if (JS_IsException(field)) return false;
    const bool ok = parse_int32(context, field, kKeys[i], *outputs[i]);
    JS_FreeValue(context, field);
    if (!ok) return false;
  }
  return true;
}

// The trailing options object both searches take: their own `variation` plus
// the shared ActionOptions. Returns false with an exception pending.
bool parse_screen_options(JSContext* context, JSValueConst value, const char* call_shape,
                          int& variation, ActionOptions& options) {
  if (!JS_IsObject(value)) {
    JS_ThrowTypeError(context, "%s: options must be an object", call_shape);
    return false;
  }
  JSValue field = JS_GetPropertyStr(context, value, "variation");
  if (JS_IsException(field)) return false;
  if (!JS_IsUndefined(field) && !JS_IsNull(field)) {
    const bool ok = parse_int32(context, field, "variation", variation);
    JS_FreeValue(context, field);
    if (!ok) return false;
    if (variation < 0 || variation > 255) {
      JS_ThrowRangeError(context, "variation must be in 0..255");
      return false;
    }
  } else {
    JS_FreeValue(context, field);
  }
  return parse_action_options(context, value, options);
}

// screen.pixelSearch(area, color, options?): first matching pixel, scanning
// the top row first and left to right. Resolves {found:false} when nothing
// matched - a miss is a result, not an error. Capability: screen.capture.
// Query, no Action Trace.
JSValue screen_pixel_search(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                            void*) {
  ScreenModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:screen is not wired");
  }
  if (argc < 2 || argc > 3) {
    return JS_ThrowTypeError(context, "pixelSearch(area, color, options?)");
  }
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  if (!parse_area(context, argv[0], "pixelSearch(area, color, options?)", left, top, right,
                  bottom)) {
    return JS_EXCEPTION;
  }
  int color = 0;
  if (!parse_int32(context, argv[1], "pixelSearch color", color)) return JS_EXCEPTION;
  if (color < 0 || color > 0xFFFFFF) {
    return JS_ThrowRangeError(context, "pixelSearch color must be in 0x000000..0xFFFFFF");
  }
  int variation = 0;
  ActionOptions options;
  if (argc == 3 &&
      !parse_screen_options(context, argv[2], "pixelSearch(area, color, options?)", variation,
                            options)) {
    return JS_EXCEPTION;
  }
  rime::action::Kernel* kernel = binding->kernel;
  ScreenService* service = binding->service;
  return start_async(
      context,
      [kernel, service, left, top, right, bottom, color, variation]() -> AsyncOutcome {
        if (!kernel->allows(kScreenCaptureCapability)) {
          return capability_denied(kScreenCaptureCapability);
        }
        bool found = false;
        int x = 0;
        int y = 0;
        if (const auto error =
                service->pixel_search(left, top, right, bottom,
                                      static_cast<std::uint32_t>(color), variation, found, x, y);
            !error.ok()) {
          return async_failure(error);
        }
        json::Value value = json::Value::object();
        value.set("found", json::Value::boolean(found));
        if (found) {
          value.set("x", json::Value::number(x));
          value.set("y", json::Value::number(y));
        }
        return async_success(json::stringify(value));
      },
      options.cancellation_id);
}

// screen.imageSearch(area, imagePath, options?): first position in the
// rectangle where the image file fits, scanning the same way pixelSearch
// does. A miss resolves {found:false} - a result, not an error; a file that
// cannot be decoded is the caller's contract and fails as invalid_contract.
// Capability: screen.capture. Query, no Action Trace.
JSValue screen_image_search(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                            void*) {
  ScreenModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:screen is not wired");
  }
  if (argc < 2 || argc > 3) {
    return JS_ThrowTypeError(context, "imageSearch(area, imagePath, options?)");
  }
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  if (!parse_area(context, argv[0], "imageSearch(area, imagePath, options?)", left, top, right,
                  bottom)) {
    return JS_EXCEPTION;
  }
  if (!JS_IsString(argv[1])) {
    return JS_ThrowTypeError(context,
                             "imageSearch(area, imagePath, options?): imagePath must be a string");
  }
  const char* path = JS_ToCString(context, argv[1]);
  if (!path) return JS_EXCEPTION;
  std::string image_path(path);
  JS_FreeCString(context, path);
  int variation = 0;
  ActionOptions options;
  if (argc == 3 &&
      !parse_screen_options(context, argv[2], "imageSearch(area, imagePath, options?)", variation,
                            options)) {
    return JS_EXCEPTION;
  }
  rime::action::Kernel* kernel = binding->kernel;
  ScreenService* service = binding->service;
  return start_async(
      context,
      [kernel, service, left, top, right, bottom, image_path, variation]() -> AsyncOutcome {
        if (!kernel->allows(kScreenCaptureCapability)) {
          return capability_denied(kScreenCaptureCapability);
        }
        bool found = false;
        int x = 0;
        int y = 0;
        if (const auto error = service->image_search(left, top, right, bottom, image_path,
                                                      variation, found, x, y);
            !error.ok()) {
          return async_failure(error);
        }
        json::Value value = json::Value::object();
        value.set("found", json::Value::boolean(found));
        if (found) {
          value.set("x", json::Value::number(x));
          value.set("y", json::Value::number(y));
        }
        return async_success(json::stringify(value));
      },
      options.cancellation_id);
}

int screen_module_init(JSContext* context, JSModuleDef* module) {
  ScreenModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    JS_ThrowInternalError(context, "rime:screen requires a screen module binding");
    return -1;
  }
  JSValue screen = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue fn = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(fn)) {
      JS_FreeValue(context, screen);
      return false;
    }
    // JS_SetPropertyStr consumes `fn` on both success and failure.
    if (JS_SetPropertyStr(context, screen, name, fn) < 0) {
      JS_FreeValue(context, screen);
      return false;
    }
    return true;
  };
  if (!add("monitorCount", screen_monitor_count, 0) || !add("monitor", screen_monitor, 0) ||
      !add("pixel", screen_pixel, 0) || !add("pixelSearch", screen_pixel_search, 0) ||
      !add("imageSearch", screen_image_search, 0)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "screen", screen);
}

JSModuleDef* create_screen_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:screen", screen_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "screen") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const ScreenModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:screen requires a screen service and kernel"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_screen_module(rime::js::Host& host, ScreenModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:screen", binding);
  if (const auto error = host.modules().add_native(
          "rime:screen", [](JSContext* context) { return create_screen_module(context); });
      !error.ok()) {
    return error;
  }
  return rime::core::Error::none();
}

rime::core::Error register_screen_module(rime::js::Runtime& runtime,
                                         ScreenModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:screen", [](JSContext* context) { return create_screen_module(context); }, binding);
}

}  // namespace rime::win32
