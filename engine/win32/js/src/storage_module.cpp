#include "rime/win32/js_storage.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "async_task.hpp"
#include "quickjs.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

// Capability names checked for this module. Pointed at from
// contracts/registry/actions.json (capabilities.filesystem.read/.write), so
// the literals live here at the top of the file rather than inline in the
// bodies. The read capability is checked inside every read body (there is no
// kernel round trip on that path); the write capability is checked inside the
// write-ish bodies (open in "a"/"w", download, selectors) and by the kernel
// for storage.write itself.
constexpr const char* kStorageReadCapability = "filesystem.read";
constexpr const char* kStorageWriteCapability = "filesystem.write";

// The storage handle id space the executor enforces (1..2^63-1) and the
// uint32 the service's file_read takes.
constexpr std::int64_t kMaxHandle = 9223372036854775807LL;
constexpr std::int64_t kMaxReadCount = 4294967295LL;

StorageModuleBinding* binding_of(JSContext* context) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return nullptr;
  return static_cast<StorageModuleBinding*>(host->module_data("rime:storage"));
}

// Copies a JS string into a std::string. Returns false with an exception
// pending when the conversion fails, so callers can just return JS_EXCEPTION.
bool copy_string(JSContext* context, JSValueConst value, std::string& out) {
  const char* text = JS_ToCString(context, value);
  if (!text) return false;
  out = text;
  JS_FreeCString(context, text);
  return true;
}

// A required string argument. `label` is the full TypeError prefix (for
// example "readText(path): path"), so the message names both the function
// and the slot that was wrong.
bool required_string(JSContext* context, JSValueConst value, const char* label, std::string& out,
                     bool allow_empty = false) {
  if (!JS_IsString(value)) {
    JS_ThrowTypeError(context, "%s must be a string", label);
    return false;
  }
  if (!copy_string(context, value, out)) return false;
  if (!allow_empty && out.empty()) {
    JS_ThrowTypeError(context, "%s must not be empty", label);
    return false;
  }
  return true;
}

// A required integer argument inside an explicit range. Non-numbers,
// fractions and out-of-range values all become TypeErrors instead of being
// silently truncated into a different handle or count.
bool required_int(JSContext* context, JSValueConst value, const char* label, std::int64_t low,
                  std::int64_t high, std::int64_t& out) {
  if (!js_int64_strict(context, value, out, label)) return false;
  if (out < low || out > high) {
    JS_ThrowTypeError(context, "%s must be between %lld and %lld", label,
                      static_cast<long long>(low), static_cast<long long>(high));
    return false;
  }
  return true;
}

// An optional string field of a caller-supplied options object. Missing,
// undefined and null leave `out` untouched; anything else must be a string.
bool optional_string_field(JSContext* context, JSValueConst object, const char* label,
                           const char* name, std::string& out, bool allow_empty) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  const bool present = !JS_IsUndefined(property) && !JS_IsNull(property);
  bool ok = true;
  if (present) {
    std::string slot = std::string(label) + "." + name;
    ok = required_string(context, property, slot.c_str(), out, allow_empty);
  }
  JS_FreeValue(context, property);
  return ok;
}

// An optional boolean field of a caller-supplied options object.
bool optional_bool_field(JSContext* context, JSValueConst object, const char* label,
                         const char* name, bool& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
    if (!JS_IsBool(property)) {
      std::string slot = std::string(label) + "." + name;
      JS_ThrowTypeError(context, "%s must be a boolean", slot.c_str());
      JS_FreeValue(context, property);
      return false;
    }
    const int value = JS_ToBool(context, property);
    if (value < 0) {
      JS_FreeValue(context, property);
      return false;
    }
    out = value != 0;
  }
  JS_FreeValue(context, property);
  return true;
}

// Builds the {"encoding": <name>} object shared by encoding() and setEncoding.
bool make_encoding_object(JSContext* context, const std::string& encoding, JSValue& out) {
  out = JS_NewObject(context);
  if (JS_IsException(out)) return false;
  JSValue value = JS_NewString(context, encoding.c_str());
  if (JS_IsException(value)) {
    JS_FreeValue(context, out);
    out = JS_EXCEPTION;
    return false;
  }
  // JS_SetPropertyStr consumes `value` on both success and failure.
  if (JS_SetPropertyStr(context, out, "encoding", value) < 0) {
    JS_FreeValue(context, out);
    out = JS_EXCEPTION;
    return false;
  }
  return true;
}

