// libcope_jni.so — the whole native surface of the Cope app.
//
// Threading contract
// ------------------
// Theme::resolve() memoises into mutable state and Highlighter is explicitly not
// thread safe, so every entry point below takes the engine's mutex. Kotlin may
// therefore call from any thread (viewport fetches happen off the main thread,
// edits on it) without a per-call ceremony. An uncontended lock is ~20ns, which
// is noise next to tokenizing a screen of text.
//
// Error contract
// --------------
// No exception ever crosses the boundary: every entry point catches and returns
// a documented failure value (0 handle, false, -1, null, empty array). The engine
// itself never throws by design, but std::bad_alloc on a 2 GB phone is real.
//
// Handle contract
// ---------------
// Handles are pointers widened to jlong. 0 is "invalid" and is always safe to
// pass to every function. Kotlin holds them in a class with an explicit close().

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ide/highlight/highlighter.h>
#include <ide/highlight/quality.h>
#include <ide/host/mapped_source.h>
#include <ide/render/markdown.h>
#include <ide/syntax/grammar.h>
#include <ide/syntax/json_lite.h>
#include <ide/syntax/regex_factory.h>
#include <ide/text/document.h>
#include <ide/text/search.h>
#include <ide/theme/theme.h>
#include <posix_host.h>

#include "jni_util.h"
#include "md_stream.h"

namespace {

using cope::jni::toByteArray;
using cope::jni::toIntArray;
using cope::jni::toJavaString;
using cope::jni::toLongArray;
using cope::jni::toUtf8;

constexpr const char* kTag = "cope";

/// Blob layout, shared with ViewportBlob.kt. Bump on any change.
///
///   header  8 x int32        magic | version | firstLine | lineCount |
///                            spanCount | textBytes | tier | flags
///   lines   lineCount x 5    byteOffset | textOffset | textLength |
///                            spanOffset | spanCount
///   spans   spanCount x 3    begin | end | styleId  (byte offsets in the line)
///   text    textBytes        line content, UTF-8, terminators excluded
///
/// `byteOffset` is the line's absolute document offset. It rides along
/// specifically so the renderer never needs a JNI call per visible line to place
/// a caret or a selection rectangle.
constexpr int32_t kBlobMagic = 0x45504F43;  // 'COPE' little-endian
constexpr int32_t kBlobVersion = 1;
constexpr size_t kHeaderInts = 8;
constexpr size_t kLineInts = 5;
constexpr size_t kSpanInts = 3;

/// One tokenizer state kept every N lines instead of one per line: 400k lines
/// cost ~1600 states (~50 KB) rather than 400k (~13 MB), and a scroll jump
/// replays at most N lines. This is the small, always-on half of phase 6.
constexpr int64_t kCheckpointStride = 256;

/// Cap on the lines the inspector measures, so `quality` on a 500 MB file cannot
/// stall the UI thread.
constexpr int64_t kQualityLineCap = 4000;

class Engine;

/// One open document: bytes, highlighter, state checkpoints, and the reusable
/// viewport blob.
class Session {
public:
    Session(Engine& owner, std::shared_ptr<const ide::text::ByteSource> source, std::string name,
            size_t byteSize);
    Session(Engine& owner, std::string bytes, std::string name);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    ide::text::Document& doc() noexcept { return doc_; }
    const ide::text::Document& doc() const noexcept { return doc_; }
    const std::string& name() const noexcept { return name_; }
    const std::string& path() const noexcept { return path_; }
    void setPath(std::string path) { path_ = std::move(path); }

    bool dirty() const noexcept { return doc_.version() != savedVersion_; }
    void markSaved() noexcept { savedVersion_ = doc_.version(); }

    /// True when the file was read whole into memory instead of mapped (the SAF
    /// path). The UI states this, because it is the reason a 200 MB SAF document
    /// is refused while a 200 MB real path opens instantly.
    bool inMemory() const noexcept { return inMemory_; }

    ide::highlight::Highlighter& highlighter() noexcept { return *highlighter_; }

    /// Drops cached state at and after `line`. Called by every mutation.
    void invalidateFrom(int64_t line) {
        if (line < 0) {
            line = 0;
        }
        const size_t keep = static_cast<size_t>(line / kCheckpointStride) + 1u;
        if (checkpoints_.size() > keep) {
            checkpoints_.resize(keep);
        }
    }

    /// Line content without its terminator, into `out`. Returns the line's range,
    /// so a caller that also needs the absolute offset pays for one lookup.
    ide::text::LineRange lineText(int64_t line, std::string& out) const {
        const ide::text::LineRange range = doc_.lineAt(line);
        out.resize(range.length());
        if (!out.empty()) {
            const size_t got = doc_.copyOut(range.start, std::span<char>(out.data(), out.size()));
            out.resize(got);
        }
        return range;
    }

    /// Tokenizer state *before* `line`, computing and checkpointing as needed.
    ide::highlight::LineState stateBefore(int64_t line);

    /// Fills `blob_` with [firstLine, firstLine + count) and returns it.
    const std::vector<char>& viewport(int64_t firstLine, int64_t count);

private:
    void initHighlighter(size_t byteSize);

    Engine& owner_;
    std::string name_;
    std::string path_;
    bool inMemory_ = false;
    ide::text::Document doc_;
    std::optional<ide::highlight::Highlighter> highlighter_;
    std::vector<ide::highlight::LineState> checkpoints_;  ///< [k] = state before k*stride
    int64_t savedVersion_ = 0;
    int64_t stateVersion_ = 0;  ///< document version the checkpoints belong to

    // Scratch, reused every frame so scrolling allocates nothing.
    std::string lineScratch_;
    std::vector<ide::highlight::StyledSpan> spanScratch_;
    std::vector<ide::highlight::ScopedSpan> scopedScratch_;
    std::vector<char> blob_;
};

/// Process-wide state: assets, grammars, regex backend, current theme, sessions.
///
/// OWNERSHIP: every Session holds a reference to its Engine, so all sessions must
/// be closed before the engine is destroyed. Kotlin guarantees that (CopeEngine
/// closes its documents in close()), and closeSession refuses to run without a
/// live engine rather than risk a use-after-free.
class Engine {
public:
    explicit Engine(AAssetManager* assets) : assets_(assets) {
        regex_ = ide::syntax::makeRegexEngine();
        theme_ = std::make_unique<ide::theme::Theme>();
        registry_.setLoader([this](std::string_view scope) -> std::optional<std::string> {
            return loadGrammarAsset(scope);
        });
    }

