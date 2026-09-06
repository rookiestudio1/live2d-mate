#include "zip_archive.h"

#include <miniz.h>

#include <QLocale>
#include <QString>

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

#ifndef _WIN32
#include <iconv.h>
#endif

// 「哪些檔名算隱藏檔」與掃資料夾時是同一條規則，所以直接用同一支判定函式，
// 不在這裡再抄一份 —— 兩邊只要有一點不一樣，就是「解壓縮就好、壓起來就壞」。
#include "model_scanner.h"
#include "string_util.h"

namespace l2m {

namespace {

// 單一條目解出來的大小上限。這不是效能考量而是防呆：惡意（或壞掉的）zip 可以
// 宣告一個幾 GiB 的條目，照著配置下去就是當場 OOM。真實模型最大的是貼圖，
// 8192² 的 PNG 檔本身也就幾十 MiB。
constexpr std::uint64_t kMaxEntryBytes = 1ull << 30;  // 1 GiB

// ZIP 的 general purpose bit 11：設了代表檔名與註解是 UTF-8。
// miniz 只在 .c 裡定義這個常數，標頭沒導出，所以自己寫一份。
constexpr mz_uint16 kUtf8NameFlag = 1 << 11;

// miniz 的自訂 IO 回呼：把 m_pIO_opaque 當成 ByteSource 用
size_t zipReadCallback(void* opaque, mz_uint64 fileOfs, void* buf, size_t n) {
  auto* source = static_cast<ByteSource*>(opaque);
  return source->read(fileOfs, buf, n);
}

#ifndef _WIN32
// iconv 嚴格解碼：整段轉完、沒有任何非法序列才算成功，半吊子一律回 nullopt。
// 寬鬆模式在這裡是毒藥 —— 用錯碼頁「大致解得開」的機率很高，
// 錯的名字跟 U+FFFD 亂碼一樣查不到資源，還更難發現。
std::optional<std::string> iconvToUtf8(const std::string& raw, const char* codepage) {
  iconv_t cd = iconv_open("UTF-8", codepage);
  if (cd == reinterpret_cast<iconv_t>(-1)) return std::nullopt;
  std::string out(raw.size() * 4 + 8, '\0');
  char* src = const_cast<char*>(raw.data());
  size_t srcLeft = raw.size();
  char* dst = out.data();
  size_t dstLeft = out.size();
  const size_t rc = iconv(cd, &src, &srcLeft, &dst, &dstLeft);
  iconv_close(cd);
  if (rc == static_cast<size_t>(-1) || srcLeft != 0) return std::nullopt;
  out.resize(out.size() - dstLeft);
  return out;
}

// Windows 檔案總管寫進 zip 的是「來源機器的 ANSI 碼頁」。Linux/macOS 的 locale
// 幾乎都是 UTF-8，「本機 ANSI 碼頁」這個概念不存在，只能用 UI 語系推測來源
// 碼頁的優先順序 —— 這種 zip 十之八九來自使用者自己語系的 Windows 機器。
// 順序有意義：Big5 與 GBK 都是雙位元組且範圍大量重疊，用錯的那個常常也
// 「解得開」，所以自己語系的碼頁一定要排最前面。
std::vector<const char*> ansiCodepageCandidates() {
  const QLocale locale = QLocale::system();
  switch (locale.language()) {
    case QLocale::Chinese:
      if (locale.script() == QLocale::TraditionalChineseScript || locale.territory() == QLocale::Taiwan || locale.territory() == QLocale::HongKong || locale.territory() == QLocale::Macao) {
        return {"CP950", "CP936", "CP932", "CP949"};
      }
      return {"CP936", "CP950", "CP932", "CP949"};
    case QLocale::Japanese:
      return {"CP932", "CP936", "CP950", "CP949"};
    case QLocale::Korean:
      return {"CP949", "CP936", "CP932", "CP950"};
    default:
      return {"CP936", "CP950", "CP932", "CP949"};
  }
}
#endif

}  // namespace

std::string decodeZipEntryName(const std::string& raw, bool utf8Flag) {
  if (utf8Flag) return raw;
  // 沒設旗標但本來就是合法 UTF-8：直接沿用。**這一條不能省** ——
  // 再丟進 ANSI 解碼一次會變成雙重編碼的亂碼，而且純 ASCII 也算合法 UTF-8，
  // 所以絕大多數 zip 根本不會走到下面那條。
  if (strutil::isValidUtf8(raw)) return raw;

  // 剩下的只能當成「來源機器的 ANSI 碼頁」（正體中文 Windows 是 CP950／Big5）。
  // 規格說是 CP437，但真的照 CP437 解只會得到一串沒有人看得懂的西歐字元；
  // 會產生這種檔案的就是 Windows 的檔案總管，用它的碼頁解才對得回去。
#ifdef _WIN32
  // Windows：fromLocal8Bit 就是系統 ANSI 碼頁，與檔案總管寫入時對稱。
  // 對不上碼頁的位元組 Qt 會換成 U+FFFD —— 仍然是合法 UTF-8，往返查找也仍然一致。
  const QString text = QString::fromLocal8Bit(raw.data(), static_cast<qsizetype>(raw.size()));
  const QByteArray utf8 = text.toUtf8();
  return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
#else
  // Linux/macOS：fromLocal8Bit 是 UTF-8（上面已驗證過不是），只會得到一串
  // U+FFFD。改用 iconv 依 UI 語系嘗試常見的 Windows CJK 碼頁 ——
  // 實測「藿藿.zip」在 Windows 修好之後拿到 Linux 上就是這樣二次踩坑的
  // （資源全部查不到，模型載入成功但畫面空白）。
  for (const char* codepage : ansiCodepageCandidates()) {
    if (auto decoded = iconvToUtf8(raw, codepage)) return *decoded;
  }
  // 全部碼頁都解不動：退回逐位元組替換，至少保證合法 UTF-8、往返一致
  const QString text = QString::fromLocal8Bit(raw.data(), static_cast<qsizetype>(raw.size()));
  const QByteArray utf8 = text.toUtf8();
  return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
#endif
}

std::string normalizeZipEntry(const std::string& raw) {
  if (raw.empty()) return {};

  std::string path = raw;
  std::replace(path.begin(), path.end(), '\\', '/');

  // 去掉開頭的 "./"（可以有很多層）與 '/'（絕對路徑一律當成相對路徑處理）
  size_t begin = 0;
  while (begin < path.size()) {
    if (path[begin] == '/') {
      ++begin;
    } else if (path.compare(begin, 2, "./") == 0) {
      begin += 2;
    } else {
      break;
    }
  }
  path = path.substr(begin);

  // 目錄項（以 '/' 結尾）不收 —— 上層要的是檔案清單
  if (path.empty() || path.back() == '/') return {};

  // 逐段檢查：空段（"a//b"）與 "." / ".." 都不是合法的相對路徑。
  // 隱藏檔（以 '.' 開頭）在這裡是**合法**的，只是不列出 —— 見 isListableZipEntry。
  size_t start = 0;
  while (start <= path.size()) {
    const size_t slash = path.find('/', start);
    const std::string segment = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (segment.empty() || segment == "." || segment == "..") return {};
    if (slash == std::string::npos) break;
    start = slash + 1;
  }

  return path;
}

bool isListableZipEntry(const std::string& normalized) {
  if (normalized.empty()) return false;

  // macOS 打包出來的資源分叉目錄，裡面全是 ._ 開頭的假檔
  if (strutil::startsWithInsensitive(normalized, "__MACOSX/")) return false;

  // 隱藏檔不列出，對齊 model_scanner.cpp 掃資料夾時的 name[0] == '.' 略過規則。
  // 唯一的例外是最後一段剛好是 stem 空的資產檔名（".model3.json"）：那是真的模型檔，
  // 不放行的話同一隻模型會「資料夾開得起來、壓成 zip 就說不是模型包」。
  size_t start = 0;
  while (start <= normalized.size()) {
    const size_t slash = normalized.find('/', start);
    const bool last = slash == std::string::npos;
    if (normalized[start] == '.' && !(last && isEmptyStemAssetName(normalized.substr(start)))) return false;
    if (last) break;
    start = slash + 1;
  }
  return true;
}

struct ZipArchive::Impl {
  std::unique_ptr<ByteSource> source;
  // miniz 的 API 全部吃非 const 指標（內部有共用解壓縮緩衝），所以 mutable。
  // 這也正是這個類別不能跨執行緒共用的原因。
  mutable mz_zip_archive zip{};
  bool opened = false;
  // 小寫的正規化路徑 → zip 條目索引
  std::map<std::string, mz_uint> index;

