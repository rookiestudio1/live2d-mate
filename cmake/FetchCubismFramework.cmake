# CubismNativeFramework（Live2D Open Software License，可公開使用）。
#
# 以 FetchContent 從官方 GitHub 取得並釘在與 Core 相同的版本，
# 選 OpenGL renderer，並把 Core、GLEW 的相依接上（Framework 的 CMake
# 把這些留給使用端自行連接）。
#
# 提供 target：Framework（含 CSM_TARGET_*_GL 與 GLEW 設定）

include(FetchContent)

# 釘住的版本。同時餵給主程式當 L2M_CUBISM_VERSION（「關於」分頁要顯示），
# 升版時只要改這一行，畫面上的數字不會跟著失真。
set(L2M_CUBISM_VERSION "5-r.5")

FetchContent_Declare(CubismNativeFramework
  GIT_REPOSITORY https://github.com/Live2D/CubismNativeFramework.git
  GIT_TAG ${L2M_CUBISM_VERSION}
  GIT_SHALLOW TRUE
)

# Framework 的 src/Rendering/CMakeLists.txt 依此變數挑渲染後端
set(FRAMEWORK_SOURCE OpenGL)

FetchContent_MakeAvailable(CubismNativeFramework)

# ── 套用本專案對 Framework 的修改 ──
#
# 目的見 patches/ 底下各 patch 的檔頭。刻意不用 FetchContent_Declare 的
# PATCH_COMMAND：那個只在「首次下載」那一次跑，既有的建置樹（_deps 已經在了）
# 永遠套不到，改 patch 內容也不會重跑。這裡改成每次 configure 自己確認一次。
#
# 順序：先逐一試反向套用 —— 全部成功就代表已經是套好的狀態，直接跳過
# （不動 mtime，才不會每次 configure 都害 Framework 重編）。有任何一份沒套過
# 就先 git checkout 還原成原始狀態再全部重套。套不上一律 FATAL_ERROR：
# 升 tag 之後上游若改了這一塊，要當場知道，而不是默默退回那條慢路徑。
find_package(Git REQUIRED)

set(L2M_FRAMEWORK_PATCHES
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/cubism-framework-lazy-blend-shaders.patch"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/cubism-framework-wind-mask.patch"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/cubism-framework-part-opacity.patch")

set(l2m_patches_all_applied TRUE)
foreach(l2m_patch IN LISTS L2M_FRAMEWORK_PATCHES)
  # 改 patch 內容要能觸發重新 configure
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${l2m_patch}")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${l2m_patch}"
    WORKING_DIRECTORY "${cubismnativeframework_SOURCE_DIR}"
    RESULT_VARIABLE l2m_patch_applied
    OUTPUT_QUIET ERROR_QUIET)
  if(NOT l2m_patch_applied EQUAL 0)
    set(l2m_patches_all_applied FALSE)
  endif()
endforeach()

if(l2m_patches_all_applied)
  message(STATUS "CubismNativeFramework patch：全部已套用，略過")
else()
  # 還原成原始狀態再全部重套，重複執行才安全（_deps 是抓下來的相依，可以放心覆蓋）
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" checkout -- .
    WORKING_DIRECTORY "${cubismnativeframework_SOURCE_DIR}"
    RESULT_VARIABLE l2m_patch_reset)
  if(NOT l2m_patch_reset EQUAL 0)
    message(FATAL_ERROR
      "無法把 CubismNativeFramework 還原成原始狀態：${cubismnativeframework_SOURCE_DIR}")
  endif()

  foreach(l2m_patch IN LISTS L2M_FRAMEWORK_PATCHES)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" apply "${l2m_patch}"
      WORKING_DIRECTORY "${cubismnativeframework_SOURCE_DIR}"
      RESULT_VARIABLE l2m_patch_result
      ERROR_VARIABLE l2m_patch_error)
    if(NOT l2m_patch_result EQUAL 0)
      message(FATAL_ERROR
        "CubismNativeFramework patch 套用失敗（通常是 GIT_TAG 升版後上游改了同一塊）：\n"
        "  patch : ${l2m_patch}\n"
        "  目標  : ${cubismnativeframework_SOURCE_DIR}\n"
        "  git   : ${l2m_patch_error}")
    endif()
  endforeach()
  message(STATUS "CubismNativeFramework patch：已套用")
endif()

# 平台巨集：Framework 的 GL renderer 靠它決定要 include 哪組 GL 標頭
if(WIN32)
  target_compile_definitions(Framework PUBLIC CSM_TARGET_WIN_GL)
elseif(APPLE)
  target_compile_definitions(Framework PUBLIC CSM_TARGET_MAC_GL)
elseif(UNIX)
  target_compile_definitions(Framework PUBLIC CSM_TARGET_LINUX_GL)
endif()

# Windows / macOS 桌面 GL 都要 GLEW（靜態連結）
target_compile_definitions(Framework PUBLIC GLEW_STATIC)
target_link_libraries(Framework PUBLIC Live2DCubismCore glew)
target_include_directories(Framework PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CubismCore/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glew/include")
