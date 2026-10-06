#include "rime/win32/storage_executor.hpp"

#include "rime/core/json.hpp"
#include "rime/core/lane.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

using Result = rime::action::Result;
using Error = rime::core::Error;
using Code = rime::core::Error::Code;
namespace json = rime::core::json;

// Fixed English payload texts. The Action result is what the trace, the JS
// promise and docs/api/storage.md all read, so nothing here interpolates
// localised Win32 text; only Win32 failures carry a status number.
constexpr const char* kPayloadPrefix = "storage.write payload ";

// Mirrors StorageService::set_time()'s accepted range: Unix epoch
// milliseconds from 0 (1970-01-01) through 9999-12-31 23:59:59.999.
constexpr double kMaxUnixMs = 253402300799999.0;

Result fail(const rime::action::Action& action, const Code code, std::string message) {
  return {action.id, false, false, message, {code, message}, {}};
}

Result cancelled(const rime::action::Action& action, const char* message) {
  return {action.id, false, true, message, {Code::Cancelled, message}, {}};
}

Error payload_error(std::string detail) {
  return {Code::InvalidContract, std::string(kPayloadPrefix) + std::move(detail)};
}

// JSON carries integers as doubles, so a handle id is exactly the doubles
// that are integral and in 1..2^63-1 (the service's id space).
bool json_handle(const json::Value& value, std::uint64_t& out) {
  if (!value.is_number()) return false;
  const double number = value.as_number();
  if (!std::isfinite(number) || std::trunc(number) != number) return false;
  if (!(number >= 1.0) || number >= 9223372036854775808.0) return false;
  out = static_cast<std::uint64_t>(number);
  return true;
}

// One element of handleWrite's `data` byte array: integral and 0..255.
bool json_byte(const json::Value& value, std::uint8_t& out) {
  if (!value.is_number()) return false;
  const double number = value.as_number();
  if (!std::isfinite(number) || std::trunc(number) != number) return false;
  if (!(number >= 0.0) || number > 255.0) return false;
  out = static_cast<std::uint8_t>(number);
  return true;
}

// setTime payload bound: integral, non-negative and at or below the year
// 9999 ceiling the service enforces, all inside the exactly representable
// double range so no rounding can move the value across the boundary.
bool json_unix_ms(const json::Value& value, std::int64_t& out) {
  if (!value.is_number()) return false;
  const double number = value.as_number();
  if (!std::isfinite(number) || std::trunc(number) != number) return false;
  if (!(number >= 0.0) || number > kMaxUnixMs) return false;
  out = static_cast<std::int64_t>(number);
  return true;
}

// Every op declares the complete field list it accepts; an extra key is a
// contract failure rather than a silently ignored preference. The `op` key
// itself dispatches to this function and is therefore never part of a list.
Error check_fields(const json::Object& object, const std::string& op,
                   std::initializer_list<const char*> allowed) {
  for (const json::Member& member : object) {
    if (member.first == "op") continue;
    bool found = false;
    for (const char* candidate : allowed) {
      if (member.first == candidate) {
        found = true;
        break;
      }
    }
    if (!found) return payload_error("op " + op + " does not accept field: " + member.first);
  }
  return Error::none();
}

Error require_string(const json::Value* value, const std::string& field, std::string& out,
                     const bool allow_empty = false) {
  if (!value || !value->is_string()) return payload_error("requires a string " + field);
  if (!allow_empty && value->as_string().empty()) {
    return payload_error("requires a non-empty string " + field);
  }
  out = value->as_string();
  return Error::none();
}

Error optional_string(const json::Value* value, const std::string& field, std::string& out) {
  out.clear();
  if (!value) return Error::none();
  if (!value->is_string()) return payload_error(field + " must be a string");
  out = value->as_string();
  return Error::none();
}

Error optional_bool(const json::Value* value, const std::string& field, bool& out) {
  out = false;
  if (!value) return Error::none();
  if (!value->is_bool()) return payload_error(field + " must be a boolean");
  out = value->as_bool();
  return Error::none();
}

