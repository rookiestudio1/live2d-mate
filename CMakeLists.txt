# Live2D Mate — Qt 6 / C++ 版
#
# 建置（Windows）：
#   cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# 三個開關：
#   -DL2M_BUILD_TESTS=OFF   不建 63 支測試
#   -DL2M_BUILD_APP=OFF     不建主程式（桌寵本體）
#   -DL2M_BUILD_VIEWER=OFF  不建 Live2D Viewer 子專案（src/viewer/）
#   —— APP 與 VIEWER 兩個都關掉時，整份建置**不需要 Live2D Cubism Core**，
#      變成只跑測試的建置（CI 用；理由見下方 option 的註解）
#
# 打包（Windows）：
#   cmake --install build --prefix dist/live2d_mate     # 便攜版的內容
#   cpack --config build/CPackConfig.cmake -B build/package   # NSIS 安裝檔
#   —— 平放的版面：exe 旁邊會有 i18n/、FrameworkShaders/、Qt DLL 與授權文件
#   —— **安裝檔不是壓上面那個 dist/**，是 cpack 自己重跑一遍 install 規則做的。
#      所以要出貨的東西一律加成 install() 規則；在 cmake --install 之後才複製
#      進 dist/ 的檔案只會進 zip，安裝檔裡靜靜地少一份。CPack 的設定在檔案最後。
cmake_minimum_required(VERSION 3.21)
project(
  live2d_mate
  VERSION 1.0.0
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# macOS 的平台實作（window_effects_mac.mm / autostart_mac.mm）是 Objective-C++。 .mm
# 要能編，OBJCXX 這個語言得先啟用；project() 的 LANGUAGES 沒辦法只在 APPLE 時加。
if(APPLE)
  enable_language(OBJCXX)
endif()

# MSVC：原始碼一律 UTF-8（含繁體中文註解與字串常值）
if(MSVC)
  add_compile_options(/utf-8)
endif()

# 測試（63 個，全部只連 l2m_core）。關掉可以省下一輪測試建置，也讓沒有 Qt Test
# 元件的環境仍然 configure 得過 —— 下面的 Qt find_package 是照這個 option 分兩次找的。
option(L2M_BUILD_TESTS "建置單元測試" ON)

# 主程式（桌寵本體）。OFF 時整份建置**完全不需要 Live2D Cubism Core**：
# 不 include SetupCubismCore、不抓 Framework、不建 l2m_live2d／l2m_media／live2d_mate，
# 只留 l2m_core 與那 63 支測試。
#
# 為什麼需要這個開關：Cubism Core 是閉源、不進 repo 的（docs/CUBISM_CORE_SETUP.md），
# 而 SetupCubismCore.cmake 缺檔時是 FATAL_ERROR —— 所以在 CI 那種乾淨環境裡，
# 連「只想跑單元測試」都會在 configure 當場死。但測試**全部只連 l2m_core**，
# 本來就一行 Cubism 都用不到；這個開關就是把那個既有事實變成建置設定。
# 順帶把 Qt 的必要元件縮到 Core/Network，CI 裝一份 qtbase 就夠（不必 WebSockets）。
option(L2M_BUILD_APP "建置桌寵主程式（需要 Live2D Cubism Core）" ON)

# Live2D Viewer 子專案（src/viewer/）：單一視窗的模型檢視器，
# 用來開一個 *.model3.json 或 *.zip 看看它長什麼樣、有哪些動作與表情。
# 與桌寵共用 l2m_core 與 l2m_live2d（同一個 ModelController、同一套內建動作
# 與名稱解析），所以同樣需要 Cubism Core；但完全不碰 l2m_media（沒有 TTS、
# 沒有音訊）、httplib 與 spdlog，所以連 Qt WebSockets 都不必找。
#
# **預設值刻意跟著 L2M_BUILD_APP 走，不是寫死 ON**：CI 的測試建置
#（.github/workflows/tests.yml）只給 -DL2M_BUILD_APP=OFF，它要的是那份
# 「完全不需要 Cubism Core」的建置。寫死 ON 的話 Viewer 會把 Cubism 又拉回來，
# CI 在 SetupCubismCore 的 FATAL_ERROR 當場死 —— 而且錯誤訊息跟這個 option
# 一點關係都看不出來。跟著 APP 走，那條命令列不必改也仍然成立。
option(L2M_BUILD_VIEWER "建置 Live2D Viewer 子專案（需要 Live2D Cubism Core）"
       ${L2M_BUILD_APP})

# 「要不要準備 Live2D」是這個變數而不是 L2M_BUILD_APP 本身 ——
# glew、Cubism Core、Framework 與 l2m_live2d 是兩個執行檔共用的相依。
if(L2M_BUILD_APP OR L2M_BUILD_VIEWER)
  set(L2M_NEEDS_LIVE2D ON)
else()
  set(L2M_NEEDS_LIVE2D OFF)
endif()

find_package(Qt6 COMPONENTS Core)
# Qt 6.8 之前，Qt 自家模組的 *Targets.cmake 在 INTERFACE_LINK_LIBRARIES 裡引用的
# 是無版本的 Qt::Core / Qt::Network（6.8 起才是 Qt6::*），這時關掉 versionless
# targets 會讓 qt_finalize_target 走訪相依時以「The Core target is mentioned as a
# dependency ... but not declared」FATAL_ERROR（實測 6.7.2 + WebSockets）。
# 所以只在 6.8+ 維持關閉。
if(Qt6_FOUND AND Qt6_VERSION VERSION_GREATER_EQUAL 6.8)
  set(QT_NO_CREATE_VERSIONLESS_TARGETS ON)
endif()
if(Qt6_FOUND)
  set(QT_VERSION_MAJOR 6)
else()
  set(QT_VERSION_MAJOR 5)
endif()
if(L2M_BUILD_APP)
  set(L2M_QT_COMPONENTS Widgets OpenGL Network WebSockets)
elseif(L2M_NEEDS_LIVE2D)
  # 只建 Viewer：WebSockets 是 Edge TTS 專用的（l2m_media），這條路上沒有
  set(L2M_QT_COMPONENTS Widgets OpenGL Network)
else()
  # l2m_core 只要 Core（yyjson/miniz/設定）與 Network（mcp_host 的 QNetworkInterface）
  set(L2M_QT_COMPONENTS Core Network)
endif()
# Viewer 的畫布是 QOpenGLWidget。Qt 6 把它從 QtOpenGL 拆成獨立的
# OpenGLWidgets 模組（桌寵的 QOpenGLWindow 留在 QtOpenGL），所以要另外找 ——
# 少了它的症狀是 C1083「找不到 QOpenGLWidget」。
if(L2M_BUILD_VIEWER)
  # Svg 是為了工具列那幾個 *.svg 圖示：QIcon 要靠 QtSvg 帶進來的 svg image
  # plugin 才畫得出來，少了它 QIcon 會安靜地變成空圖示（不會有任何錯誤訊息）。
  list(APPEND L2M_QT_COMPONENTS OpenGLWidgets Svg)
endif()
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS ${L2M_QT_COMPONENTS})

# ---------------------------------------------------------------------------
# 第三方：yyjson（C 檔直接編進 core）
# ---------------------------------------------------------------------------
add_library(yyjson STATIC third_party/yyjson/yyjson.c)
target_include_directories(yyjson PUBLIC third_party/yyjson)