  ~Impl() {
    if (opened) mz_zip_reader_end(&zip);
  }
};

ZipArchive::ZipArchive() : impl_(std::make_unique<Impl>()) {}
ZipArchive::~ZipArchive() = default;

std::unique_ptr<ZipArchive> ZipArchive::open(std::unique_ptr<ByteSource> source) {
  if (!source) return nullptr;
  const std::uint64_t total = source->size();
  // 最小的合法 zip（空壓縮檔）就是 22 個位元組的 end-of-central-directory
  if (total < 22) return nullptr;

  std::unique_ptr<ZipArchive> archive(new ZipArchive());
  Impl& impl = *archive->impl_;
  impl.source = std::move(source);

  std::memset(&impl.zip, 0, sizeof(impl.zip));
  impl.zip.m_pRead = &zipReadCallback;
  impl.zip.m_pIO_opaque = impl.source.get();
  if (!mz_zip_reader_init(&impl.zip, total, 0)) return nullptr;
  impl.opened = true;

  // 先蒐集（正規化名稱, 條目索引），排序後才建清單與索引 ——
  // zip 的中央目錄順序不保證，排序過行為才穩定（同 model_scanner 的作法）。
  // 索引收全部（讀得到隱藏檔），entries_ 只收該列出的（見 isListableZipEntry）。
  std::vector<std::pair<std::string, mz_uint>> collected;
  const mz_uint count = mz_zip_reader_get_num_files(&impl.zip);
  collected.reserve(count);
  for (mz_uint i = 0; i < count; ++i) {
    // file_stat 一次拿到名字、目錄旗標與 general purpose bit flag（都在記憶體裡的
    // 中央目錄，不會回頭讀檔）
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&impl.zip, i, &stat)) continue;
    if (stat.m_is_directory) continue;
    // m_filename 是固定 512 bytes，太長的名字會被 miniz 靜靜截斷 ——
    // 截斷過的名字拿去查找一定對不上，寧可整條不收
    if (std::strlen(stat.m_filename) >= sizeof(stat.m_filename) - 1) continue;