// A byte array on the wire: every element stays an integer 0..255, which is
// exactly what the storage.write executor revalidates.
json::Value bytes_json(const std::vector<std::uint8_t>& bytes) {
  json::Value array = json::Value::array();
  for (const std::uint8_t byte : bytes) {
    array.push(json::Value::number(static_cast<double>(byte)));
  }
  return array;
}

// The read result of stat(): size, last write time in unix milliseconds, the
// AHK attribute letter string and the directory flag.
json::Value file_stat_json(const FileStatInfo& info) {
  json::Value result = json::Value::object();
  result.set("size", json::Value::number(static_cast<double>(info.size)));
  result.set("mtimeMs", json::Value::number(static_cast<double>(info.mtime_unix_ms)));
  result.set("attrib", json::Value::string(info.attrib_string));
  result.set("isDir", json::Value::boolean(info.is_dir));
  return result;
}

json::Value dir_entry_json(const DirEntryInfo& entry) {
  json::Value result = json::Value::object();
  result.set("name", json::Value::string(entry.name));
  result.set("isDir", json::Value::boolean(entry.is_dir));
  return result;
}

// One drive query result. Only the field the query asked for carries data;
// the rest keep the service defaults (empty / 0 / -1).
json::Value drive_json(const DriveInfo& info) {
  json::Value result = json::Value::object();
  result.set("letter", json::Value::string(info.letter));
  result.set("filesystem", json::Value::string(info.filesystem));
  result.set("label", json::Value::string(info.label));
  result.set("type", json::Value::string(info.type));
  result.set("status", json::Value::string(info.status));
  result.set("totalBytes", json::Value::number(static_cast<double>(info.total_bytes)));
  result.set("freeBytes", json::Value::number(static_cast<double>(info.free_bytes)));
  result.set("serial", json::Value::number(static_cast<double>(info.serial)));
  json::Value list = json::Value::array();
  for (const std::string& letter : info.list) list.push(json::Value::string(letter));
  result.set("list", list);
  result.set("capacityPercent", json::Value::number(static_cast<double>(info.capacity_percent)));
  return result;
}

// ---- reads -----------------------------------------------------------------