    std::mutex& mutex() noexcept { return mutex_; }
    ide::syntax::GrammarRegistry& registry() noexcept { return registry_; }
    ide::syntax::IRegexEngine& regex() noexcept { return *regex_; }
    const ide::theme::Theme& theme() const noexcept { return *theme_; }
    ide::host::PosixHost& fs() noexcept { return fs_; }

    /// index/grammars.tsv: scope \t assetFile \t ext,ext,...
    /// Only the *mapping* is loaded here; grammar JSON is read on first use.
    void loadGrammarIndex(std::string_view tsv) {
        size_t pos = 0;
        while (pos < tsv.size()) {
            size_t eol = tsv.find('\n', pos);
            if (eol == std::string_view::npos) {
                eol = tsv.size();
            }
            const std::string_view line = tsv.substr(pos, eol - pos);
            pos = eol + 1u;
            const size_t t1 = line.find('\t');
            if (t1 == std::string_view::npos) {
                continue;
            }
            const size_t t2 = line.find('\t', t1 + 1u);
            const std::string_view scope = line.substr(0, t1);
            const std::string_view file =
                line.substr(t1 + 1u, (t2 == std::string_view::npos ? line.size() : t2) - t1 - 1u);
            if (scope.empty() || file.empty()) {
                continue;
            }
            scopeToAsset_.emplace(std::string(scope), std::string(file));
            if (t2 == std::string_view::npos) {
                continue;
            }
            std::string_view exts = line.substr(t2 + 1u);
            while (!exts.empty()) {
                const size_t comma = exts.find(',');
                const std::string_view ext = exts.substr(0, comma);
                if (!ext.empty()) {
                    // First index entry wins, matching the CLI: a later grammar
                    // must not steal an extension an earlier one claimed.
                    extToScope_.emplace(std::string(ext), std::string(scope));
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                exts = exts.substr(comma + 1u);
            }
        }
        applyCanonicalExtensions();
    }

    /// Scope name for a file name, "" when nothing claims it. Extension lookup
    /// only; the registry's own map is populated lazily from the same table.
    std::string scopeForFile(std::string_view name) const {
        const size_t dot = name.find_last_of('.');
        if (dot == std::string_view::npos || dot + 1u == name.size()) {
            return std::string();
        }
        std::string ext(name.substr(dot + 1u));
        for (char& c : ext) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        const auto it = extToScope_.find(ext);
        return it == extToScope_.end() ? std::string() : it->second;
    }

    /// Makes the registry able to resolve `name`'s extension. Must run before a
    /// Highlighter is constructed for that file.
    void ensureExtensionMapped(std::string_view name) {
        const std::string scope = scopeForFile(name);
        if (scope.empty()) {
            return;
        }
        const size_t dot = name.find_last_of('.');
        std::string ext(name.substr(dot + 1u));
        for (char& c : ext) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        registry_.mapExtension(ext, scope);
    }

    /// Replaces the theme, keeping every live Highlighter pointing at a live
    /// object: the new theme is handed to all sessions BEFORE the old one dies.
    bool setThemeJson(std::string_view json) {
        const auto parsed = ide::syntax::json::parse(json);
        if (!parsed.ok) {
            return false;
        }
        auto loaded = ide::theme::Theme::fromJson(parsed.root, nullptr);
        if (!loaded.has_value()) {
            return false;
        }
        auto next = std::make_unique<ide::theme::Theme>(std::move(*loaded));
        for (Session* session : sessions_) {
            session->highlighter().setTheme(*next);
        }
        theme_ = std::move(next);
        return true;
    }

    void addSession(Session* session) { sessions_.push_back(session); }
    void removeSession(Session* session) {
        sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), session), sessions_.end());
    }

    /// Whole asset file as a string. nullopt when absent or unreadable.
    std::optional<std::string> readAsset(const std::string& path) {
        if (assets_ == nullptr) {
            return std::nullopt;
        }
        AAsset* asset = AAssetManager_open(assets_, path.c_str(), AASSET_MODE_STREAMING);
        if (asset == nullptr) {
            return std::nullopt;
        }
        const off64_t length = AAsset_getLength64(asset);
        std::string out;
        if (length > 0) {
            out.resize(static_cast<size_t>(length));
            size_t filled = 0;
            while (filled < out.size()) {
                const int read = AAsset_read(asset, out.data() + filled,
                                             out.size() - filled);
                if (read <= 0) {
                    break;
                }
                filled += static_cast<size_t>(read);
            }
            out.resize(filled);
        }
        AAsset_close(asset);
        return out;
    }

private:
    std::optional<std::string> loadGrammarAsset(std::string_view scope) {
        const auto it = scopeToAsset_.find(std::string(scope));
        if (it == scopeToAsset_.end()) {
            return std::nullopt;
        }
        return readAsset("grammars/" + it->second);
    }

    /// The same canonical map cope_cli applies: grammars that ship no fileTypes
    /// because the tmLanguage idiom leaves the mapping to the editor. Applied
    /// only when the index actually knows the target scope.
    void applyCanonicalExtensions() {
        const std::pair<const char*, const char*> canonical[] = {
            {"js", "source.js"},          {"mjs", "source.js"},
            {"cjs", "source.js"},         {"jsx", "source.js"},
            {"ts", "source.ts"},          {"tsx", "source.ts"},
            {"php", "source.php"},        {"html", "text.html.basic"},
            {"htm", "text.html.basic"},   {"css", "source.css"},
            {"scss", "source.css"},       {"less", "source.css"},
            {"py", "source.python"},      {"pyi", "source.python"},
            {"json", "source.json"},      {"c", "source.c"},
            {"h", "source.c"},            {"cpp", "source.cpp"},
            {"cc", "source.cpp"},         {"hpp", "source.cpp"},
            {"cxx", "source.cpp"},        {"hxx", "source.cpp"},
            {"ino", "source.cpp"},        {"md", "text.html.markdown"},
            {"markdown", "text.html.markdown"},
        };
        for (const auto& [ext, scope] : canonical) {
            if (scopeToAsset_.find(scope) != scopeToAsset_.end()) {
                extToScope_[ext] = scope;
            }
        }
    }

    std::mutex mutex_;
    AAssetManager* assets_ = nullptr;
    ide::host::PosixHost fs_;
    ide::syntax::GrammarRegistry registry_;
    std::unique_ptr<ide::syntax::IRegexEngine> regex_;
    std::unique_ptr<ide::theme::Theme> theme_;
    std::unordered_map<std::string, std::string> scopeToAsset_;
    std::unordered_map<std::string, std::string> extToScope_;
    std::vector<Session*> sessions_;
};

