#include "rime/core/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rime::core::json {
namespace {

constexpr std::size_t k_max_depth = 128;
constexpr double k_max_safe_integer = 9007199254740991.0;

void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

class Parser final {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  ParseOutcome run() {
    skip_whitespace();
    Value root;
    if (!parse_value(root, 0)) return {std::nullopt, std::move(error_), error_offset_};
    skip_whitespace();
    if (position_ != text_.size()) {
      return {std::nullopt, "unexpected trailing input", position_};
    }
    return {std::move(root), {}, 0};
  }

 private:
  bool fail(std::string message) {
    if (error_.empty()) {
      error_ = std::move(message);
      error_offset_ = position_;
    }
    return false;
  }

  void skip_whitespace() {
    while (position_ < text_.size()) {
      const char character = text_[position_];
      if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
        ++position_;
      } else {
        break;
      }
    }
  }

  bool consume(char expected) {
    if (position_ < text_.size() && text_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

  bool literal(std::string_view expected) {
    if (text_.substr(position_, expected.size()) != expected) return false;
    position_ += expected.size();
    return true;
  }

  bool parse_value(Value& out, std::size_t depth) {
    if (depth > k_max_depth) return fail("maximum nesting depth exceeded");
    if (position_ >= text_.size()) return fail("unexpected end of input");
    switch (text_[position_]) {
      case 'n':
        if (!literal("null")) return fail("invalid literal");
        out = Value::null();
        return true;
      case 't':
        if (!literal("true")) return fail("invalid literal");
        out = Value::boolean(true);
        return true;
      case 'f':
        if (!literal("false")) return fail("invalid literal");
        out = Value::boolean(false);
        return true;
      case '"': {
        std::string value;
        if (!parse_string(value)) return false;
        out = Value::string(std::move(value));
        return true;
      }
      case '[':
        return parse_array(out, depth);
      case '{':
        return parse_object(out, depth);
      default:
        return parse_number(out);
    }
  }

  bool parse_string(std::string& out) {
    if (!consume('"')) return fail("expected string");
    out.clear();
    while (position_ < text_.size()) {
      const unsigned char character = static_cast<unsigned char>(text_[position_]);
      if (character == '"') {
        ++position_;
        return true;
      }
      if (character < 0x20) return fail("unescaped control character in string");
      if (character != '\\') {
        out.push_back(static_cast<char>(character));
        ++position_;
        continue;
      }
      ++position_;
      if (position_ >= text_.size()) return fail("unterminated escape sequence");
      const char escape = text_[position_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t code_point = 0;
          if (!hex4(code_point)) return fail("invalid unicode escape");
          if (code_point >= 0xD800 && code_point <= 0xDBFF) {
            if (position_ + 1 >= text_.size() || text_[position_] != '\\' ||
                text_[position_ + 1] != 'u') {
              return fail("unpaired surrogate escape");
            }
            position_ += 2;
            std::uint32_t low = 0;
            if (!hex4(low)) return fail("invalid unicode escape");
            if (low < 0xDC00 || low > 0xDFFF) return fail("invalid low surrogate");
            code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
          } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
            return fail("unpaired low surrogate escape");
          }
          append_utf8(out, code_point);
          break;
        }
        default:
          return fail("invalid escape sequence");
      }
    }
    return fail("unterminated string");
  }

  bool hex4(std::uint32_t& out) {
    if (position_ + 4 > text_.size()) return false;
    out = 0;
    for (int index = 0; index < 4; ++index) {
      const char character = text_[position_ + index];
      out <<= 4;
      if (character >= '0' && character <= '9') out |= static_cast<std::uint32_t>(character - '0');
      else if (character >= 'a' && character <= 'f') out |= static_cast<std::uint32_t>(character - 'a' + 10);
      else if (character >= 'A' && character <= 'F') out |= static_cast<std::uint32_t>(character - 'A' + 10);
      else return false;
    }
    position_ += 4;
    return true;
  }

  bool parse_number(Value& out) {
    const std::size_t start = position_;
    if (position_ < text_.size() && text_[position_] == '-') ++position_;
    if (position_ >= text_.size()) return fail("invalid number");
    if (text_[position_] == '0') {
      ++position_;
    } else if (text_[position_] >= '1' && text_[position_] <= '9') {
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
    } else {
      return fail("invalid number");
    }
    if (position_ < text_.size() && text_[position_] == '.') {
      ++position_;
      if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
        return fail("invalid number fraction");
      }
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
      if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
        return fail("invalid number exponent");
      }
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
    }
    const std::string copy(text_.substr(start, position_ - start));
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(value)) return fail("invalid number");
    out = Value::number(value);
    return true;
  }

  bool parse_array(Value& out, std::size_t depth) {
    if (!consume('[')) return fail("expected array");
    Value array = Value::array();
    skip_whitespace();
    if (consume(']')) {
      out = std::move(array);
      return true;
    }
    for (;;) {
      skip_whitespace();
      Value element;
      if (!parse_value(element, depth + 1)) return false;
      array.push(std::move(element));
      skip_whitespace();
      if (consume(',')) continue;
      if (consume(']')) {
        out = std::move(array);
        return true;
      }
      return fail("expected ',' or ']' in array");
    }
  }

  bool parse_object(Value& out, std::size_t depth) {
    if (!consume('{')) return fail("expected object");
    Value object = Value::object();
    skip_whitespace();
    if (consume('}')) {
      out = std::move(object);
      return true;
    }
    for (;;) {
      skip_whitespace();
      std::string key;
      if (!parse_string(key)) return false;
      skip_whitespace();
      if (!consume(':')) return fail("expected ':' after object key");
      skip_whitespace();
      Value member;
      if (!parse_value(member, depth + 1)) return false;
      object.set(std::move(key), std::move(member));
      skip_whitespace();
      if (consume(',')) continue;
      if (consume('}')) {
        out = std::move(object);
        return true;
      }
      return fail("expected ',' or '}' in object");
    }
  }

  std::string_view text_;
  std::size_t position_{0};
  std::string error_;
  std::size_t error_offset_{0};
};

