// Minimal JSON for the BuffrX protocol and config.
//
// Deliberately small and dependency-free: one value type, a recursive-descent parser and a
// serializer. It is correct for the subset this product emits and accepts; anything outside that
// subset is a parse error rather than a guess. Integers are preserved as integers so byte counts and
// timestamps never round-trip through a double.
#pragma once

#include <cstdint>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bx {

#ifndef _WIN32
// JSON's number grammar is locale-independent. GTK (the tray) calls setlocale(LC_ALL, ""), which on a
// comma-decimal desktop would write 63,49 into a frame; uselocale pins the serializer and parser.
inline locale_t json_c_locale() {
  static locale_t value = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(nullptr));
  return value;
}
inline double json_strtod(const char* text) {
  locale_t previous = uselocale(json_c_locale());
  const double value = std::strtod(text, nullptr);
  uselocale(previous);
  return value;
}
#else
inline double json_strtod(const char* text) { return std::strtod(text, nullptr); }
#endif

// How many values one document may hold. A frame is bounded in bytes at the pipe, and every value in
// it costs a heap object: without this a small frame of tiny numbers grows into hundreds of megabytes
// and the engine dies with a running buffer. No command or reply comes near this figure.
constexpr size_t kMaxJsonItems = 65536;

class Json {
public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() : type_(Type::Null) {}
  Json(std::nullptr_t) : type_(Type::Null) {}
  Json(bool value) : type_(Type::Bool), bool_(value) {}
  Json(int value) : type_(Type::Number), number_(static_cast<double>(value)), integer_(true), int_(value) {}
  Json(int64_t value) : type_(Type::Number), number_(static_cast<double>(value)), integer_(true), int_(value) {}
  Json(uint64_t value) : type_(Type::Number), number_(static_cast<double>(value)), integer_(true), int_(static_cast<int64_t>(value)) {}
  Json(double value) : type_(Type::Number), number_(value), integer_(false) {}
  Json(const char* value) : type_(Type::String), string_(value ? value : "") {}
  Json(const std::string& value) : type_(Type::String), string_(value) {}

  static Json array() {
    Json value;
    value.type_ = Type::Array;
    return value;
  }

  static Json object() {
    Json value;
    value.type_ = Type::Object;
    return value;
  }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
  double as_double(double fallback = 0.0) const { return type_ == Type::Number ? number_ : fallback; }
  int64_t as_int(int64_t fallback = 0) const {
    if (type_ != Type::Number) return fallback;
    if (integer_) return int_;
    // A double past the int64 range would make llround undefined: clamp it instead, as the caller
    // wanted a number and this is the largest one there is.
    if (number_ <= -9.2e18) return INT64_MIN;
    if (number_ >= 9.2e18) return INT64_MAX;
    return static_cast<int64_t>(std::llround(number_));
  }
  std::string as_string(const std::string& fallback = std::string()) const {
    return type_ == Type::String ? string_ : fallback;
  }

  const std::vector<Json>& items() const { return array_; }
  std::vector<Json>& items() { return array_; }
  const std::map<std::string, Json>& fields() const { return object_; }

  bool has(const std::string& key) const {
    return type_ == Type::Object && object_.find(key) != object_.end();
  }

  // Missing keys read as Null so callers never crash on an older client or a hand-edited config.
  const Json& operator[](const std::string& key) const {
    static const Json kNull;
    if (type_ != Type::Object) return kNull;
    auto found = object_.find(key);
    return found == object_.end() ? kNull : found->second;
  }


  // There is deliberately no non-const operator[]: it used to turn a Null into an empty Object (and insert the key) on a
  // plain read, which made "not loaded yet" look like "loaded, empty" and hid the Advanced rows. Write with set().
  void set(const std::string& key, Json value) {
    type_ = Type::Object;
    object_[key] = std::move(value);
  }

  // Removes a key, saying whether it was there: writing a JSON null for a field that did not exist is not the
  // same as the field not existing, and a hand-edited config reads the two differently.
  bool erase(const std::string& key) {
    if (type_ != Type::Object) return false;
    return object_.erase(key) > 0;
  }

  void push(Json value) {
    type_ = Type::Array;
    array_.push_back(std::move(value));
  }

  std::string dump(int indent = -1) const {
    std::string out;
    write(out, indent, 0);
    return out;
  }

  static bool parse(const std::string& text, Json& out, std::string& error) {
    // Notepad saves "UTF-8 with BOM" and PowerShell's own UTF8 encoder writes one too: a settings file
    // made that way must still be read, not fall back to every default.
    const bool marked = text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF;
    size_t position = marked ? 3 : 0;
    size_t items = 0;
    if (!parse_value(text, position, out, error, 0, items)) return false;
    skip_space(text, position);
    if (position != text.size()) {
      error = "trailing characters after JSON value";
      return false;
    }
    return true;
  }

private:
  static const int kMaxDepth = 32;