# ---------------------------------------------------------------------------
# 第三方：miniz 3.0.2（zip 讀取；模型可以直接是壓縮檔）
#
# 只用 mz_zip_reader_* 一組 API。刻意關掉 stdio —— 讀檔一律走 core/byte_source.h
# 的自訂 IO 回呼（m_pRead / m_pIO_opaque）：一來 miniz 的 file API 吃 char* 路徑，
# Windows 上非 ASCII 路徑會壞（同 core/json_doc.h 不用 yyjson_read_file 的理由），
# 二來未來換成 HTTP Range 來源時這一層完全不用動。
# 寫入 API 刻意保留：測試要在 QTemporaryDir 裡現做 zip fixture，
# 才不必把二進位檔放進 repo（tests/fixtures 全是小 JSON）。
# ---------------------------------------------------------------------------
add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
target_compile_definitions(miniz PUBLIC MINIZ_NO_STDIO
                                        MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# GLEW 與 Live2D 兩件是「凡是要渲染模型的執行檔」都要的（桌寵與 Viewer）；
# l2m_core 與測試一個都不碰，所以掛在 L2M_NEEDS_LIVE2D 底下
# （yyjson 與 miniz 在上面，那兩個 l2m_core 要用）。
if(L2M_NEEDS_LIVE2D)
  # ---------------------------------------------------------------------------
  # 第三方：GLEW（靜態；CubismNativeFramework 的桌面 GL renderer 需要）
  # ---------------------------------------------------------------------------
  add_library(glew STATIC third_party/glew/src/glew.c)
  target_include_directories(glew PUBLIC third_party/glew/include)
  target_compile_definitions(glew PUBLIC GLEW_STATIC)
  if(WIN32)
    target_link_libraries(glew PUBLIC opengl32)
  elseif(APPLE)
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
    target_link_libraries(glew PUBLIC ${OPENGL_FRAMEWORK})
  else()
    # Linux：glew 的靜態建置預設含 GLX 支援，連結時需要 GL 與 X11 的符號
    target_link_libraries(glew PUBLIC GL X11)
  endif()

  # ---------------------------------------------------------------------------
  # Live2D：Cubism Core（閉源，need 手動放置）+ CubismNativeFramework（FetchContent）
  # ---------------------------------------------------------------------------
  include(cmake/SetupCubismCore.cmake)
  include(cmake/FetchCubismFramework.cmake)

  # ---------------------------------------------------------------------------
  # 第三方：zlib-ng + libspng 0.7.4（貼圖的 PNG 解碼；BSD-2）
  #
  # 為什麼不用 Qt 內建的 PNG handler、為什麼真正的瓶頸是 inflate 而不是反濾波、
  # 以及指令集為什麼壓到 SSE2，全部寫在 cmake/FetchZlibNg.cmake 的檔頭。
  # 掛在 L2M_NEEDS_LIVE2D 底下是刻意的：CI 的測試建置（L2M_BUILD_APP=OFF）
  # 因此完全不會去抓 zlib-ng，那條路上連一張貼圖都不會解。
  # ---------------------------------------------------------------------------
  include(cmake/FetchZlibNg.cmake)
  add_library(spng STATIC third_party/spng/spng.c)
  target_include_directories(spng PUBLIC third_party/spng)
  # SPNG_STATIC 必須是 PUBLIC：spng.h 沒看到它就會把整組 API 宣告成
  # __declspec(dllimport)，連結時變成一串 LNK4217 警告與多餘的 thunk。
  target_compile_definitions(spng PUBLIC SPNG_STATIC)
  # 帶命名空間的那個名字是無條件建立的；裸的 zlibstatic 掛在 zlib-ng 的
  # option(ZLIB_ALIASES) 底下，關掉就會炸（理由同 cmake/FetchZlibNg.cmake 檔頭）
  target_link_libraries(spng PUBLIC zlib-ng::zlibstatic)
endif()

# ---------------------------------------------------------------------------
# windeployqt 的 install 規則。桌寵與 Viewer 各要一份 —— Viewer 多連了
# Qt6OpenGLWidgets，那顆 DLL 不在桌寵的相依裡，只跑一次的話安裝版的 Viewer
# 雙擊會完全沒反應（缺 DLL 的載入失敗不留任何訊息）。
#
# 抽成函式而不是複製兩份，是因為中間那句 QT_DEPLOY_BIN_DIR 是**載重**的：
# 它預設是 "bin"（Qt 的 QtDeploySupport.cmake 只在該變數沒定義時才給預設值），
# 而本專案的執行檔裝在安裝根目錄 —— 不改的話 DLL 會全部掉進 bin/，打包出來的
# 程式連起都起不來，而且 install 全程一聲不吭。兩份拷貝的話，日後要再加別的
# QT_DEPLOY_* 一定會漏掉其中一支，而漏掉的症狀正好是這種沉默的壞法。
#
# 產出的是「安裝時才執行」的腳本，所以 windeployqt 只在 cmake --install
#（與 cpack 重跑 install 規則）那一步跑，平常的開發建置完全不受影響。
# NO_TRANSLATIONS：本專案的 i18n 是自己的執行期 JSON，Qt 的 .qm 一個都用不到。
# ---------------------------------------------------------------------------
function(l2m_install_qt_deploy target)
  if(QT_VERSION_MAJOR LESS 6)
    return()
  endif()
  qt_generate_deploy_app_script(
    TARGET ${target}
    OUTPUT_SCRIPT l2m_deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
    NO_TRANSLATIONS)
  # 注意這裡不能用 set()：deploy script 是在 install 階段才跑的，變數得由
  # install(CODE) 送進那個 scope，configure 期的 set() 到不了。
  # （install() 在函式裡呼叫沒有特別待遇 —— 函式不開新的 directory scope，
  #   規則照樣登記在呼叫端的目錄，順序也就是呼叫的順序。）
  install(CODE "set(QT_DEPLOY_BIN_DIR \".\")")
  install(SCRIPT ${l2m_deploy_script})
endfunction()

# 以下三個第三方只有桌寵主程式會用到（MCP 伺服器、日誌、音訊）。
if(L2M_BUILD_APP)
  # ---------------------------------------------------------------------------
  # 第三方：cpp-httplib（header-only） 只有 MCP 的兩個 TU 會 include 它，編譯成本才不會擴散到整個專案。
  # ---------------------------------------------------------------------------
  add_library(httplib INTERFACE)
  target_include_directories(httplib INTERFACE third_party/httplib)
  if(WIN32)
    target_link_libraries(httplib INTERFACE ws2_32 crypt32)
  endif()

  # ---------------------------------------------------------------------------
  # 第三方：spdlog 1.15.3（header-only） 只有 src/app/logging.cpp 一個 TU 會 include 它 ——
  # bundled fmt 的編譯成本才不會擴散到 l2m_core 與 60 支測試（同 httplib 的理由）。
  # 刻意不用 spdlog 自帶的 CMakeLists：它在 CMAKE_BUILD_TYPE 為空時會 FORCE 成 Release，
  # 而且會被本專案的全域 AUTOMOC 掃過整棵原始碼樹。vendored 目錄也只留 include/ 與 LICENSE。
  # ---------------------------------------------------------------------------
  add_library(spdlog INTERFACE)
  target_include_directories(spdlog INTERFACE third_party/spdlog/include)

  # ---------------------------------------------------------------------------
  # 第三方：miniaudio（單一實作 TU，以 C 編譯） 只留解碼與播放：編碼／生成／資源管理／node graph／engine 全部關掉，
  # 省下大半的編譯時間，那些子系統這個 app 一個都用不到。
  # ---------------------------------------------------------------------------
  add_library(miniaudio STATIC src/media/miniaudio_impl.c)
  target_include_directories(miniaudio PUBLIC third_party/miniaudio)
  target_compile_definitions(
    miniaudio PUBLIC MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER
                     MA_NO_NODE_GRAPH MA_NO_ENGINE)
  if(UNIX AND NOT APPLE)
    # miniaudio 在 Linux 用 dlopen 動態載入 ALSA/PulseAudio，連結期只要這三個
    target_link_libraries(miniaudio PUBLIC dl pthread m)
  endif()
endif()

# ---------------------------------------------------------------------------
# l2m_core：純邏輯（不依賴 GUI / GL），所有單元測試唯一連結的對象
# ---------------------------------------------------------------------------
add_library(
  l2m_core STATIC
  src/core/bubble_placement.h
  src/core/bubble_placement.cpp
  src/core/bubble_shape.h
  src/core/bubble_shape.cpp
  src/core/bottom_align.h
  src/core/bottom_align.cpp
  src/core/canvas_center.h
  src/core/canvas_center.cpp
  src/core/ellipse_text_fit.h
  src/core/ellipse_text_fit.cpp
  src/core/bubble_font.h
  src/core/bubble_font.cpp
  src/core/echo_filter.h
  src/core/echo_filter.cpp
  src/core/parameter_tracks.h
  src/core/parameter_tracks.cpp
  src/core/drag_swing.h
  src/core/drag_swing.cpp
  src/core/motion_builder.h
  src/core/motion_builder.cpp
  src/core/json_doc.h
  src/core/json_doc.cpp
  src/core/byte_source.h
  src/core/byte_source.cpp
  src/core/zip_archive.h
  src/core/zip_archive.cpp
  src/core/model_assets.h
  src/core/model_assets.cpp
  src/core/texture_format.h
  src/core/texture_format.cpp
  src/core/string_util.h
  src/core/audio_stream_format.h
  src/core/audio_stream_format.cpp
  src/core/text_segments.h
  src/core/text_segments.cpp
  src/core/tts_segment_pipeline.h
  src/core/tts_segment_pipeline.cpp
  src/core/model_types.h
  src/core/display_info.h
  src/core/display_info.cpp
  src/core/model_settings.h
  src/core/model_settings.cpp
  src/core/motion_meta.h
  src/core/motion_meta.cpp
  src/core/motion_curve_ids.h
  src/core/motion_curve_ids.cpp
  src/core/idle_motion_pick.h
  src/core/idle_motion_pick.cpp
  src/core/annotations.h
  src/core/annotations.cpp
  src/core/builtin_actions.h
  src/core/builtin_actions.cpp
  src/core/model_scanner.h
  src/core/model_scanner.cpp
  src/core/external_model.h
  src/core/external_model.cpp
  src/core/viewer_settings.h
  src/core/viewer_settings.cpp
  src/core/config_schema.h
  src/core/config_schema.cpp
  src/core/config_store.h
  src/core/config_store.cpp
  src/core/config_patch.h
  src/core/config_patch.cpp
  src/core/persona.h
  src/core/persona.cpp
  src/core/persona_doc.h
  src/core/persona_doc.cpp
  src/core/persona_default.h
  src/core/persona_default.cpp
  src/core/layout_fit.h
  src/core/layout_fit.cpp
  src/core/png_probe.h
  src/core/png_probe.cpp
  src/core/presence_tracker.h
  src/core/presence_tracker.cpp
  src/core/idle_signal.h
  src/core/day_period.h
  src/core/day_period.cpp
  src/core/hit_area_semantics.h
  src/core/hit_area_semantics.cpp
  src/core/gesture_detector.h
  src/core/gesture_detector.cpp
  src/core/mood.h
  src/core/screen_rescue.h
  src/core/screen_rescue.cpp
  # 預設角色的五份 markdown。必須掛在 l2m_core 而不是 resources.qrc（那份掛在
  # live2d_mate 執行檔上，只連 l2m_core 的測試看不到）；AUTORCC 全域開著。
  resources/personas.qrc
  src/core/idle.h
  src/core/idle.cpp
  src/core/schedule_book.h
  src/core/schedule_book.cpp
  src/core/behavior_pick.h
  src/core/behavior_pick.cpp
  src/core/idle_director.h
  src/core/idle_director.cpp
  src/core/autonomy_gates.h
  src/core/autonomy_gates.cpp
  src/core/ambient_wind.h
  src/core/ambient_wind.cpp
  src/core/wind_targets.h
  src/core/wind_targets.cpp
  src/core/gaze_director.h
  src/core/gaze_director.cpp
  src/core/interaction_logic.h
  src/core/interaction_logic.cpp
  src/core/model_regions.h
  src/core/model_regions.cpp
  src/core/i18n.h
  src/core/i18n.cpp
  src/core/command_result.h
  src/core/model_commands.h
  src/core/model_commands.cpp
  src/core/tray_label.h
  src/core/tray_label.cpp
  src/core/settings_layout.h
  src/core/settings_layout.cpp
  src/core/qt_timer_host.h
  src/core/qt_timer_host.cpp
  src/core/perform_step.h
  src/core/perform_step.cpp
  src/core/tts_types.h
  src/core/voice_list.h
  src/core/voice_list.cpp
  src/core/tts_http.h
  src/core/tts_http.cpp
  src/core/tts_manager.h
  src/core/tts_manager.cpp
  src/core/edge_tts_protocol.h
  src/core/edge_tts_protocol.cpp
  src/core/happy_eyeballs.h
  src/core/happy_eyeballs.cpp
  src/core/jsonrpc.h
  src/core/jsonrpc.cpp
  src/core/log_category.h
  src/core/log_category.cpp
  src/core/log_rotation.h
  src/core/log_rotation.cpp
  src/core/mcp_host.h
  src/core/mcp_host.cpp
  src/core/mcp_snippets.h
  src/core/mcp_snippets.cpp
  src/core/mcp_tool_specs.h
  src/core/mcp_tool_specs.cpp
  src/core/mcp_resources.h
  src/core/mcp_resources.cpp
  src/core/llm_types.h
  src/core/sse_parser.h
  src/core/sse_parser.cpp
  src/core/llm_stream_accumulator.h
  src/core/llm_stream_accumulator.cpp
  src/core/llm_http.h
  src/core/llm_http.cpp
  src/core/llm_behavior_prompt.h
  src/core/llm_behavior_prompt.cpp
  src/core/persona_memory.h
  src/core/persona_memory.cpp
  src/core/persona_generate.h
  src/core/persona_generate.cpp
  src/core/weather_types.h
  src/core/weather_code.h
  src/core/weather_code.cpp
  src/core/weather_http.h
  src/core/weather_http.cpp
  src/core/weather_alert.h
  src/core/weather_alert.cpp
  src/core/autostart_command.h
  src/core/autostart_command.cpp
  src/core/topmost_watchdog.h
  src/core/topmost_watchdog.cpp
  src/core/splash_timing.h
  src/core/splash_timing.cpp
  src/core/fade_timing.h
  src/core/fade_timing.cpp
  src/core/frame_profiler.h
  src/core/crash_report.h
  src/core/crash_report.cpp)
target_include_directories(l2m_core PUBLIC src)
# Network：mcp_host 的 QNetworkInterface（列出可綁定的網卡位址）
target_link_libraries(l2m_core PUBLIC Qt${QT_VERSION_MAJOR}::Core
                                      Qt${QT_VERSION_MAJOR}::Network yyjson
                                      miniz)
if(APPLE)
  # zip 條目名的 ANSI 碼頁 fallback 走 iconv（zip_archive.cpp）；
  # glibc 內建 iconv 不用連，macOS 的在獨立的 libiconv 裡
  target_link_libraries(l2m_core PUBLIC iconv)
endif()

if(L2M_NEEDS_LIVE2D)
  # ---------------------------------------------------------------------------
  # l2m_live2d：CubismFramework + OpenGL 渲染（桌寵與 Viewer 共用）
  # ---------------------------------------------------------------------------
  add_library(
    l2m_live2d STATIC
    src/live2d/cubism_runtime.h
    src/live2d/cubism_runtime.cpp
    src/live2d/model_controller.h
    src/live2d/model_controller.cpp
    src/live2d/hit_mask.h
    src/live2d/hit_mask.cpp
    src/live2d/parameter_overlay.h
    src/live2d/parameter_overlay.cpp
    src/live2d/action_player.h
    src/live2d/action_player.cpp
    src/live2d/png_decoder.h
    src/live2d/png_decoder.cpp)
  target_link_libraries(l2m_live2d PUBLIC l2m_core Framework glew
                                          Qt${QT_VERSION_MAJOR}::Gui)
  # 貼圖的 PNG 快路徑（spng 自己把 zlib-ng 接進來）。PRIVATE：只有
  # png_decoder.cpp 認得 spng，上層一個 include 都不需要。
  target_link_libraries(l2m_live2d PRIVATE spng)
  # 開發環境直接指到 FetchContent 原始碼裡的 shader 目錄；部署時改放執行檔旁的 FrameworkShaders/
  target_compile_definitions(
    l2m_live2d
    PRIVATE
      L2M_CUBISM_SHADER_DIR="${cubismnativeframework_SOURCE_DIR}/src/Rendering/OpenGL/Shaders/Standard"
  )
endif()

if(L2M_BUILD_APP)
  # ---------------------------------------------------------------------------
  # l2m_media：音訊播放與 TTS 引擎（會碰網路、行程與音效裝置）
  # ---------------------------------------------------------------------------
  add_library(
    l2m_media STATIC
    src/media/audio_player.h
    src/media/audio_player.cpp
    src/media/mp3_push_decoder.h
    src/media/stream_decoders.h
    src/media/stream_decoders.cpp
    src/media/http_json.h
    src/media/http_json.cpp
    src/media/endpoint_prober.h
    src/media/endpoint_prober.cpp
    src/media/tts_engine_edge.h
    src/media/tts_engine_edge.cpp
    src/media/tts_engine_gptsovits.h
    src/media/tts_engine_gptsovits.cpp
    src/media/tts_engine_voicebox.h
    src/media/tts_engine_voicebox.cpp
    src/media/tts_engine_custom.h
    src/media/tts_engine_custom.cpp
    src/media/weather_service.h
    src/media/weather_service.cpp
    # 內建 LLM（src/llm/）：貼網路的引擎與非同步橋接。掛在 l2m_media 而不是
    # 開第五個 target —— HttpJson 在這裡，兩個 TU 不值得一個新函式庫。
    src/llm/llm_engine_http.h
    src/llm/llm_engine_http.cpp
    src/llm/llm_manager.h
    src/llm/llm_manager.cpp
    src/llm/llm_behavior_planner.h
    src/llm/llm_behavior_planner.cpp)
  target_link_libraries(
    l2m_media PUBLIC l2m_core miniaudio Qt${QT_VERSION_MAJOR}::Network
                     Qt${QT_VERSION_MAJOR}::WebSockets)
  # 標頭也必須跟著實作一起是 WIN32-only：AUTOMOC 看到列進來的 Q_OBJECT 標頭 就會產生 moc TU，裡面的 vtable 與
  # QMetaType 預設建構子會參照到不存在的 成員函式，在非 Windows 上變成一串 undefined symbol（實際踩過）。
  if(WIN32)
    target_sources(l2m_media PRIVATE src/media/tts_engine_sapi.h
                                     src/media/tts_engine_sapi_win.cpp)
  endif()

  # ---------------------------------------------------------------------------
  # 主程式
  # ---------------------------------------------------------------------------
  set(PROJECT_SOURCES
      src/app/main.cpp
      src/app/logging.h
      src/app/logging.cpp
      src/app/splash_process.h
      src/app/splash_process.cpp
      src/app/single_instance.h
      src/app/single_instance.cpp
      src/app/app_controller.h
      src/app/app_controller.cpp
      src/app/perform_runner.h
      src/app/perform_runner.cpp
      src/app/speech_controller.h
      src/app/speech_controller.cpp
      src/windows/character_window.h
      src/windows/character_window.cpp
      src/windows/window_manager.h
      src/windows/window_manager.cpp
      src/windows/tray.h
      src/windows/tray.cpp
      src/windows/bubble_window.h
      src/windows/bubble_window.cpp
      src/windows/splash_window.h
      src/windows/splash_window.cpp
      src/windows/settings/settings_context.h
      src/windows/settings/settings_window.h
      src/windows/settings/settings_window.cpp
      src/windows/settings/settings_window.ui
      src/windows/settings/general_page.h
      src/windows/settings/general_page.cpp
      src/windows/settings/general_page.ui
      src/windows/settings/model_page.h
      src/windows/settings/model_page.cpp
      src/windows/settings/model_page.ui
      src/windows/settings/persona_page.h
      src/windows/settings/persona_page.cpp
      src/windows/settings/persona_page.ui
      src/windows/settings/voice_page.h
      src/windows/settings/voice_page.cpp
      src/windows/settings/voice_page.ui
      src/windows/settings/mcp_page.h
      src/windows/settings/mcp_page.cpp
      src/windows/settings/mcp_page.ui
      src/windows/settings/llm_page.h
      src/windows/settings/llm_page.cpp
      src/windows/settings/llm_page.ui
      src/windows/settings/about_page.h
      src/windows/settings/about_page.cpp
      src/windows/settings/about_page.ui
      src/mcp/pending_call.h
      src/mcp/pending_call.cpp
      src/mcp/mcp_tools.h
      src/mcp/mcp_tools.cpp
      src/mcp/mcp_http_server.h
      src/mcp/mcp_http_server.cpp
      src/mcp/mcp_stdio.h
      src/mcp/mcp_stdio.cpp
      src/platform/window_effects.h
      src/platform/autostart.h
      src/platform/user_idle.h
      src/platform/console.h)

  if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(live2d_mate WIN32 MACOSX_BUNDLE ${PROJECT_SOURCES}
                      resources/resources.qrc)
  else()
    add_executable(live2d_mate WIN32 MACOSX_BUNDLE ${PROJECT_SOURCES}
                                                   resources/resources.qrc)
  endif()

  # 設定視窗的靜態骨架走 Qt Designer（src/windows/settings/*.ui）。
  # AUTOUIC 只開在這個 target 上：l2m_core 與 62 個測試沒有 .ui，
  # 開全域只是讓它們每次 configure 多跑一次 uic 掃描。
  # .ui 與 .cpp 同層放置，AUTOUIC 預設就找得到，不必設 AUTOUIC_SEARCH_PATHS。
  set_target_properties(live2d_mate PROPERTIES AUTOUIC ON)

  if(WIN32)
    # app.rc 只放一個 ICON 資源，給的是「執行檔在檔案總管／工作列釘選上的樣子」。
    # 視窗標題列與工作列按鈕的圖示是另一回事，走 main.cpp 的 setWindowIcon。
    target_sources(live2d_mate PRIVATE src/platform/window_effects_win.cpp
                                       src/platform/autostart_win.cpp
                                       src/platform/user_idle_win.cpp
                                       src/platform/console_win.cpp
                                       src/platform/crash_handler_win.cpp
                                       resources/app.rc)
    # dwmapi：window_effects_win.cpp 用 DwmGetWindowAttribute 問「這個視窗是不是
    # cloaked」（置頂看門狗的第二個症狀要靠它濾掉別的虛擬桌面上的視窗）。
    # user32/gdi32 那些在 MSVC 的預設函式庫清單裡，dwmapi 不在，一定要自己連。
    target_link_libraries(live2d_mate PRIVATE dwmapi)
  elseif(APPLE)
    target_sources(live2d_mate PRIVATE src/platform/window_effects_mac.mm
                                       src/platform/autostart_mac.mm
                                       src/platform/user_idle_mac.mm
                                       src/platform/console_mac.mm
                                       src/platform/crash_handler_mac.mm)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(SERVICEMANAGEMENT_FRAMEWORK ServiceManagement REQUIRED)
    target_link_libraries(live2d_mate PRIVATE ${APPKIT_FRAMEWORK}
                                              ${SERVICEMANAGEMENT_FRAMEWORK})
    # 開機自啟用的 launchd agent，必須放進 bundle 的 Contents/Library/LaunchAgents/，
    # SMAppService.agentServiceWithPlistName 才找得到（見 resources/macos/*.plist）
    set(l2m_login_agent
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/com.live2dmate.app.login.plist
    )
    target_sources(live2d_mate PRIVATE ${l2m_login_agent})
    set_source_files_properties(
      ${l2m_login_agent} PROPERTIES MACOSX_PACKAGE_LOCATION
                                    "Library/LaunchAgents")

    # SMAppService 認的是 bundle identifier，沒設會拿到 CMake 的 com.yourcompany.*
    # 樣板值，開機自啟註冊不起來。 Info.plist 用自己的樣板：預設樣板沒有 LSUIElement（純選單列 app）。
    set_target_properties(
      live2d_mate
      PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER com.live2dmate.app
                 MACOSX_BUNDLE_BUNDLE_NAME "Live2D Mate"
                 MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
                 MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
                 MACOSX_BUNDLE_INFO_PLIST
                 ${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Info.plist.in)

    # 建置後 ad-hoc 簽章。 不是可有可無的收尾：SMAppService.agentServiceWithPlistName 會驗證
    # Contents/Library/LaunchAgents/ 底下那份 plist 有沒有被 bundle 的簽章蓋住， 沒簽的 bundle
    # 註冊會直接失敗（實測：errSecCS -67056 "Codesigning failure loading plist"）。有趣的是
    # mainAppService 沒這個要求， 所以這條是換成 agent 之後才冒出來的。 出貨時應該換成真正的 Developer ID
    # 簽章；ad-hoc 只讓本機開發能用。
    add_custom_command(
      TARGET live2d_mate
      POST_BUILD
      COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:live2d_mate>
      COMMENT "ad-hoc 簽章（SMAppService 註冊 launchd agent 的前提）")
  else()
    # Linux：穿透點擊走 XShape 的 input region、置頂走 EWMH，都要直接下 Xlib，
    # 所以連 X11 與 Xext。Wayland session 下這些呼叫會拿不到 Display 而安靜退化
    # （見 window_effects_linux.cpp 檔頭）。
    target_sources(live2d_mate PRIVATE src/platform/window_effects_linux.cpp
                                       src/platform/autostart_linux.cpp
                                       src/platform/user_idle_linux.cpp
                                       src/platform/console_linux.cpp
                                       src/platform/crash_handler_linux.cpp)
    target_link_libraries(live2d_mate PRIVATE X11 Xext)
  endif()

  # QT_MESSAGELOGCONTEXT：讓 QMessageLogContext 帶著 file/line/function。
  # Qt 預設只在 Debug 建置填這些欄位，而 log 檔最需要來源位置的時機恰恰是
  # RelWithDebInfo 的現場回報（見 src/app/logging.cpp 的 qtMessageHandler）。
  #
  # L2M_DEFAULT_LOG_LEVEL：沒設 L2M_LOG_LEVEL 環境變數時的預設日誌等級。
  # **全專案唯一一個看建置型別的編譯定義** —— 上面那個 QT_MESSAGELOGCONTEXT 就是刻意
  # 無條件加的，這裡破例是因為「日誌要多吵」本來就跟「這份建置給誰用」綁在一起：
  # 開發時要看逐幀（[perf]）、逐句（[tts] 句段 N/M）的細節，出貨版留著那些只會讓 1 MB 的
  # log 檔幾分鐘輪替一輪，真正該留下來的骨幹反而先被沖掉。
  # 刻意不用 QT_NO_DEBUG_OUTPUT 把 qDebug 整個編掉 —— 那跟上面那條「RelWithDebInfo 的
  # 現場回報最需要來源位置」自相矛盾。改成執行期過濾，現場只要請對方設一次
  # L2M_LOG_LEVEL=debug 就能把細節全部調回來。
  # CMAKE_BUILD_TYPE 為空時 $<CONFIG> 是空字串、不等於 Debug，因此落在 info ——
  # 沒指定型別的建置一律當成出貨建置處理。
  # L2M_BUILD_CONFIG：建置型別，寫進 crash 檔的 version 那一行。
  # **這是診斷欄位不是裝飾**：Debug 與 RelWithDebInfo 在
  # 「_set_invalid_parameter_handler 拿不拿得到 expression／file／line」與
  # 「inline 把多少層 frame 收掉」上差很多，同一份堆疊在兩種建置下讀起來不一樣，
  # 而使用者寄回來的報告不會附帶「我裝的是哪一種建置」。
  # CMAKE_BUILD_TYPE 為空時 $<CONFIG> 是空字串，那時寫 unknown 而不是留一對空括號
  target_compile_definitions(
    live2d_mate PRIVATE L2M_APP_VERSION="${PROJECT_VERSION}"
                        L2M_CUBISM_VERSION="${L2M_CUBISM_VERSION}"
                        L2M_DEV_I18N_DIR="${CMAKE_CURRENT_SOURCE_DIR}/i18n"
                        L2M_DEFAULT_LOG_LEVEL="$<IF:$<CONFIG:Debug>,debug,info>"
                        L2M_BUILD_CONFIG="$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,unknown>"
                        QT_MESSAGELOGCONTEXT)

  target_link_libraries(
    live2d_mate
    PRIVATE l2m_core
            l2m_live2d
            l2m_media
            httplib
            spdlog
            Qt${QT_VERSION_MAJOR}::Widgets
            Qt${QT_VERSION_MAJOR}::OpenGL
            Qt${QT_VERSION_MAJOR}::Network
            # 當機報告第二階段的符號化（src/platform/crash_handler_win.cpp）。
            # 只有 Windows 有這支，其他平台整個 crash handler 也還沒接
            $<$<BOOL:${WIN32}>:Dbghelp>)

  # -------------------------------------------------------------------------
  # 安裝與打包（GitHub Actions 的 release 走這條）
  #
  # 版面刻意是**平的**：執行檔直接放安裝根目錄，i18n/ 與 FrameworkShaders/ 就在旁邊。
  # 這不是美觀問題 —— main.cpp 的 resolveI18nDir() 與 live2d/cubism_runtime.cpp 的
  # shader 解析都是「先試編譯期烤進去的開發路徑，不存在才退回執行檔旁的同名目錄」，
  # 平放才踩得到那條 fallback。改成 bin/ + share/ 那種版面，出貨的執行檔會壞成
  # 「介面全是 raw key、模型一動也不動」那種沒有錯誤訊息的壞法。
  #
  # macOS 的 .app bundle 版面不同（資源要進 Contents/Resources），還沒接，
  # 所以整段只在 WIN32 生效。
  if(WIN32)
    # 精簡符號檔：只有 public symbols（函式名，沒有行號也沒有型別）。
    # 實測（RelWithDebInfo / MSVC2022 x64）完整 pdb 165.2 MB、這一份 14.2 MB，
    # 十一分之一，隨 exe 出貨負擔得起 —— 沒有它，出貨版的 --- symbolized --- 段
    # 就整段只剩位址。之所以還有 14 MB，是因為 Cubism Framework／spdlog／httplib
    # 全都靜態連進來，public symbol 本身就這麼多。
    # 要行號時再拿 release 另外保存的完整 pdb 離線解（見設計文件 §9）。
    # /INCREMENTAL:NO 是配套：CMake 給 RelWithDebInfo 的預設連結旗標帶 /INCREMENTAL，
    # 跟 /PDBSTRIPPED 併用會出 LNK4075（連結器自己把它忽略掉），
    # 留著只是每次建置多一則沒人會處理的警告。
    target_link_options(
      live2d_mate PRIVATE
      "$<$<CONFIG:RelWithDebInfo,Release>:/PDBSTRIPPED:$<TARGET_FILE_DIR:live2d_mate>/live2d_mate.stripped.pdb>"
      "$<$<CONFIG:RelWithDebInfo,Release>:/INCREMENTAL:NO>")

    install(TARGETS live2d_mate RUNTIME DESTINATION .)

    # **RENAME 是必要的，不是美觀問題**：DbgHelp 是靠 exe 的 debug directory 裡記的
    # 「pdb 檔名 + GUID + age」去找符號檔的，檔名對不上就根本不會去看它 ——
    # 裝成 live2d_mate.stripped.pdb 等於沒裝，而且完全沒有錯誤訊息，
    # 使用者只會看到 crash 檔裡整段都是 "(no symbol)"。
    # OPTIONAL 是因為 Debug 建置不產這個檔（上面的 generator expression 沒展開）
    install(
      FILES "$<TARGET_FILE_DIR:live2d_mate>/live2d_mate.stripped.pdb"
      DESTINATION .
      RENAME live2d_mate.pdb
      OPTIONAL)

    install(
      DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/i18n/"
      DESTINATION i18n
      FILES_MATCHING
      PATTERN "*.json")

    # Framework 5-r.5 的 GL renderer 是執行期才從磁碟讀 shader 原始碼的
    install(
      DIRECTORY
        "${cubismnativeframework_SOURCE_DIR}/src/Rendering/OpenGL/Shaders/Standard/"
      DESTINATION FrameworkShaders)

    # 授權文件：散布 binary 時這幾份必須同行，理由見 THIRD_PARTY_NOTICES.md 開頭。
    # 那幾家寬鬆授權的著作權聲明只存在原始碼檔頭，光給 exe 是傳不到使用者手上的。
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                  "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            DESTINATION .)
    install(
      FILES "${cubismnativeframework_SOURCE_DIR}/LICENSE.md"
      DESTINATION .
      RENAME LICENSE.CubismNativeFramework.md)
    # Core 的授權全文要使用者自己從 SDK 複製（SetupCubismCore.cmake 缺了會警告）
    if(EXISTS "${CUBISM_CORE_DIR}/LICENSE.md")
      install(
        FILES "${CUBISM_CORE_DIR}/LICENSE.md"
        DESTINATION .
        RENAME LICENSE.Live2DCubismCore.md)
    endif()

    # Qt 是 LGPL v3，散布 binary 要附授權全文。Qt 的安裝樹版面各版不同，找得到的
    # 那一份由 CI 在 **configure 之前**複製到這裡（見 .github/workflows/release.yml）。
    #
    # 為什麼要繞這一圈、不在 cmake --install 之後直接複製進 dist/：
    # NSIS 安裝檔是 cpack **自己重跑一遍 install 規則**產生的，外面補進 dist/ 的
    # 檔案它看不到。走 install() 規則，zip 與安裝檔才會拿到同一份內容 ——
    # 不然就會出現「zip 有 LGPL 全文、安裝檔沒有」這種發布之後才有人發現的差異。
    # 開發機沒有這個目錄，整條規則就不存在（本機建置本來就不是散布）。
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qt-licenses")
      install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qt-licenses/"
              DESTINATION Qt-LICENSES)
    endif()

    # Qt DLL。兩支執行檔各要一份，抽成函式的理由見它的註解。
    l2m_install_qt_deploy(live2d_mate)
  endif()
endif()

# ---------------------------------------------------------------------------
# Live2D Viewer 子專案（src/viewer/）
#
# 單一視窗的模型檢視器：開一個 *.model3.json 或 *.zip，右邊列出所有動作與表情
# 可以逐一預覽。待機動作、呼吸、眨眼與桌寵完全相同 —— 因為那三件事本來就在
# ModelController::update() 裡面，這裡只是每幀呼叫同一支。
#
# 刻意不連 l2m_media、httplib 與 spdlog：Viewer 沒有語音、沒有 MCP，
# 也不需要日誌輪替。qDebug 直接走 Qt 預設的輸出。
#
# **跟著桌寵一起出貨**（install 規則在這一段的最後）：兩支執行檔平放在同一個
# 目錄，共用旁邊的 i18n/ 與 FrameworkShaders/ —— Viewer 的資源解析規則本來
# 就跟主程式一樣是「找執行檔旁邊」，所以放在一起是零成本的。
# 只建 Viewer 的組合不接 install()，理由寫在下面那個 if 裡。
# ---------------------------------------------------------------------------
if(L2M_BUILD_VIEWER)
  set(L2M_VIEWER_SOURCES
      src/viewer/main.cpp
      src/viewer/collapsible_splitter.h
      src/viewer/collapsible_splitter.cpp
      src/viewer/viewer_canvas.h
      src/viewer/viewer_canvas.cpp
      src/viewer/viewer_window.h
      src/viewer/viewer_window.cpp
      src/viewer/tinted_icon.h
      src/viewer/tinted_icon.cpp
      # 工具列圖示（Material Symbols 的 *.svg）。AUTORCC 全域開著，
      # 列進來就會編進執行檔，路徑是 :/viewer/icons/<名稱>.svg。
      src/viewer/icons.qrc
      # 應用程式圖示（:/icons/app.ico），給 main.cpp 的 setWindowIcon 用。
      # 刻意**不是**主程式那份 resources/resources.qrc —— 那裡面還有啟動畫面、
      # 系統匣與 Ko-fi 圖，光 icon.png 與 splash@2x.png 就 2.5 MB，Viewer 一張
      # 都不會用到。所以另外切一份只含 app.ico 的 qrc（兩個 target 各自編譯，
      # 同名資源路徑不會互相干擾）。
      resources/app_icon.qrc)

  if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(live2d_viewer WIN32 MACOSX_BUNDLE ${L2M_VIEWER_SOURCES})
  else()
    add_executable(live2d_viewer WIN32 MACOSX_BUNDLE ${L2M_VIEWER_SOURCES})
  endif()

  # exe 本身的圖示（檔案總管、工作列釘選）。與桌寵共用同一份 app.rc ——
  # 裡面只有一個 IDI_ICON1 資源，沒有任何跟哪支執行檔有關的東西。
  # 這與 main.cpp 的 setWindowIcon 是兩回事，兩邊都要設（理由見 app.rc 的註解）。
  if(WIN32)
    target_sources(live2d_viewer PRIVATE resources/app.rc)
  endif()

  # 介面全部手刻，沒有 .ui，所以這個 target 不必開 AUTOUIC
  #（那個開關只開在 live2d_mate 上，見那邊的註解）。
  #
  # i18n 的訊息表跟主程式共用同一份 i18n/：開發時走這個編譯期路徑，
  # 部署時退回執行檔旁的 i18n/（解析規則抄自 main.cpp 的 resolveI18nDir）。
  target_compile_definitions(
    live2d_viewer PRIVATE L2M_DEV_I18N_DIR="${CMAKE_CURRENT_SOURCE_DIR}/i18n")

  target_link_libraries(
    live2d_viewer
    PRIVATE l2m_core l2m_live2d Qt${QT_VERSION_MAJOR}::Widgets
            Qt${QT_VERSION_MAJOR}::OpenGL Qt${QT_VERSION_MAJOR}::OpenGLWidgets
            Qt${QT_VERSION_MAJOR}::Svg)

  # Viewer 隨桌寵一起出貨：安裝檔會多一個「Live2D Viewer」的開始選單捷徑。
  #
  # **條件是 L2M_BUILD_APP 而不是只有 WIN32**，因為 Viewer 執行時需要的
  # i18n/ 與 FrameworkShaders/ 兩個目錄是由主程式那組 install() 規則裝進去的。
  # 只建 Viewer（-DL2M_BUILD_APP=OFF -DL2M_BUILD_VIEWER=ON）時裝出來的會是
  # 一支「介面全是 raw key、模型一動也不動」的執行檔 —— 那種安裝比不裝更糟，
  # 所以那個組合維持原本的行為：不接 install()，從建置樹直接跑。
  if(WIN32 AND L2M_BUILD_APP)
    install(TARGETS live2d_viewer RUNTIME DESTINATION .)

    # windeployqt 必須對這支**再跑一次**（同一個安裝前綴，第二輪只補缺的幾顆）。
    # 理由與那句 QT_DEPLOY_BIN_DIR 為什麼載重，寫在函式本體的註解裡。
    l2m_install_qt_deploy(live2d_viewer)
  endif()
endif()

# ---------------------------------------------------------------------------
# 測試：全部只連 l2m_core（見 l2m_add_test）
# ---------------------------------------------------------------------------
if(L2M_BUILD_TESTS)
  # Qt Test 刻意不併進上面那個 REQUIRED 元件清單：併進去的話這個 option 只做半套
  # —— 沒裝 Qt Test 元件的環境照樣會在 configure 階段就失敗。
  find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)
  enable_testing()

  # ARGN：個別測試可以再多連函式庫（例如需要 httplib 的整合測試）
  function(l2m_add_test name)
    if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
      qt_add_executable(${name} tests/${name}.cpp)
    else()
      add_executable(${name} tests/${name}.cpp)
    endif()
    target_link_libraries(${name} PRIVATE l2m_core Qt${QT_VERSION_MAJOR}::Test
                                          ${ARGN})
    target_compile_definitions(
      ${name}
      PRIVATE L2M_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/models"
              L2M_I18N_DIR="${CMAKE_CURRENT_SOURCE_DIR}/i18n")
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  l2m_add_test(test_bubble_placement)
  l2m_add_test(test_bubble_shape)
  l2m_add_test(test_bubble_font)
  l2m_add_test(test_ellipse_text_fit)
  l2m_add_test(test_echo_filter)
  l2m_add_test(test_parameter_tracks)
  l2m_add_test(test_motion_builder)
  l2m_add_test(test_zip_archive)
  l2m_add_test(test_model_assets)
  l2m_add_test(test_texture_format)
  l2m_add_test(test_display_info)
  l2m_add_test(test_model_settings)
  l2m_add_test(test_motion_meta)
  l2m_add_test(test_motion_curve_ids)
  l2m_add_test(test_idle_motion_pick)
  l2m_add_test(test_annotations)
  l2m_add_test(test_builtin_actions)
  l2m_add_test(test_model_scanner)
  l2m_add_test(test_viewer_settings)
  l2m_add_test(test_config)
  l2m_add_test(test_idle_reset)
  l2m_add_test(test_idle_performer)
  l2m_add_test(test_mcp_quiet_reset)
  l2m_add_test(test_interaction_logic)
  l2m_add_test(test_model_regions)
  l2m_add_test(test_i18n)
  l2m_add_test(test_tray_label)
  l2m_add_test(test_qt_timer_host)
  l2m_add_test(test_controller_parameters)
  l2m_add_test(test_controller_naming)
  l2m_add_test(test_tts)
  l2m_add_test(test_tts_http)
  l2m_add_test(test_edge_ssml)
  l2m_add_test(test_happy_eyeballs)
  l2m_add_test(test_mcp_host)
  l2m_add_test(test_mcp_origin)
  l2m_add_test(test_mcp_snippets)
  l2m_add_test(test_mcp_tools)
  l2m_add_test(test_jsonrpc)
  l2m_add_test(test_autostart)
  l2m_add_test(test_splash_timing)
  l2m_add_test(test_fade_timing)
  l2m_add_test(test_config_patch)
  l2m_add_test(test_voice_list)
  l2m_add_test(test_settings_layout)
  l2m_add_test(test_persona)
  l2m_add_test(test_audio_stream_format)
  l2m_add_test(test_text_segments)
  l2m_add_test(test_tts_segments)
  l2m_add_test(test_drag_swing)
  l2m_add_test(test_bottom_align)
  l2m_add_test(test_canvas_center)
  l2m_add_test(test_schedule_book)
  l2m_add_test(test_behavior_pick)
  l2m_add_test(test_idle_director)
  l2m_add_test(test_autonomy_gates)
  l2m_add_test(test_ambient_wind)
  l2m_add_test(test_wind_targets)
  l2m_add_test(test_persona_default)
  l2m_add_test(test_persona_doc)
  l2m_add_test(test_presence_tracker)
  l2m_add_test(test_day_period)
  l2m_add_test(test_gesture_detector)
  l2m_add_test(test_mood)
  l2m_add_test(test_screen_rescue)
  l2m_add_test(test_sse_parser)
  l2m_add_test(test_llm_stream_accumulator)
  l2m_add_test(test_llm_http)
  l2m_add_test(test_llm_behavior_prompt)
  l2m_add_test(test_persona_memory)
  l2m_add_test(test_persona_generate)
  l2m_add_test(test_log_rotation)
  l2m_add_test(test_log_category)
  l2m_add_test(test_topmost_watchdog)
  l2m_add_test(test_gaze_director)
  l2m_add_test(test_crash_report)
  l2m_add_test(test_weather_code)
  l2m_add_test(test_weather_http)
  l2m_add_test(test_weather_alert)
  l2m_add_test(test_external_model)
  l2m_add_test(test_png_probe)
  l2m_add_test(test_layout_fit)