// Shared pre-check for the copy/move family: two non-empty paths plus an
// optional overwrite flag.
Error parse_paths(const json::Value& payload, std::string& src, std::string& dst, bool& overwrite) {
  if (const Error error = require_string(payload.find("src"), "src", src); !error.ok()) return error;
  if (const Error error = require_string(payload.find("dst"), "dst", dst); !error.ok()) return error;
  return optional_bool(payload.find("overwrite"), "overwrite", overwrite);
}

// Validates one op's payload and runs it against the service. Returning the
// service/contract Error (instead of a Result) keeps the success shape, the
// cancellation checks and the action id in execute() where they belong.
Error run_op(const std::string& op, const json::Value& payload, StorageService& service) {
  if (op == "append" || op == "write") {
    if (const Error error = check_fields(payload.as_object(), op, {"path", "text"}); !error.ok()) {
      return error;
    }
    std::string path;
    std::string text;
    if (const Error error = require_string(payload.find("path"), "path", path); !error.ok()) {
      return error;
    }
    if (const Error error = require_string(payload.find("text"), "text", text, true); !error.ok()) {
      return error;
    }
    return op == "append" ? service.append_text(path, text) : service.write_text(path, text);
  }

  if (op == "copy" || op == "move" || op == "install" || op == "dircopy" || op == "dirmove") {
    if (const Error error =
            check_fields(payload.as_object(), op, {"src", "dst", "overwrite"});
        !error.ok()) {
      return error;
    }
    std::string src;
    std::string dst;
    bool overwrite = false;
    if (const Error error = parse_paths(payload, src, dst, overwrite); !error.ok()) return error;
    if (op == "copy") return service.file_copy(src, dst, overwrite);
    if (op == "move") return service.file_move(src, dst, overwrite);
    if (op == "install") return service.file_install(src, dst, overwrite);
    if (op == "dircopy") return service.dir_copy(src, dst, overwrite);
    return service.dir_move(src, dst, overwrite);
  }

  if (op == "delete" || op == "mkdir" || op == "recycle") {
    if (const Error error = check_fields(payload.as_object(), op, {"path"}); !error.ok()) {
      return error;
    }
    std::string path;
    if (const Error error = require_string(payload.find("path"), "path", path); !error.ok()) {
      return error;
    }
    if (op == "delete") return service.file_delete(path);
    if (op == "mkdir") return service.dir_create(path);
    return service.recycle(path);
  }

  if (op == "rmdir") {
    if (const Error error = check_fields(payload.as_object(), op, {"path", "recursive"});
        !error.ok()) {
      return error;
    }
    std::string path;
    bool recursive = false;
    if (const Error error = require_string(payload.find("path"), "path", path); !error.ok()) {
      return error;
    }
    if (const Error error = optional_bool(payload.find("recursive"), "recursive", recursive);
        !error.ok()) {
      return error;
    }
    return service.dir_delete(path, recursive);
  }

  if (op == "setAttrib") {
    if (const Error error = check_fields(payload.as_object(), op, {"path", "add", "remove"});
        !error.ok()) {
      return error;
    }
    std::string path;
    std::string add;
    std::string remove;
    if (const Error error = require_string(payload.find("path"), "path", path); !error.ok()) {
      return error;
    }
    if (const Error error = optional_string(payload.find("add"), "add", add); !error.ok()) {
      return error;
    }
    if (const Error error = optional_string(payload.find("remove"), "remove", remove); !error.ok()) {
      return error;
    }
    if (add.empty() && remove.empty()) {
      return payload_error("setAttrib requires at least one of add or remove");
    }
    return service.set_attrib(path, add, remove);
  }

  if (op == "setTime") {
    if (const Error error = check_fields(payload.as_object(), op, {"path", "which", "unixMs"});
        !error.ok()) {
      return error;
    }
    std::string path;
    std::string which;
    std::int64_t unix_ms = 0;
    if (const Error error = require_string(payload.find("path"), "path", path); !error.ok()) {
      return error;
    }
    if (const Error error = require_string(payload.find("which"), "which", which); !error.ok()) {
      return error;
    }
    if (which != "mtime" && which != "atime" && which != "ctime") {
      return payload_error("which must be one of mtime, atime, ctime");
    }
    const json::Value* unix_ms_value = payload.find("unixMs");
    if (!unix_ms_value || !json_unix_ms(*unix_ms_value, unix_ms)) {
      return payload_error("unixMs must be an integer between 0 and 253402300799999");
    }
    return service.set_time(path, which, unix_ms);
  }

  if (op == "recycleEmpty") {
    if (const Error error = check_fields(payload.as_object(), op, {"root"}); !error.ok()) {
      return error;
    }
    std::string root;
    if (const Error error = optional_string(payload.find("root"), "root", root); !error.ok()) {
      return error;
    }
    return service.recycle_empty(root);
  }

  if (op == "shortcut") {
    if (const Error error = check_fields(payload.as_object(), op,
                                         {"path", "target", "args", "workdir", "icon",
                                          "description"});
        !error.ok()) {
      return error;
    }
    std::string path;
    std::string target;
    std::string args;
    std::string workdir;
    std::string icon;
    std::string description;
    if (const Error error = require_string(payload.find("path"), "path", path); !error.ok()) {
      return error;
    }
    if (const Error error = require_string(payload.find("target"), "target", target); !error.ok()) {
      return error;
    }
    if (const Error error = optional_string(payload.find("args"), "args", args); !error.ok()) {
      return error;
    }
    if (const Error error = optional_string(payload.find("workdir"), "workdir", workdir);
        !error.ok()) {
      return error;
    }
    if (const Error error = optional_string(payload.find("icon"), "icon", icon); !error.ok()) {
      return error;
    }
    if (const Error error =
            optional_string(payload.find("description"), "description", description);
        !error.ok()) {
      return error;
    }
    return service.make_shortcut(path, target, args, workdir, icon, description);
  }

  if (op == "env") {
    if (const Error error = check_fields(payload.as_object(), op, {"name", "value"}); !error.ok()) {
      return error;
    }
    std::string name;
    std::string value;
    if (const Error error = require_string(payload.find("name"), "name", name); !error.ok()) {
      return error;
    }
    if (const Error error = require_string(payload.find("value"), "value", value, true);
        !error.ok()) {
      return error;
    }
    return service.env_set(name, value);
  }

  if (op == "iniWrite") {
    if (const Error error =
            check_fields(payload.as_object(), op, {"path", "section", "key", "value"});
        !error.ok()) {
      return error;
    }
    std::string path;
    std::string section;
    std::string key;
    std::string value;
    if (const Error error = require_string(payload.find("path"), "path", path); !error.ok()) {
      return error;
    }
    if (const Error error = require_string(payload.find("section"), "section", section);
        !error.ok()) {
      return error;
    }
    if (const Error error = require_string(payload.find("key"), "key", key); !error.ok()) {
      return error;
    }
    if (const Error error = require_string(payload.find("value"), "value", value, true);
        !error.ok()) {
      return error;
    }
    return service.ini_write(path, section, key, value);
  }

  if (op == "iniDelete") {
    if (const Error error = check_fields(payload.as_object(), op, {"path", "section", "key"});
        !error.ok()) {
      return error;
    }
    std::string path;
    std::string section;
    std::string key;
    if (const Error error = require_string(payload.find("path"), "path", path); !error.ok()) {
      return error;
    }
    if (const Error error = require_string(payload.find("section"), "section", section);
        !error.ok()) {
      return error;
    }
    if (const Error error = optional_string(payload.find("key"), "key", key); !error.ok()) {
      return error;
    }
    return service.ini_delete(path, section, key);
  }

  if (op == "driveLabel") {
    if (const Error error = check_fields(payload.as_object(), op, {"letter", "label"});
        !error.ok()) {
      return error;
    }
    std::string string_letter;
    std::string label;
    if (const Error error = require_string(payload.find("letter"), "letter", string_letter);
        !error.ok()) {
      return error;
    }
    if (const Error error = require_string(payload.find("label"), "label", label, true);
        !error.ok()) {
      return error;
    }
    return service.drive_set_label(string_letter, label);
  }

  if (op == "driveLock" || op == "driveUnlock" || op == "driveEject" || op == "driveRetract") {
    if (const Error error = check_fields(payload.as_object(), op, {"letter"}); !error.ok()) {
      return error;
    }
    std::string letter;
    if (const Error error = require_string(payload.find("letter"), "letter", letter); !error.ok()) {
      return error;
    }
    if (op == "driveLock") return service.drive_lock(letter);
    if (op == "driveUnlock") return service.drive_unlock(letter);
    if (op == "driveEject") return service.drive_eject(letter);
    return service.drive_retract(letter);
  }

  if (op == "handleWrite") {
    if (const Error error = check_fields(payload.as_object(), op, {"handle", "data"});
        !error.ok()) {
      return error;
    }
    const json::Value* handle_value = payload.find("handle");
    std::uint64_t handle = 0;
    if (!handle_value || !json_handle(*handle_value, handle)) {
      return payload_error(
          "requires an integer handle between 1 and 9223372036854775807");
    }
    const json::Value* data_value = payload.find("data");
    if (!data_value) return payload_error("requires a data field");
    std::vector<std::uint8_t> bytes;
    if (data_value->is_string()) {
      const std::string& text = data_value->as_string();
      bytes.assign(text.begin(), text.end());
    } else if (data_value->is_array()) {
      for (const json::Value& item : data_value->as_array()) {
        std::uint8_t byte = 0;
        if (!json_byte(item, byte)) {
          return payload_error("data elements must be integers 0..255");
        }
        bytes.push_back(byte);
      }
    } else {
      return payload_error("data must be a string or an array of 0..255");
    }
    return service.handle_write(handle, bytes);
  }

  return payload_error("does not support op: " + op);
}

}  // namespace