  Type type_;
  bool bool_ = false;
  double number_ = 0.0;
  bool integer_ = false;
  int64_t int_ = 0;
  std::string string_;
  std::vector<Json> array_;
  std::map<std::string, Json> object_;

  static void skip_space(const std::string& text, size_t& position) {
    while (position < text.size()) {
      char c = text[position];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++position;
      } else {
        break;
      }
    }
  }

  static bool parse_value(const std::string& text, size_t& position, Json& out, std::string& error, int depth,
                          size_t& items) {
    if (depth > kMaxDepth) {
      error = "nesting too deep";
      return false;
    }
    if (++items > kMaxJsonItems) {
      error = "too many values";
      return false;
    }
    skip_space(text, position);
    if (position >= text.size()) {
      error = "unexpected end of input";
      return false;
    }
    char c = text[position];
    switch (c) {
      case '{': return parse_object(text, position, out, error, depth, items);
      case '[': return parse_array(text, position, out, error, depth, items);
      case '"': {
        std::string value;
        if (!parse_string(text, position, value, error)) return false;
        out = Json(value);
        return true;
      }
      case 't':
        if (text.compare(position, 4, "true") == 0) { position += 4; out = Json(true); return true; }
        error = "invalid literal";
        return false;
      case 'f':
        if (text.compare(position, 5, "false") == 0) { position += 5; out = Json(false); return true; }
        error = "invalid literal";
        return false;
      case 'n':
        if (text.compare(position, 4, "null") == 0) { position += 4; out = Json(); return true; }
        error = "invalid literal";
        return false;
      default: return parse_number(text, position, out, error);
    }
  }

  static bool parse_object(const std::string& text, size_t& position, Json& out, std::string& error, int depth,
                           size_t& items) {
    ++position;  // '{'
    out = Json::object();
    skip_space(text, position);
    if (position < text.size() && text[position] == '}') {
      ++position;
      return true;
    }
    while (true) {
      skip_space(text, position);
      if (position >= text.size() || text[position] != '"') {
        error = "expected object key";
        return false;
      }
      std::string key;
      if (!parse_string(text, position, key, error)) return false;
      skip_space(text, position);
      if (position >= text.size() || text[position] != ':') {
        error = "expected ':'";
        return false;
      }
      ++position;
      Json value;
      if (!parse_value(text, position, value, error, depth + 1, items)) return false;
      out.set(key, std::move(value));
      skip_space(text, position);
      if (position < text.size() && text[position] == ',') {
        ++position;
        continue;
      }
      if (position < text.size() && text[position] == '}') {
        ++position;
        return true;
      }
      error = "expected ',' or '}'";
      return false;
    }
  }

  static bool parse_array(const std::string& text, size_t& position, Json& out, std::string& error, int depth,
                          size_t& items) {
    ++position;  // '['
    out = Json::array();
    skip_space(text, position);
    if (position < text.size() && text[position] == ']') {
      ++position;
      return true;
    }
    while (true) {
      Json value;
      if (!parse_value(text, position, value, error, depth + 1, items)) return false;
      out.push(std::move(value));
      skip_space(text, position);
      if (position < text.size() && text[position] == ',') {
        ++position;
        continue;
      }
      if (position < text.size() && text[position] == ']') {
        ++position;
        return true;
      }
      error = "expected ',' or ']'";
      return false;
    }
  }

  static void append_utf8(std::string& out, unsigned int code) {
    if (code <= 0x7F) {
      out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  // The four hex digits of a \u escape, read where the position points.
  static bool read_hex4(const std::string& text, size_t& position, unsigned int& code, std::string& error) {
    if (position + 4 > text.size()) {
      error = "bad \\u escape";
      return false;
    }
    code = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = text[position++];
      code <<= 4;
      if (h >= '0' && h <= '9') code |= static_cast<unsigned int>(h - '0');
      else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned int>(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned int>(h - 'A' + 10);
      else {
        error = "bad \\u escape";
        return false;
      }
    }
    return true;
  }

  static bool parse_string(const std::string& text, size_t& position, std::string& out, std::string& error) {
    ++position;  // opening quote
    out.clear();
    while (position < text.size()) {
      char c = text[position++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (position >= text.size()) break;
      char esc = text[position++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned int code = 0;
          if (!read_hex4(text, position, code, error)) return false;
          if (code >= 0xD800 && code <= 0xDBFF) {
            // A high half names no character on its own: its low half follows as the next escape.
            // Writing the halves out as they came made six bytes that no file name accepts.
            unsigned int low = 0;
            if (position + 1 >= text.size() || text[position] != '\\' || text[position + 1] != 'u') {
              error = "a surrogate half without its pair";
              return false;
            }
            position += 2;
            if (!read_hex4(text, position, low, error) || low < 0xDC00 || low > 0xDFFF) {
              error = "a surrogate half without its pair";
              return false;
            }
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          } else if (code >= 0xDC00 && code <= 0xDFFF) {
            error = "a surrogate half without its pair";
            return false;
          }
          append_utf8(out, code);
          break;
        }
        default: error = "unknown escape"; return false;
      }
    }
    error = "unterminated string";
    return false;
  }

  static bool parse_number(const std::string& text, size_t& position, Json& out, std::string& error) {
    const size_t start = position;
    if (position < text.size() && text[position] == '-') ++position;
    size_t digits = 0;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
      ++position;
      ++digits;
    }
    bool is_integer = true;
    if (position < text.size() && text[position] == '.') {
      is_integer = false;
      ++position;
      size_t fraction = 0;
      while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
        ++position;
        ++fraction;
      }
      if (fraction == 0) {
        error = "a dot with no digits after it";
        return false;
      }
    }
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
      is_integer = false;
      ++position;
      if (position < text.size() && (text[position] == '+' || text[position] == '-')) ++position;
      size_t exponent = 0;
      while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
        ++position;
        ++exponent;
      }
      if (exponent == 0) {
        error = "an exponent with no digits";
        return false;
      }
    }
    if (digits == 0) {
      error = "not a number";
      return false;
    }
    const std::string slice = text.substr(start, position - start);
    if (is_integer) {
      errno = 0;
      long long value = std::strtoll(slice.c_str(), nullptr, 10);
      if (errno == 0) {
        out = Json(static_cast<int64_t>(value));
        return true;
      }
    }
    errno = 0;
    const double number = json_strtod(slice.c_str());
    // 1e999 reads as +inf, which has no JSON spelling: written back it became null and the value was
    // gone. A number that is not finite, or that overflowed/underflowed, is a parse error instead.
    if (errno == ERANGE || !std::isfinite(number)) {
      error = "a number out of range";
      return false;
    }
    out = Json(number);
    return true;
  }

  static void escape_to(std::string& out, const std::string& value) {
    out.push_back('"');
    for (char c : value) {
      unsigned char u = static_cast<unsigned char>(c);
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (u < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", u);
            out += buffer;
          } else {
            out.push_back(c);
          }
      }
    }
    out.push_back('"');
  }

  void write(std::string& out, int indent, int depth) const {
    switch (type_) {
      case Type::Null: out += "null"; return;
      case Type::Bool: out += bool_ ? "true" : "false"; return;
      case Type::Number: {
        if (integer_) {
          out += std::to_string(int_);
        } else if (!std::isfinite(number_)) {
          // JSON has no spelling for these; "nan" made a config file unreadable on the next start.
          out += "null";
        } else {
          char buffer[40];
#ifndef _WIN32
          locale_t previous = uselocale(json_c_locale());
          std::snprintf(buffer, sizeof(buffer), "%.10g", number_);
          uselocale(previous);
#else
          std::snprintf(buffer, sizeof(buffer), "%.10g", number_);
#endif
          out += buffer;
        }
        return;
      }
      case Type::String: escape_to(out, string_); return;
      case Type::Array: {
        out.push_back('[');
        for (size_t i = 0; i < array_.size(); ++i) {
          if (i) out.push_back(',');
          if (indent >= 0) { out.push_back('\n'); out.append(static_cast<size_t>((depth + 1) * indent), ' '); }
          array_[i].write(out, indent, depth + 1);
        }
        if (indent >= 0 && !array_.empty()) { out.push_back('\n'); out.append(static_cast<size_t>(depth * indent), ' '); }
        out.push_back(']');
        return;
      }
      case Type::Object: {
        out.push_back('{');
        bool first = true;
        for (const auto& entry : object_) {
          if (!first) out.push_back(',');
          first = false;
          if (indent >= 0) { out.push_back('\n'); out.append(static_cast<size_t>((depth + 1) * indent), ' '); }
          escape_to(out, entry.first);
          out.push_back(':');
          if (indent >= 0) out.push_back(' ');
          entry.second.write(out, indent, depth + 1);
        }
        if (indent >= 0 && !object_.empty()) { out.push_back('\n'); out.append(static_cast<size_t>(depth * indent), ' '); }
        out.push_back('}');
        return;
      }
    }
  }
};

}  // namespace bx