endif()

# ---------------------------------------------------------------------------
# CPack：Windows 安裝檔（NSIS）
#
# 跟 zip **並存而不是取代**。便攜版（解壓即用、不寫登錄檔、可以放隨身碟、
# 不必管理員權限）是這個 app 的使用者真的會要的東西；安裝檔只是多一個
# 「雙擊、下一步、桌面出現捷徑」的入口給不想解壓縮的人。
#
# 內容完全由上面那組 install() 規則決定 —— cpack 自己跑一遍安裝到暫存目錄，
# 再把整包餵給 NSIS。**所以「zip 裡有、安裝檔裡沒有」這種差異不會發生**，
# 前提是所有要出貨的東西都走 install()，而不是在 cmake --install 之後另外
# 複製進 dist/（Qt 授權文字原本就是那樣補的，已經改成 install() 規則）。
#
# 為什麼是 NSIS 而不是 WIX/MSI：MSI 的價值在企業 GPO 派送，而這是個人桌寵；
# NSIS 也不必經營 upgrade GUID（那東西一旦發布就不能再改，改了就變成兩套並存
# 的安裝）。另外 CPack 的 WIX generator 只認 WiX v3 的 candle/light，
# v4 之後換了 CLI 它吃不動 —— 等於要嘛釘死在一個停止維護的版本，
# 要嘛自己手寫 .wxs。
#
# 幾個非預設值，每一個都是踩點：
#   CPACK_NSIS_EXECUTABLES_DIRECTORY —— CPack 產捷徑時寫死用 "$INSTDIR\bin\"，
#     而本專案的安裝版面是**平的**（執行檔就在根目錄，理由見上面 install 段）。
#     不改的話開始選單與桌面捷徑會全部指向不存在的 bin\live2d_mate.exe，
#     而且 cpack 從頭到尾不會出一句警告 —— 跟 QT_DEPLOY_BIN_DIR 是同一個坑。
#   CPACK_NSIS_INSTALL_ROOT —— 不明寫的話會裝進 Program Files (x86)，
#     一個 x64 程式擺在那裡只是讓人以為裝錯版本。
#   CPACK_RESOURCE_FILE_LICENSE —— 不設的話 CPack 會拿**它自己的**通用授權樣板
#     當同意頁，等於讓使用者對一份跟這個軟體無關的文字按「我同意」。
#
# 刻意**不設** CPACK_NSIS_MUI_FINISHPAGE_RUN（安裝完直接啟動）：安裝程式是提權
# 跑的（樣板寫死 RequestExecutionLevel admin），從它啟動的話桌寵會帶著管理員
# 權杖跑完整個 session —— 一個常駐、還會開 HTTP 伺服器接受外部指令的程式，
# 沒有理由用管理員權限跑。使用者從捷徑啟動拿到的才是正常權限。
# ---------------------------------------------------------------------------
if(L2M_BUILD_APP AND WIN32)
  # 安裝檔顯示的版本號。預設用 project() 的，CI 會用 git tag 覆蓋（見
  # release.yml 的 Resolve version）—— 否則 tag 打了 v1.2.0，安裝檔的
  # 「應用程式與功能」裡卻還寫著 1.0.0，而且沒有任何地方會抱怨。
  set(L2M_PACKAGE_VERSION "${PROJECT_VERSION}"
      CACHE STRING "安裝檔顯示的版本號；CI 以 git tag 覆蓋")

  set(CPACK_GENERATOR "NSIS")
  set(CPACK_SOURCE_GENERATOR "")  # 不產原始碼包（會把整棵樹連 build/ 一起壓進去）
  set(CPACK_PACKAGE_NAME "Live2D Mate")
  set(CPACK_PACKAGE_VENDOR "Live2D Mate")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "Live2D desktop mate with text-to-speech and an MCP server")
  set(CPACK_PACKAGE_VERSION "${L2M_PACKAGE_VERSION}")
  set(CPACK_PACKAGE_FILE_NAME "live2d_mate-${L2M_PACKAGE_VERSION}-win64-setup")

  # 版本字串**沒有格式限制**：CPack 的 NSIS 樣板不產 VIProductVersion，也不碰
  # CPACK_PACKAGE_VERSION_{MAJOR,MINOR,PATCH}（實際 grep 過樣板，一個都沒有），
  # CPACK_PACKAGE_VERSION 只會原樣寫進反安裝登錄檔的 DisplayVersion。
  # 所以 `1.2.0-rc1` 之類的字串直接給就好，不必先拆成三段。

  # 授權同意頁：MIT 全文接 THIRD_PARTY_NOTICES.md。
  # **只給 LICENSE 是不對的** —— 這個 binary 不是 MIT（靜態連了 Live2D Cubism
  # Core 這份專有物件碼、動態連了 LGPL v3 的 Qt），只顯示 MIT 會讓按下「我同意」
  # 的人同意到一份跟他實際收到的東西不符的條款。
  # 行尾必須是 CRLF：NSIS 的授權頁是 RichEdit 控制項，吃到 LF-only 的檔案會把
  # 整份文件擠成一行。而 checkout 出來是 CRLF 還是 LF 取決於使用者的
  # core.autocrlf，不能賭 —— 所以這裡先**全部壓成 LF**，再交給 file(WRITE)。
  # 不要自己組 CRLF：file(WRITE) 在 Windows 上是文字模式，會再把每個 LF 換成
  # CRLF，先寫成 CRLF 的話結果是 CR CR LF（實測），RichEdit 每行會多一個空行。
  #
  # **內容還要折成純 ASCII**：CPack 的 NSIS 樣板沒有下 `Unicode true`，
  # 產出的是 ANSI 安裝檔，授權頁會用**使用者的系統碼頁**去解那個檔案。
  # UTF-8 的破折號在 CP1252 上會變 `â€"`、在正體中文的 CP950 上變成隨機漢字 ——
  # 一份要人按「我同意」的法律文件長成亂碼，比字型醜嚴重得多。
  # 目前整份只有三種非 ASCII（—、×、→，實測），折起來毫無損失；
  # 折不掉的（例如日後有人加了日文廠商名）由下面那個 MATCH 當場叫出來，
  # 免得又變成一個只有使用者看得到、CI 全綠的失敗。
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" l2m_license_text)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" l2m_notices_text)
  string(REGEX REPLACE "\r\n" "\n" l2m_license_page
         "${l2m_license_text}\n\n${l2m_notices_text}")
  string(REPLACE "—" "--" l2m_license_page "${l2m_license_page}")
  string(REPLACE "×" "x" l2m_license_page "${l2m_license_page}")
  string(REPLACE "→" "->" l2m_license_page "${l2m_license_page}")
  string(REGEX MATCH "[^ -~\n\t\r]" l2m_license_leftover "${l2m_license_page}")
  if(l2m_license_leftover)
    message(
      WARNING
        "LICENSE／THIRD_PARTY_NOTICES.md 出現沒被折成 ASCII 的字元，"
        "安裝檔的授權頁在部分系統碼頁上會顯示成亂碼。"
        "請在上面補一條 string(REPLACE) 或改寫該處文字。")
  endif()
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/InstallerLicense.txt" "${l2m_license_page}")
  set(CPACK_RESOURCE_FILE_LICENSE
      "${CMAKE_CURRENT_BINARY_DIR}/InstallerLicense.txt")
  # file(READ) 不會登記相依，改了那兩個檔案不會重跑 configure ——
  # 症狀是「加了新的第三方元件，安裝檔的同意頁還停在舊版本」。
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
             "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md")

  set(CPACK_NSIS_PACKAGE_NAME "Live2D Mate")
  set(CPACK_NSIS_DISPLAY_NAME "Live2D Mate")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "Live2D Mate")
  set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
  set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
  # 開始選單的捷徑，格式是「exe 檔名（不含 .exe）;顯示名」。
  # Viewer 那一條**必須跟著 L2M_BUILD_VIEWER 走**：關掉它時執行檔根本不存在，
  # 硬留著只會裝出一個雙擊跳「找不到項目」的捷徑，而 cpack 全程不會抱怨
  #（-DL2M_BUILD_VIEWER=OFF 是文件裡就有的組合，不能讓它變成 configure 失敗）。
  set(CPACK_PACKAGE_EXECUTABLES "live2d_mate;Live2D Mate")
  if(L2M_BUILD_VIEWER)
    list(APPEND CPACK_PACKAGE_EXECUTABLES "live2d_viewer;Live2D Viewer")
  else()
    message(STATUS "L2M_BUILD_VIEWER=OFF：安裝檔不會含 Live2D Viewer 與它的捷徑")
  endif()
  # 桌面捷徑只給桌寵。Viewer 是「想看某個模型時才開一次」的工具，
  # 常駐在桌面上只是佔位子 —— 要用的時候從開始選單找得到就夠了。
  #
  # 變數名沒有 NSIS_ 前綴（實測踩過：寫成 CPACK_NSIS_CREATE_DESKTOP_LINKS 的話
  # 安裝程式照樣會問「要不要建立桌面捷徑」，勾了也**什麼都不會發生**，
  # 產出的 .nsi 裡根本沒有那一行 CreateShortCut，而 cpack 不會有任何抱怨）。
  # 另外它只認已經在 CPACK_PACKAGE_EXECUTABLES 裡的項目。
  set(CPACK_CREATE_DESKTOP_LINKS "live2d_mate")
  set(CPACK_NSIS_INSTALLED_ICON_NAME "live2d_mate.exe")
  set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")
  set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")
  # 升級時先移除舊版：這個安裝包是整包覆蓋，殘留的舊 Qt DLL 會讓新版起不來。
  # 已知限制：桌寵正在執行時（開機自啟動的人就是這個狀態）舊版的檔案鎖著刪不掉，
  # NSIS 會跳「Error opening file for writing」。目前沒有做行程偵測 ——
  # 要做得靠 nsProcess 這種不隨 NSIS 出貨的外掛，或賭 Qt 的視窗類別名稱。
  # 升級前請先從系統匣結束桌寵。
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

  # 反安裝時一併清掉開機自啟動的登錄值。CPack 的反安裝器只刪它自己裝過的東西，
  # 而這個值是**程式自己寫的**（src/platform/autostart_win.cpp 的 kRunKey／
  # kValueName），不清的話移除程式之後每次登入 Windows 都會去啟動一個已經不存在
  # 的執行檔 —— 沒有錯誤訊息，只是每次開機都失敗一次。
  # 兩個 key／value 名稱必須跟那支 .cpp 一字不差，改到那邊記得回來改這裡。
  set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
      "DeleteRegValue HKCU 'Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Run' 'Live2D Mate'"
  )
  set(CPACK_NSIS_MODIFY_PATH OFF)  # 桌寵沒有命令列用途，不該動使用者的 PATH
  # 預設的 zlib 壓不動這包（Qt DLL 佔了大半）。實測（同一份內容、本機 cpack）：
  # zlib 30.30 MB / 13 秒，/SOLID lzma 22.15 MB / 36 秒 —— 少 8.16 MB（27%），
  # 多花 23 秒。這是每次發布跑一次、每個使用者下載一次的東西，換得過。
  set(CPACK_NSIS_COMPRESSOR "/SOLID lzma")
  # 想讓「應用程式與功能」裡出現說明連結的話，在這裡補
  # CPACK_NSIS_URL_INFO_ABOUT / CPACK_NSIS_HELP_LINK。目前刻意留空：
  # repo 還沒有對外的正式網址，填一個之後會變的進去比不填更糟。

  include(CPack)
endif()
