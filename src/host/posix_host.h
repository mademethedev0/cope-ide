#pragma once

// Why this file exists
// -------------------
// The real-filesystem Host: open/read/fstat/opendir/mmap, nothing else. It is
// the concrete IO boundary behind ember_cli and the Linux tests, and the
// reference a future Android (SAF/JNI) Host must behave like.
//
// It lives in src/ (not include/ide/) on purpose: hosts are an implementation
// detail chosen at the application boundary, not part of the engine's API
// surface. Include it as <posix_host.h> (src/host is on ide_host's PUBLIC
// include path).
//
// Error model per host.h: no method throws. Failures are nullopt / false /
// empty vector / nullptr, and every descriptor is closed on every path.

#include <ide/host/host.h>

namespace ide::host {

/// POSIX Host. All methods are total: any syscall failure maps to the
/// documented empty result, never an exception.
class PosixHost final : public Host {
public:
    PosixHost() = default;

    // --- Host ----------------------------------------------------------------
    std::optional<std::string> readFile(std::string_view path) override;
    bool writeFile(std::string_view path, std::string_view data) override;
    std::vector<std::string> readDir(std::string_view path) override;
    std::optional<FileInfo> stat(std::string_view path) override;
    std::unique_ptr<MappedFile> mapFile(std::string_view path) override;
};

}  // namespace ide::host