JSValue storage_read_text(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "readText(path, options?)");
  std::string path;
  if (!required_string(context, argv[0], "readText(path): path", path)) return JS_EXCEPTION;
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;

  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  return start_async(
      context,
      [kernel, service, path = std::move(path)]() -> AsyncOutcome {
        if (!kernel->allows(kStorageReadCapability)) {
          return capability_denied(kStorageReadCapability);
        }
        std::string text;
        if (const auto error = service->read_text(path, text); !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("text", json::Value::string(text));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

JSValue storage_read_bytes(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "readBytes(path, options?)");
  std::string path;
  if (!required_string(context, argv[0], "readBytes(path): path", path)) return JS_EXCEPTION;
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;

  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  return start_async(
      context,
      [kernel, service, path = std::move(path)]() -> AsyncOutcome {
        if (!kernel->allows(kStorageReadCapability)) {
          return capability_denied(kStorageReadCapability);
        }
        std::vector<std::uint8_t> bytes;
        if (const auto error = service->read_bytes(path, bytes); !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("bytes", bytes_json(bytes));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

JSValue storage_stat(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "stat(path, options?)");
  std::string path;
  if (!required_string(context, argv[0], "stat(path): path", path)) return JS_EXCEPTION;
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;

  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  return start_async(
      context,
      [kernel, service, path = std::move(path)]() -> AsyncOutcome {
        if (!kernel->allows(kStorageReadCapability)) {
          return capability_denied(kStorageReadCapability);
        }
        FileStatInfo info;
        if (const auto error = service->stat(path, info); !error.ok()) {
          return async_failure(error);
        }
        return async_success(json::stringify(file_stat_json(info)));
      },
      options.cancellation_id);
}

// FileGetShortcut: decoded .lnk fields (target, working dir, args, icon).
// A read (capability filesystem.read); corrupt links fail, unset fields
// read back empty.
JSValue storage_shortcut(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "shortcut(path, options?)");
  std::string path;
  if (!required_string(context, argv[0], "shortcut(path): path", path)) return JS_EXCEPTION;
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;

  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  return start_async(
      context,
      [kernel, service, path = std::move(path)]() -> AsyncOutcome {
        if (!kernel->allows(kStorageReadCapability)) {
          return capability_denied(kStorageReadCapability);
        }
        ShortcutInfo info;
        if (const auto error = service->read_shortcut(path, info); !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("target", json::Value::string(info.target));
        result.set("workingDir", json::Value::string(info.working_dir));
        result.set("args", json::Value::string(info.args));
        result.set("icon", json::Value::string(info.icon));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

// FileGetVersion: "M.m.b.r" or "" when the file carries no version resource.
JSValue storage_version(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "version(path, options?)");
  std::string path;
  if (!required_string(context, argv[0], "version(path): path", path)) return JS_EXCEPTION;
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;

  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  return start_async(
      context,
      [kernel, service, path = std::move(path)]() -> AsyncOutcome {
        if (!kernel->allows(kStorageReadCapability)) {
          return capability_denied(kStorageReadCapability);
        }
        std::string version;
        if (const auto error = service->read_version(path, version); !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("version", json::Value::string(version));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

JSValue storage_list(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "list(path, options?)");
  std::string path;
  if (!required_string(context, argv[0], "list(path): path", path)) return JS_EXCEPTION;
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;

  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  return start_async(
      context,
      [kernel, service, path = std::move(path)]() -> AsyncOutcome {
        if (!kernel->allows(kStorageReadCapability)) {
          return capability_denied(kStorageReadCapability);
        }
        std::vector<DirEntryInfo> entries;
        if (const auto error = service->list(path, entries); !error.ok()) {
          return async_failure(error);
        }
        json::Value array = json::Value::array();
        for (const DirEntryInfo& entry : entries) array.push(dir_entry_json(entry));
        json::Value result = json::Value::object();
        result.set("entries", array);
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

JSValue storage_env_get(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "envGet(name, options?)");
  std::string name;
  if (!required_string(context, argv[0], "envGet(name): name", name)) return JS_EXCEPTION;
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;

  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  return start_async(
      context,
      [kernel, service, name = std::move(name)]() -> AsyncOutcome {
        if (!kernel->allows(kStorageReadCapability)) {
          return capability_denied(kStorageReadCapability);
        }
        std::string value;
        if (const auto error = service->env_get(name, value); !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("value", json::Value::string(value));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

JSValue storage_ini_read(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 3 || argc > 4) {
    return JS_ThrowTypeError(context, "iniRead(path, section, key, options?)");
  }
  std::string path;
  std::string section;
  std::string key;
  if (!required_string(context, argv[0], "iniRead(path, section, key, options?): path", path)) {
    return JS_EXCEPTION;
  }
  if (!required_string(context, argv[1], "iniRead(path, section, key, options?): section",
                       section)) {
    return JS_EXCEPTION;
  }
  if (!required_string(context, argv[2], "iniRead(path, section, key, options?): key", key)) {
    return JS_EXCEPTION;
  }
  ActionOptions options;
  if (argc == 4 && !parse_action_options(context, argv[3], options)) return JS_EXCEPTION;

  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  return start_async(
      context,
      [kernel, service, path = std::move(path), section = std::move(section),
       key = std::move(key)]() -> AsyncOutcome {
        if (!kernel->allows(kStorageReadCapability)) {
          return capability_denied(kStorageReadCapability);
        }
        std::string value;
        if (const auto error = service->ini_read(path, section, key, value); !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("value", json::Value::string(value));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

JSValue storage_drive_get(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 1 || argc > 3) return JS_ThrowTypeError(context, "driveGet(field, letter?, options?)");
  std::string field;
  if (!required_string(context, argv[0], "driveGet(field, letter?, options?): field", field)) {
    return JS_EXCEPTION;
  }

  // Argument shapes: driveGet(field), driveGet(field, options),
  // driveGet(field, letter) and driveGet(field, letter, options). The second
  // slot is the letter only when it is a string, so an options object there
  // still parses.
  std::string letter;
  int options_index = -1;
  if (argc == 2) {
    if (JS_IsString(argv[1])) {
      if (!required_string(context, argv[1], "driveGet(field, letter?): letter", letter, true)) {
        return JS_EXCEPTION;
      }
    } else {
      options_index = 1;
    }
  } else if (argc == 3) {
    if (JS_IsString(argv[1])) {
      if (!required_string(context, argv[1], "driveGet(field, letter?): letter", letter, true)) {
        return JS_EXCEPTION;
      }
      options_index = 2;
    } else if (JS_IsUndefined(argv[1]) || JS_IsNull(argv[1])) {
      options_index = 2;
    } else {
      return JS_ThrowTypeError(context,
                               "driveGet(field, letter?, options?): letter must be a string");
    }
  }
  ActionOptions options;
  if (options_index >= 0 && !parse_action_options(context, argv[options_index], options)) {
    return JS_EXCEPTION;
  }

  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  return start_async(
      context,
      [kernel, service, field = std::move(field), letter = std::move(letter)]() -> AsyncOutcome {
        if (!kernel->allows(kStorageReadCapability)) {
          return capability_denied(kStorageReadCapability);
        }
        DriveInfo info;
        if (const auto error = service->drive_get(field, letter, info); !error.ok()) {
          return async_failure(error);
        }
        return async_success(json::stringify(drive_json(info)));
      },
      options.cancellation_id);
}

// ---- file handles ----------------------------------------------------------

JSValue storage_open(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "open(path, mode, options?)");
  std::string path;
  if (!required_string(context, argv[0], "open(path, mode, options?): path", path)) {
    return JS_EXCEPTION;
  }
  std::string mode;
  if (!required_string(context, argv[1], "open(path, mode, options?): mode", mode)) {
    return JS_EXCEPTION;
  }
  // A mode is a caller mistake, not a runtime outcome, so it fails here
  // instead of reaching the service (same rule as setView's view).
  if (mode != "r" && mode != "a" && mode != "w") {
    return JS_ThrowTypeError(context,
                             "open(path, mode, options?): mode must be \"r\", \"a\" or \"w\"");
  }
  ActionOptions options;
  if (argc == 3 && !parse_action_options(context, argv[2], options)) return JS_EXCEPTION;

  // The mode decides which gate the open itself passes: reading needs the
  // read capability, creating and appending need the write capability.
  const char* capability = mode == "r" ? kStorageReadCapability : kStorageWriteCapability;
  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  return start_async(
      context,
      [kernel, service, path = std::move(path), mode = std::move(mode),
       capability]() -> AsyncOutcome {
        if (!kernel->allows(capability)) return capability_denied(capability);
        std::uint64_t handle = 0;
        std::uint64_t length = 0;
        if (const auto error = service->file_open(path, mode, handle, length); !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("handle", json::Value::number(static_cast<double>(handle)));
        result.set("length", json::Value::number(static_cast<double>(length)));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

JSValue storage_file_read(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "fileRead(handle, count, options?)");
  std::int64_t handle = 0;
  std::int64_t count = 0;
  if (!required_int(context, argv[0], "fileRead(handle, count, options?): handle", 1, kMaxHandle,
                    handle)) {
    return JS_EXCEPTION;
  }
  if (!required_int(context, argv[1], "fileRead(handle, count, options?): count", 0, kMaxReadCount,
                    count)) {
    return JS_EXCEPTION;
  }
  ActionOptions options;
  if (argc == 3 && !parse_action_options(context, argv[2], options)) return JS_EXCEPTION;

  StorageService* service = binding->service;
  return start_async(
      context,
      [service, handle, count]() -> AsyncOutcome {
        // No capability check here: the handle could only be obtained through
        // open(), which already passed the mode's gate.
        std::vector<std::uint8_t> bytes;
        bool eof = false;
        if (const auto error = service->file_read(static_cast<std::uint64_t>(handle),
                                                  static_cast<std::uint32_t>(count), bytes, eof);
            !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("bytes", bytes_json(bytes));
        result.set("eof", json::Value::boolean(eof));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

JSValue storage_file_seek(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 3 || argc > 4) {
    return JS_ThrowTypeError(context, "fileSeek(handle, offset, whence, options?)");
  }
  std::int64_t handle = 0;
  std::int64_t offset = 0;
  std::int64_t whence = 0;
  if (!required_int(context, argv[0], "fileSeek(handle, offset, whence, options?): handle", 1,
                    kMaxHandle, handle)) {
    return JS_EXCEPTION;
  }
  if (!js_int64_strict(context, argv[1], offset,
                       "fileSeek(handle, offset, whence, options?): offset")) {
    return JS_EXCEPTION;
  }
  if (!required_int(context, argv[2], "fileSeek(handle, offset, whence, options?): whence", 0, 2,
                    whence)) {
    return JS_EXCEPTION;
  }
  ActionOptions options;
  if (argc == 4 && !parse_action_options(context, argv[3], options)) return JS_EXCEPTION;

  StorageService* service = binding->service;
  return start_async(
      context,
      [service, handle, offset, whence]() -> AsyncOutcome {
        std::uint64_t pos = 0;
        if (const auto error = service->file_seek(static_cast<std::uint64_t>(handle), offset,
                                                  static_cast<int>(whence), pos);
            !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("pos", json::Value::number(static_cast<double>(pos)));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

JSValue storage_file_stat(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "fileStat(handle, options?)");
  std::int64_t handle = 0;
  if (!required_int(context, argv[0], "fileStat(handle, options?): handle", 1, kMaxHandle,
                    handle)) {
    return JS_EXCEPTION;
  }
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;

  StorageService* service = binding->service;
  return start_async(
      context,
      [service, handle]() -> AsyncOutcome {
        std::uint64_t pos = 0;
        std::uint64_t length = 0;
        if (const auto error = service->file_stat(static_cast<std::uint64_t>(handle), pos, length);
            !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("pos", json::Value::number(static_cast<double>(pos)));
        result.set("length", json::Value::number(static_cast<double>(length)));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

JSValue storage_file_close(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "fileClose(handle, options?)");
  std::int64_t handle = 0;
  if (!required_int(context, argv[0], "fileClose(handle, options?): handle", 1, kMaxHandle,
                    handle)) {
    return JS_EXCEPTION;
  }
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;

  StorageService* service = binding->service;
  return start_async(
      context,
      [service, handle]() -> AsyncOutcome {
        if (const auto error = service->file_close(static_cast<std::uint64_t>(handle));
            !error.ok()) {
          return async_failure(error);
        }
        return async_success("{}");
      },
      options.cancellation_id);
}

// ---- writes ----------------------------------------------------------------

JSValue storage_write(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "write(payload, options?)");
  if (!JS_IsObject(argv[0]) || JS_IsArray(argv[0])) {
    return JS_ThrowTypeError(context, "write(payload): payload must be an object");
  }
  // The target is fixed, so unlike registry.write there is nothing payload
  // derived to resolve synchronously: every field (op included) is validated
  // by the executor, which keeps a rejected op inside the trace as an
  // invalid_contract Action result instead of a silent local throw.
  JSValue json_value = JS_JSONStringify(context, argv[0], JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(json_value)) return JS_EXCEPTION;
  if (!JS_IsString(json_value)) {
    JS_FreeValue(context, json_value);
    return JS_ThrowTypeError(context, "write(payload): payload must serialize to JSON");
  }
  std::string payload;
  if (!copy_string(context, json_value, payload)) {
    JS_FreeValue(context, json_value);
    return JS_EXCEPTION;
  }
  JS_FreeValue(context, json_value);

  ActionOptions options;
  if (argc >= 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;

  auto action = make_action(*binding->next_action_id, "rime:storage", "storage.write",
                            kStorageWriteCapability, {"storage", "fs"}, std::move(payload),
                            options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

// ---- session encoding ------------------------------------------------------

JSValue storage_encoding(JSContext* context, JSValueConst, int argc, JSValueConst*, int, void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc > 0) return JS_ThrowTypeError(context, "encoding()");
  JSValue object = JS_EXCEPTION;
  if (!make_encoding_object(context, binding->service->encoding(), object)) return object;
  return object;
}

JSValue storage_set_encoding(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                             void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc != 1) return JS_ThrowTypeError(context, "setEncoding(encoding)");
  std::string encoding;
  if (!required_string(context, argv[0], "setEncoding(encoding): encoding", encoding)) {
    return JS_EXCEPTION;
  }
  // An unknown encoding is a caller mistake: it never mutates the session, so
  // it surfaces as a TypeError rather than as a resolved value.
  if (const auto error = binding->service->set_encoding(encoding); !error.ok()) {
    return JS_ThrowTypeError(context, "%s", error.message.c_str());
  }
  JSValue object = JS_EXCEPTION;
  if (!make_encoding_object(context, binding->service->encoding(), object)) return object;
  return object;
}

// ---- download --------------------------------------------------------------

JSValue storage_download(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "download(url, path, options?)");
  std::string url;
  std::string path;
  if (!required_string(context, argv[0], "download(url, path, options?): url", url)) {
    return JS_EXCEPTION;
  }
  if (!required_string(context, argv[1], "download(url, path, options?): path", path)) {
    return JS_EXCEPTION;
  }
  ActionOptions options;
  if (argc == 3 && !parse_action_options(context, argv[2], options)) return JS_EXCEPTION;

  // The deadline is frozen here, at call time, exactly like make_action does
  // for a queued Action, so queue wait time is not silently charged to the
  // transfer budget.
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const std::uint64_t now_u = now_ms > 0 ? static_cast<std::uint64_t>(now_ms) : 0;
  const std::uint64_t raw_deadline =
      options.deadline_ms > UINT64_MAX - now_u ? UINT64_MAX : now_u + options.deadline_ms;
  const std::int64_t deadline_unix_ms =
      raw_deadline > static_cast<std::uint64_t>(INT64_MAX) ? INT64_MAX
                                                           : static_cast<std::int64_t>(raw_deadline);

  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  const std::uint64_t cancellation_id = options.cancellation_id;
  return start_async(
      context,
      [kernel, service, host, url = std::move(url), path = std::move(path), cancellation_id,
       deadline_unix_ms]() -> AsyncOutcome {
        if (!kernel->allows(kStorageWriteCapability)) {
          return capability_denied(kStorageWriteCapability);
        }
        rime::core::CancellationToken cancel;
        if (cancellation_id != 0) cancel = host->cancellation_token(cancellation_id);
        std::uint64_t bytes = 0;
        if (const auto error = service->download(url, path, cancel, deadline_unix_ms, bytes);
            !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("bytes", json::Value::number(static_cast<double>(bytes)));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

// ---- selectors -------------------------------------------------------------

JSValue storage_select_file(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                            void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc > 1) return JS_ThrowTypeError(context, "selectFile(options?)");
  std::string filter;
  std::string default_name;
  bool multi = false;
  ActionOptions options;
  if (argc == 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    if (!JS_IsObject(argv[0])) {
      return JS_ThrowTypeError(context, "selectFile(options?): options must be an object");
    }
    if (!parse_action_options(context, argv[0], options)) return JS_EXCEPTION;
    if (!optional_string_field(context, argv[0], "selectFile(options)", "filter", filter, false)) {
      return JS_EXCEPTION;
    }
    if (!optional_string_field(context, argv[0], "selectFile(options)", "defaultName", default_name,
                               false)) {
      return JS_EXCEPTION;
    }
    if (!optional_bool_field(context, argv[0], "selectFile(options)", "multi", multi)) {
      return JS_EXCEPTION;
    }
  }

  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  const std::uint64_t cancellation_id = options.cancellation_id;
  return start_async(
      context,
      [kernel, service, host, filter = std::move(filter), default_name = std::move(default_name),
       multi, cancellation_id]() -> AsyncOutcome {
        if (!kernel->allows(kStorageReadCapability)) {
          return capability_denied(kStorageReadCapability);
        }
        rime::core::CancellationToken cancel;
        if (cancellation_id != 0) cancel = host->cancellation_token(cancellation_id);
        std::vector<std::string> paths;
        if (const auto error = service->select_file(filter, default_name, multi, paths, cancel);
            !error.ok()) {
          return async_failure(error);
        }
        json::Value array = json::Value::array();
        for (const std::string& path : paths) array.push(json::Value::string(path));
        json::Value result = json::Value::object();
        result.set("paths", array);
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

JSValue storage_select_dir(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void*) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:storage is not wired");
  }
  if (argc > 1) return JS_ThrowTypeError(context, "selectDir(options?)");
  std::string caption;
  ActionOptions options;
  if (argc == 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    if (!JS_IsObject(argv[0])) {
      return JS_ThrowTypeError(context, "selectDir(options?): options must be an object");
    }
    if (!parse_action_options(context, argv[0], options)) return JS_EXCEPTION;
    if (!optional_string_field(context, argv[0], "selectDir(options)", "caption", caption, true)) {
      return JS_EXCEPTION;
    }
  }

  rime::action::Kernel* kernel = binding->kernel;
  StorageService* service = binding->service;
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  const std::uint64_t cancellation_id = options.cancellation_id;
  return start_async(
      context,
      [kernel, service, host, caption = std::move(caption), cancellation_id]() -> AsyncOutcome {
        if (!kernel->allows(kStorageReadCapability)) {
          return capability_denied(kStorageReadCapability);
        }
        rime::core::CancellationToken cancel;
        if (cancellation_id != 0) cancel = host->cancellation_token(cancellation_id);
        std::string path;
        if (const auto error = service->select_dir(caption, path, cancel); !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("path", json::Value::string(path));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

// ---- module ----------------------------------------------------------------

int storage_module_init(JSContext* context, JSModuleDef* module) {
  StorageModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    JS_ThrowInternalError(context, "rime:storage requires a storage module binding");
    return -1;
  }
  JSValue storage = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue fn = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(fn)) {
      JS_FreeValue(context, storage);
      return false;
    }
    // JS_SetPropertyStr consumes `fn` on both success and failure.
    if (JS_SetPropertyStr(context, storage, name, fn) < 0) {
      JS_FreeValue(context, storage);
      return false;
    }
    return true;
  };
  if (!add("readText", storage_read_text, 1) || !add("readBytes", storage_read_bytes, 1) ||
      !add("stat", storage_stat, 1) || !add("shortcut", storage_shortcut, 1) ||
      !add("version", storage_version, 1) || !add("list", storage_list, 1) ||
      !add("envGet", storage_env_get, 1) || !add("iniRead", storage_ini_read, 3) ||
      !add("driveGet", storage_drive_get, 1) || !add("open", storage_open, 2) ||
      !add("fileRead", storage_file_read, 2) || !add("fileSeek", storage_file_seek, 3) ||
      !add("fileStat", storage_file_stat, 1) || !add("fileClose", storage_file_close, 1) ||
      !add("write", storage_write, 1) || !add("encoding", storage_encoding, 0) ||
      !add("setEncoding", storage_set_encoding, 1) || !add("download", storage_download, 2) ||
      !add("selectFile", storage_select_file, 0) || !add("selectDir", storage_select_dir, 0)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "storage", storage);
}

JSModuleDef* create_storage_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:storage", storage_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "storage") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const StorageModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:storage requires a storage service, kernel, dispatcher and action id source"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_storage_module(rime::js::Host& host, StorageModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:storage", binding);
  if (const auto error = host.modules().add_native(
          "rime:storage", [](JSContext* context) { return create_storage_module(context); });
      !error.ok()) {
    return error;
  }
  return rime::core::Error::none();
}

rime::core::Error register_storage_module(rime::js::Runtime& runtime,
                                          StorageModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:storage", [](JSContext* context) { return create_storage_module(context); }, binding);
}

}  // namespace rime::win32
