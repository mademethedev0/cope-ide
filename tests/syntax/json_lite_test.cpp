#include "syntax_test_util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <ide/syntax/json_lite.h>

namespace {

namespace json = ide::syntax::json;
using ember_test::Lcg;

// --- accessors and builders -------------------------------------------------

TEST(JsonLiteValue, DefaultIsNullAndAllReadersAreTotal) {
    json::Value v;
    EXPECT_EQ(v.type(), json::Type::Null);
    EXPECT_TRUE(v.isNull());
    EXPECT_FALSE(v.isBool());
    EXPECT_FALSE(v.isNumber());
    EXPECT_FALSE(v.isString());
    EXPECT_FALSE(v.isArray());
    EXPECT_FALSE(v.isObject());

    // Wrong type never throws: every reader falls back.
    EXPECT_FALSE(v.boolean());
    EXPECT_TRUE(v.boolean(true));
    EXPECT_DOUBLE_EQ(v.number(), 0.0);
    EXPECT_DOUBLE_EQ(v.number(-3.5), -3.5);
    EXPECT_EQ(v.integer(), 0);
    EXPECT_EQ(v.integer(-9), -9);
    EXPECT_EQ(v.string(), "");
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.memberCount(), 0u);
    EXPECT_TRUE(v.at(0).isNull());
    EXPECT_EQ(v.keyAt(0), "");
    EXPECT_TRUE(v.valueAt(0).isNull());
    EXPECT_EQ(v.find("x"), nullptr);
    EXPECT_FALSE(v.contains("x"));
    EXPECT_TRUE(v["x"].isNull());
}

TEST(JsonLiteValue, ScalarSetters) {
    json::Value v;
    v.setBool(true);
    EXPECT_TRUE(v.isBool());
    EXPECT_TRUE(v.boolean());
    EXPECT_DOUBLE_EQ(v.number(7.0), 7.0);  // wrong type -> fallback

    v.setNumber(12.75);
    EXPECT_TRUE(v.isNumber());
    EXPECT_DOUBLE_EQ(v.number(), 12.75);
    EXPECT_EQ(v.integer(), 12);  // truncates toward zero
    EXPECT_EQ(v.string(), "");

    v.setString("hi");
    EXPECT_TRUE(v.isString());
    EXPECT_EQ(v.string(), "hi");
    EXPECT_DOUBLE_EQ(v.number(-1.0), -1.0);

    v.setNull();
    EXPECT_TRUE(v.isNull());
}

TEST(JsonLiteValue, ObjectInsertIsLastWinsAndKeepsInsertionOrder) {
    json::Value one;
    one.setNumber(1.0);
    json::Value two;
    two.setNumber(2.0);
    json::Value three;
    three.setNumber(3.0);

    json::Value obj;
    obj.becomeObject();
    obj.insert("a", one);
    obj.insert("b", two);
    obj.insert("a", three);  // duplicate key: replaces in place

    EXPECT_TRUE(obj.isObject());
    EXPECT_EQ(obj.memberCount(), 2u);
    EXPECT_EQ(obj.size(), 2u);
    EXPECT_EQ(obj.keyAt(0), "a");
    EXPECT_DOUBLE_EQ(obj.valueAt(0).number(), 3.0);
    EXPECT_EQ(obj.keyAt(1), "b");
    EXPECT_DOUBLE_EQ(obj.valueAt(1).number(), 2.0);

    EXPECT_EQ(obj.keyAt(2), "");
    EXPECT_TRUE(obj.valueAt(2).isNull());
    ASSERT_NE(obj.find("b"), nullptr);
    EXPECT_DOUBLE_EQ(obj.find("b")->number(), 2.0);
    EXPECT_EQ(obj.find("c"), nullptr);

    // Absent lookups resolve to the shared immortal null, so chaining is safe.
    EXPECT_EQ(&obj["c"], &json::Value::nullValue());
    EXPECT_TRUE(obj["c"]["deeper"].at(0).isNull());
    EXPECT_TRUE(obj.at(0).isNull());  // at() is array-only
}

TEST(JsonLiteValue, PushConvertsToArray) {
    json::Value item;
    item.setString("x");

    json::Value arr;  // starts as null
    arr.push(item);
    EXPECT_TRUE(arr.isArray());
    EXPECT_EQ(arr.size(), 1u);
    EXPECT_EQ(arr.at(0).string(), "x");
    EXPECT_TRUE(arr.at(1).isNull());
    EXPECT_EQ(arr.memberCount(), 0u);  // arrays have no members
    EXPECT_EQ(arr.keyAt(0), "");

    arr.becomeObject();  // switching container kind clears children
    EXPECT_TRUE(arr.isObject());
    EXPECT_EQ(arr.size(), 0u);
}

