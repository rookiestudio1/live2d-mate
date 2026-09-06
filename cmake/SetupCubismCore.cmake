# Live2D Cubism Core（閉源）的匯入設定。
#
# Core 因專有授權不進 repo，放在 third_party/CubismCore/（.gitignore 已排除）。
# 取得方式見 docs/CUBISM_CORE_SETUP.md：從官方 Cubism SDK for Native 壓縮包
# 複製 include 與 lib 進來即可。
#
# 匯入後提供 target：Live2DCubismCore

set(CUBISM_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CubismCore")

if(NOT EXISTS "${CUBISM_CORE_DIR}/include/Live2DCubismCore.h")
  message(FATAL_ERROR
    "找不到 Live2D Cubism Core（${CUBISM_CORE_DIR}）。\n"
    "請依 docs/CUBISM_CORE_SETUP.md 的說明，自官方 Cubism SDK for Native "
    "壓縮包複製 Core/include 與 Core/lib 到 third_party/CubismCore/。")
endif()

# 散布 binary 時必須附上 Live2D 的授權全文（見 THIRD_PARTY_NOTICES.md 第 1 節）。
# 缺了不擋建置 —— 開發時沒有它一樣跑得動；但 cmake --install 打出來的包會少一份，
# 而那是到了發布當下才會發現的種類，所以在 configure 就先出一聲。
if(NOT EXISTS "${CUBISM_CORE_DIR}/LICENSE.md")
  message(WARNING
    "third_party/CubismCore/LICENSE.md 不存在。開發不受影響，但打包出來的 zip 會缺 "
    "Live2D 的授權全文；請依 docs/CUBISM_CORE_SETUP.md 第 2 步從 SDK 複製過來。")
endif()

add_library(Live2DCubismCore STATIC IMPORTED GLOBAL)
set_target_properties(Live2DCubismCore PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${CUBISM_CORE_DIR}/include")

if(WIN32)
  # MSVC：依 runtime（MD/MDd）選對應的 .lib；143 = VS2022 工具集
  set(_core_lib_dir "${CUBISM_CORE_DIR}/lib/windows/x86_64/143")
  set_target_properties(Live2DCubismCore PROPERTIES
    IMPORTED_LOCATION "${_core_lib_dir}/Live2DCubismCore_MD.lib"
    IMPORTED_LOCATION_DEBUG "${_core_lib_dir}/Live2DCubismCore_MDd.lib")
elseif(APPLE)
  # macOS：依目標架構選庫（官方分別提供 arm64 與 x86_64 的 .a）
  if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(_core_arch "x86_64")
  else()
    set(_core_arch "arm64")
  endif()
  set_target_properties(Live2DCubismCore PROPERTIES
    IMPORTED_LOCATION "${CUBISM_CORE_DIR}/lib/macos/${_core_arch}/libLive2DCubismCore.a")
elseif(UNIX)
  # Linux：官方 SDK 本來就附 x86_64 靜態庫（arm64 在 lib/experimental/ 底下，
  # 需要時再接）
  set_target_properties(Live2DCubismCore PROPERTIES
    IMPORTED_LOCATION "${CUBISM_CORE_DIR}/lib/linux/x86_64/libLive2DCubismCore.a")
else()
  message(FATAL_ERROR "此平台的 Cubism Core 尚未設定（僅支援 Windows / macOS / Linux）")
endif()
