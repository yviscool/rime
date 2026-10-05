// Golden contract consumer (C++ side), the twin of tools/golden-smoke.ts.
//
// Every file under contracts/golden is parsed and checked on four layers:
//   shape     - the same JSON Schema Draft 2020-12 subset as
//               tools/schema-smoke.ts / tools/golden-smoke.ts, implemented
//               test-local below (it must stay in sync with both copies);
//   lifecycle - real Kernel execution: decode_action + ManualClock deadline
//               rewrite + StaticCapabilityPolicy, asserting the pre-executor
//               error code the golden declares;
//   semantic  - round-1 check.kind whitelist and expect types;
//   failure   - executor rejection shape, codes and message literals;
//   envelope  - ActionV1 samples decode_action must reject.
//
// The C++ and TypeScript consumers must agree on every file.

#include "rime/action/codec.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/clock.hpp"
#include "rime/core/json.hpp"
#include "rime/core/types.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using rime::core::json::Value;

[[noreturn]] void fail(const std::string& message) {
  std::fprintf(stderr, "golden contract failure: %s\n", message.c_str());
  std::fflush(stderr);
  std::abort();
}

void expect(const bool condition, const std::string& message) {
  if (!condition) fail(message);
}

std::string read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  expect(static_cast<bool>(input), "cannot read " + path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool is_non_empty_string(const Value* value) {
  return value != nullptr && value->is_string() && !value->as_string().empty();
}

std::string require_string(const Value& object, const char* key, const std::string& where) {
  const Value* value = object.find(key);
  if (!is_non_empty_string(value)) fail(where + ": missing non-empty string field '" + key + "'");
  return value->as_string();
}

const std::unordered_set<std::string>& error_codes() {
  static const std::unordered_set<std::string> codes = {
      "none",           "invalid_state",  "queue_closed",       "queue_full",
      "cancelled",      "timeout",        "capability_denied",  "invalid_contract",
      "unsupported",    "execution_failed", "target_gone"};
  return codes;
}

const std::unordered_set<std::string>& rect_expects() {
  static const std::unordered_set<std::string> expects = {
      "work-left-half", "work-right-half", "work-full", "work-top", "work-bottom"};
  return expects;
}

bool matches_type(const std::string& type, const Value& value) {
  if (type == "object") return value.is_object();
  if (type == "array") return value.is_array();
  if (type == "string") return value.is_string();
  if (type == "integer") return value.is_number() && value.as_number() == std::floor(value.as_number());
  if (type == "number") return value.is_number() && std::isfinite(value.as_number());
  if (type == "boolean") return value.is_bool();
  if (type == "null") return value.is_null();
  return false;
}

// Draft 2020-12 subset identical to tools/schema-smoke.ts and
// tools/golden-smoke.ts: type, const, enum, oneOf, anyOf, required,
// properties, additionalProperties, items, $ref -> $defs, minLength,
// minimum, maximum. Like the TypeScript copies, `additionalProperties` is
// only reported for schemas that also declare `properties`.
bool validate(const Value& schema, const Value& value, const Value& root, std::string& why,
              const std::string& path) {
  if (const Value* ref = schema.find("$ref")) {
    expect(ref->is_string(), path + ": $ref must be a string");
    const std::string reference = ref->as_string();
    constexpr const char* prefix = "#/$defs/";
    expect(reference.rfind(prefix, 0) == 0, path + ": only #/$defs/ references are supported");
    const Value* defs = root.find("$defs");
    expect(defs != nullptr && defs->is_object(), path + ": schema has no $defs");
    const Value* target = defs->find(reference.substr(std::string(prefix).size()));
    expect(target != nullptr, path + ": unresolved $ref " + reference);
    return validate(*target, value, root, why, path);
  }
  if (const Value* constant = schema.find("const")) {
    if (!rime::core::json::equals(*constant, value)) {
      why = path + ": must equal " + rime::core::json::stringify(*constant);
      return false;
    }
    return true;
  }
  if (const Value* enumeration = schema.find("enum")) {
    expect(enumeration->is_array(), path + ": enum must be an array");
    for (const Value& entry : enumeration->as_array()) {
      if (rime::core::json::equals(entry, value)) return true;
    }
    why = path + ": must be one of " + rime::core::json::stringify(*enumeration);
    return false;
  }
  if (const Value* one_of = schema.find("oneOf")) {
    expect(one_of->is_array(), path + ": oneOf must be an array");
    int matches = 0;
    for (const Value& branch : one_of->as_array()) {
      std::string ignored;
      if (validate(branch, value, root, ignored, path)) ++matches;
    }
    if (matches != 1) {
      why = path + ": must match exactly one oneOf branch";
      return false;
    }
  }
  if (const Value* any_of = schema.find("anyOf")) {
    expect(any_of->is_array(), path + ": anyOf must be an array");
    int matches = 0;
    for (const Value& branch : any_of->as_array()) {
      std::string ignored;
      if (validate(branch, value, root, ignored, path)) ++matches;
    }
    if (matches == 0) {
      why = path + ": must match at least one anyOf branch";
      return false;
    }
  }
  if (const Value* type = schema.find("type")) {
    bool matched = false;
    if (type->is_string()) {
      matched = matches_type(type->as_string(), value);
    } else if (type->is_array()) {
      for (const Value& entry : type->as_array()) {
        if (entry.is_string() && matches_type(entry.as_string(), value)) {
          matched = true;
          break;
        }
      }
    }
    if (!matched) {
      why = path + ": type mismatch";
      return false;
    }
  }
  if (value.is_number()) {
    const double number = value.as_number();
    if (const Value* minimum = schema.find("minimum"); minimum && number < minimum->as_number()) {
      why = path + ": below minimum";
      return false;
    }
    if (const Value* maximum = schema.find("maximum"); maximum && number > maximum->as_number()) {
      why = path + ": above maximum";
      return false;
    }
  }
  if (value.is_string()) {
    if (const Value* min_length = schema.find("minLength");
        min_length &&
        static_cast<std::int64_t>(value.as_string().size()) < min_length->as_integer(-1)) {
      why = path + ": below minLength";
      return false;
    }
  }
  if (value.is_array()) {
    if (const Value* items = schema.find("items")) {
      std::size_t index = 0;
      for (const Value& element : value.as_array()) {
        if (!validate(*items, element, root, why, path + "[" + std::to_string(index) + "]")) {
          return false;
        }
        ++index;
      }
    }
  }
  if (value.is_object()) {
    if (const Value* required = schema.find("required")) {
      expect(required->is_array(), path + ": required must be an array");
      for (const Value& key : required->as_array()) {
        expect(key.is_string(), path + ": required entries must be strings");
        if (value.find(key.as_string()) == nullptr) {
          why = path + ": missing required property '" + key.as_string() + "'";
          return false;
        }
      }
    }
    if (const Value* properties = schema.find("properties")) {
      const Value* additional = schema.find("additionalProperties");
      const bool reject_extra = additional != nullptr && additional->is_bool() && !additional->as_bool();
      if (reject_extra) {
        for (const auto& member : value.as_object()) {
          if (properties->find(member.first) == nullptr) {
            why = path + ": unexpected property '" + member.first + "'";
            return false;
          }
        }
      }
      for (const auto& member : properties->as_object()) {
        if (const Value* present = value.find(member.first)) {
          if (!validate(member.second, *present, root, why, path + "." + member.first)) return false;
        }
      }
    }
  }
  return true;
}

struct Counts {
  std::size_t files{0};
  std::size_t valid{0};
  std::size_t violations{0};
  std::size_t lifecycle{0};
  std::size_t semantic{0};
  std::size_t failure{0};
  std::size_t envelope{0};
};

void check_shape(const Value& golden, const std::string& file, const Value* schema_root,
                 const Value* definition, Counts& counts) {
  const Value* shape = golden.find("shape");
  expect(shape != nullptr && shape->is_object(), file + ": shape must be an object");
  const Value* valid = shape->find("valid");
  const Value* violations = shape->find("violations");
  expect(valid != nullptr && valid->is_array(), file + ": shape.valid must be an array");
  expect(violations != nullptr && violations->is_array(), file + ": shape.violations must be an array");
  expect(valid->size() > 0u, file + ": shape.valid must not be empty");

  std::size_t index = 0;
  for (const Value& entry : valid->as_array()) {
    const std::string where = file + ": shape.valid[" + std::to_string(index) + "]";
    ++index;
    const std::string name = require_string(entry, "name", where);
    const std::string label = where + "(" + name + ")";
    const Value* payload = entry.find("payload");
    expect(payload != nullptr && !payload->is_null(), label + ": payload must be present");
    if (definition != nullptr) {
      expect(payload->is_object(), label + ": schema'd payload must be an object");
      std::string why;
      if (!validate(*definition, *payload, *schema_root, why, "$")) {
        fail(label + ": payload must validate: " + why);
      }
    } else {
      expect(payload->is_object() || payload->is_array(),
             label + ": inline payload must be an object or a step array");
    }
    ++counts.valid;
  }

  index = 0;
  for (const Value& entry : violations->as_array()) {
    const std::string where = file + ": shape.violations[" + std::to_string(index) + "]";
    ++index;
    const std::string name = require_string(entry, "name", where);
    const std::string label = where + "(" + name + ")";
    const std::string code = require_string(entry, "code", where);
    expect(error_codes().contains(code), label + ": code must be a contract error code");
    const Value* payload = entry.find("payload");
    expect(payload != nullptr && !payload->is_null(), label + ": payload must be present");
    if (definition != nullptr) {
      std::string why;
      if (validate(*definition, *payload, *schema_root, why, "$")) {
        fail(label + ": payload must violate its schema");
      }
      expect(code == "invalid_contract", label + ": schema violations report invalid_contract");
    }
    ++counts.violations;
  }
}

void check_lifecycle(const Value& golden, const std::string& file, const std::string& type,
                     const std::string& capability, Counts& counts) {
  const Value* lifecycle = golden.find("lifecycle");
  expect(lifecycle != nullptr && lifecycle->is_array(), file + ": lifecycle must be an array");
  expect(lifecycle->size() >= 2u, file + ": lifecycle needs at least two cases");

  bool has_timeout = false;
  bool has_denied = false;
  bool has_unsupported = false;
  std::size_t index = 0;
  for (const Value& entry : lifecycle->as_array()) {
    const std::string where = file + ": lifecycle[" + std::to_string(index) + "]";
    ++index;
    const std::string name = require_string(entry, "name", where);
    const std::string label = where + "(" + name + ")";
    const std::string mode = require_string(entry, "deadlineMode", where);
    expect(mode == "past" || mode == "future", label + ": deadlineMode must be past or future");
    const Value* expectation = entry.find("expect");
    expect(expectation != nullptr && expectation->is_object(), label + ": expect must be an object");
    const std::string expected_name = require_string(*expectation, "code", label);
    rime::core::Error::Code expected_code = rime::core::Error::Code::None;
    expect(rime::core::error_code_from_name(expected_name, expected_code),
           label + ": expect.code must be a contract error code");

    const Value* action_json = entry.find("action");
    expect(action_json != nullptr && action_json->is_object(), label + ": action must be an object");
    auto decoded = rime::action::decode_action(rime::core::json::stringify(*action_json));
    expect(decoded.ok(), label + ": decode_action failed: " + decoded.error.message);
    rime::action::Action action = *decoded.action;
    expect(action.type == type, label + ": action.type must match the golden type");
    if (expected_code == rime::core::Error::Code::CapabilityDenied) {
      expect(action.capability != capability, label + ": capability mismatch case must differ");
    } else {
      expect(action.capability == capability, label + ": capability must match the golden");
    }
    if (expected_code == rime::core::Error::Code::Unsupported) {
      expect(!action.preconditions.empty(), label + ": unsupported needs a precondition");
    } else {
      expect(action.preconditions.empty(), label + ": precondition not expected here");
    }
    expect((mode == "past") == (expected_code == rime::core::Error::Code::Timeout),
           label + ": timeout must come from a past deadline");
    if (expected_code == rime::core::Error::Code::Timeout) has_timeout = true;
    if (expected_code == rime::core::Error::Code::CapabilityDenied) has_denied = true;
    if (expected_code == rime::core::Error::Code::Unsupported) has_unsupported = true;

    // The golden stores the schema's fixed sample deadline; the runner owns
    // the real one, so ManualClock decides timeout vs. not-yet-expired.
    rime::core::ManualClock clock;
    if (mode == "past") {
      action.deadline_unix_ms = static_cast<std::uint64_t>(clock.unix_ms() - 1);
    } else {
      action.deadline_unix_ms = static_cast<std::uint64_t>(clock.unix_ms() + 60000);
    }
    expect(action.deadline_unix_ms > 0u, label + ": rewritten deadline must stay positive");

    // The capability_mismatch case is denied, so its (different) capability
    // is withheld; every other case is granted both spellings.
    std::unordered_set<std::string> granted{capability};
    if (expected_code != rime::core::Error::Code::CapabilityDenied) granted.insert(action.capability);
    auto policy = std::make_shared<rime::action::StaticCapabilityPolicy>(std::move(granted));
    rime::action::Kernel kernel(policy, {}, &clock);
    const rime::action::Result result = kernel.execute(action);
    expect(!result.error.ok(), label + ": expected a failing result");
    expect(result.error.code == expected_code,
           label + ": expected " + expected_name + ", got " +
               rime::core::error_code_name(result.error.code));
    ++counts.lifecycle;
  }
  expect(has_timeout, file + ": missing the expired-deadline (timeout) case");
  expect(has_denied, file + ": missing the capability-mismatch case");
  expect(has_unsupported, file + ": missing the precondition (unsupported) case");
}

void check_semantic(const Value& golden, const std::string& file, Counts& counts) {
  const Value* semantic = golden.find("semantic");
  expect(semantic != nullptr && semantic->is_array(), file + ": semantic must be an array");
  std::size_t index = 0;
  for (const Value& entry : semantic->as_array()) {
    const std::string where = file + ": semantic[" + std::to_string(index) + "]";
    ++index;
    const std::string name = require_string(entry, "name", where);
    const std::string label = where + "(" + name + ")";
    const Value* payload = entry.find("payload");
    expect(payload != nullptr && payload->is_object(), label + ": payload must be an object");
    const Value* check = entry.find("check");
    expect(check != nullptr && check->is_object(), label + ": check must be an object");
    const std::string kind = require_string(*check, "kind", label);
    const Value* expect_value = check->find("expect");
    expect(expect_value != nullptr, label + ": check.expect must be present");
    if (kind == "window.rect") {
      expect(expect_value->is_string() && rect_expects().contains(expect_value->as_string()),
             label + ": window.rect expect must be a work-area placement");
    } else if (kind == "process.exists") {
      expect(expect_value->is_bool() && expect_value->as_bool(), label + ": process.exists expects true");
    } else if (kind == "clipboard.text") {
      expect(expect_value->is_string(), label + ": clipboard.text expects a string");
    } else {
      fail(label + ": check.kind must be window.rect, process.exists or clipboard.text");
    }
    ++counts.semantic;
  }
}

void check_failure(const Value& golden, const std::string& file, Counts& counts) {
  const Value* failure = golden.find("failure");
  expect(failure != nullptr && failure->is_array(), file + ": failure must be an array");
  std::size_t index = 0;
  for (const Value& entry : failure->as_array()) {
    const std::string where = file + ": failure[" + std::to_string(index) + "]";
    ++index;
    const std::string name = require_string(entry, "name", where);
    const std::string label = where + "(" + name + ")";
    const Value* payload = entry.find("payload");
    expect(payload != nullptr && payload->is_object(), label + ": payload must be an object");
    const std::string code = require_string(entry, "code", where);
    expect(error_codes().contains(code), label + ": code must be a contract error code");
    if (const Value* message = entry.find("messageContains"); message != nullptr) {
      expect(is_non_empty_string(message), label + ": messageContains must be a non-empty string");
    }
    if (const Value* target = entry.find("target"); target != nullptr) {
      expect(target->is_object(), label + ": target must be an object");
      expect(is_non_empty_string(target->find("kind")), label + ": target.kind must be a non-empty string");
      expect(is_non_empty_string(target->find("id")), label + ": target.id must be a non-empty string");
    }
    ++counts.failure;
  }
}

void check_envelope(const Value& golden, const std::string& file, Counts& counts) {
  const Value* envelope = golden.find("envelope");
  expect(envelope != nullptr && envelope->is_array(), file + ": envelope must be an array");
  expect(envelope->size() > 0u, file + ": envelope needs at least one rejected sample");
  std::size_t index = 0;
  for (const Value& entry : envelope->as_array()) {
    const std::string where = file + ": envelope[" + std::to_string(index) + "]";
    ++index;
    const std::string name = require_string(entry, "name", where);
    const std::string label = where + "(" + name + ")";
    const Value* expectation = entry.find("expect");
    expect(expectation != nullptr && expectation->is_object(), label + ": expect must be an object");
    expect(require_string(*expectation, "code", label) == "invalid_contract",
           label + ": envelope samples fail with invalid_contract");
    const Value* action_json = entry.find("action");
    expect(action_json != nullptr && action_json->is_object(), label + ": action must be an object");
    auto decoded = rime::action::decode_action(rime::core::json::stringify(*action_json));
    expect(!decoded.ok(), label + ": decode_action must reject this sample");
    expect(decoded.error.code == rime::core::Error::Code::InvalidContract,
           label + ": decode_action must fail with invalid_contract");
    ++counts.envelope;
  }
}

void check_golden(const std::string& root, const std::filesystem::path& path, Counts& counts) {
  const std::string file = path.filename().string();
  const std::string text = read_file(path.string());
  auto parsed = rime::core::json::parse(text);
  expect(parsed.ok(), file + ": malformed JSON: " + parsed.error);
  const Value& golden = *parsed.value;
  expect(golden.is_object(), file + ": golden file must be an object");

  const std::string type = require_string(golden, "type", file);
  expect(type + ".json" == file, file + ": type must match the file name");
  const std::string capability = require_string(golden, "capability", file);
  const Value* payload_schema = golden.find("payloadSchema");
  expect(payload_schema != nullptr, file + ": payloadSchema must be present (string or null)");

  std::optional<Value> schema_root;
  const Value* definition = nullptr;
  if (payload_schema->is_string()) {
    const std::string reference = payload_schema->as_string();
    const std::size_t separator = reference.find('#');
    expect(separator != std::string::npos, file + ": payloadSchema must be <file>#/$defs/<name>");
    const std::string fragment = reference.substr(separator + 1);
    expect(fragment.rfind("/$defs/", 0) == 0, file + ": payloadSchema must point into $defs");
    auto schema_parsed = rime::core::json::parse(read_file(root + "/" + reference.substr(0, separator)));
    expect(schema_parsed.ok(), file + ": cannot parse the referenced schema");
    schema_root = std::move(*schema_parsed.value);
    const Value* defs = schema_root->find("$defs");
    expect(defs != nullptr && defs->is_object(), file + ": referenced schema has no $defs");
    definition = defs->find(fragment.substr(std::string("/$defs/").size()));
    expect(definition != nullptr, file + ": unresolved $defs entry in " + reference);
  }

  check_shape(golden, file, definition == nullptr ? nullptr : &*schema_root, definition, counts);
  check_semantic(golden, file, counts);
  check_lifecycle(golden, file, type, capability, counts);
  check_failure(golden, file, counts);
  check_envelope(golden, file, counts);
  ++counts.files;
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string root = argv[1];
  const std::filesystem::path golden_dir = std::filesystem::path(root) / "contracts" / "golden";
  expect(std::filesystem::is_directory(golden_dir), "missing contracts/golden directory");

  std::vector<std::string> files;
  for (const auto& entry : std::filesystem::directory_iterator(golden_dir)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.ends_with(".json")) files.push_back(name);
  }
  std::sort(files.begin(), files.end());
  expect(files.size() == 15u, "expected exactly 15 golden files, got " + std::to_string(files.size()));

  Counts counts;
  for (const std::string& file : files) {
    check_golden(root, golden_dir / file, counts);
  }

  std::printf(
      "golden contract checks passed (%zu files, %zu valid, %zu violations, %zu lifecycle, "
      "%zu semantic, %zu failure, %zu envelope)\n",
      counts.files, counts.valid, counts.violations, counts.lifecycle, counts.semantic,
      counts.failure, counts.envelope);
  return 0;
}