// --- parsing: structure ----------------------------------------------------

TEST(JsonLiteParse, EmptyContainersAndWhitespace) {
    const json::ParseResult obj = json::parse("  \t\r\n {}  \n");
    ASSERT_TRUE(obj.ok) << obj.error;
    EXPECT_TRUE(obj.root.isObject());
    EXPECT_EQ(obj.root.memberCount(), 0u);

    const json::ParseResult arr = json::parse("[]");
    ASSERT_TRUE(arr.ok) << arr.error;
    EXPECT_TRUE(arr.root.isArray());
    EXPECT_EQ(arr.root.size(), 0u);
}

TEST(JsonLiteParse, Literals) {
    EXPECT_TRUE(json::parse("true").root.boolean());
    const json::ParseResult f = json::parse("false");
    ASSERT_TRUE(f.ok);
    EXPECT_TRUE(f.root.isBool());
    EXPECT_FALSE(f.root.boolean(true));
    const json::ParseResult n = json::parse("null");
    ASSERT_TRUE(n.ok);
    EXPECT_TRUE(n.root.isNull());
}

TEST(JsonLiteParse, LeadingBomIsSkipped) {
    const json::ParseResult r = json::parse("\xEF\xBB\xBF{\"a\":true}");
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_TRUE(r.root.isObject());
    EXPECT_TRUE(r.root["a"].boolean());
}

TEST(JsonLiteParse, NestedStructure) {
    const json::ParseResult r =
        json::parse(R"({"a":[1,2,{"b":["c"]}],"d":{"e":null},"f":true})");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.root.memberCount(), 3u);
    ASSERT_TRUE(r.root["a"].isArray());
    EXPECT_EQ(r.root["a"].size(), 3u);
    EXPECT_DOUBLE_EQ(r.root["a"].at(0).number(), 1.0);
    EXPECT_DOUBLE_EQ(r.root["a"].at(1).number(), 2.0);
    EXPECT_EQ(r.root["a"].at(2)["b"].at(0).string(), "c");
    EXPECT_TRUE(r.root["d"]["e"].isNull());
    EXPECT_TRUE(r.root["f"].boolean());
}

TEST(JsonLiteParse, DuplicateKeysLastWins) {
    const json::ParseResult r = json::parse(R"({"a":1,"b":2,"a":3})");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.root.memberCount(), 2u);
    EXPECT_DOUBLE_EQ(r.root["a"].number(), 3.0);
    EXPECT_DOUBLE_EQ(r.root["b"].number(), 2.0);
}

// --- parsing: strings ------------------------------------------------------

TEST(JsonLiteParse, TwoCharacterEscapes) {
    const json::ParseResult r = json::parse(R"({"s":"a\"b\\c\/d\be\ff\ng\rh\ti"})");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.root["s"].string(), std::string("a\"b\\c/d\be\ff\ng\rh\ti"));
}

TEST(JsonLiteParse, UnicodeEscapesIncludingSurrogatePairs) {
    // U+0041, U+00E9 (2 bytes), U+20AC (3 bytes), U+1F600 via a surrogate pair.
    const json::ParseResult r = json::parse(R"({"s":"\u0041\u00e9\u20AC\uD83D\uDE00"})");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.root["s"].string(), std::string("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"));
}

TEST(JsonLiteParse, UnpairedSurrogatesBecomeReplacementCharacter) {
    // Documented deviation: unpaired halves decode to U+FFFD instead of failing.
    EXPECT_EQ(json::parse(R"("\uD800")").root.string(), std::string("\xEF\xBF\xBD"));
    EXPECT_EQ(json::parse(R"("\uDC00")").root.string(), std::string("\xEF\xBF\xBD"));
    EXPECT_EQ(json::parse(R"("\uDBFF")").root.string(), std::string("\xEF\xBF\xBD"));

    // A high surrogate followed by a non-low escape: the escape must be re-read,
    // not swallowed.
    EXPECT_EQ(json::parse(R"("\uD800\u0041")").root.string(), std::string("\xEF\xBF\xBD" "A"));
    // High, high, low: the first is replaced, the second pairs up (U+10200).
    EXPECT_EQ(json::parse(R"("\uD800\uD800\uDE00")").root.string(),
              std::string("\xEF\xBF\xBD\xF0\x90\x88\x80"));
    // A high surrogate at the very end of the string.
    EXPECT_EQ(json::parse(R"("x\uD83D")").root.string(), std::string("x\xEF\xBF\xBD"));
}

