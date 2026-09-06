#include "console.h"

#include <windows.h>

#include <cstdio>

namespace l2m {
namespace platform {

void attachParentConsole() {
  // 已經有有效的 stderr 就不要動它 —— 那可能是 Qt Creator 的 pipe，
  // 也可能是父行程傳下來的 console handle，兩種都已經通了（理由見標頭）
  const HANDLE existing = ::GetStdHandle(STD_ERROR_HANDLE);
  if (existing != nullptr && existing != INVALID_HANDLE_VALUE) return;

  // ATTACH_PARENT_PROCESS 失敗就是「不是從終端機啟動的」，什麼都不做。
  // 刻意不退回 AllocConsole：那會在雙擊啟動時多開一個黑視窗
  if (!::AttachConsole(ATTACH_PARENT_PROCESS)) return;

  FILE* stream = nullptr;
  ::freopen_s(&stream, "CONOUT$", "w", stderr);
}

}  // namespace platform
}  // namespace l2m
