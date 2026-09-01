#include <ide/syntax/json_lite.h>

#include <cmath>
#include <utility>

namespace ide::syntax::json {
namespace {

constexpr bool isWs(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

/// Appends `cp` as UTF-8. Only called with values <= 0x10FFFF.
void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) noexcept : s_(text) {}

    bool run(Value& out) {
        skipBom();
        skipWs();
        if (!parseValue(out, 0)) return false;
        skipWs();
        if (i_ != s_.size()) return fail("trailing content after the JSON value");
        return true;
    }

    [[nodiscard]] size_t errorOffset() const noexcept { return errorAt_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    bool fail(const char* message) {
        if (error_.empty()) {
            error_ = message;
            errorAt_ = i_;
        }
        return false;
    }

    void skipBom() {
        if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEFu &&
            static_cast<unsigned char>(s_[1]) == 0xBBu &&
            static_cast<unsigned char>(s_[2]) == 0xBFu) {
            i_ = 3;
        }
    }
    void skipWs() {
        while (i_ < s_.size() && isWs(s_[i_])) ++i_;
    }
    [[nodiscard]] bool eof() const noexcept { return i_ >= s_.size(); }
    [[nodiscard]] char peek() const noexcept { return i_ < s_.size() ? s_[i_] : '\0'; }

    bool parseValue(Value& out, int depth) {
        if (depth > kMaxJsonDepth) return fail("nesting too deep");
        if (eof()) return fail("unexpected end of input");
        switch (peek()) {
            case '{':
                return parseObject(out, depth);
            case '[':
                return parseArray(out, depth);
            case '"': {
                std::string str;
                if (!parseString(str)) return false;
                out.setString(std::move(str));
                return true;
            }
            case 't':
                if (s_.compare(i_, 4, "true") != 0) return fail("invalid literal");
                i_ += 4;
                out.setBool(true);
                return true;
            case 'f':
                if (s_.compare(i_, 5, "false") != 0) return fail("invalid literal");
                i_ += 5;
                out.setBool(false);
                return true;
            case 'n':
                if (s_.compare(i_, 4, "null") != 0) return fail("invalid literal");
                i_ += 4;
                out.setNull();
                return true;
            default:
                return parseNumber(out);
        }
    }

