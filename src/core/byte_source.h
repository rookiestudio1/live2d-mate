#pragma once

// 隨機存取的位元組來源。
//
// 這是 zip 讀取的「自訂 IO」介面，直接對應 miniz 的 m_pRead / m_pIO_opaque 回呼
// （見 core/zip_archive.h）。之所以是**隨機存取**而不是單向串流：zip 的中央目錄
// 放在檔案尾端，不先 seek 到尾巴就不知道裡面有什麼，單向串流天生解不了 zip。
//
// 目前只有 FileByteSource。未來要支援「HTTP 下載的模型」時，只要再寫一個用
// Range: bytes=off-(off+n-1) 實作 read() 的來源即可，上面三層（ZipArchive /
// ModelAssets / ModelController）一行都不用動。
//
// 執行緒：實作一律**不保證**執行緒安全，而且 read() 是**阻塞**的。
// HTTP 版會真的等網路，所以呼叫端要自己負責別在 GUI 執行緒上跑
// （現行的 FileByteSource 只讀本機磁碟，在 GUI 執行緒用是可以的）。

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace l2m {

class ByteSource {
public:
  virtual ~ByteSource() = default;

  // 整體大小（位元組）
  virtual std::uint64_t size() const = 0;

  // 從 offset 讀最多 n 個位元組到 out，回傳實際讀到的量。
  // 讀到結尾會短讀，錯誤回 0 —— 兩者都不是例外，由呼叫端判斷。
  virtual std::size_t read(std::uint64_t offset, void* out, std::size_t n) = 0;
};

// 開啟本機檔案。開不起來（不存在、沒權限）回 nullptr。
//
// 注意：回傳的物件在存活期間會一直開著檔案，Windows 上那個檔案就刪不掉、改不了名。
// 模型載入中的 zip 因此是「鎖住」的狀態，要換掉模型才放得開。
std::unique_ptr<ByteSource> openFileByteSource(const std::filesystem::path& path);

}  // namespace l2m