// --- Session ---------------------------------------------------------------

Session::Session(Engine& owner, std::shared_ptr<const ide::text::ByteSource> source,
                 std::string name, size_t byteSize)
    : owner_(owner), name_(std::move(name)), doc_(std::move(source)) {
    initHighlighter(byteSize);
}

Session::Session(Engine& owner, std::string bytes, std::string name)
    : owner_(owner), name_(std::move(name)), inMemory_(true), doc_(std::move(bytes)) {
    initHighlighter(doc_.size());
}

Session::~Session() { owner_.removeSession(this); }

void Session::initHighlighter(size_t byteSize) {
    owner_.ensureExtensionMapped(name_);
    ide::highlight::FileInfo info;
    info.name = name_;
    info.byteSize = byteSize;
    info.lineCount = static_cast<size_t>(doc_.lineCount());
    highlighter_.emplace(owner_.registry(), owner_.regex(), owner_.theme(), info);

    // Probe with the head of the file so a useless grammar is demoted before the
    // first frame instead of after the user notices grey text.
    std::vector<std::string> head;
    std::vector<std::string_view> views;
    const int64_t probeLines =
        std::min<int64_t>(doc_.lineCount(), static_cast<int64_t>(highlighter_->limits().probeLines));
    head.reserve(static_cast<size_t>(probeLines));
    for (int64_t i = 0; i < probeLines; ++i) {
        std::string text;
        lineText(i, text);
        head.push_back(std::move(text));
    }
    views.reserve(head.size());
    for (const std::string& line : head) {
        views.emplace_back(line);
    }
    highlighter_->probe(views);

    checkpoints_.clear();
    checkpoints_.push_back(highlighter_->initialState());
    stateVersion_ = doc_.version();
    savedVersion_ = doc_.version();
    owner_.addSession(this);
}

ide::highlight::LineState Session::stateBefore(int64_t line) {
    if (stateVersion_ != doc_.version()) {
        // A mutation happened without invalidateFrom (never expected, but a
        // wrong colour is better than a stale-state crash).
        checkpoints_.resize(1);
        stateVersion_ = doc_.version();
    }
    if (checkpoints_.empty()) {
        checkpoints_.push_back(highlighter_->initialState());
    }
    if (line <= 0) {
        return checkpoints_.front();
    }

    const size_t want = static_cast<size_t>(line / kCheckpointStride);
    while (checkpoints_.size() <= want) {
        const int64_t from = static_cast<int64_t>(checkpoints_.size() - 1u) * kCheckpointStride;
        ide::highlight::LineState state = checkpoints_.back();
        const int64_t to = from + kCheckpointStride;
        for (int64_t i = from; i < to; ++i) {
            lineText(i, lineScratch_);
            highlighter_->scopeLine(lineScratch_, state, scopedScratch_);
        }
        checkpoints_.push_back(state);
    }

    ide::highlight::LineState state = checkpoints_[want];
    for (int64_t i = static_cast<int64_t>(want) * kCheckpointStride; i < line; ++i) {
        lineText(i, lineScratch_);
        highlighter_->scopeLine(lineScratch_, state, scopedScratch_);
    }
    return state;
}

const std::vector<char>& Session::viewport(int64_t firstLine, int64_t count) {
    const int64_t lines = doc_.lineCount();
    if (firstLine < 0) {
        firstLine = 0;
    }
    if (firstLine > lines - 1) {
        firstLine = lines - 1;
    }
    if (count < 0) {
        count = 0;
    }
    if (firstLine + count > lines) {
        count = lines - firstLine;
    }

    // Pass 1: highlight into flat arrays. Two vectors instead of a vector of
    // vectors, because the blob wants exactly this shape.
    std::vector<int32_t> lineRecords;
    std::vector<int32_t> spanRecords;
    std::string text;
    lineRecords.reserve(static_cast<size_t>(count) * kLineInts);
    spanRecords.reserve(static_cast<size_t>(count) * kSpanInts * 8u);
    text.reserve(static_cast<size_t>(count) * 64u);

    ide::highlight::LineState state = stateBefore(firstLine);
    for (int64_t i = 0; i < count; ++i) {
        const ide::text::LineRange range = lineText(firstLine + i, lineScratch_);
        highlighter_->highlightLine(lineScratch_, state, spanScratch_);

        lineRecords.push_back(static_cast<int32_t>(range.start));
        lineRecords.push_back(static_cast<int32_t>(text.size()));
        lineRecords.push_back(static_cast<int32_t>(lineScratch_.size()));
        lineRecords.push_back(static_cast<int32_t>(spanRecords.size() / kSpanInts));
        lineRecords.push_back(static_cast<int32_t>(spanScratch_.size()));
        text.append(lineScratch_);
        for (const ide::highlight::StyledSpan& span : spanScratch_) {
            spanRecords.push_back(static_cast<int32_t>(span.begin));
            spanRecords.push_back(static_cast<int32_t>(span.end));
            spanRecords.push_back(static_cast<int32_t>(span.style));
        }
    }

    // Pass 2: assemble. Header is int-aligned; text is byte data at the end, so
    // nothing needs padding.
    const size_t spanCount = spanRecords.size() / kSpanInts;
    const size_t intCount = kHeaderInts + lineRecords.size() + spanRecords.size();
    blob_.resize(intCount * sizeof(int32_t) + text.size());

    int32_t header[kHeaderInts];
    header[0] = kBlobMagic;
    header[1] = kBlobVersion;
    header[2] = static_cast<int32_t>(firstLine);
    header[3] = static_cast<int32_t>(count);
    header[4] = static_cast<int32_t>(spanCount);
    header[5] = static_cast<int32_t>(text.size());
    header[6] = static_cast<int32_t>(highlighter_->tier());
    header[7] = (highlighter_->hasGrammar() ? 1 : 0) | (dirty() ? 2 : 0) | (inMemory_ ? 4 : 0);

    char* out = blob_.data();
    std::memcpy(out, header, sizeof(header));
    out += sizeof(header);
    if (!lineRecords.empty()) {
        std::memcpy(out, lineRecords.data(), lineRecords.size() * sizeof(int32_t));
        out += lineRecords.size() * sizeof(int32_t);
    }
    if (!spanRecords.empty()) {
        std::memcpy(out, spanRecords.data(), spanRecords.size() * sizeof(int32_t));
        out += spanRecords.size() * sizeof(int32_t);
    }
    if (!text.empty()) {
        std::memcpy(out, text.data(), text.size());
    }
    return blob_;
}

