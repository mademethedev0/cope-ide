#pragma once

// Why this file exists
// -------------------
// Grammar and theme files are JSON, but core/ is not allowed to depend on a
// third-party library and nlohmann/json is not vendored yet. This is a small,
// self-contained, allocation-honest JSON value + parser that is "correct enough
// for grammar and theme files": objects, arrays, strings with escapes and
// \uXXXX (including surrogate pairs), numbers, booleans, null.
//
// It is deliberately shaped like nlohmann::json's *accessor* surface (find(),
// operator[], size(), at(), string(), number()) so that swapping in
// nlohmann::json later is a change to this header plus json_lite.cpp only, with
// no edits to grammar.cpp / theme loading.
//
// Deviations from RFC 8259, all deliberate:
//   * a leading UTF-8 BOM is skipped;
//   * duplicate object keys resolve to "last one wins";
//   * raw control characters inside strings are rejected;
//   * unpaired UTF-16 surrogates decode to U+FFFD instead of failing.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ide::syntax::json {

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

/// One JSON node. Copyable (grammars are small) and self-owning; children live
/// in a flat vector so an object member is (keys_[i], children_[i]).
class Value {
public:
    Value() = default;

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool isNull() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool isBool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool isString() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return type_ == Type::Object; }

    /// Scalar readers. All total: a wrong type yields the fallback, never a throw.
    [[nodiscard]] bool boolean(bool fallback = false) const noexcept {
        return isBool() ? bool_ : fallback;
    }
    [[nodiscard]] double number(double fallback = 0.0) const noexcept {
        return isNumber() ? num_ : fallback;
    }
    [[nodiscard]] int64_t integer(int64_t fallback = 0) const noexcept {
        return isNumber() ? static_cast<int64_t>(num_) : fallback;
    }
    /// Empty string when this is not a string node.
    [[nodiscard]] const std::string& string() const noexcept { return str_; }

    /// Element count for arrays, member count for objects, 0 otherwise.
    [[nodiscard]] size_t size() const noexcept { return children_.size(); }
    /// Array element; the shared null value when out of range.
    [[nodiscard]] const Value& at(size_t index) const noexcept;

    [[nodiscard]] size_t memberCount() const noexcept { return isObject() ? keys_.size() : 0u; }
    [[nodiscard]] std::string_view keyAt(size_t index) const noexcept;
    [[nodiscard]] const Value& valueAt(size_t index) const noexcept;

    /// nullptr when absent or when this is not an object.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    [[nodiscard]] bool contains(std::string_view key) const noexcept { return find(key) != nullptr; }
    /// The shared null value when absent, so chains like v["a"]["b"] are safe.
    [[nodiscard]] const Value& operator[](std::string_view key) const noexcept;
    /// Deleted on purpose. A literal `v[0]` is a null pointer constant, so it
    /// would silently bind to the string_view overload and construct a view
    /// from nullptr (undefined behaviour, in practice a segfault). Deleting the
    /// nullptr_t overload turns that mistake into a compile error. Index into
    /// arrays with at(index).
    const Value& operator[](std::nullptr_t) const = delete;

    // --- builders (used by the parser and by tests) --------------------------
    void setNull() noexcept;
    void setBool(bool v);
    void setNumber(double v);
    void setString(std::string v);
    void becomeArray();
    void becomeObject();
    void push(Value v);                        ///< array append
    void insert(std::string key, Value v);     ///< object insert, last wins

    /// A single immortal null node, used by the safe accessors.
    [[nodiscard]] static const Value& nullValue() noexcept;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<std::string> keys_;  ///< object keys, insertion order
    std::vector<Value> children_;   ///< array elements or object values
};

/// Parse outcome. `ok == false` leaves `root` null and fills error/errorOffset.
struct ParseResult {
    Value root;
    bool ok = false;
    size_t errorOffset = 0;
    std::string error;
};

/// Maximum container nesting the parser accepts. Deep input is rejected rather
/// than allowed to overflow the C++ stack.
inline constexpr int kMaxJsonDepth = 200;

/// Parses `text`. Never throws for malformed input; on success the whole input
/// (modulo trailing whitespace) was consumed.
[[nodiscard]] ParseResult parse(std::string_view text);

}  // namespace ide::syntax::json