    const std::string decoded = decodeZipEntryName(stat.m_filename, (stat.m_bit_flag & kUtf8NameFlag) != 0);
    const std::string normalized = normalizeZipEntry(decoded);
    if (normalized.empty()) continue;
    collected.push_back({normalized, i});
  }
  std::sort(collected.begin(), collected.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

  archive->entries_.reserve(collected.size());
  for (const auto& [path, fileIndex] : collected) {
    if (isListableZipEntry(path)) archive->entries_.push_back(path);
    // 只有大小寫不同的兩個條目：排序後的第一個勝出，行為才可預期
    impl.index.emplace(strutil::toLowerAscii(path), fileIndex);
  }

  return archive;
}

std::optional<unsigned int> ZipArchive::locate(const std::string& path) const {
  const std::string normalized = normalizeZipEntry(path);
  if (normalized.empty()) return std::nullopt;
  const auto it = impl_->index.find(strutil::toLowerAscii(normalized));
  if (it == impl_->index.end()) return std::nullopt;
  return it->second;
}

bool ZipArchive::contains(const std::string& path) const { return locate(path).has_value(); }

std::optional<std::string> ZipArchive::read(const std::string& path) const {
  const auto fileIndex = locate(path);
  if (!fileIndex) return std::nullopt;

  mz_zip_archive_file_stat stat{};
  if (!mz_zip_reader_file_stat(&impl_->zip, *fileIndex, &stat)) return std::nullopt;
  if (stat.m_uncomp_size > kMaxEntryBytes) return std::nullopt;

  std::string out(static_cast<std::size_t>(stat.m_uncomp_size), '\0');
  if (out.empty()) return out;  // 零位元組的條目是合法的
  if (!mz_zip_reader_extract_to_mem(&impl_->zip, *fileIndex, out.data(), out.size(), 0)) {
    return std::nullopt;
  }
  return out;
}

std::optional<std::string> ZipArchive::readPrefix(const std::string& path, std::size_t maxBytes) const {
  const auto fileIndex = locate(path);
  if (!fileIndex) return std::nullopt;
  if (maxBytes == 0) return std::string();

  mz_zip_reader_extract_iter_state* iter = mz_zip_reader_extract_iter_new(&impl_->zip, *fileIndex, 0);
  if (!iter) return std::nullopt;

  std::string out(maxBytes, '\0');
  const size_t got = mz_zip_reader_extract_iter_read(iter, out.data(), out.size());
  // 提早收掉 iterator 是刻意的：這裡本來就不打算讀完，
  // extract_iter_free 對「沒讀完」的狀態會回 MZ_FALSE（CRC 對不起來），不是錯誤。
  mz_zip_reader_extract_iter_free(iter);

  out.resize(got);
  return out;
}

}  // namespace l2m
