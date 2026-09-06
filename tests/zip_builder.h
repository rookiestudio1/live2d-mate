#pragma once

// 測試用的 zip 產生器。
//
// 刻意在執行期現做 zip，而不是把 .zip 放進 tests/fixtures/ ——
// 那個目錄至今全是 git 追蹤得到 diff 的小 JSON，塞二進位檔進去等於放棄
// 「fixture 出了什麼問題用眼睛就看得出來」這件事。
//
// 用的是 miniz 的 writer（mz_zip_writer_init_heap），也就是 CMakeLists 為什麼
// 沒有定義 MINIZ_NO_ARCHIVE_WRITING_APIS —— 產品程式碼只讀不寫，寫入 API
// 純粹是留給這裡的。

#include <miniz.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

struct ZipEntry {
  // zip 內的路徑；以 '/' 結尾代表目錄項（用來驗證目錄項會被濾掉）。
  // 直接是原始位元組 —— 要模擬 Windows 檔案總管寫出的 Big5 檔名就塞 Big5 進來。
  std::string name;
  std::string content;
};

// 把條目寫成一個 zip 檔。
//
// markUtf8=false 時不設 general purpose bit 11（UTF-8 旗標），模擬 Windows
// 檔案總管右鍵「壓縮成 ZIP 檔」的產物 —— 它把檔名寫成系統 ANSI 碼頁的位元組
// 而且不標記。miniz 的 writer 預設會設旗標，要靠 MZ_ZIP_FLAG_ASCII_FILENAME 關掉。
inline bool writeZip(const std::filesystem::path& out, const std::vector<ZipEntry>& entries, bool markUtf8 = true, int level = MZ_DEFAULT_LEVEL) {
  mz_zip_archive zip{};
  if (!mz_zip_writer_init_heap(&zip, 0, 64 * 1024)) return false;

  const mz_uint flags = static_cast<mz_uint>(level) | (markUtf8 ? 0u : MZ_ZIP_FLAG_ASCII_FILENAME);

  bool ok = true;
  for (const auto& entry : entries) {
    if (!mz_zip_writer_add_mem(&zip, entry.name.c_str(), entry.content.data(), entry.content.size(), flags)) {
      ok = false;
      break;
    }
  }

  void* buffer = nullptr;
  size_t size = 0;
  if (ok) ok = mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size) != MZ_FALSE;
  if (ok) {
    std::ofstream file(out, std::ios::binary);
    file.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
    ok = file.good();
  }

  // buffer 的所有權在 archive 身上，end() 會一併釋放
  mz_zip_writer_end(&zip);
  return ok;
}

// 把一整個資料夾做成 zip。prefix 非空時會多包一層（模擬 Windows 檔案總管
// 右鍵「壓縮成 ZIP 檔」的結果，例如 prefix = "Hiyori/"）。
inline bool zipDirectory(const std::filesystem::path& dir, const std::filesystem::path& out, const std::string& prefix = "") {
  namespace fs = std::filesystem;
  std::vector<ZipEntry> entries;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    if (ec) return false;
    if (it->is_directory(ec)) continue;
    const fs::path rel = fs::relative(it->path(), dir, ec);
    if (ec) return false;
    std::ifstream file(it->path(), std::ios::binary);
    if (!file) return false;
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    entries.push_back({prefix + rel.generic_u8string(), std::move(content)});
  }
  return writeZip(out, entries);
}