TEST(JsonLiteParse, RawUtf8PassesThroughByteExact) {
    const json::ParseResult r = json::parse("{\"s\":\"\xE6\x97\xA5\xC3\xA9\"}");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.root["s"].string(), std::string("\xE6\x97\xA5\xC3\xA9"));
    EXPECT_EQ(r.root["s"].string().size(), 5u);
}

TEST(JsonLiteParse, EmptyStringAndEmptyKey) {
    const json::ParseResult r = json::parse(R"({"":""})");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.root.memberCount(), 1u);
    EXPECT_EQ(r.root.keyAt(0), "");
    EXPECT_TRUE(r.root[""].isString());
    EXPECT_EQ(r.root[""].string(), "");
}

// --- parsing: numbers ------------------------------------------------------

TEST(JsonLiteParse, Numbers) {
    EXPECT_DOUBLE_EQ(json::parse("0").root.number(), 0.0);
    EXPECT_DOUBLE_EQ(json::parse("-0").root.number(), 0.0);
    EXPECT_DOUBLE_EQ(json::parse("42").root.number(), 42.0);
    EXPECT_DOUBLE_EQ(json::parse("-7").root.number(), -7.0);
    EXPECT_DOUBLE_EQ(json::parse("12.75").root.number(), 12.75);
    EXPECT_DOUBLE_EQ(json::parse("-0.5").root.number(), -0.5);
    EXPECT_DOUBLE_EQ(json::parse("1e3").root.number(), 1000.0);
    EXPECT_DOUBLE_EQ(json::parse("1E+2").root.number(), 100.0);
    EXPECT_DOUBLE_EQ(json::parse("1e-2").root.number(), 0.01);
    EXPECT_DOUBLE_EQ(json::parse("2.5e2").root.number(), 250.0);
    EXPECT_DOUBLE_EQ(json::parse("1234567890123456789").root.number(), 1234567890123456789.0);
    EXPECT_EQ(json::parse("-7").root.integer(), -7);
    EXPECT_EQ(json::parse("12.75").root.integer(), 12);
}

TEST(JsonLiteParse, LeadingZerosAreAcceptedLeniency) {
    // RFC 8259 forbids "01", json_lite accepts it: grammar and theme files never
    // contain it, and rejecting costs a branch in the hot path. Pinned here so a
    // future strict parser has to change this expectation deliberately.
    const json::ParseResult r = json::parse("01");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_DOUBLE_EQ(r.root.number(), 1.0);
}

// --- parsing: malformed input ----------------------------------------------

TEST(JsonLiteParse, MalformedInputIsRejectedWithAReason) {
    struct Case {
        std::string_view text;
        const char* error;  // const char* so EXPECT_EQ uses std::string's own operator==
    };
    const Case cases[] = {
        {"", "unexpected end of input"},
        {"   ", "unexpected end of input"},
        {"{", "expected a string key"},
        {"{\"a\"}", "expected ':'"},
        {"{\"a\":1,}", "expected a string key"},
        {"{\"a\":1 \"b\":2}", "expected ',' or '}'"},
        {"{a:1}", "expected a string key"},
        {"[", "unexpected end of input"},
        {"[1,]", "invalid number"},
        {"[1 2]", "expected ',' or ']'"},
        {"[1", "expected ',' or ']'"},
        {"\"abc", "unterminated string"},
        {"\"a\\", "unterminated escape"},
        {"\"\\q\"", "invalid string escape"},
        {"\"a\x01" "b\"", "raw control character in string"},
        {"\"\\u12\"", "truncated \\u escape"},
        {"\"\\uZZZZ\"", "invalid hex digit in \\u escape"},
        {"tru", "invalid literal"},
        {"fals", "invalid literal"},
        {"nul", "invalid literal"},
        {"nulll", "trailing content after the JSON value"},
        {"1 2", "trailing content after the JSON value"},
        {"+1", "leading '+' is not valid JSON"},
        {"1.", "expected a digit after '.'"},
        {"1e", "expected a digit in the exponent"},
        {"1e+", "expected a digit in the exponent"},
        {"-", "invalid number"},
        {".5", "invalid number"},
        {"x", "invalid number"},
    };
    for (const Case& c : cases) {
        const json::ParseResult r = json::parse(c.text);
        EXPECT_FALSE(r.ok) << "input: " << c.text;
        EXPECT_EQ(r.error, c.error) << "input: " << c.text;
        EXPECT_TRUE(r.root.isNull()) << "input: " << c.text;
        EXPECT_LE(r.errorOffset, c.text.size()) << "input: " << c.text;
    }
}

