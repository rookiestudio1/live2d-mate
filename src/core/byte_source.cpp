#include "byte_source.h"

#include <fstream>

namespace l2m {

namespace {

class FileByteSource : public ByteSource {
public:
  // 用 std::ifstream 而不是 fopen：它吃 std::filesystem::path，Windows 上會走
  // wide 版本開檔，非 ASCII 路徑不會壞（同 jsonu::readFileUtf8 的理由）。
  explicit FileByteSource(const std::filesystem::path& path) : file_(path, std::ios::binary | std::ios::ate) {
    if (!file_) return;
    const std::streamoff end = file_.tellg();
    if (end < 0) {
      file_.close();
      return;
    }
    size_ = static_cast<std::uint64_t>(end);
  }

  bool valid() const { return file_.is_open(); }

  std::uint64_t size() const override { return size_; }

  std::size_t read(std::uint64_t offset, void* out, std::size_t n) override {
    if (!file_.is_open() || n == 0) return 0;
    if (offset >= size_) return 0;
    const std::uint64_t available = size_ - offset;
    const std::size_t want = available < n ? static_cast<std::size_t>(available) : n;

    // 上一次讀到結尾會把 eofbit 立起來，不清掉之後每一次 seekg 都會失敗
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file_) return 0;
    file_.read(static_cast<char*>(out), static_cast<std::streamsize>(want));
    const std::streamsize got = file_.gcount();
    return got > 0 ? static_cast<std::size_t>(got) : 0;
  }

private:
  mutable std::ifstream file_;
  std::uint64_t size_ = 0;
};

}  // namespace

std::unique_ptr<ByteSource> openFileByteSource(const std::filesystem::path& path) {
  auto source = std::make_unique<FileByteSource>(path);
  if (!source->valid()) return nullptr;
  return source;
}

}  // namespace l2m