// --- handle helpers --------------------------------------------------------

Engine* engineOf(jlong handle) { return reinterpret_cast<Engine*>(handle); }
Session* sessionOf(jlong handle) { return reinterpret_cast<Session*>(handle); }

ide::text::SearchOptions searchOptions(jint flags) {
    ide::text::SearchOptions options;
    options.caseSensitive = (flags & 1) != 0;
    options.wholeWord = (flags & 2) != 0;
    return options;
}

jint packArgb(ide::theme::Rgba colour) {
    return static_cast<jint>((static_cast<uint32_t>(colour.a) << 24) |
                             (static_cast<uint32_t>(colour.r) << 16) |
                             (static_cast<uint32_t>(colour.g) << 8) |
                             static_cast<uint32_t>(colour.b));
}

}  // namespace

// Every entry point is wrapped so a throw becomes a documented failure value.
#define COPE_GUARD_BEGIN try {
#define COPE_GUARD_END(fallback)                                       \
  }                                                                    \
  catch (...) {                                                        \
    __android_log_print(ANDROID_LOG_ERROR, kTag, "native call failed"); \
    return fallback;                                                   \
  }
#define COPE_GUARD_END_VOID                                            \
  }                                                                    \
  catch (...) {                                                        \
    __android_log_print(ANDROID_LOG_ERROR, kTag, "native call failed"); \
  }

extern "C" {

// --- engine ---------------------------------------------------------------

JNIEXPORT jlong JNICALL Java_dev_cope_ide_core_CopeNative_createEngine(JNIEnv* env, jclass,
                                                                      jobject assetManager,
                                                                      jstring grammarIndexTsv) {
    COPE_GUARD_BEGIN
    AAssetManager* assets =
        assetManager == nullptr ? nullptr : AAssetManager_fromJava(env, assetManager);
    auto engine = std::make_unique<Engine>(assets);
    const std::string tsv = toUtf8(env, grammarIndexTsv);
    engine->loadGrammarIndex(tsv);
    return reinterpret_cast<jlong>(engine.release());
    COPE_GUARD_END(0)
}

JNIEXPORT void JNICALL Java_dev_cope_ide_core_CopeNative_destroyEngine(JNIEnv*, jclass,
                                                                      jlong handle) {
    COPE_GUARD_BEGIN
    delete engineOf(handle);
    COPE_GUARD_END_VOID
}

JNIEXPORT jboolean JNICALL Java_dev_cope_ide_core_CopeNative_setTheme(JNIEnv* env, jclass,
                                                                     jlong handle, jstring json) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(handle);
    if (engine == nullptr) {
        return JNI_FALSE;
    }
    const std::string text = toUtf8(env, json);
    std::lock_guard<std::mutex> lock(engine->mutex());
    return engine->setThemeJson(text) ? JNI_TRUE : JNI_FALSE;
    COPE_GUARD_END(JNI_FALSE)
}

/// Palette: 3 ints per style — fgArgb, bgArgb, flags.
/// flags = fontStyle mask | hasFg<<8 | hasBg<<9.
JNIEXPORT jintArray JNICALL Java_dev_cope_ide_core_CopeNative_palette(JNIEnv* env, jclass,
                                                                     jlong handle) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(handle);
    if (engine == nullptr) {
        return toIntArray(env, {});
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    const std::span<const ide::theme::Style> palette = engine->theme().palette();
    std::vector<jint> out;
    out.reserve(palette.size() * 3u);
    for (const ide::theme::Style& style : palette) {
        out.push_back(packArgb(style.fg));
        out.push_back(packArgb(style.bg));
        out.push_back(static_cast<jint>(style.fontStyle) | (style.hasFg ? 0x100 : 0) |
                      (style.hasBg ? 0x200 : 0));
    }
    return toIntArray(env, out);
    COPE_GUARD_END(nullptr)
}

/// UI colours for newline-separated `keys`, ARGB per key, 0 when absent.
/// 0 is unambiguous: a fully transparent colour is never a usable chrome value,
/// so Kotlin treats it as "derive this one".
JNIEXPORT jintArray JNICALL Java_dev_cope_ide_core_CopeNative_uiColors(JNIEnv* env, jclass,
                                                                      jlong handle, jstring keys) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(handle);
    const std::string joined = toUtf8(env, keys);
    std::vector<jint> out;
    if (engine == nullptr) {
        return toIntArray(env, out);
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    size_t pos = 0;
    while (pos <= joined.size()) {
        size_t eol = joined.find('\n', pos);
        if (eol == std::string::npos) {
            eol = joined.size();
        }
        const std::string_view key(joined.data() + pos, eol - pos);
        if (!key.empty()) {
            const std::optional<ide::theme::Rgba> colour = engine->theme().uiColor(key);
            out.push_back(colour.has_value() ? packArgb(*colour) : 0);
        }
        if (eol == joined.size()) {
            break;
        }
        pos = eol + 1u;
    }
    return toIntArray(env, out);
    COPE_GUARD_END(nullptr)
}

