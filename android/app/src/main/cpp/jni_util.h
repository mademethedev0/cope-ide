#pragma once

// JNI plumbing shared by the Cope bridge.
//
// The one non-obvious thing in this file: **never GetStringUTFChars**. That
// function returns *modified* UTF-8, in which a supplementary codepoint (every
// emoji, every CJK extension character) arrives as a six-byte CESU-8 surrogate
// pair instead of the four-byte UTF-8 sequence the engine stores. Inserting a
// typed emoji through it would silently corrupt the document, and the corruption
// would be invisible until the file was reopened elsewhere.
//
// So strings cross as UTF-16 and are converted here, once, correctly:
//   Java String -> engine bytes   toUtf8()
//   engine bytes -> Java String   toJavaString()   (invalid bytes -> U+FFFD)
//
// Anything derived from *document content* does not use either: it crosses as a
// byte[] or inside the viewport blob, and Kotlin decodes it leniently. A code
// editor must be able to open a file that is not valid UTF-8 and show it without
// mangling the bytes it did understand.

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cope::jni {

inline constexpr uint32_t kReplacement = 0xFFFDu;

/// UTF-16 (with surrogate pairs) -> UTF-8. Unpaired surrogates become U+FFFD, so
/// the result is always well-formed UTF-8 regardless of what Java handed us.
inline void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
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

/// Converts a Java string to UTF-8. Returns an empty string for null.
inline std::string toUtf8(JNIEnv* env, jstring value) {
    std::string out;
    if (env == nullptr || value == nullptr) {
        return out;
    }
    const jsize length = env->GetStringLength(value);
    const jchar* chars = env->GetStringCritical(value, nullptr);
    if (chars == nullptr) {
        return out;
    }
    out.reserve(static_cast<size_t>(length) * 3u / 2u + 4u);
    for (jsize i = 0; i < length; ++i) {
        const uint32_t unit = static_cast<uint32_t>(chars[i]);
        if (unit >= 0xD800u && unit <= 0xDBFFu && i + 1 < length) {
            const uint32_t low = static_cast<uint32_t>(chars[i + 1]);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                appendUtf8(out, 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u));
                ++i;
                continue;
            }
        }
        if (unit >= 0xD800u && unit <= 0xDFFFu) {
            appendUtf8(out, kReplacement);  // unpaired surrogate
            continue;
        }
        appendUtf8(out, unit);
    }
    env->ReleaseStringCritical(value, chars);
    return out;
}

/// UTF-8 -> Java String. Invalid sequences become U+FFFD; never fails.
inline jstring toJavaString(JNIEnv* env, std::string_view text) {
    std::vector<jchar> units;
    units.reserve(text.size() + 1u);
    size_t i = 0;
    while (i < text.size()) {
        const uint8_t b0 = static_cast<uint8_t>(text[i]);
        uint32_t cp = kReplacement;
        size_t width = 1;
        if (b0 < 0x80u) {
            cp = b0;
        } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < text.size() &&
                   (static_cast<uint8_t>(text[i + 1]) & 0xC0u) == 0x80u) {
            cp = ((b0 & 0x1Fu) << 6) | (static_cast<uint8_t>(text[i + 1]) & 0x3Fu);
            width = 2;
            if (cp < 0x80u) {
                cp = kReplacement;  // overlong
            }
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < text.size() &&
                   (static_cast<uint8_t>(text[i + 1]) & 0xC0u) == 0x80u &&
                   (static_cast<uint8_t>(text[i + 2]) & 0xC0u) == 0x80u) {
            cp = ((b0 & 0x0Fu) << 12) | ((static_cast<uint8_t>(text[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<uint8_t>(text[i + 2]) & 0x3Fu);
            width = 3;
            if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
                cp = kReplacement;
            }
        } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < text.size() &&
                   (static_cast<uint8_t>(text[i + 1]) & 0xC0u) == 0x80u &&
                   (static_cast<uint8_t>(text[i + 2]) & 0xC0u) == 0x80u &&
                   (static_cast<uint8_t>(text[i + 3]) & 0xC0u) == 0x80u) {
            cp = ((b0 & 0x07u) << 18) | ((static_cast<uint8_t>(text[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<uint8_t>(text[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<uint8_t>(text[i + 3]) & 0x3Fu);
            width = 4;
            if (cp < 0x10000u || cp > 0x10FFFFu) {
                cp = kReplacement;
            }
        }
        i += width;
        if (cp <= 0xFFFFu) {
            units.push_back(static_cast<jchar>(cp));
        } else {
            const uint32_t v = cp - 0x10000u;
            units.push_back(static_cast<jchar>(0xD800u + (v >> 10)));
            units.push_back(static_cast<jchar>(0xDC00u + (v & 0x3FFu)));
        }
    }
    return env->NewString(units.data(), static_cast<jsize>(units.size()));
}

/// Copies raw bytes into a fresh Java byte[]. Used for document-derived text, so
/// invalid UTF-8 survives the trip and Kotlin decides how to display it.
inline jbyteArray toByteArray(JNIEnv* env, std::string_view bytes) {
    jbyteArray array = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (array == nullptr) {
        return nullptr;
    }
    if (!bytes.empty()) {
        env->SetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()),
                                reinterpret_cast<const jbyte*>(bytes.data()));
    }
    return array;
}

/// Little helper for the int arrays the theme surface returns.
inline jintArray toIntArray(JNIEnv* env, const std::vector<jint>& values) {
    jintArray array = env->NewIntArray(static_cast<jsize>(values.size()));
    if (array == nullptr) {
        return nullptr;
    }
    if (!values.empty()) {
        env->SetIntArrayRegion(array, 0, static_cast<jsize>(values.size()), values.data());
    }
    return array;
}

inline jlongArray toLongArray(JNIEnv* env, const std::vector<jlong>& values) {
    jlongArray array = env->NewLongArray(static_cast<jsize>(values.size()));
    if (array == nullptr) {
        return nullptr;
    }
    if (!values.empty()) {
        env->SetLongArrayRegion(array, 0, static_cast<jsize>(values.size()), values.data());
    }
    return array;
}

}  // namespace cope::jni