void stringify_string(const std::string& value, std::string& out) {
  out.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (character < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", character);
          out += buffer;
        } else {
          out.push_back(static_cast<char>(character));
        }
    }
  }
  out.push_back('"');
}

void stringify_number(double value, std::string& out) {
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }
  if (value == std::floor(value) && std::fabs(value) <= k_max_safe_integer) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    out += buffer;
    return;
  }
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  out += buffer;
}

void stringify_value(const Value& value, std::string& out) {
  switch (value.type()) {
    case Value::Type::Null:
      out += "null";
      break;
    case Value::Type::Bool:
      out += value.as_bool() ? "true" : "false";
      break;
    case Value::Type::Number:
      stringify_number(value.as_number(), out);
      break;
    case Value::Type::String:
      stringify_string(value.as_string(), out);
      break;
    case Value::Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const Value& element : value.as_array()) {
        if (!first) out.push_back(',');
        first = false;
        stringify_value(element, out);
      }
      out.push_back(']');
      break;
    }
    case Value::Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const Member& member : value.as_object()) {
        if (!first) out.push_back(',');
        first = false;
        stringify_string(member.first, out);
        out.push_back(':');
        stringify_value(member.second, out);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

Value Value::boolean(const bool value) {
  Value result;
  result.type_ = Type::Bool;
  result.bool_ = value;
  return result;
}

Value Value::number(const double value) {
  Value result;
  result.type_ = Type::Number;
  result.number_ = value;
  return result;
}

Value Value::string(std::string value) {
  Value result;
  result.type_ = Type::String;
  result.string_ = std::move(value);
  return result;
}

Value Value::array() {
  Value result;
  result.type_ = Type::Array;
  return result;
}

Value Value::object() {
  Value result;
  result.type_ = Type::Object;
  return result;
}

bool Value::as_bool(const bool fallback) const noexcept {
  return type_ == Type::Bool ? bool_ : fallback;
}

double Value::as_number(const double fallback) const noexcept {
  return type_ == Type::Number ? number_ : fallback;
}

std::int64_t Value::as_integer(const std::int64_t fallback) const noexcept {
  if (type_ != Type::Number) return fallback;
  return static_cast<std::int64_t>(number_);
}

const std::string& Value::as_string() const {
  static const std::string empty;
  return type_ == Type::String ? string_ : empty;
}

const Array& Value::as_array() const {
  static const Array empty;
  return type_ == Type::Array ? array_ : empty;
}

const Object& Value::as_object() const {
  static const Object empty;
  return type_ == Type::Object ? object_ : empty;
}

const Value* Value::find(const std::string_view key) const {
  if (type_ != Type::Object) return nullptr;
  for (const Member& member : object_) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

Value* Value::find(const std::string_view key) {
  return const_cast<Value*>(static_cast<const Value*>(this)->find(key));
}

void Value::set(std::string key, Value value) {
  type_ = Type::Object;
  for (Member& member : object_) {
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  }
  object_.emplace_back(std::move(key), std::move(value));
}

void Value::push(Value value) {
  type_ = Type::Array;
  array_.push_back(std::move(value));
}

std::size_t Value::size() const noexcept {
  if (type_ == Type::Array) return array_.size();
  if (type_ == Type::Object) return object_.size();
  return 0;
}

ParseOutcome parse(const std::string_view text) {
  Parser parser(text);
  ParseOutcome outcome = parser.run();
  if (!outcome.ok()) return outcome;
  return outcome;
}

std::string stringify(const Value& value) {
  std::string out;
  stringify_value(value, out);
  return out;
}

bool equals(const Value& left, const Value& right) {
  if (left.type() != right.type()) return false;
  switch (left.type()) {
    case Value::Type::Null:
      return true;
    case Value::Type::Bool:
      return left.as_bool() == right.as_bool();
    case Value::Type::Number:
      return left.as_number() == right.as_number();
    case Value::Type::String:
      return left.as_string() == right.as_string();
    case Value::Type::Array: {
      const Array& left_array = left.as_array();
      const Array& right_array = right.as_array();
      if (left_array.size() != right_array.size()) return false;
      for (std::size_t index = 0; index < left_array.size(); ++index) {
        if (!equals(left_array[index], right_array[index])) return false;
      }
      return true;
    }
    case Value::Type::Object: {
      const Object& left_object = left.as_object();
      const Object& right_object = right.as_object();
      if (left_object.size() != right_object.size()) return false;
      for (const Member& member : left_object) {
        const Value* other = right.find(member.first);
        if (!other || !equals(member.second, *other)) return false;
      }
      return true;
    }
  }
  return false;
}

}  // namespace rime::core::json