/// "name \t isDark \t paletteSize"
JNIEXPORT jstring JNICALL Java_dev_cope_ide_core_CopeNative_themeInfo(JNIEnv* env, jclass,
                                                                     jlong handle) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(handle);
    if (engine == nullptr) {
        return toJavaString(env, "");
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    std::string out(engine->theme().displayName());
    out.push_back('\t');
    out += engine->theme().isDark() ? "1" : "0";
    out.push_back('\t');
    out += std::to_string(engine->theme().paletteSize());
    return toJavaString(env, out);
    COPE_GUARD_END(nullptr)
}

/// Whole asset file as bytes. Kotlin uses this for theme JSON so the asset
/// reader lives in one place.
JNIEXPORT jbyteArray JNICALL Java_dev_cope_ide_core_CopeNative_readAsset(JNIEnv* env, jclass,
                                                                        jlong handle,
                                                                        jstring path) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(handle);
    if (engine == nullptr) {
        return nullptr;
    }
    const std::string assetPath = toUtf8(env, path);
    std::lock_guard<std::mutex> lock(engine->mutex());
    const std::optional<std::string> data = engine->readAsset(assetPath);
    if (!data.has_value()) {
        return nullptr;
    }
    return toByteArray(env, *data);
    COPE_GUARD_END(nullptr)
}

/// Directory listing as TSV: name \t isDir \t sizeBytes, one entry per line.
/// One JNI call per directory: a tree with 400 entries is one call, not 400.
JNIEXPORT jstring JNICALL Java_dev_cope_ide_core_CopeNative_listDir(JNIEnv* env, jclass,
                                                                   jlong handle, jstring path) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(handle);
    if (engine == nullptr) {
        return toJavaString(env, "");
    }
    const std::string dir = toUtf8(env, path);
    std::lock_guard<std::mutex> lock(engine->mutex());
    std::vector<std::string> names = engine->fs().readDir(dir);
    std::sort(names.begin(), names.end());
    std::string out;
    out.reserve(names.size() * 24u);
    for (const std::string& name : names) {
        const std::optional<ide::host::FileInfo> info =
            engine->fs().stat(dir.empty() ? name : dir + "/" + name);
        out += name;
        out.push_back('\t');
        out += (info.has_value() && info->isDirectory) ? "1" : "0";
        out.push_back('\t');
        out += std::to_string(info.has_value() ? info->size : 0u);
        out.push_back('\n');
    }
    return toJavaString(env, out);
    COPE_GUARD_END(nullptr)
}

/// "exists \t isDir \t size"; exists is 0 or 1.
JNIEXPORT jstring JNICALL Java_dev_cope_ide_core_CopeNative_statPath(JNIEnv* env, jclass,
                                                                    jlong handle, jstring path) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(handle);
    if (engine == nullptr) {
        return toJavaString(env, "0\t0\t0");
    }
    const std::string target = toUtf8(env, path);
    std::lock_guard<std::mutex> lock(engine->mutex());
    const std::optional<ide::host::FileInfo> info = engine->fs().stat(target);
    std::string out;
    if (!info.has_value()) {
        out = "0\t0\t0";
    } else {
        out = "1\t";
        out += info->isDirectory ? "1" : "0";
        out.push_back('\t');
        out += std::to_string(info->size);
    }
    return toJavaString(env, out);
    COPE_GUARD_END(nullptr)
}

/// Grammar scope a file name would use, "" when nothing claims it.
JNIEXPORT jstring JNICALL Java_dev_cope_ide_core_CopeNative_scopeForFile(JNIEnv* env, jclass,
                                                                        jlong handle,
                                                                        jstring name) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(handle);
    if (engine == nullptr) {
        return toJavaString(env, "");
    }
    const std::string fileName = toUtf8(env, name);
    std::lock_guard<std::mutex> lock(engine->mutex());
    return toJavaString(env, engine->scopeForFile(fileName));
    COPE_GUARD_END(nullptr)
}

// --- sessions -------------------------------------------------------------

/// Opens a real filesystem path with zero copies (mmap through PosixHost).
JNIEXPORT jlong JNICALL Java_dev_cope_ide_core_CopeNative_openPath(JNIEnv* env, jclass,
                                                                  jlong handle, jstring path) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(handle);
    if (engine == nullptr) {
        return 0;
    }
    const std::string target = toUtf8(env, path);
    std::lock_guard<std::mutex> lock(engine->mutex());
    std::unique_ptr<ide::host::MappedFile> mapped = engine->fs().mapFile(target);
    if (mapped == nullptr) {
        return 0;
    }
    const size_t byteSize = mapped->size();
    const size_t slash = target.find_last_of('/');
    std::string name = slash == std::string::npos ? target : target.substr(slash + 1u);
    auto source = ide::host::MappedSource::make(std::move(mapped));
    auto session = std::make_unique<Session>(*engine, std::move(source), std::move(name), byteSize);
    session->setPath(target);
    return reinterpret_cast<jlong>(session.release());
    COPE_GUARD_END(0)
}

/// Opens from bytes (the SAF path, and new unsaved buffers).
JNIEXPORT jlong JNICALL Java_dev_cope_ide_core_CopeNative_openBytes(JNIEnv* env, jclass,
                                                                   jlong handle, jstring name,
                                                                   jbyteArray bytes) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(handle);
    if (engine == nullptr) {
        return 0;
    }
    const std::string fileName = toUtf8(env, name);
    std::string content;
    if (bytes != nullptr) {
        const jsize length = env->GetArrayLength(bytes);
        content.resize(static_cast<size_t>(length));
        if (length > 0) {
            env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte*>(content.data()));
        }
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    auto session = std::make_unique<Session>(*engine, std::move(content), fileName);
    return reinterpret_cast<jlong>(session.release());
    COPE_GUARD_END(0)
}