    bool parseObject(Value& out, int depth) {
        ++i_;  // '{'
        out.becomeObject();
        skipWs();
        if (peek() == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            skipWs();
            if (peek() != '"') return fail("expected a string key");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (peek() != ':') return fail("expected ':'");
            ++i_;
            skipWs();
            Value child;
            if (!parseValue(child, depth + 1)) return false;
            out.insert(std::move(key), std::move(child));
            skipWs();
            if (peek() == ',') {
                ++i_;
                continue;
            }
            if (peek() == '}') {
                ++i_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(Value& out, int depth) {
        ++i_;  // '['
        out.becomeArray();
        skipWs();
        if (peek() == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            skipWs();
            Value child;
            if (!parseValue(child, depth + 1)) return false;
            out.push(std::move(child));
            skipWs();
            if (peek() == ',') {
                ++i_;
                continue;
            }
            if (peek() == ']') {
                ++i_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parseHex4(uint32_t& out) {
        if (i_ + 4 > s_.size()) return fail("truncated \\u escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_ + static_cast<size_t>(k)];
            uint32_t d = 0;
            if (c >= '0' && c <= '9') {
                d = static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<uint32_t>(c - 'a') + 10u;
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<uint32_t>(c - 'A') + 10u;
            } else {
                return fail("invalid hex digit in \\u escape");
            }
            v = v * 16u + d;
        }
        i_ += 4;
        out = v;
        return true;
    }

    bool parseString(std::string& out) {
        if (peek() != '"') return fail("expected '\"'");
        ++i_;
        out.clear();
        for (;;) {
            if (eof()) return fail("unterminated string");
            const char c = s_[i_];
            if (c == '"') {
                ++i_;
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20u) {
                return fail("raw control character in string");
            }
            if (c != '\\') {
                out.push_back(c);
                ++i_;
                continue;
            }
            ++i_;  // '\'
            if (eof()) return fail("unterminated escape");
            const char e = s_[i_];
            ++i_;
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!parseHex4(cp)) return false;
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        // high surrogate: expect a following low surrogate
                        if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                            const size_t save = i_;
                            i_ += 2;
                            uint32_t low = 0;
                            if (!parseHex4(low)) return false;
                            if (low >= 0xDC00u && low <= 0xDFFFu) {
                                cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                            } else {
                                // not a low surrogate: emit U+FFFD for the high
                                // half and re-read the escape we consumed
                                i_ = save;
                                cp = 0xFFFDu;
                            }
                        } else {
                            cp = 0xFFFDu;
                        }
                    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                        cp = 0xFFFDu;  // lone low surrogate
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail("invalid string escape");
            }
        }
    }

    bool parseNumber(Value& out) {
        const size_t start = i_;
        bool negative = false;
        if (peek() == '-') {
            negative = true;
            ++i_;
        } else if (peek() == '+') {
            return fail("leading '+' is not valid JSON");
        }
        if (eof() || !isDigit(peek())) {
            i_ = start;
            return fail("invalid number");
        }
        uint64_t mantissa = 0;
        int exponent = 0;
        int significant = 0;
        while (!eof() && isDigit(peek())) {
            if (significant < 19) {
                mantissa = mantissa * 10u + static_cast<uint64_t>(peek() - '0');
                if (mantissa != 0) ++significant;
            } else {
                ++exponent;
            }
            ++i_;
        }
        if (!eof() && peek() == '.') {
            ++i_;
            if (eof() || !isDigit(peek())) return fail("expected a digit after '.'");
            while (!eof() && isDigit(peek())) {
                if (significant < 19) {
                    mantissa = mantissa * 10u + static_cast<uint64_t>(peek() - '0');
                    if (mantissa != 0) ++significant;
                    --exponent;
                }
                ++i_;
            }
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            ++i_;
            bool expNegative = false;
            if (!eof() && (peek() == '+' || peek() == '-')) {
                expNegative = (peek() == '-');
                ++i_;
            }
            if (eof() || !isDigit(peek())) return fail("expected a digit in the exponent");
            int value = 0;
            while (!eof() && isDigit(peek())) {
                if (value < 100000) value = value * 10 + (peek() - '0');
                ++i_;
            }
            exponent += expNegative ? -value : value;
        }
        double result = static_cast<double>(mantissa);
        if (exponent != 0) result *= std::pow(10.0, static_cast<double>(exponent));
        out.setNumber(negative ? -result : result);
        return true;
    }

    std::string_view s_;
    size_t i_ = 0;
    size_t errorAt_ = 0;
    std::string error_;
};

}  // namespace

const Value& Value::nullValue() noexcept {
    static const Value kNull;
    return kNull;
}

const Value& Value::at(size_t index) const noexcept {
    if (!isArray() || index >= children_.size()) return nullValue();
    return children_[index];
}

std::string_view Value::keyAt(size_t index) const noexcept {
    if (!isObject() || index >= keys_.size()) return {};
    return keys_[index];
}

const Value& Value::valueAt(size_t index) const noexcept {
    if (!isObject() || index >= children_.size()) return nullValue();
    return children_[index];
}

const Value* Value::find(std::string_view key) const noexcept {
    if (!isObject()) return nullptr;
    for (size_t k = 0; k < keys_.size(); ++k) {
        if (keys_[k] == key) return &children_[k];
    }
    return nullptr;
}

const Value& Value::operator[](std::string_view key) const noexcept {
    const Value* v = find(key);
    return (v == nullptr) ? nullValue() : *v;
}

void Value::setNull() noexcept { type_ = Type::Null; }

void Value::setBool(bool v) {
    type_ = Type::Bool;
    bool_ = v;
}

void Value::setNumber(double v) {
    type_ = Type::Number;
    num_ = v;
}

void Value::setString(std::string v) {
    type_ = Type::String;
    str_ = std::move(v);
}

void Value::becomeArray() {
    type_ = Type::Array;
    keys_.clear();
    children_.clear();
}

void Value::becomeObject() {
    type_ = Type::Object;
    keys_.clear();
    children_.clear();
}

void Value::push(Value v) {
    if (!isArray()) becomeArray();
    children_.push_back(std::move(v));
}

void Value::insert(std::string key, Value v) {
    if (!isObject()) becomeObject();
    for (size_t k = 0; k < keys_.size(); ++k) {
        if (keys_[k] == key) {
            children_[k] = std::move(v);
            return;
        }
    }
    keys_.push_back(std::move(key));
    children_.push_back(std::move(v));
}

ParseResult parse(std::string_view text) {
    ParseResult result;
    Parser parser(text);
    Value root;
    if (parser.run(root)) {
        result.root = std::move(root);
        result.ok = true;
        return result;
    }
    result.ok = false;
    result.error = parser.error().empty() ? std::string("parse error") : parser.error();
    result.errorOffset = parser.errorOffset();
    return result;
}

}  // namespace ide::syntax::json