TEST(JsonLiteParse, ErrorOffsetPointsAtTheOffendingByte) {
    const json::ParseResult r = json::parse("[1,]");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorOffset, 3u);

    const json::ParseResult t = json::parse("42x");
    EXPECT_FALSE(t.ok);
    EXPECT_EQ(t.errorOffset, 2u);
}

TEST(JsonLiteParse, NestingIsBoundedNotStackOverflowing) {
    const std::string tooDeep(static_cast<size_t>(json::kMaxJsonDepth) + 60u, '[');
    const json::ParseResult bad = json::parse(tooDeep);
    EXPECT_FALSE(bad.ok);
    EXPECT_EQ(bad.error, "nesting too deep");

    // Just inside the limit must still work.
    const size_t deep = 150u;
    const std::string ok = std::string(deep, '[') + std::string(deep, ']');
    const json::ParseResult good = json::parse(ok);
    ASSERT_TRUE(good.ok) << good.error;
    const json::Value* v = &good.root;
    for (size_t i = 0; i + 1u < deep; ++i) {
        ASSERT_TRUE(v->isArray()) << "level " << i;
        ASSERT_EQ(v->size(), 1u) << "level " << i;
        v = &v->at(0);
    }
    EXPECT_TRUE(v->isArray());
    EXPECT_EQ(v->size(), 0u);
}

TEST(JsonLiteParse, HugeDocument) {
    const size_t kCount = 5000u;
    std::string text = "[";
    for (size_t i = 0; i < kCount; ++i) {
        if (i != 0u) text.push_back(',');
        text += "{\"k\":";
        text += std::to_string(i);
        text += ",\"s\":\"v";
        text += std::to_string(i);
        text += "\"}";
    }
    text.push_back(']');

    const json::ParseResult r = json::parse(text);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.root.size(), kCount);
    EXPECT_DOUBLE_EQ(r.root.at(0)["k"].number(), 0.0);
    EXPECT_EQ(r.root.at(0)["s"].string(), "v0");
    EXPECT_DOUBLE_EQ(r.root.at(kCount - 1u)["k"].number(), static_cast<double>(kCount - 1u));
    EXPECT_EQ(r.root.at(kCount - 1u)["s"].string(), "v4999");
    EXPECT_TRUE(r.root.at(kCount).isNull());
}

// --- fuzz-style robustness -------------------------------------------------

constexpr std::string_view kFuzzDoc =
    R"({"a":[1,2,{"b":"c\u00e9\t"},true,null,-1.5e3],"d":{"e":[[]],"f":""}})";

TEST(JsonLiteParse, EveryTruncationOfAValidDocumentFailsCleanly) {
    const json::ParseResult full = json::parse(kFuzzDoc);
    ASSERT_TRUE(full.ok) << full.error;

    // Every strict prefix leaves a container or string unclosed, so all of them
    // must fail - with a reason, an in-range offset, and a null root.
    for (size_t n = 0; n < kFuzzDoc.size(); ++n) {
        const json::ParseResult r = json::parse(kFuzzDoc.substr(0, n));
        EXPECT_FALSE(r.ok) << "prefix length " << n;
        EXPECT_FALSE(r.error.empty()) << "prefix length " << n;
        EXPECT_TRUE(r.root.isNull()) << "prefix length " << n;
        EXPECT_LE(r.errorOffset, n) << "prefix length " << n;
    }
}

TEST(JsonLiteParse, SingleByteMutationsNeverCrash) {
    static const char kBytes[] = {'"', '\\', '{', '}',  '[',   ']',    ':',
                                  ',', 'x',  '0', '\n', '\x01', '\x7F', '\xFF'};
    for (size_t i = 0; i < kFuzzDoc.size(); ++i) {
        for (const char b : kBytes) {
            std::string mutated(kFuzzDoc);
            mutated[i] = b;
            const json::ParseResult r = json::parse(mutated);
            if (!r.ok) {
                EXPECT_FALSE(r.error.empty()) << "at " << i;
                EXPECT_LE(r.errorOffset, mutated.size()) << "at " << i;
            }
        }
    }
}

// --- differential round trip ----------------------------------------------
//
// An independently written serializer produces the text; parse() must rebuild an
// identical tree. Numbers are restricted to integers so the comparison can be
// exact and the test can never fail for floating point reasons.