JNIEXPORT void JNICALL Java_dev_cope_ide_core_CopeNative_closeSession(JNIEnv*, jclass,
                                                                     jlong engineHandle,
                                                                     jlong sessionHandle) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    if (engine == nullptr) {
        // ~Session() unregisters itself from its Engine. Without a live engine
        // that is a use-after-free, so leaking the session is the correct
        // trade: it can only happen if Kotlin closed the engine first, which
        // its own API prevents.
        __android_log_print(ANDROID_LOG_WARN, kTag, "closeSession without an engine");
        return;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    delete sessionOf(sessionHandle);
    COPE_GUARD_END_VOID
}

/// [lineCount, byteSize, version, dirty, tier, hasGrammar, inMemory, canUndo, canRedo]
JNIEXPORT jlongArray JNICALL Java_dev_cope_ide_core_CopeNative_sessionInfo(JNIEnv* env, jclass,
                                                                          jlong engineHandle,
                                                                          jlong sessionHandle) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return toLongArray(env, {});
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    std::vector<jlong> out{
        static_cast<jlong>(session->doc().lineCount()),
        static_cast<jlong>(session->doc().size()),
        static_cast<jlong>(session->doc().version()),
        session->dirty() ? 1 : 0,
        static_cast<jlong>(session->highlighter().tier()),
        session->highlighter().hasGrammar() ? 1 : 0,
        session->inMemory() ? 1 : 0,
        session->doc().canUndo() ? 1 : 0,
        session->doc().canRedo() ? 1 : 0,
    };
    return toLongArray(env, out);
    COPE_GUARD_END(nullptr)
}

/// The viewport blob. See the layout comment at the top of this file.
///
/// LIFETIME: the returned buffer is owned by the session and reused by the next
/// call. Decode it before fetching again. Kotlin does exactly that in one place
/// (ViewportBlob.decode).
JNIEXPORT jobject JNICALL Java_dev_cope_ide_core_CopeNative_viewport(JNIEnv* env, jclass,
                                                                    jlong engineHandle,
                                                                    jlong sessionHandle,
                                                                    jint firstLine, jint count) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    const std::vector<char>& blob = session->viewport(firstLine, count);
    if (blob.empty()) {
        return nullptr;
    }
    return env->NewDirectByteBuffer(const_cast<char*>(blob.data()),
                                    static_cast<jlong>(blob.size()));
    COPE_GUARD_END(nullptr)
}

/// Raw bytes of one line, terminator excluded. Bytes, not a String: a code
/// editor must show a file that is not valid UTF-8.
JNIEXPORT jbyteArray JNICALL Java_dev_cope_ide_core_CopeNative_lineBytes(JNIEnv* env, jclass,
                                                                        jlong engineHandle,
                                                                        jlong sessionHandle,
                                                                        jint line) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    const ide::text::LineRange range = session->doc().lineAt(line);
    return toByteArray(env, session->doc().textRange(range.start, range.length()));
    COPE_GUARD_END(nullptr)
}

JNIEXPORT jbyteArray JNICALL Java_dev_cope_ide_core_CopeNative_textRange(JNIEnv* env, jclass,
                                                                        jlong engineHandle,
                                                                        jlong sessionHandle,
                                                                        jlong offset,
                                                                        jlong length) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr || offset < 0 || length < 0) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    return toByteArray(env, session->doc().textRange(static_cast<size_t>(offset),
                                                     static_cast<size_t>(length)));
    COPE_GUARD_END(nullptr)
}

// --- mutation -------------------------------------------------------------

JNIEXPORT jlong JNICALL Java_dev_cope_ide_core_CopeNative_insert(JNIEnv* env, jclass,
                                                                 jlong engineHandle,
                                                                 jlong sessionHandle, jlong offset,
                                                                 jstring text) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr || offset < 0) {
        return -1;
    }
    const std::string bytes = toUtf8(env, text);
    std::lock_guard<std::mutex> lock(engine->mutex());
    const size_t at = std::min(static_cast<size_t>(offset), session->doc().size());
    session->invalidateFrom(session->doc().lineColumnOf(at).line);
    session->doc().insert(at, bytes);
    return static_cast<jlong>(at + bytes.size());
    COPE_GUARD_END(-1)
}

JNIEXPORT jlong JNICALL Java_dev_cope_ide_core_CopeNative_erase(JNIEnv*, jclass,
                                                                jlong engineHandle,
                                                                jlong sessionHandle, jlong offset,
                                                                jlong length) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr || offset < 0 || length <= 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    const size_t at = std::min(static_cast<size_t>(offset), session->doc().size());
    session->invalidateFrom(session->doc().lineColumnOf(at).line);
    session->doc().erase(at, static_cast<size_t>(length));
    return static_cast<jlong>(at);
    COPE_GUARD_END(-1)
}

JNIEXPORT jlong JNICALL Java_dev_cope_ide_core_CopeNative_replaceRange(JNIEnv* env, jclass,
                                                                      jlong engineHandle,
                                                                      jlong sessionHandle,
                                                                      jlong offset, jlong length,
                                                                      jstring text) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr || offset < 0 || length < 0) {
        return -1;
    }
    const std::string bytes = toUtf8(env, text);
    std::lock_guard<std::mutex> lock(engine->mutex());
    const size_t at = std::min(static_cast<size_t>(offset), session->doc().size());
    session->invalidateFrom(session->doc().lineColumnOf(at).line);
    session->doc().replace(at, static_cast<size_t>(length), bytes);
    return static_cast<jlong>(at + bytes.size());
    COPE_GUARD_END(-1)
}

/// Undo/redo return the restored cursor offset, or -1 when there was nothing.
JNIEXPORT jlong JNICALL Java_dev_cope_ide_core_CopeNative_undo(JNIEnv*, jclass, jlong engineHandle,
                                                               jlong sessionHandle) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    if (!session->doc().undo()) {
        return -1;
    }
    session->invalidateFrom(0);
    return static_cast<jlong>(session->doc().cursor().offset);
    COPE_GUARD_END(-1)
}

