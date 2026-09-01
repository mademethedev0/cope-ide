#include <ide/text/byte_source.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ide::text {

OwnedByteSource::OwnedByteSource(std::string bytes) : bytes_(bytes.begin(), bytes.end()) {}

OwnedByteSource::OwnedByteSource(std::vector<char> bytes) : bytes_(std::move(bytes)) {}

const char* OwnedByteSource::data() const noexcept { return bytes_.data(); }

size_t OwnedByteSource::size() const noexcept { return bytes_.size(); }

std::shared_ptr<const ByteSource> OwnedByteSource::make(std::string bytes) {
  return std::make_shared<const OwnedByteSource>(std::move(bytes));
}

std::shared_ptr<const ByteSource> OwnedByteSource::make(std::vector<char> bytes) {
  return std::make_shared<const OwnedByteSource>(std::move(bytes));
}

MappedByteSource::MappedByteSource(const char* data, size_t size) noexcept
    : data_(data), size_(size) {}

const char* MappedByteSource::data() const noexcept { return data_; }

size_t MappedByteSource::size() const noexcept { return size_; }

}  // namespace ide::text
