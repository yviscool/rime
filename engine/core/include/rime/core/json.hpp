#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rime::core::json {

class Value;

using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
using Object = std::vector<Member>;

class Value {
 public:
  enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

  Value() = default;

  static Value null() { return Value{}; }
  static Value boolean(bool value);
  static Value number(double value);
  static Value string(std::string value);
  static Value array();
  static Value object();

  [[nodiscard]] Type type() const noexcept { return type_; }
  [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }
  [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
  [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
  [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
  [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }

  [[nodiscard]] bool as_bool(bool fallback = false) const noexcept;
  [[nodiscard]] double as_number(double fallback = 0.0) const noexcept;
  [[nodiscard]] std::int64_t as_integer(std::int64_t fallback = 0) const noexcept;
  [[nodiscard]] const std::string& as_string() const;
  [[nodiscard]] const Array& as_array() const;
  [[nodiscard]] const Object& as_object() const;

  [[nodiscard]] const Value* find(std::string_view key) const;
  [[nodiscard]] Value* find(std::string_view key);
  void set(std::string key, Value value);
  void push(Value value);
  [[nodiscard]] std::size_t size() const noexcept;

 private:
  Type type_{Type::Null};
  bool bool_{false};
  double number_{0.0};
  std::string string_;
  Array array_;
  Object object_;
};

struct ParseOutcome {
  std::optional<Value> value;
  std::string error;
  std::size_t offset{0};
  [[nodiscard]] bool ok() const noexcept { return value.has_value(); }
};

// Deterministic JSON parser: preserves member order, rejects trailing input,
// limits nesting depth and keeps integers inside JavaScript's safe range exact.
ParseOutcome parse(std::string_view text);

// Compact deterministic serializer. Non-finite numbers are emitted as null.
std::string stringify(const Value& value);

// Order-sensitive structural equality over parsed or generated values.
bool equals(const Value& left, const Value& right);

}  // namespace rime::core::json