JNIEXPORT jlong JNICALL Java_dev_cope_ide_core_CopeNative_redo(JNIEnv*, jclass, jlong engineHandle,
                                                               jlong sessionHandle) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    if (!session->doc().redo()) {
        return -1;
    }
    session->invalidateFrom(0);
    return static_cast<jlong>(session->doc().cursor().offset);
    COPE_GUARD_END(-1)
}

JNIEXPORT jboolean JNICALL Java_dev_cope_ide_core_CopeNative_save(JNIEnv* env, jclass,
                                                                 jlong engineHandle,
                                                                 jlong sessionHandle,
                                                                 jstring path) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return JNI_FALSE;
    }
    const std::string target = path == nullptr ? std::string() : toUtf8(env, path);
    std::lock_guard<std::mutex> lock(engine->mutex());
    const std::string& destination = target.empty() ? session->path() : target;
    if (destination.empty()) {
        return JNI_FALSE;
    }
    if (!engine->fs().writeFile(destination, session->doc().text())) {
        return JNI_FALSE;
    }
    session->setPath(destination);
    session->markSaved();
    return JNI_TRUE;
    COPE_GUARD_END(JNI_FALSE)
}

/// Whole document as bytes, for the SAF write path (Kotlin owns the stream).
JNIEXPORT jbyteArray JNICALL Java_dev_cope_ide_core_CopeNative_documentBytes(JNIEnv* env, jclass,
                                                                            jlong engineHandle,
                                                                            jlong sessionHandle) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    return toByteArray(env, session->doc().text());
    COPE_GUARD_END(nullptr)
}

JNIEXPORT void JNICALL Java_dev_cope_ide_core_CopeNative_markSaved(JNIEnv*, jclass,
                                                                   jlong engineHandle,
                                                                   jlong sessionHandle) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    session->markSaved();
    COPE_GUARD_END_VOID
}

// --- positions ------------------------------------------------------------

JNIEXPORT jlong JNICALL Java_dev_cope_ide_core_CopeNative_offsetOf(JNIEnv*, jclass,
                                                                   jlong engineHandle,
                                                                   jlong sessionHandle, jint line,
                                                                   jint byteColumn) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    return static_cast<jlong>(session->doc().offsetOf(line, byteColumn));
    COPE_GUARD_END(0)
}

/// [line, byteColumn]
JNIEXPORT jlongArray JNICALL Java_dev_cope_ide_core_CopeNative_lineColumnOf(JNIEnv* env, jclass,
                                                                           jlong engineHandle,
                                                                           jlong sessionHandle,
                                                                           jlong offset) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr || offset < 0) {
        return toLongArray(env, {0, 0});
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    const ide::text::Position position =
        session->doc().lineColumnOf(static_cast<size_t>(offset));
    return toLongArray(env, {static_cast<jlong>(position.line),
                             static_cast<jlong>(position.column)});
    COPE_GUARD_END(nullptr)
}

/// direction > 0: next codepoint boundary; <= 0: previous.
JNIEXPORT jlong JNICALL Java_dev_cope_ide_core_CopeNative_moveCodepoint(JNIEnv*, jclass,
                                                                        jlong engineHandle,
                                                                        jlong sessionHandle,
                                                                        jlong offset,
                                                                        jint direction) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr || offset < 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    const size_t at = static_cast<size_t>(offset);
    return static_cast<jlong>(direction > 0 ? session->doc().nextCodepoint(at)
                                            : session->doc().prevCodepoint(at));
    COPE_GUARD_END(0)
}

JNIEXPORT jint JNICALL Java_dev_cope_ide_core_CopeNative_displayColumnOf(JNIEnv*, jclass,
                                                                         jlong engineHandle,
                                                                         jlong sessionHandle,
                                                                         jlong offset,
                                                                         jint tabWidth) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr || offset < 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    return static_cast<jint>(
        session->doc().displayColumnOf(static_cast<size_t>(offset), tabWidth));
    COPE_GUARD_END(0)
}

JNIEXPORT jlong JNICALL Java_dev_cope_ide_core_CopeNative_offsetOfDisplayColumn(
    JNIEnv*, jclass, jlong engineHandle, jlong sessionHandle, jint line, jint displayColumn,
    jint tabWidth) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    return static_cast<jlong>(
        session->doc().offsetOfDisplayColumn(line, displayColumn, tabWidth));
    COPE_GUARD_END(0)
}

// --- search ---------------------------------------------------------------

/// [offset, length] or an empty array. `backwards != 0` searches before `from`.
JNIEXPORT jlongArray JNICALL Java_dev_cope_ide_core_CopeNative_find(JNIEnv* env, jclass,
                                                                   jlong engineHandle,
                                                                   jlong sessionHandle,
                                                                   jstring needle, jlong from,
                                                                   jint flags, jint backwards) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr || from < 0) {
        return toLongArray(env, {});
    }
    const std::string text = toUtf8(env, needle);
    std::lock_guard<std::mutex> lock(engine->mutex());
    const std::optional<ide::text::Match> hit =
        backwards != 0
            ? ide::text::findPrev(session->doc(), text, static_cast<size_t>(from),
                                  searchOptions(flags))
            : ide::text::findNext(session->doc(), text, static_cast<size_t>(from),
                                  searchOptions(flags));
    if (!hit.has_value()) {
        return toLongArray(env, {});
    }
    return toLongArray(env, {static_cast<jlong>(hit->offset), static_cast<jlong>(hit->length)});
    COPE_GUARD_END(nullptr)
}