rime::action::Result StorageExecutor::execute(const rime::action::Action& action,
                                              rime::core::CancellationToken cancellation) {
  if (const auto lane_error = rime::core::require_lane(rime::core::Lane::Worker);
      !lane_error.ok()) {
    return fail(action, lane_error.code, lane_error.message);
  }
  if (action.type != "storage.write") {
    return fail(action, Code::InvalidContract, "unsupported action type: " + action.type);
  }
  if (action.target.kind != "storage") {
    return fail(action, Code::InvalidContract,
                "storage.write requires target kind 'storage', got: " + action.target.kind);
  }
  if (action.target.id != "fs") {
    return fail(action, Code::InvalidContract, "storage.write target id must be 'fs'");
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  const auto payload = json::parse(action.payload);
  if (!payload.ok() || !payload.value->is_object()) {
    return fail(action, Code::InvalidContract,
                std::string(kPayloadPrefix) + "must be a JSON object");
  }
  const json::Value* op_value = payload.value->find("op");
  if (!op_value || !op_value->is_string() || op_value->as_string().empty()) {
    return fail(action, Code::InvalidContract,
                std::string(kPayloadPrefix) + "requires a non-empty string op");
  }
  const std::string op = op_value->as_string();

  // No per-call timeout here by design: StorageService calls are synchronous
  // and bounded (download() yields to its own deadline between chunks; the
  // selectors run on the UI pump), so the kernel's pre-dispatch and
  // post-commit deadline checks are the timeout enforcement for these
  // actions.
  if (const Error op_error = run_op(op, *payload.value, service_); !op_error.ok()) {
    return fail(action, op_error.code, op_error.message);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled after execution");
  }

  json::Value result = json::Value::object();
  result.set("op", json::Value::string(op));
  return {action.id, true, false, "storage write applied", {}, std::move(result)};
}

}  // namespace rime::win32