void writeJson(const json::Value& v, std::string& out);

void writeJsonString(std::string_view s, std::string& out) {
    static const char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (u < 0x20u) {
            out += "\\u00";
            out.push_back(kHex[(u >> 4) & 0xFu]);
            out.push_back(kHex[u & 0xFu]);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

void writeJson(const json::Value& v, std::string& out) {
    switch (v.type()) {
        case json::Type::Null:
            out += "null";
            return;
        case json::Type::Bool:
            out += v.boolean() ? "true" : "false";
            return;
        case json::Type::Number:
            out += std::to_string(v.integer());
            return;
        case json::Type::String:
            writeJsonString(v.string(), out);
            return;
        case json::Type::Array:
            out.push_back('[');
            for (size_t i = 0; i < v.size(); ++i) {
                if (i != 0u) out.push_back(',');
                writeJson(v.at(i), out);
            }
            out.push_back(']');
            return;
        case json::Type::Object:
            out.push_back('{');
            for (size_t i = 0; i < v.memberCount(); ++i) {
                if (i != 0u) out.push_back(',');
                writeJsonString(v.keyAt(i), out);
                out.push_back(':');
                writeJson(v.valueAt(i), out);
            }
            out.push_back('}');
            return;
    }
}

[[nodiscard]] bool deepEqual(const json::Value& a, const json::Value& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case json::Type::Null:
            return true;
        case json::Type::Bool:
            return a.boolean() == b.boolean();
        case json::Type::Number:
            return a.integer() == b.integer();
        case json::Type::String:
            return a.string() == b.string();
        case json::Type::Array:
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (!deepEqual(a.at(i), b.at(i))) return false;
            }
            return true;
        case json::Type::Object:
            if (a.memberCount() != b.memberCount()) return false;
            for (size_t i = 0; i < a.memberCount(); ++i) {
                if (a.keyAt(i) != b.keyAt(i)) return false;
                if (!deepEqual(a.valueAt(i), b.valueAt(i))) return false;
            }
            return true;
    }
    return false;
}

[[nodiscard]] std::string randomString(Lcg& rng) {
    // Deliberately includes quotes, backslashes, control bytes and multi-byte
    // UTF-8 so the writer's escaping and the parser's unescaping must agree.
    static const std::string_view kPieces[] = {"a",  "Z",  "\"", "\\", "/",  "\n", "\t",
                                               "\x01", " ", "\xC3\xA9", "\xE2\x82\xAC",
                                               "\xF0\x9F\x98\x80", "-", "0"};
    std::string out;
    const size_t pieces = rng.below(6u);
    for (size_t i = 0; i < pieces; ++i) {
        out += kPieces[rng.below(sizeof(kPieces) / sizeof(kPieces[0]))];
    }
    return out;
}

[[nodiscard]] json::Value randomValue(Lcg& rng, int depth) {
    json::Value v;
    const size_t kind = rng.below(depth >= 4 ? 4u : 6u);
    switch (kind) {
        case 0:
            v.setNull();
            return v;
        case 1:
            v.setBool(rng.below(2u) == 0u);
            return v;
        case 2:
            v.setNumber(static_cast<double>(static_cast<int64_t>(rng.below(200001u)) - 100000));
            return v;
        case 3:
            v.setString(randomString(rng));
            return v;
        case 4: {
            v.becomeArray();
            const size_t n = rng.below(5u);
            for (size_t i = 0; i < n; ++i) v.push(randomValue(rng, depth + 1));
            return v;
        }
        default: {
            v.becomeObject();
            const size_t n = rng.below(5u);
            for (size_t i = 0; i < n; ++i) {
                // Unique keys: duplicate keys would legitimately collapse.
                v.insert("k" + std::to_string(i), randomValue(rng, depth + 1));
            }
            return v;
        }
    }
}

TEST(JsonLiteParse, RoundTripsAgainstAnIndependentWriter) {
    Lcg rng(0xC0FFEEu);
    for (int trial = 0; trial < 400; ++trial) {
        const json::Value original = randomValue(rng, 0);
        std::string text;
        writeJson(original, text);
        const json::ParseResult r = json::parse(text);
        ASSERT_TRUE(r.ok) << "trial " << trial << " text: " << text << " error: " << r.error;
        EXPECT_TRUE(deepEqual(original, r.root)) << "trial " << trial << " text: " << text;

        // And the reparsed tree must serialize back to exactly the same bytes.
        std::string again;
        writeJson(r.root, again);
        EXPECT_EQ(again, text) << "trial " << trial;
    }
}

}  // namespace
