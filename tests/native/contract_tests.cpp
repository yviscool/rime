#include "rime/action/codec.hpp"
#include "rime/core/json.hpp"

#include <cassert>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace {

std::string read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  assert(input);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

using rime::core::json::Value;

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string root = argv[1];

  const std::string action_text = read_file(root + "/contracts/schema/examples/action-v1.json");
  const std::string result_text = read_file(root + "/contracts/schema/examples/result-v1.json");

  // JSON parser determinism: parse -> stringify -> parse is stable.
  {
    auto first = rime::core::json::parse(action_text);
    assert(first.ok());
    const std::string encoded = rime::core::json::stringify(*first.value);
    auto second = rime::core::json::parse(encoded);
    assert(second.ok());
    assert(rime::core::json::equals(*first.value, *second.value));
    assert(encoded.find("\n") == std::string::npos);
  }

  // Action example decodes, re-encodes and round-trips structurally.
  {
    auto decoded = rime::action::decode_action(action_text);
    assert(decoded.ok());
    assert(decoded.action->id == 42);
    assert(decoded.action->type == "window.move");
    assert(decoded.action->capability == "windows.window.write");
    assert(decoded.action->source.kind == "user");
    assert(decoded.action->target.id == "active");
    assert(decoded.action->preconditions.size() == 1);
    assert(decoded.action->deadline_unix_ms == 1893456000000ULL);
    assert(decoded.action->idempotency_key == "move-active-left-1");

    auto encoded = rime::action::encode_action(*decoded.action);
    assert(encoded.has_value());
    auto reparsed = rime::core::json::parse(*encoded);
    assert(reparsed.ok());
    auto original = rime::core::json::parse(action_text);
    assert(original.ok());
    assert(rime::core::json::equals(*original.value, *reparsed.value));
  }

  // Result example decodes, re-encodes and round-trips structurally.
  {
    auto decoded = rime::action::decode_result(result_text);
    assert(decoded.ok());
    assert(decoded.result->id == 42);
    assert(decoded.result->succeeded);
    assert(decoded.result->value.is_object());
    auto encoded = rime::action::encode_result(*decoded.result);
    assert(encoded.has_value());
    auto reparsed = rime::core::json::parse(*encoded);
    assert(reparsed.ok());
    auto original = rime::core::json::parse(result_text);
    assert(original.ok());
    assert(rime::core::json::equals(*original.value, *reparsed.value));
  }

  // Failed result encoding carries the contract error shape.
  {
    rime::action::Result failed;
    failed.id = 7;
    failed.detail = "window is gone";
    failed.error = {rime::core::Error::Code::ExecutionFailed, "window is gone"};
    auto encoded = rime::action::encode_result(failed);
    assert(encoded.has_value());
    auto decoded = rime::action::decode_result(*encoded);
    assert(decoded.ok());
    assert(!decoded.result->succeeded && !decoded.result->cancelled);
    assert(decoded.result->error.code == rime::core::Error::Code::ExecutionFailed);
    assert(rime::core::error_code_name(decoded.result->error.code) ==
           std::string("execution_failed"));
  }

  // Contract violations are rejected with InvalidContract.
  {
    const char* cases[] = {
        "{}",
        "{\"schemaVersion\":2,\"id\":1,\"source\":{\"kind\":\"a\",\"id\":\"b\"},"
        "\"type\":\"t\",\"capability\":\"c\",\"target\":{\"kind\":\"w\",\"id\":\"x\"},"
        "\"preconditions\":[],\"deadlineUnixMs\":1,\"payload\":{}}",
        "{\"schemaVersion\":1,\"id\":9007199254740993,"
        "\"source\":{\"kind\":\"a\",\"id\":\"b\"},\"type\":\"t\",\"capability\":\"c\","
        "\"target\":{\"kind\":\"w\",\"id\":\"x\"},\"preconditions\":[],"
        "\"deadlineUnixMs\":1,\"payload\":{}}",
        "{\"schemaVersion\":1,\"id\":1,\"source\":{\"kind\":\"a\",\"id\":\"b\"},"
        "\"type\":\"t\",\"capability\":\"c\",\"target\":{\"kind\":\"w\",\"id\":\"x\"},"
        "\"preconditions\":[],\"deadlineUnixMs\":1,\"payload\":{},\"extra\":true}",
        "{\"schemaVersion\":1,\"id\":1,\"source\":{\"kind\":\"a\"},\"type\":\"t\","
        "\"capability\":\"c\",\"target\":{\"kind\":\"w\",\"id\":\"x\"},"
        "\"preconditions\":[],\"deadlineUnixMs\":1,\"payload\":{}}",
        "{\"schemaVersion\":1,\"id\":1,\"source\":{\"kind\":\"a\",\"id\":\"b\"},"
        "\"type\":\"t\",\"capability\":\"c\",\"target\":{\"kind\":\"w\",\"id\":\"x\"},"
        "\"preconditions\":[],\"deadlineUnixMs\":1,\"payload\":[]} trailing",
    };
    for (const char* text : cases) {
      auto decoded = rime::action::decode_action(text);
      assert(!decoded.ok());
      assert(decoded.error.code == rime::core::Error::Code::InvalidContract);
    }
    assert(!rime::action::decode_result("{\"schemaVersion\":1,\"actionId\":1,"
                                        "\"status\":\"nope\",\"error\":null}").ok());
    assert(!rime::action::decode_result("{\"schemaVersion\":1,\"actionId\":1,"
                                        "\"status\":\"failed\",\"error\":null}").ok());
  }

  // Parser boundaries: escapes, unicode, depth limit and malformed input.
  {
    auto unicode = rime::core::json::parse("\"\\u4f60\\u597d \\ud83d\\ude00\"");
    assert(unicode.ok());
    assert(unicode.value->as_string() == "\xE4\xBD\xA0\xE5\xA5\xBD \xF0\x9F\x98\x80");
    assert(!rime::core::json::parse("\"\\ud800\"").ok());
    assert(!rime::core::json::parse("01").ok());
    assert(!rime::core::json::parse("").ok());
    assert(!rime::core::json::parse("{\"a\":1,}").ok());
    std::string deep(200, '[');
    deep.append(200, ']');
    assert(!rime::core::json::parse(deep).ok());

    Value object = Value::object();
    object.set("b", Value::number(2));
    object.set("a", Value::boolean(false));
    const std::string encoded = rime::core::json::stringify(object);
    assert(encoded == "{\"b\":2,\"a\":false}");

    Value big = Value::number(1893456000000.0);
    assert(rime::core::json::stringify(big) == "1893456000000");
    Value broken = Value::number(std::numeric_limits<double>::quiet_NaN());
    assert(rime::core::json::stringify(broken) == "null");
  }

  // Error code names round-trip through the contract mapping.
  {
    for (rime::core::Error::Code code :
         {rime::core::Error::Code::None, rime::core::Error::Code::InvalidState,
          rime::core::Error::Code::QueueClosed, rime::core::Error::Code::QueueFull,
          rime::core::Error::Code::Cancelled, rime::core::Error::Code::CapabilityDenied,
          rime::core::Error::Code::InvalidContract, rime::core::Error::Code::Unsupported,
          rime::core::Error::Code::ExecutionFailed, rime::core::Error::Code::TargetGone}) {
      rime::core::Error::Code parsed{};
      assert(rime::core::error_code_from_name(rime::core::error_code_name(code), parsed));
      assert(parsed == code);
    }
  }

  return 0;
}