/// Flat [offset, length] pairs, capped at `max` (0 = unlimited).
JNIEXPORT jlongArray JNICALL Java_dev_cope_ide_core_CopeNative_findAll(JNIEnv* env, jclass,
                                                                      jlong engineHandle,
                                                                      jlong sessionHandle,
                                                                      jstring needle, jint flags,
                                                                      jint max) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return toLongArray(env, {});
    }
    const std::string text = toUtf8(env, needle);
    std::lock_guard<std::mutex> lock(engine->mutex());
    const std::vector<ide::text::Match> hits =
        ide::text::findAll(session->doc(), text, searchOptions(flags),
                           max < 0 ? 0u : static_cast<size_t>(max));
    std::vector<jlong> out;
    out.reserve(hits.size() * 2u);
    for (const ide::text::Match& hit : hits) {
        out.push_back(static_cast<jlong>(hit.offset));
        out.push_back(static_cast<jlong>(hit.length));
    }
    return toLongArray(env, out);
    COPE_GUARD_END(nullptr)
}

/// Replacement count, or -1 when the span was too large and nothing changed.
JNIEXPORT jlong JNICALL Java_dev_cope_ide_core_CopeNative_replaceAll(JNIEnv* env, jclass,
                                                                     jlong engineHandle,
                                                                     jlong sessionHandle,
                                                                     jstring needle,
                                                                     jstring replacement,
                                                                     jint flags) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return 0;
    }
    const std::string from = toUtf8(env, needle);
    const std::string to = toUtf8(env, replacement);
    std::lock_guard<std::mutex> lock(engine->mutex());
    const ide::text::ReplaceResult result =
        ide::text::replaceAll(session->doc(), from, to, searchOptions(flags));
    if (result.tooLarge) {
        return -1;
    }
    if (result.replacements > 0) {
        session->invalidateFrom(0);
    }
    return static_cast<jlong>(result.replacements);
    COPE_GUARD_END(0)
}

// --- inspector ------------------------------------------------------------

/// The honest-limits surface: one formatted quality line plus the facts the
/// status strip shows. "tierName \t grammarScope \t refused \t unusable \t
/// coverage \t repairRatio \t report".
JNIEXPORT jstring JNICALL Java_dev_cope_ide_core_CopeNative_inspect(JNIEnv* env, jclass,
                                                                   jlong engineHandle,
                                                                   jlong sessionHandle) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return toJavaString(env, "");
    }
    std::lock_guard<std::mutex> lock(engine->mutex());

    const int64_t lines = std::min<int64_t>(session->doc().lineCount(), kQualityLineCap);
    std::vector<std::string> owned;
    owned.reserve(static_cast<size_t>(lines));
    for (int64_t i = 0; i < lines; ++i) {
        std::string text;
        session->lineText(i, text);
        owned.push_back(std::move(text));
    }
    const ide::highlight::QualityReport report =
        ide::highlight::analyzeDocument(session->highlighter(), owned);

    // Checkpoints were built against the highlighter's own state machine; the
    // analysis restarted it from line 0, so drop them.
    session->invalidateFrom(0);

    std::string out;
    out += ide::highlight::tierName(session->highlighter().tier());
    out.push_back('\t');
    out += session->highlighter().grammarScope();
    out.push_back('\t');
    out += std::to_string(report.refusedPatterns);
    out.push_back('\t');
    out += std::to_string(report.unusableRules);
    out.push_back('\t');
    out += std::to_string(static_cast<int>(report.coverage() * 1000.0 + 0.5));
    out.push_back('\t');
    out += std::to_string(static_cast<int>(report.repairRatio() * 1000.0 + 0.5));
    out.push_back('\t');
    out += ide::highlight::formatQualityReport(report);
    return toJavaString(env, out);
    COPE_GUARD_END(nullptr)
}

/// Scope stack at (line, byteColumn), innermost last, newline separated. This is
/// trace.h surfaced as "Show scopes" — the tool that would have saved attempts
/// 1 through 5.
JNIEXPORT jstring JNICALL Java_dev_cope_ide_core_CopeNative_scopesAt(JNIEnv* env, jclass,
                                                                    jlong engineHandle,
                                                                    jlong sessionHandle, jint line,
                                                                    jint byteColumn) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return toJavaString(env, "");
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    std::string text;
    session->lineText(line, text);
    ide::highlight::LineState state = session->stateBefore(line);
    std::vector<ide::highlight::ScopedSpan> spans;
    session->highlighter().scopeLine(text, state, spans);

    const size_t column = byteColumn < 0 ? 0u : static_cast<size_t>(byteColumn);
    for (const ide::highlight::ScopedSpan& span : spans) {
        if (column >= span.begin && column < span.end) {
            return toJavaString(
                env, session->highlighter().scopeTable().flatten(span.scopes, '\n'));
        }
    }
    return toJavaString(env, "");
    COPE_GUARD_END(nullptr)
}

/// Forces a tier: 0 = automatic, 1 = grammar, 2 = fallback, 3 = plain.
JNIEXPORT void JNICALL Java_dev_cope_ide_core_CopeNative_forceTier(JNIEnv*, jclass,
                                                                   jlong engineHandle,
                                                                   jlong sessionHandle, jint tier) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    switch (tier) {
        case 1:
            session->highlighter().forceTier(ide::highlight::Tier::kGrammar);
            break;
        case 2:
            session->highlighter().forceTier(ide::highlight::Tier::kFallback);
            break;
        case 3:
            session->highlighter().forceTier(ide::highlight::Tier::kPlain);
            break;
        default:
            break;  // 0 == leave the probe's decision alone
    }
    session->invalidateFrom(0);
    COPE_GUARD_END_VOID
}

// --- markdown -------------------------------------------------------------

/// The document parsed as markdown and flattened to the preview stream.
/// Format is documented in md_stream.h / MarkdownStream.kt.
JNIEXPORT jbyteArray JNICALL Java_dev_cope_ide_core_CopeNative_markdownStream(JNIEnv* env, jclass,
                                                                             jlong engineHandle,
                                                                             jlong sessionHandle) {
    COPE_GUARD_BEGIN
    Engine* engine = engineOf(engineHandle);
    Session* session = sessionOf(sessionHandle);
    if (engine == nullptr || session == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(engine->mutex());
    const std::string source = session->doc().text();
    const ide::render::Doc parsed = ide::render::parse(source);
    return toByteArray(env, cope::md::streamOf(parsed));
    COPE_GUARD_END(nullptr)
}

}  // extern "C"
