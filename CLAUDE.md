# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 專案定位

Live2D Mate 桌寵，**Qt 6 / C++** 實作。
改動前先看檔案開頭的區塊註解，那裡通常寫了「這是什麼／為什麼這樣設計」。

執行期資料在 `%APPDATA%/live2d_mate/config.json` 與 `%APPDATA%/live2d_mate/models/`
（Live2D Viewer 的視窗位置另外記在同一個目錄的 `viewer.json`，兩支行程各寫各的檔）。
`serializeConfig` 的鍵順序是既有 config.json 的合約：新欄位一律接在既有鍵後面，
插在中間會讓每次重寫都整份 diff。

## 建置前置：Cubism Core（必做一次）

`third_party/CubismCore/` 是閉源、gitignored，**沒放好 CMake configure 會直接 FATAL_ERROR**。
依 `docs/CUBISM_CORE_SETUP.md` 從官網下載 **Cubism SDK for Native 5-r.5** 手動放置。
開源的 CubismNativeFramework 由 `cmake/FetchCubismFramework.cmake` 以 FetchContent 釘在 tag `5-r.5` 自動抓取
（所以**第一次 configure 需要網路**）。

抓下來之後**每次 configure 都會套用 `patches/` 底下的三份 patch**（混合模式 shader 改成延遲產生，
省掉啟動時 474 支白編的 shader，`CreateRenderer` 2995 ms → 55 ms；環境風改吃「哪些 PhysicsSetting 該吹」的遮罩；
`PartOpacity` 動作曲線真的寫進部件不透明度 —— 上游是拿部件 id 去查參數表，
整條曲線靜靜地沒有作用，症狀見「幾個容易誤解的子系統」那條）。已套用會自動略過，
套不上一律 FATAL_ERROR —— **升 `GIT_TAG` 之後若上游改了同一塊，會在 configure 當場失敗**，
要重做 patch 而不是繞過。理由與實測數字寫在 patch 檔頭。

## 建置前置：Qt Image Formats（WebP 貼圖）

貼圖解碼**除了 PNG 之外**走 `QImage`（`live2d/model_controller.cpp` 的 `DecodeTask`；
PNG 走自己的 libspng + zlib-ng 快路徑，見下面「幾個容易誤解的子系統」），
能解哪些格式**完全取決於這個 Qt 建置裝了哪些 imageformats 外掛**。PNG/BMP 是編進 QtGui 的內建
handler、JPEG/GIF/ICO/SVG 隨 Qt 附，而 **WebP 在另外要勾的 Qt Image Formats 模組裡**
（Maintenance Tool → Qt 6.x → Additional Libraries → Qt Image Formats）。
沒裝的話 webp 貼圖的模型會走完一條**完全靜默**的失敗鏈，症狀是「模型載入完成」
配一片空白 —— 現在那條鏈至少會回報失敗並說出原因，理由與實測寫在
`src/core/texture_format.h`。CI 走 `release.yml` 的 `modules: qtwebsockets qtimageformats`；
`windeployqt` 會把整個 `imageformats/` 目錄帶進安裝檔，不必另外寫規則。
**只跑測試的建置不需要它**（`l2m_core` 一行 Qt Gui 都不碰）。

## 常用指令

```bash
# Configure（Qt 6.11.2 / MSVC2022 x64 / Ninja）
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64 -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 不建測試（預設 ON）。關掉連 Qt6 的 Test 元件都不會去找，
# 所以沒裝 Qt Test 的環境也 configure 得過。
cmake -S . -B build -G Ninja ... -DL2M_BUILD_TESTS=OFF

# 不建主程式（預設 ON）。這種建置**完全不需要 Cubism Core**：不 include
# SetupCubismCore、不抓 Framework、只留 l2m_core 與 84 支測試，Qt 也只要
# Core/Network/Test。CI 走的就是這條（.github/workflows/tests.yml）。
# Viewer 的預設值跟著這個開關走，所以這條命令列不必再多關一個。
cmake -S . -B build -G Ninja ... -DL2M_BUILD_APP=OFF

# 不建 Live2D Viewer 子專案（預設同 L2M_BUILD_APP）。反過來「只建 Viewer」是
# -DL2M_BUILD_APP=OFF -DL2M_BUILD_VIEWER=ON（仍然需要 Cubism Core）。
cmake -S . -B build -G Ninja ... -DL2M_BUILD_VIEWER=OFF

# 打包（Windows）。平放版面：exe 旁邊放 i18n/、FrameworkShaders/、Qt DLL 與授權文件，
# 因為那兩個資源目錄的 fallback 就是找執行檔旁邊。
cmake --install build --config RelWithDebInfo --prefix dist/live2d_mate

# NSIS 安裝檔（需要 makensis 在 PATH 上）。內容不是壓上面那個 dist/，而是 cpack
# 自己再跑一遍 install 規則 —— 所以**要出貨的東西一律加成 install() 規則**，
# 事後補進 dist/ 的檔案只會進 zip，安裝檔裡靜靜地少一份。
cpack --config build/CPackConfig.cmake -C RelWithDebInfo -B build/package

# 全部測試
ctest --test-dir build --output-on-failure

# 單一測試（三種）
ctest --test-dir build -R test_mcp_host --output-on-failure
cmake --build build --target test_mcp_host        # 只建這一個
build/rel/test_mcp_host.exe <testFunctionName>    # 單一 QTest slot；-functions 可列出
```

打包會一併裝上 `/PDBSTRIPPED` 產的精簡 pdb（改名成 `live2d_mate.pdb` 放在 exe 旁邊——
DbgHelp 是靠 debug directory 裡記的檔名找符號檔的，改名是必要的）。完整 pdb 太肥
（實測 165.18 MB，精簡版 14.23 MB，比例約 1/11.6），由 `release.yml` 掛成獨立資產；
crash 報告記了 RVA 與 PDB GUID/age，抓那一包就能離線精確還原到 `file:line`。

現有的建置樹：`build/Desktop_Qt_6_11_2_MSVC2022_64bit-{Debug,Release}`（Qt Creator kit）與 `build/rel`（RelWithDebInfo）。
執行檔平放在建置樹根目錄。有 `.clang-format`（根目錄），沒有 `.clang-tidy`。
`install()`、windeployqt 與 CPack（NSIS）規則在 `CMakeLists.txt` 尾端（只有 WIN32，macOS 的 bundle 版面還沒接）；
CI 在 `.github/workflows/`：`tests.yml` 每次 push/PR 跑測試（`L2M_BUILD_APP=OFF`，不需要 Cubism Core），
`release.yml` 推 `v*` tag 時打包發布（需要 repo 變數 `CUBISM_SDK_URL`），
產出 **NSIS 安裝檔 + 便攜版 zip + 完整 pdb** 三個資產。
安裝檔是 `cpack` **重跑一遍 install 規則**做出來的，不是壓 `dist/` ——
所以要出貨的東西一律加成 `install()` 規則，事後補進 `dist/` 的檔案只會進 zip
（Qt 的 LGPL 全文原本就是那樣補的，已改成由 `third_party/qt-licenses/` 走 install 規則；
那兩份全文**直接進 repo**，不再從 Qt 安裝樹複製 —— CI 的 aqtinstall 只給模組二進位、
從來不帶授權文字，舊作法每一輪都找不到而靜靜出貨一個缺全文的包）。
安裝檔顯示的版本走 `L2M_PACKAGE_VERSION`（預設 `project()` 的版本，CI 以 tag 覆蓋）。
授權：原始碼 MIT（`LICENSE`），binary 因為含 Cubism Core 與 Qt 不是 —— 見 `THIRD_PARTY_NOTICES.md`。

### 執行

```bash
build/rel/live2d_mate.exe                  # 一般啟動（single-instance；第二次啟動只會叫醒第一個）
build/rel/live2d_mate.exe --hidden         # 縮在系統匣啟動（只剩手動用；開機自動啟動已不再帶這個旗標）
build/rel/live2d_mate.exe --mcp-stdio [url] [token]   # stdio↔HTTP 橋接（Claude Desktop 用）
build/rel/live2d_mate.exe --set-model "E:/any/where/Foo.model3.json"  # 用這隻外部模型（見下）

build/rel/live2d_viewer.exe                            # 空視窗，再用「開啟模型…」或拖放
build/rel/live2d_viewer.exe "path/to/Foo.model3.json"  # 也吃整個 *.zip；沒有 single-instance，可以同時開好幾個比對
```

MCP HTTP 伺服器預設 `http://127.0.0.1:3777/mcp`（另有 `GET /health`）。
支援的方法：`initialize`、`ping`、`tools/list`、`tools/call`、`resources/list`、`resources/read`。

診斷用環境變數：`L2M_PROFILE`（每 2 秒印各階段 avg/p50/p95/max 幀時間）、
`L2M_SAY`（啟動 3 秒後說一句話，一次驗證 synth→播放→氣泡→嘴形）、
`L2M_STRAIGHT_ALPHA`、`L2M_FORCE_REGION`、`L2M_TEST_OPAQUE`、`L2M_DUMP_FRAME`；
stdio 橋接讀 `L2D_MCP_URL` / `L2D_MCP_TOKEN`（注意是 `L2D_` 前綴，不是 `L2M_`）。

## 架構

### 五個 target，切法是刻意的

單一根 `CMakeLists.txt`，沒有子目錄 CMakeLists。`src/` 本身是 include root，
所以跨函式庫用帶目錄的 `#include "core/config_store.h"`，同層／鄰近目錄用相對路徑。

| Target | 內容 | 連結 |
|---|---|---|
| `l2m_core` | `src/core/*` — **純邏輯，不碰 GUI/GL** | Qt6::Core, Qt6::Network, yyjson, miniz |
| `l2m_live2d` | `src/live2d/*` — Cubism 包裝與 GL 渲染 | l2m_core, Framework, glew, Qt6::Gui, spng（→ zlib-ng） |
| `l2m_media` | `src/media/*` — 音訊播放、TTS 引擎與天氣服務；`src/llm/*` — LLM 引擎與行為大腦（掛在這裡而不是開第五個 target：HttpJson 在 media，幾個 TU 不值得一個新函式庫） | l2m_core, miniaudio, Qt6::Network, Qt6::WebSockets |
| `live2d_mate` | `src/app/`、`src/windows/`、`src/mcp/`、`src/platform/` | 以上全部 + httplib, Qt6::Widgets/OpenGL |
| `live2d_viewer` | `src/viewer/*` — 單一視窗的模型檢視器（子專案，見下） | l2m_core, l2m_live2d, Qt6::Widgets/OpenGL/**OpenGLWidgets** |

**Live2D Viewer 子專案**（`-DL2M_BUILD_VIEWER`，**預設跟著 `L2M_BUILD_APP` 走**）：
開一個 `*.model3.json` 或 `*.zip`（也吃拖放與命令列參數；拖進**整個模型資料夾**也可以，
走 `core/model_scanner.h` 的 `findDirectoryEntry()` 換成第一層的入口檔 —— **只看第一層**，
再往下找的話「拖一個裝了十隻模型的目錄」會靜靜開了其中一隻），右邊列出所有動作與表情、
點一下就預覽；待機動作／呼吸／眨眼與桌寵完全相同，因為那三件事本來就在
`ModelController::update()` 裡，Viewer 只是每幀呼叫同一支。模型隨視窗縮放也不必寫程式碼
（`draw()` 每幀依 viewport 長寬比重算投影）。它**不連** `l2m_media`／httplib／spdlog，
所以連 Qt WebSockets 都不必找。**預設值跟著 APP 而不是寫死 ON**：CI 的測試建置
只給 `-DL2M_BUILD_APP=OFF`，寫死 ON 會讓 Viewer 又把 Cubism Core 拉回來而在
`SetupCubismCore` 當場 FATAL_ERROR。
**記住視窗位置與大小**：關窗時寫進 `%APPDATA%/live2d_mate/viewer.json`
（跟桌寵**同一個資料夾、不同檔案** —— 桌寵的 `ConfigStore` 是整份重寫的，
Viewer 在關閉時插一筆進去只會跟正在跑的桌寵互相覆蓋；Viewer 也不該動得到桌寵的設定）。
判斷邏輯全在 `core/viewer_settings.h`，`src/viewer/` 那端只有讀檔／寫檔與問 `QScreen`。
四個踩過的點：記的是 **`normalGeometry()`**（最大化時 `geometry()` 是滿版矩形，
記下去等於把「還原後的大小」弄丟）；**全螢幕不記**（F11 是臨時狀態，
下次一開就整片蓋住桌面、狀態列還收著，看起來像當掉）；還原要**先 `setGeometry`
再設最大化狀態**（Qt 把當下的矩形記成還原後的大小，反過來的話取消最大化會跳回預設位置）；
「看不看得見」要**橫直各自比**而不是比面積（只露出 10 px 的一條縫乘上 600 px 的高度
就有 6000，面積門檻輕鬆跨過，畫面上卻只有一條抓不住的細線）。
沒有 single-instance，所以同時開好幾個時**最後關掉的那個說了算**。
**跟著桌寵一起出貨**：`install()` 規則掛在 `L2M_BUILD_VIEWER` 區塊尾端，條件是
`WIN32 AND L2M_BUILD_APP` —— Viewer 執行時要的 `i18n/` 與 `FrameworkShaders/`
是主程式那組規則裝進去的，「只建 Viewer」裝出來會是一支介面全是 raw key、
模型一動也不動的執行檔，所以那個組合維持不接 install()。
`windeployqt` 要**對 Viewer 再跑一次**（它多連了 `Qt6OpenGLWidgets`，那顆 DLL
不在主程式的相依裡；少了它安裝版的 Viewer 雙擊完全沒反應、沒有任何訊息）。
安裝檔的開始選單有兩個捷徑，桌面捷徑只給桌寵。
圖示與桌寵共用同一份 `resources/app.ico`：exe 圖示走 `resources/app.rc`（兩個
target 都列進去），視窗／工作列圖示走 `main.cpp` 的 `setWindowIcon`，來源是
只含 `app.ico` 的 `resources/app_icon.qrc`（不是主程式那份 `resources.qrc`——
那裡面的啟動畫面與系統匣圖 Viewer 一張都用不到，光兩張 PNG 就 2.5 MB）。

**內建動作／表情不出現在 Viewer**：`core/builtin_actions.h` 合成的那十來個
（`wave`／`nod`／`smile`…）是**桌寵餵給 AI 的介面** —— 讓只綁一個 Idle 的模型也叫得動
「揮手」。檢視器的用途卻是看這隻模型到底做了什麼，混進合成項目會讓人分不清哪些是
作者做的，所以 `loadModel()` 收下 `describeModel()` 的結果之後就用
`removeBuiltinActions()` 整組拿掉（連 `model_` 都不留：留著的話狀態列計數與
`playMotionByName` 的解析看得到、清單卻沒有，等於自己製造兩份不一致的真相）。
**掃描端一個字都不動** —— `describeModel` 與 `scanModels` 一字不差是它的合約
（`tests/test_model_scanner.cpp` 釘住），「掃到什麼」與「要顯示什麼」是兩件事。

清單那一欄最上面是一條 **QToolBar**（`buildToolBar()`），六顆**純圖示**按鈕：
左邊是開啟模型／設定為桌寵／重置動作與表情／自動重播，右邊靠齊的是浮動清單／全螢幕（靠右是塞一個
`QSizePolicy::Expanding` 的空 widget 撐開的 —— QToolBar 沒有靠右對齊這種選項，
而那塊空白的 minimumWidth 是 0，欄位變窄時先被壓扁的是它，按鈕不會提早被收進
「»」延伸選單）。說明字一律走 tooltip：欄寬只有 300 px 上下，帶文字一定塞不下。
**刻意不是 `QMainWindow::addToolBar()` 的視窗級工具列**，而是那一欄版面裡的一般
widget（QToolBar 本來就是 QWidget，放進 QVBoxLayout 沒有特別待遇，也因此不必設
objectName —— 那是 `saveState` 才要的）：這樣它跟著清單走，清單浮起來它一起浮、
收合一起消失。**全螢幕時工具列刻意留著**（只收狀態列）—— 全螢幕本身就是這條上的
一顆 toggle，收掉的話按下去就再也按不回來，只剩 F11；要整片乾淨的話手把上那顆
收合鈕會把整欄一起收掉。

圖示是 `src/viewer/icons/*.svg`（Material Symbols 24px），經
`src/viewer/icons.qrc` 編進執行檔，路徑 `:/viewer/icons/<名稱>.svg`。
**`live2d_viewer` 因此要連 `Qt6::Svg`** —— 少了它會**安靜地**變成空圖示，
一句錯誤訊息都沒有。

**顏色不是 svg 裡寫的那個**（`src/viewer/tinted_icon.h`）：Material Symbols 的原檔把
`fill` 寫死成近黑的 `#1f1f1f`，深色主題下工具列底色也是深的，六顆按鈕會整排看不見
（實測 Fusion 深色：底 `#3c3c3c`／圖 `#1f1f1f`，灰階只差 29）。所以一律過
`tintedIcon()` —— 先讓 `QSvgRenderer` 照原樣畫成點陣，再用 `CompositionMode_SourceIn`
整片塗成調色盤的 `ButtonText`（停用態走 `Disabled` 群組）。**刻意不看 svg 裡寫的是什麼
顏色**，所以換素材、換色碼都不必回來改；限制是只適用單色圖示。另外兩種做法都不行：
淺／深各備一份 svg 只認得「深色主題」這一種情況，字串替換 `fill="#1f1f1f"` 則會在
下次從網站重新下載（可以選顏色）時靜靜失效。
用 `QIconEngine` 而不是預先烘好幾個尺寸：畫的時機是 style 真的要畫的那一刻，尺寸與
devicePixelRatio 都是當下正確的值。**切換主題時整組重新指派新的 `QIcon` 物件**
（`changeEvent` → `updateToolBarState()`，六顆一起；`changeEvent` 除了
`ApplicationPaletteChange` 也接 `ThemeChange` —— 使用者若曾以 `QApplication::setPalette()`
明示指定過調色盤，Qt 就不再覆寫、也不發前者）。引擎每次畫都重讀調色盤，理論上
下一次 repaint 就是新顏色；重新指派是**便宜的保險**：Qt 內建的 `QPixmapIconEngine`／
`QIconLoaderEngine` 會把 pixmap 存進 `QPixmapCache`、鍵是 `QIcon::cacheKey()`，
那條快取寫在內建引擎裡面、自訂引擎照理走不到，但那是實作細節、跨版本沒有保證，
而換一個新 `QIcon` 的成本只是重解一張 600 位元組的 svg。

浮動清單／全螢幕／自動重播三顆都是 **checkable**（凹著＝那個模式正開著），
圖示則畫「按下去會發生什麼」（`fullscreen` ↔ `exit_fullscreen` 那組向外／向內的
箭頭本來就是這個慣例，另外兩組跟著走才一致）。三顆的 checked、圖示與 tooltip
由 `updateToolBarState()` **一起**重寫 —— 分開寫的話總有一顆會忘了跟上
（例如 F11 走鍵盤那條就不經過按鈕）。**一律接 `triggered` 而不是 `toggled`**：
後者連 `setChecked()` 也會發，`updateToolBarState()` 一寫就繞回 slot。

**自動重播**（`autoReplay_`，預設關）決定預覽動作要不要一直重播，一路傳到
`ModelController::startMotion(..., bool loop)` 的 `ACubismMotion::SetLoop`。
兩個要點：**每次起播都明寫 loop**（`motions_` 快取的是同一個 ACubismMotion 實例，
loop 是寫在物件上的狀態，不覆寫的話關掉之後再播同一段還是會循環）；
切換開關時會拿 `lastMotionGroup_` **當場重播一次**，不然按了要等下一次點清單
才看得出差別（按過「重置動作與表情」就清掉，否則切一下它會自己活過來）。

**重置動作與表情**（`ViewerWindow::resetPlayback()`，工具列第三顆）把畫面收回原狀：
動作走 `startIdleMotion()` 接回待機（**不是**寫死 `startMotion("Idle", …)` —— 理由同
`core/idle_motion_pick.h`），表情走 `applyExpressionByName(..., nullopt)`。
**表情一定要一起清** —— 那是跟動作平行的另外兩層（`.exp3.json` 走 Cubism 的表情管理、
虛擬表情走 `ParameterOverlay`），停動作一點都碰不到它，症狀是按了重置臉上那個笑
還掛著，得自己再去清單上點一次「（不套用表情）」。順手把兩份清單的游標也收掉
（表情回到第一列的「（不套用表情）」、動作清單取消選取），不然畫面上是素臉、
清單卻還反白著剛才那一項，等於自己製造兩份不一致的真相。
`setCurrentRow()`／`setCurrentItem()` 發的是 `currentItemChanged` 而不是
`itemClicked`／`itemActivated`，套用掛在後兩者上，所以不會繞回來。

版面另外三件事：中間是 `src/viewer/collapsible_splitter.h` 的 **`CollapsibleSplitter`** ——
手把（`createHandle()` 換成自家的 `QSplitterHandle` 子類別）中央多一顆箭頭按鈕，
一鍵把右邊的清單收起來／放回**收合前的原寬**（QSplitter 自己只能拖到 0，還原不回去，
所以要多記一個 `restoreWidth_`）。按鈕是手把的子 widget，自己吃自己的滑鼠事件，
按鈕以外的手把照樣拖得動，不必去攔 `mousePressEvent` 分辨「這一下是拖還是按」。
**F11 切換全螢幕**用 `QShortcut` 而不是 `keyPressEvent`：焦點多半在清單或畫布那些子
widget 上，主視窗的 `keyPressEvent` 根本收不到；進全螢幕順手把狀態列收起來。

**清單的停靠／浮動**（`ViewerWindow::docked_`，工具列右半邊那顆，預設停靠）：
那顆是 **checkable**（凹著＝清單正浮著）而不是每次換字的動作鈕 —— 工具列上的按鈕
會一直停在畫面上，用凹凸表示狀態比換字好認。**訊號接 `triggered` 而不是 `toggled`**：
後者連 `setChecked()` 也會發，`updateDockAction()` 一寫就繞回 `setDocked()`。
停靠＝清單佔走視窗一塊、畫布跟著縮小（本來的樣子）；浮動＝畫布吃滿整個視窗、
清單蓋在它上面，**拖手把只改浮層寬度，畫布一動也不動**。實作刻意只有一句話：
畫布永遠是 splitter 的第一格（手把要靠它才拖得動），浮動時只是在 splitter 每次排完版
之後，用 `eventFilter` 把畫布的 geometry 改回整條寬度；清單是後加入的兄弟，本來就疊在
畫布上面。於是不必動 parent、不必另外做一層透明佔位 widget，滑鼠命中判定也維持原生
行為（清單與手把不透明、照樣先吃到事件，其餘落到底下的畫布 —— 視線跟隨要用）。
兩個踩過的坑：**切換的兩個方向不對稱**，splitter 內部尺寸從頭到尾沒變過，
切成浮動時 `refresh()` 設回去的是同一個矩形，`setGeometry` 值沒變就不發 Resize、
`eventFilter` 也就永遠不會動（症狀是「按鈕按了沒反應」），所以那個方向要自己
`setGeometry` 撐開一次；清單要 `setAutoFillBackground(true)`，否則浮在模型上會整片透出去。
其他做法都試不得：另做一層透明佔位 widget 蓋住畫布的話，`WA_TransparentForMouseEvents`
會讓 `QWidget::childAt` 連整棵子樹一起跳過，手把就再也點不到；把畫布 `setParent` 出去
則是拿 QOpenGLWidget 的 GL context 去賭「同一個 top-level 不會重建」。

與桌寵共用的兩處新切法：`core/model_scanner.h` 的 `describeModel()`（單一入口版的
`scanModels`，兩條路共用同一段內容解析，清單才不會分岔）與
`live2d/action_player.h`（「一個名稱」→「Cubism 動作管理／合成動作／參數覆寫層」
三條路的分岔與收尾，`AppController` 走的也是這兩支）。
畫布是 **QOpenGLWidget** 而不是桌寵的 QOpenGLWindow —— 後者是為了「頂層透明視窗」
（M0 實測 QOpenGLWidget 在那個情境下合成不到螢幕），嵌在版面裡沒有那個限制；
Cubism 的 `DoDrawModel` 會存下並還原當前 FBO，所以畫進 widget 自己的 FBO 沒問題。

**最重要的一條規則**：84 個測試全部只連 `l2m_core`（見 `CMakeLists.txt` 的 `l2m_add_test()`）。
所以「決策邏輯放 `src/core/`，Qt/GL/OS 互動放其他目錄」不是風格建議，而是能不能被測到的分界。
專案裡已經有好幾處是為了這條而拆開的，且都在標頭註解裡寫明理由：
`core/tray_label.h`（vs `windows/tray.cpp`）、`core/autostart_command.h`（vs `platform/`）、
`core/model_commands.h`（`AppController` 的唯讀半邊）、`core/mcp_host.h`（`isAllowedOrigin` 不必開伺服器就能測）、
`core/mcp_tool_specs.h`（工具的描述與 schema 就是 AI 介面本身，必須可測）、
`core/settings_layout.h`（分頁 id 是 MCP 與 UI 之間的介面；開關表決定「哪個核取方塊寫哪個欄位」，
讀寫成對放一起才驗得到）、`core/config_patch.h`（patch JSON 的逃逸與數字格式）、
`core/voice_list.h`（語音清單的排序與過濾，vs 原本 `windows/tray.cpp` 的 static 函式）、
`core/gaze_director.h`（視線的追蹤權重 —— 滑鼠靜止、說話中、MCP 釘住、總開關關閉
四個理由匯進同一個 0..1 權重；各自寫 focus 會在 25 Hz 上逐幀互相覆寫，
而「靜止多久才回正」的邊界只有純函數化才驗得到）、
`core/persona.h`（角色描述的名稱規則與長度上限 —— 錯誤字串就是使用者與 AI 共用的 hint）、
`core/mcp_resources.h`（resources/list 與 read 的 JSON，只吃 `PersonaSnapshot` 不碰磁碟）、
`core/tts_http.h`（各家 TTS 服務的請求組裝；自訂端點的 `${TEXT}` 替換與編碼規則也在這裡，
錯一個位元組就是整句唸不出來，所以不能只靠真的架一台服務去試）、
`core/text_segments.h`（切句規則 —— 3.14 被切成兩半、URL 斷在中間都是踩過的坑）、
`core/tts_segment_pipeline.h`（句段管線的時序：誰先送出、順序會不會亂、取消之後還有沒有回呼）、
`core/audio_stream_format.h`（容器嗅探；認錯的後果是「有氣泡沒聲音」那種靜默失敗）、
`core/model_assets.h`（模型資源存取層 —— 資料夾模型與 zip 模型的 read／exists／list
必須一模一樣，分歧的症狀是「解壓縮就好、壓起來就壞」，只有把兩個實作放在同一支測試裡
逐一比對才擋得住）。
`core/model_regions.h`（點擊落在角色的哪個部位 —— `model3.json` 的 `HitAreas` 實測
2956 隻只有 34 隻有填（1.1%），所以「摸頭要播 touch_head」九成九得靠外接框的幾何推算，
而「頭佔多少」不能寫死：全身立繪 1/4、胸像一半，錯的那一邊就是摸頭播成摸肚子）、
`core/canvas_center.h`（畫布中心相對 moc3 原點的位移 —— 像素座標的 Y 由上往下、
模型座標的 Y 向上，正負號寫反就是把模型往壞的那一邊再推一次，而原點剛好在中央的
那 19/23 隻必須算出**精確的 0**，浮點誤差漏出來就是每一隻的構圖都跟從前差一點點）、
`core/texture_format.h`（貼圖解不出來時「為什麼」那句話 —— 副檔名比對的邊界
（大小寫、jpg/jpeg、沒有副檔名）錯一格就是叫人去修一個沒壞的檔案，或反過來
叫人裝一個用不上的外掛；`l2m_live2d` 連不進測試，支援清單因此由呼叫端注入）。
`core/sample_models.h`（一隻模型都沒有時去哪裡拿 —— 官網的語系路徑五個裡有三個是例外
（日文無前綴、簡中叫 zh-CHS、繁中根本沒有那個版本），用「locale 當前綴」的通則生成
就是給使用者一個 404；「該不該問」的閘門錯一格則是每次啟動都彈對話框，
或在開機自動啟動時當著登入畫面彈 modal）、
`core/layout_fit.h`（model3.json 的 `Layout` 能不能信 —— Framework 的位置算式假設
原點在畫布角落，對原點在正中央的模型會把畫布整個推掉半個身子；判別要把
`SetupFromLayout()` 的算式原樣重跑一遍，鍵的**出現順序**也算數，症狀見「幾個容易
誤解的子系統」那條）、
`core/png_probe.h`（PNG 簽章與 IHDR 的解析 —— 貼圖的 PNG 改走 libspng 之後
「這一張要不要進快路徑」每張都要判一次，判錯的兩個方向都是靜默失敗：把 JPEG
餵給 spng 只是白跑一趟，把 PNG 誤判成不是 PNG 卻會整批悄悄退回舊路徑，
畫面完全正常、一句警告都沒有，只是速度回到從前）、
`core/wind_targets.h`（環境風該吹哪些 PhysicsSetting —— 判別要粒子數與輸出參數 id
兩條一起看，只看粒子數會讓一部分模型「開了風身體大幅擺動」，症狀見「幾個容易誤解的
子系統」那條）。
`core/builtin_actions.h`（內建動作／表情的槽位解析 —— 「揮手」在官方模型上是
`ParamArmRA`、在使用者的魔女上卻是 `Param28`，中間那層「標準 id 優先 → cdi3 名稱
關鍵字補救」的比對規則錯一格就是整個動作對到別的部位，只有純函式化才驗得到）。
`core/external_model.h`（外部模型的 id 判定、路徑正規化，以及 Viewer ↔ 桌寵那條
QLocalSocket 的訊息格式 —— 三組錯了都是靜默失敗：判定錯就是拿絕對路徑去接
`modelsDir_` 而載到不存在的地方、正規化錯就是同一個檔案在清單上變成兩隻、
訊息切錯就是含空白的路徑整個功能不會動）、
`core/viewer_settings.h`（Viewer 的視窗幾何 —— `viewer.json` 的解析與
「螢幕變了怎麼把視窗擺回看得見的地方」。刻意不用 `QWidget::saveGeometry()` 那個
不透明 blob：本專案的設定檔一律是記事本改得動的 JSON，代價是多螢幕要自己想清楚，
而「上次開在副螢幕、這次副螢幕沒接」就是純函式化才驗得到的那一類）、
`core/weather_code.h`（WMO 碼表 0..99 **不連續**，65 大雨、66 凍雨、71 下雪
相鄰卻完全不同，用區間 if 去猜的症狀是「下雪天叫你帶傘」）、
`core/weather_http.h`（Open-Meteo 的 query 少一個欄位就是「從來不提醒下雨」，
沒有錯誤訊息只有沉默；當地時間字串要減掉 utc_offset_seconds 才是 UTC，
少這一步的話設在別的時區的地點會整整差好幾小時）、
`core/weather_alert.h`（**整個天氣功能的靈魂** —— 什麼時候該開口、
同一場雨怎麼只講一次，四條規則全在這裡）、
`core/idle_motion_pick.h`（動作播完之後接回哪一段 —— 待機群組名不是 model3.json
的規格而是官方範例的慣例，寫死 `"Idle"` 會讓「全部動作塞在同一個空字串群組、
待機只以檔名 `idle.motion3.json` 存在」的模型永遠接不回來，症狀見下面那條）、
`core/motion_curve_ids.h`（一支動作會驅動哪些參數／部件 —— `restoreBaseline()`
靠它決定哪些值留給新動作自己接手，漏收就是淡入那一秒看得到模型的編輯狀態）。
**新功能請照這個切法走。**

### 啟動順序（`src/app/main.cpp`）

順序敏感，改動前先讀該檔註解。關鍵約束：

1. `--mcp-stdio` 在最前面短路 —— 不建 QApplication、不拿 single-instance 鎖，同一個執行檔完全不同的程式。
2. `QSurfaceFormat`（alpha=8、**CompatibilityProfile**，因為 Cubism 的 GL renderer 用 client-side vertex array）必須在 `QApplication` 之前。
3. `setApplicationName("live2d_mate")` 必須在任何 `QStandardPaths` 查詢之前，否則路徑會少一層。
4. `app.disableHardwareAcceleration` 用手刻 yyjson 讀一次 —— `AA_UseSoftwareOpenGL` 得在 Qt 建 GL 前設定，
   而 `ConfigStore` 是 QObject 不能在 QApplication 前建。這份重複是刻意的。
5. 物件圖全是 `main()` 的 stack local，解構順序即建構的反序：
   `ConfigStore → CharacterWindow → WindowManager → BubbleWindow → AppController → TTS 一組 →
   McpTools/McpHttpServer → SettingsWindow → Tray`。
   `SettingsWindow` 持有 `McpHttpServer&`，一定要建在它之後（解構是反序，視窗才會先斷開 `statusChanged`）。
   TTS 引擎 vector 的**陣列順序就是 fallback 順序**（edge → gptsovits → voicebox → custom → sapi）。
   `custom` 不能排第一 —— `TtsManager::defaultEngineId()` 取的就是第一個。
6. `aboutToQuit` 裡 **`mcpServer.stop()` 必須排第一**，否則 httplib worker 會對著正在離開事件迴圈的 GUI 執行緒等滿 185 秒。

### 執行緒模型（只有三條）

1. **GUI 執行緒** —— 渲染、模型、設定、TTS 回呼、MCP 工具執行、計時器，全部在這裡。
2. **httplib 伺服器執行緒與其 worker pool**（`McpHttpServer::start`）。`bind_to_port()` 刻意先在 GUI 執行緒呼叫，
   讓綁定失敗能同步回報，之後才把 `listen_after_bind()` 丟到執行緒上。
3. **miniaudio 即時音訊執行緒** —— 只做「從環形緩衝 memcpy → 算 RMS → 寫五個 atomic」，
   絕不碰 Qt、不配置記憶體、不上鎖、**也不解碼**（解碼在 GUI 執行緒的 `AudioPlayer::pump()`）。
   播放結束是 GUI 端每 50 ms 輪詢 `lastAudioFrame_` 與 `framesRendered_` 判定的，
   **不是**從音訊執行緒發 Qt 訊號（會造成爆音）。

**MCP → GUI 的交接**（`McpHttpServer::dispatchTool` + `src/mcp/pending_call.h`）：
worker 執行緒建 `PendingCall`、用 functor 形式的 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 投給 `McpTools`，
然後**由 worker 去阻塞等結果，GUI 執行緒永遠不阻塞**。期限一般 20 秒、`speak(wait=true)` 與 `perform` 185 秒，
長呼叫最多 4 個併發。逾時的呼叫會標記 abandoned，遲到的 `complete()` 直接丟棄。

**持鎖不得 emit**（`src/mcp/mcp_http_server.h` 的 `setStatus()`，commit 565d928 修的就是這個）：
`statusChanged` 是 direct connection，接收端（`Tray::updateToolTip` / 設定視窗的 MCP 分頁）
會在 slot 裡回頭呼叫 `status()` 而重入同一個
非遞迴 `std::mutex` → MSVC 丟 `std::system_error` → 訊號發送路徑上沒人接 → `0xC0000409` 直接死。
**慣例：鎖的作用域只包住寫狀態，解鎖之後才 emit。** 任何「有鎖的狀態 + 訊號」都照這條走。

### 兩種相依方向

- **Qt signal** 用於向上／廣播通知：`AppController::stateChanged`、`WindowManager::boundsChanged`、
  `ConfigStore::changed`、`McpHttpServer::statusChanged`…
- **`main()` 指派的 public `std::function` 成員**用於注入能力，讓相依保持無環：
  `AppController::speakHandler / mouthOpenSource / …`、`CharacterWindow::onDragBy / clickThroughEnabled / …`、
  `Tray::Deps`、`SettingsWindow` 的 `VoiceDeps` 與 `SettingsContext`、
  `ModelController::earlyParameterHook / lateParameterHook / mouthOpenProvider`、`Idle*::Deps`。
  理由寫在 `windows/tray.h`：`AppController` 不該知道設定視窗與伺服器的存在；
  設定分頁也不該反過來認識 `SettingsWindow`，所以套用／回報能力是用 `SettingsContext` 注入的。
- 非同步一律是**傳 callback（`std::function<void(T)> done`）**，不用 future/promise。
  長命的非同步物件用 `QPointer<T> self` 護衛或自持 `deleteLater()`。

### 幾個容易誤解的子系統

- **貼圖的 PNG 不走 Qt 的解碼器**（`live2d/png_decoder.h`／`core/png_probe.h`／
  `cmake/FetchZlibNg.cmake`）。`DecodeTask` 先試 libspng + zlib-ng，回 null image
  才退回 `QImage::loadFromData` —— jpg／webp 與各種 PNG 變體都靠後者，所以兩條都要留。
  實測 LiveroiD_A-Y01 的單張 8192×16384 貼圖（30 MB → 512 MiB，i5-13500）：
  **671 ms → 309 ms（2.2x）**，而且輸出跟 Qt **逐位元組完全一致**（536,870,912 個
  位元組一個不差）。**「完全一致」的前提是 8 位元的 PNG** —— 16 位元的兩條路必然
  差最多 1（spng 是右移截斷、Qt 是 `div_257` 四捨五入），畫面上看不出來但別當成
  整條路徑的保證。幾個容易踩的點：
  **真正的瓶頸是 inflate 不是反濾波** —— 只做 inflate 不做反濾波，miniz／原版 zlib
  是 500 ms，整張解完是 671 ms，也就是九成時間在解壓縮。所以「libpng 沒開 SIMD」
  雖然是事實（qtbase 的 libpng CMakeLists 明寫 `PNG_ARM_NEON_OPT=0`，x86 那條要
  `PNG_INTEL_SSE` 而 Qt 沒定義），**那塊根本不是瓶頸**：libspng + miniz 實測 675 ms，
  跟 Qt 的 671 ms 落在誤差裡。換解碼器不換 inflate 完全是白做的。
  **zlib-ng 的指令集刻意壓到只剩 SSE2**（x86-64 的基準線）。用它的預設選項
  （AVX2 + AVX2VNNI + PCLMULQDQ + VPCLMULQDQ + AVX512）建出來的版本，在開發機
  （i5-13500，E-core 讓整顆 CPU 沒有 AVX-512）上一呼叫 `inflate()` 就 **0xC000001D
  illegal instruction 當場死，一句訊息都沒有**；逐項關掉確認 AVX2／SSE4.2／PCLMULQDQ
  都正常，問題在 VPCLMULQDQ 或 AVX2VNNI 的執行期偵測。開到 AVX2 是 277 ms（再快 11%），
  拿使用者的機器去賭那 11% 不值得 —— 症狀是「開起來就閃退、沒有任何線索」。
  `WITH_RUNTIME_CPU_DETECTION` 維持 ON：那正是「舊 CPU 也安全」的機制。
  **libdeflate 試過，排除** —— inflate 300 ms 比 miniz 快但輸給 zlib-ng，而且它
  **只有 one-shot API 沒有 streaming**，塞不進逐列解碼器：要先把 IDAT 串成一塊、
  再配一份完整的 537 MiB raw 緩衝，等於 `kDecodeBudgetBytes` 的併發預算翻倍。
  **miniz 留著不動**：那邊用的是 zip 容器（`mz_zip_reader_*`），三者不是二選一。
  **失敗一律回 null，不自己報錯** —— 診斷鏈在 `core/texture_format.h`，這裡橫插一腳
  只會多出兩套說法。
  **`SPNG_DECODE_TRNS` 不能省**：tRNS 是調色盤／灰階 PNG 表達透明的方式，
  不給旗標 spng 就當它不存在，而 libpng 是會套用的。
  **列間距要驗**：spng 寫的是緊密排列的 w×4，QImage 的 `bytesPerLine()` 32bpp 下
  算出來剛好一樣，但那是實作細節不是保證 —— 對不上就退回 QImage，總比把整張圖畫成斜的好。
  **尺寸探測走同一支 `readPngHeader`**，估併發用的尺寸才跟真的解出來的那張一致。
  spng／zlib-ng 都掛在 `L2M_NEEDS_LIVE2D` 底下，**CI 的測試建置（`L2M_BUILD_APP=OFF`）
  完全不會去抓 zlib-ng**。
- **模型可以是 zip 壓縮檔**（`core/model_assets.h`／`core/zip_archive.h`／`core/byte_source.h`）。
  掃描時遇到 `.zip` 就打開來看看裡面是不是模型包，是的話當成一般模型列出，**磁碟上不解壓縮**。
  幾個刻意的決定：
  **一個 zip = 一個模型**，`ModelInfo::id` 就是那個 zip 的相對路徑（`"Foo.zip"`），
  所以 `entryPathOf()` 仍然回傳真實存在的檔案路徑，`config.json` 的 `model.current` 也照舊。
  入口檔先找 zip 根目錄，找不到時**若剛好只有一個頂層資料夾就鑽進去**（Windows 右鍵
  「壓縮成 ZIP 檔」產生的就是這種多包一層的結構，不接的話十次有八次會踩到）；
  兩個以上頂層資料夾一律放棄。多包的那一層由 `ZipModelAssets` 吸收成前綴，上層看不到。
  **查找一律大小寫不敏感** —— zip 是大小寫敏感的容器，但 model3.json 的引用常跟實際檔名
  對不上，那種模型在 NTFS 上完全正常，壓成 zip 卻會整組資源靜默讀不到（模型照樣載入，
  只是什麼都不會動）。
  **條目名稱一律先解碼成 UTF-8**：ZIP 的 general purpose bit 11 有設才保證是 UTF-8，
  沒設時規格說 CP437，但實務上一半的工具寫 UTF-8 卻忘了標記，另一半（**Windows 檔案總管
  的「壓縮成 ZIP 檔」**）寫的是系統 ANSI 碼頁（正體中文 Windows ＝ CP950／Big5）。
  所以規則是「有旗標就信它 → 否則驗是不是合法 UTF-8 → 都不是才用本機碼頁解」，
  中間那一步不能省，否則已經是 UTF-8 的名字會被解成雙重編碼的亂碼。
  實測「藿藿.zip」踩到的三個症狀依序是：中文檔名的資源全部讀不到 →
  設定補全把非法 UTF-8 寫回 model3.json 讓 yyjson 解析失敗、整個模型被靜靜略過
  （使用者看到的就是「資料夾掃得到、zip 掃不到」）→ 那些位元組拿去建
  `std::filesystem::path`，**MSVC 會丟例外**，沒人接就整個行程 abort 成 0xC0000409。
  因此 `scanModels` 的 try 刻意包住整個候選的處理，而不是只包 `loadCubism4Settings`：
  一個壞掉的模型不准把整輪掃描帶走。**命名檔（annotations）改走 sidecar**：讀取時 zip 旁邊的
  `Foo.annotations.json` 優先，沒有才讀 zip 內建那一份；寫入永遠寫 sidecar，zip 保持唯讀。
  已知取捨：`FileByteSource` 在模型載入期間一直開著檔案，Windows 上那個 zip 要換掉模型
  才刪得動。
  miniz 刻意關掉 stdio，讀取一律走 `ByteSource` 的自訂 IO 回呼 —— 未來換成 HTTP Range
  來源時，`ZipArchive` 以上完全不用動。
- **模型可以在 models 目錄外面**（`core/external_model.h`）。Live2D Viewer 的
  「設定為桌寵」把看到的那一隻直接交給桌寵，於是 `config.model.current`
  **可以是一條絕對路徑**（`ModelInfo::id` 就是那條路徑，POSIX 斜線）。
  `AppController::entryPathOf()` 因此多一個分支；`switchModel()` 一行都沒改 ——
  外部模型只要進得了 `models_`，後面整條路跟內建模型一模一樣。
  幾個刻意的決定：
  **外部模型只活在記憶體裡**（`AppController::externalModels_`），config 不另外存清單。
  切到別隻之後它還在清單上（每次 `rescan()` 重新附上），但關掉 app 就沒了 ——
  這是「從檢視器試用一隻模型」的定位，不是第二個模型庫。
  **唯一活得過重開的是「關掉時正在用的那一隻」**：它在 `config.model.current` 裡，
  由 `rescan()` 開頭那段 `describeModel()` 補回清單。這一步不能省 ——
  少了它，用外部模型的桌寵重開之後會自己掉回第一隻內建模型。
  **一律接在掃描結果尾端**（同內建動作的理由：排前面會讓 `resolveModel` 的
  「名稱包含」比對先命中外部模型，models 目錄裡同名的那隻反而叫不動）。
  **models 目錄底下的模型不算外部模型**：`adoptExternalModel()` 先用
  `modelsDirRelativeId()` 還原成相對 id。少了這一步，在檢視器裡開自己 models
  目錄裡的模型（最順手的瀏覽目標）再按下去，清單上會多一隻名字一模一樣的分身，
  而且因為 config 記的是絕對路徑，每次重開都會再長回來。
  **檔案不見了不必特別處理**：`describeModel` 回 nullopt → 不進清單 →
  既有的「找不到就取第一個」fallback 自然接手（只在冷啟動那一次會驗；
  已經記進 `externalModels_` 的不會再驗，`rescan()` 才不必每次重讀磁碟。
  讀不到的那條路徑記在 `failedExternalId_`，否則掛在 `list_models` 上的
  `rescan()` 會被 AI 叫幾次就重試幾次）。
  行程之間走的是 **single-instance 那條 QLocalSocket**（`app/single_instance.h`），
  請求從「不看 payload、連上就當成 show」改成一行文字（`show` / `setmodel <path>`）
  並**回一行**（`ok` / `error <訊息>`）—— Viewer 要說得出到底成不成功。
  收端要讀到換行**或**讀到斷線才算一則：舊版送的 `show` 不帶換行。
  桌寵沒在跑時 Viewer 改用 `QProcess::startDetached` 啟動一個帶 `--set-model` 的，
  那條只知道行程生出來了。**已知取捨**：外部 zip 當桌寵時 `FileByteSource` 會一直
  開著那個檔案，要換掉模型才刪得動（同 zip 模型那條），而它放在使用者自己的目錄裡，
  比放在 `%APPDATA%` 底下更容易撞到。
- **moc3 的原點不保證在畫布正中央，所以 `draw()` 的 fit 只縮放是不夠的**
  （`ModelController::draw()` 的 `canvasCenterX_`／`canvasCenterY_`）。
  原點是作者在 Cubism 編輯器裡擺的那個十字，全身立繪多半擺在**腳底**，畫布整片往上長。
  官方範例只做「把畫布縮到視窗大小」不做平移，於是那種模型畫出來是「腳底貼在視窗正中央」，
  上半身整個被推到視窗上緣外面 —— 症狀是**畫面上只看得到下半身**，而且桌寵與檢視器一起中招
  （兩邊走的是同一支 `draw()`）。實測 Reverse:1999 的 `300301_hujisheng`：畫布 3508×4961、
  原點 (1754, 4663)、PixelsPerUnit 201.9，也就是原點離畫布底部只有 1.47 單位、離頂部 23.10 單位。
  修法是每幀在 fit 之後補一次平移，把**畫布中心**對到視窗中心
  （`Translate(-canvasCenterX_ * fitScale, -canvasCenterY_ * fitScale)`）。四個要點：
  **原點要跟 Core 拿**（Framework 只轉出了 canvas 尺寸與 PixelsPerUnit，
  原點沒有 getter —— `Live2D::Cubism::Core::csmReadCanvasInfo(_model->GetModel(), …)`）；
  **像素座標的 Y 是由上往下量的**，換成模型座標（Y 向上）時中心位移是 `(originY - height/2) / ppu`，
  正負號搞反的話會把模型往壞的那一邊再推一次；
  **每幀都要重設**，位移是「模型單位 × fit 的縮放」而縮放隨視窗大小變；
  **作者在 `Layout` 指定過位置就一步都不碰** —— `CubismMatrix44::Translate()` 是絕對指派，
  補下去等於把 `SetupFromLayout()` 擺好的位置蓋掉。判別的是**位置**（`x`／`y`／`center_x`／
  `center_y`／`top`／`bottom`／`left`／`right`）而不是「有沒有 Layout」：`SetupFromLayout()`
  只有碰到那八個鍵才寫平移，`width`／`height` 走的是 `Scale()`、平移從頭到尾是 0，
  所以拿「有 Layout」當條件會讓只寫了 `{"height": 2.2}` 的模型白白跳過置中 ——
  明明沒有任何東西會被蓋掉，卻讓「只看得到下半身」原封不動地留著。
  **但「作者寫了就照做」不夠** —— 那份 Layout 還得**真的能用**（`core/layout_fit.h`）。
  手邊 29 隻模型只有 2 隻有 Layout，而那 2 隻正是位置壞掉的那 2 隻（《原神》可莉
  `{"height":2.6,"bottom":2.0,"top":0.3}` 與派蒙 `{"height":2.2,"bottom":2.3}`，
  都是從別的桌寵程式搬過來的）。根因是 **`CubismModelMatrix` 的位置算式假設原點在
  畫布角落**（`Bottom(y)` 是 `TranslateY(y - 畫布高)`、`CenterY(y)` 是 `TranslateY(y - 畫布高/2)`），
  而這兩隻的原點都在畫布正中央 —— 同一條算式於是把畫布整個推掉半個身子。
  可莉還同時寫了互斥的 `top` 與 `bottom`，Framework 照 map 順序兩個都套、後面那個
  無聲地蓋掉前面那個。所以 `planLayout()` 把 `SetupFromLayout()` 的算式原樣重跑一遍，
  算出畫布在 view 座標的矩形，**落在標準視野 [-1,1]² 之內才採用**，否則整份 Layout
  都不採用、退回 fit + 置中（`SetWidth`／`SetHeight`／`Translate` 寫的都是絕對值，
  蓋得乾淨）。判別**刻意與視窗長寬比無關**：拿當下的視窗去算的話，同一隻模型會在拖動
  視窗邊緣的過程中在兩種構圖之間跳來跳去，而且檢視器與桌寵會給出不同的答案。
  順帶修掉同一條上的另一半：採用 Layout 時的投影原本一律 `Scale(1, viewAspect)`，
  等於假設視窗永遠是直的 —— 桌寵那個 400×600 的舞台剛好成立，但檢視器可以拉成任何形狀，
  橫的視窗會把看得見的高度壓到 2 以下，作者寫 `height` 2.x 就整個爆出去。
  改成跟其他分支一樣挑受限維度（橫向以高度為準、直向以寬度為準）。
  **對沒有 Layout 的 27 隻是完全的 no-op**：三個判斷全 false，走的分支一行都沒變。
  平移刻意放在 `_modelMatrix` 而不是 `projection`：`visibleBoundsView()` 用 `TransformX/Y`、
  `IsHit()` 用 `InvertTransformX/Y`，兩支都吃這個矩陣的平移，而 `screenToView()` 只除以
  projection 的縮放 —— projection 保持純縮放，點擊落點才跟畫面對得起來。
  桌寵那邊的 alpha 遮罩與腳底對齊都是量**畫完之後**的畫面，自動跟著走。
  **對原點本來就在中央的模型是實實在在的 no-op**（位移算出來剛好 0,0）：
  手邊 23 個 moc3 有 19 個是 0.5，另外 4 個是 0.16／0.20／0.20／0.94
  （0.94 那隻原點在腳底、0.16／0.20 那三隻在頭頂附近，同一條算式兩個方向都要對）。
- **穿透點擊不是 `WS_EX_TRANSPARENT`**，而是每幀從模型 alpha 重算視窗 region：
  `AlphaHitMask` 用 PBO + `glClientWaitSync(0)` **非同步**回讀 192×288 FBO（同步 `glReadPixels` 量到固定 ~6 ms），
  代價是遮罩慢一幀，所以位元遮罩**外擴 4 格（~10.4 px）**（2 格實測不夠）。
  `regionGate_` 由 `AppController::pollCursor` 依游標是否靠近驅動 —— 游標遠離時根本不套 region，角色才不會被切到。
- **點擊要播哪一段動作，九成九不是靠 `HitAreas` 決定的**
  （`core/interaction_logic.h` 的 `pickTapMotion` ＋ `core/model_regions.h` ＋
  `ModelController::tapBodyPart`）。規格上「哪一塊是頭」寫在 model3.json 的 `HitAreas`，
  但實測手邊 2956 個 model3.json **只有 34 個（1.1%）有填**，其餘連鍵都沒有或寫成 `[]` ——
  `hitTest()` 因此幾乎永遠回空陣列。所以部位改由**模型在畫面上的外接框**推算：
  `visibleBoundsView()` 掃可見 drawable 的頂點取 min/max，再用 `_modelMatrix` 轉成 view 座標
  （與 `CubismUserModel::IsHit` 的 `InvertTransform` 反向對稱），`regionAt()` 按相對高度切四段。
  **頭部佔比不能寫死**：全身立繪的頭約佔 1/4、胸像佔到一半，所以拿外接框長寬比當代理
  （`0.55 / aspect` 夾在 0.16~0.5）。判錯的代價很小 —— 最差是摸頭播成摸身體，而在這之前
  那一下**本來就是隨機播 `wedding` 或 `mail`**。
  挑動作時**群組名與動作檔名兩邊都比**：語料庫裡 1535 個（52%）模型把全部動作塞在同一個
  空字串群組裡（`"Motions": { "": [ {"File":"motions/touch_head.motion3.json"}, … ] }`），
  `touch_head` 只存在於檔名，只看群組名一個都對不上。`MotionGroupInfo::files` 掃描時本來就
  填好了，比對它就同時吃得下這種寫法與 215 個把 `touch_head` 取成群組名的正規寫法。
  順序是：作者標的 area 名稱 → 部位關鍵字（限「觸摸池」內）→ 觸摸池裡非 idle／drag 的 →
  觸摸池全部 → 挑不出來就**退回 `playRandomMotion`**（點了完全沒反應比播錯一段還糟）。
  `touch_idle*`／`touch_drag*` 刻意排到最後：前者是待機變體、後者是拖曳反應，都不是點擊該播的。
  `Chest` 的關鍵字最後一個是 `special`（碧藍航線全系列的「特殊觸摸」就是胸口那一下，
  而那個字本身沒有部位含意，所以排在真正的部位字之後當補救）。
  沒有 `files` 的群組一律略過 —— 那是 `core/builtin_actions.h` 合成的內建動作，
  model3.json 裡不存在這個群組，挑到了只會被 `GetMotionCount` 當場回絕。
  `Chest` 那一段（`touch_special` 的入口）**刻意切得窄**：開大的結果是整個上半身戳下去
  都播 special、`touch_body` 幾乎叫不出來。
  **挑不出來、或挑到的那一段播不起來（`preloadMotions` 會跳過 Meta 壞掉的 motion3.json，
  那個 index 在 `motions_` 裡根本不存在），一律退回 `playRandomMotion`。**
  **Live2D Viewer 走同一條路**（`ViewerCanvas::modelTapped` → `ViewerWindow::playTapMotion`），
  所以檢視器裡點到的那一段，就是「設定為桌寵」之後點下去會播的那一段；順手把清單的游標
  移到那一項，才答得出「剛剛那是哪一個動作」。共用的那一半刻意放在 `ModelController` 上
  而不是各寫一份：**Viewer 沒有 alpha 剪影可用**（畫布底色是不透明深灰），所以外接框一律
  走 drawable 頂點而不是 `AlphaHitMask`。
  也因為沒有那層形狀視窗，**Viewer 這端要自己擋掉「點在角色外面」** ——
  桌寵靠 alpha region 把剪影外的點擊整個穿透掉，Viewer 不擋的話點畫布右上角的深灰空白
  也會播摸頭（`regionAt` 會把框外的點夾回框內）。
- **`restoreBaseline()` 的 baseline 是 moc3 預設值，而那是作者的編輯狀態，不是「乾淨的靜止姿勢」**
  （`core/idle_motion_pick.h`／`core/motion_curve_ids.h`／`ModelController::restoreBaseline`）。
  很多模型在預設狀態下「所有肢體變體一起開著」—— 實測碧藍航線 40 隻裡有 13 隻（33%）如此，
  用 Cubism Core 直接讀 drawable 不透明度可以證實：`yichui_2` 的預設同時開著
  `foot`／`foot2`／`foot_r`／`foot_r3`（**4 隻腳**）、`aidang_2` 同時開著兩組手臂與三隻右手（**4 隻手**），
  而它的每一支動作都只開其中一組。所以還原 baseline 有兩個代價，兩個都修過：
  **① 沒有後續動作接手時會定格在那個狀態** —— 那批模型把 15 支動作全塞在**同一個空字串群組**裡
  （`"Motions": { "": [ … ] }`），待機只以檔名 `idle.motion3.json` 存在，寫死的
  `startMotion("Idle", …)` 每幀都回 false，於是「播完 touch 動作 → 還原成 4 隻腳 → 沒人接手」
  就一直掛在畫面上。改走 `pickIdleMotion()`：群組名 `Idle` → 檔名 `idle` → 群組名含 idle →
  檔名含 idle，四條都落空時**刻意連 baseline 都不還原**（丟回編輯狀態比停在最後一幀更糟）。
  Viewer 的「重置動作與表情」是同一個寫死字串，在那些模型上按了完全沒反應，一併改掉。
  **② 動作淡入的那一秒是從編輯狀態淡過去的** —— 所以起播前的還原改成「新動作自己會驅動的
  參數／部件不還原」（`motionDrivenIds()` 在 `preloadMotions()` 順手從同一份 JSON 解出來），
  交給它照 Cubism 原本的交叉淡入接手；新動作**不管**的參數才是真正的殘值，照樣清回預設。
  **③ 上面兩條修完，`aidang_2` 播 `home` 還是多一隻右手** —— 那一條的根因不在本專案，
  而是 Framework 把 `PartOpacity` 曲線當成參數在寫（`patches/cubism-framework-part-opacity.patch`，
  症狀、實測與 `GetParameterIndex` 為什麼永遠不回 -1 都寫在 patch 檔頭）。
  這類模型的換手臂／換腿有兩套機制：一套是參數（`ParamFootRWalk` 這種），一套是部件不透明度
  （`{"Target":"PartOpacity","Id":"Part31"}`）。前者本來就會動，後者整條靜靜地沒有作用，
  所以「腳」修好了「手」還在。**判斷是哪一套的方法**：用 Cubism Core 載入 moc3、套上動作在
  某個時間點的曲線值、讀 `csmGetDrawableOpacities`，再逐一把曲線拿掉看誰讓多出來的部位復活。
- **環境風是全域的一個力，所以「吹誰」要自己挑**（`core/ambient_wind.h` 算多大、
  `core/wind_targets.h` 挑吹誰、Framework patch `cubism-framework-wind-mask.patch` 負責照做）。
  Cubism 的 `Options.Wind` 所有 PhysicsSetting 一起吃，但模型裡有兩種 rig：會飄的
  **鏈**（頭髮、衣襬）與把 `ParamAngleX/Y/Z` 轉成 `ParamBodyAngleX/Y/Z` 的**角度跟隨器**。
  持續的風對跟隨器只是把它推到新的平衡角，再乘上作者寫的數十倍輸出倍率寫進身體參數 ——
  畫面上就是「身體搖得比頭髮還大，像站不穩」，關掉風就正常。
  判別**兩條規則缺一不可**（實測手邊 30 隻）：粒子數 < 3（單節擺錘）不吹；
  **輸出寫進那六個標準姿勢參數的也不吹，不管幾粒子** —— 30 隻裡有 4 隻把跟隨器
  做成 3~5 節（`Gan Yu` 的 PhysicsSetting2／3 各 5 粒子 → `ParamBodyAngleX/Y`，
  `椿`／`镜流`／`长离` 各 3 粒子），只有前者的話那 4 隻照樣抖。
  **不能拿輸出倍率當判別**（跟隨器 30~74、頭髮約 1 看起來很好用，但 `Gan Yu` 的頭髮
  也寫 30），也不能拿 setting 名稱或參數 id 猜（作者自訂的「身体x」「hf」）。
  遮罩在載入時算好存在 `ModelController::windMask_`，`Options.WindMask` 存的是
  **指標不是複本** —— Options 每幀 Get/Set 來回複製，放 csmVector 等於每幀兩次配置，
  而位元遮罩不夠用（實測有 58 個 PhysicsSetting 的模型）。
- **游標是 40 ms 全域輪詢**（不是視窗滑鼠事件），因為視線追蹤要跟到視窗外面；`lookAtPinned_` 防止 25 Hz 輪詢
  蓋掉 MCP 的 `look_at`。
- **視線不是直接跟隨游標，而是一個 0..1 的權重插值**（`core/gaze_director.h`）：
  `focus = 視窗中心 + (游標 - 視窗中心) × weight`。平時直視前方，滑鼠一動權重升起（`engageMs` 500 ms），
  靜止 `holdMs`（3 秒）後降回 0（`releaseMs` 2000 ms —— 刻意比 engage 慢四倍，
  「被吸引」該快、「失去興趣」該慢）。輸出再過一層 smoothstep，加上 `CubismTargetPoint`
  自帶的 ~0.35 秒慣性，共三層平滑。**說話期間正視前方走的是同一個權重**
  （原本的 `core/speaking_gaze.h` 已移除）—— 兩套都寫 focus 一定在 25 Hz 上互相覆寫。
  權重的推進**必須排在 `pollCursor` 的 2px 死區 return 之前**：游標靜止時死區以下整段不執行，
  回正動畫排在後面就永遠不會推進。手感常數寫死在 `GazeDirectorTuning`，照
  `AmbientWindTuning`／`DragSwingTuning` 的慣例不進 config。
- **`ModelController::update()` 有固定的 10 步順序**，`earlyParameterHook`（物理之前，讓頭髮衣服會跟著動）與
  `lateParameterHook`（眨眼／呼吸／視線之後，才不會被蓋掉）插在特定位置。
  參數覆寫要同時讀 `live2d/model_controller.h` 與 `live2d/parameter_overlay.h` 才看得懂。
- **有一批動作與表情不是模型做的，是掃描時合成的**（`core/builtin_actions.h`）。
  實務上大多數模型只綁一個 Idle，`list_motions` 給 AI 看的就是一行，想叫它揮手根本無從叫起。
  所以掃描時依模型的參數表現算出動作 `wave`／`nod`／`shake`／`tilt`／`grin`／`wink`／
  `look_away`／`sigh`／`doze`／`yawn`／`excited` 與表情 `smile`／`surprised`／`sleepy`／`sad`／`angry`，
  **接在模型自己的項目後面**併進 `ModelInfo::motions`／`expressions`，名字登記在
  `builtinMotions`／`builtinExpressions`。往後所有讀那兩個欄位的路徑（兩支 list JSON、
  `resolveMotion`／`resolveExpression`、命名 UI）因此免改就通了，兩支 list JSON 只多一個
  `"builtin": true` 欄位。幾個刻意的決定：
  **排在後面**（排前面會讓 `resolveMotion` 的前綴比對先命中合成的那些，作者做的反而叫不動）；
  **撞名一律跳過**（模型自己有 `wave` 時內建的那個永遠 resolve 不到，留著只是幽靈項目）；
  **數值用絕對值靠 Cubism 夾**（掃描期讀不到參數上下限 —— 範圍在 moc3 裡，cdi3 不帶 ——
  所以揮手直接寫 ±30，模型範圍小就夾成它的滿舵）；
  **清除內建表情是 `release` 而不是寫 0**（不像 `virtualExpressionParams`：`ParamEyeLOpen`
  的靜止值是 1，寫 0 會讓眼睛永遠閉著）；
  **會閉眼的那幾個一律是動作不是表情**（`grin` 與 `wink`）（表情掛著不走，眼睛被覆寫層釘在 0 就再也不會眨 ——
  覆寫在晚寫掛點、眨眼在它前面 —— 掛久了是睡臉；動作有時長，而且動作播放期間
  CubismEyeBlink 整個停用，見 `model_controller.cpp` 的 `!motionUpdated && _eyeBlink`，
  閉眼不會被眨回去。同理 `sleepy` 只半睜到 0.35、`sad`／`angry` 完全不碰眼睛開閉）；
  **`angry` 的 required 掛在眉毛角度上**（sad 與 angry 的差別全在 `ParamBrowLAngle` 的正負，
  沒有眉毛角度的模型只給 sad —— 兩個長得一模一樣的項目 AI 選哪個都一樣，比少一個更糟）；
  **`excited` 的身體彈跳是第一個「越過物理」的內建動作**（`BuiltinMotion::carryPastPhysics`）——
  「跳」沒有現成參數（Cubism 標準表裡沒有整體上下位移），畫面上做出「微蹲 → 伸展」的是
  `ParamBodyAngleY`，也就是「頭往下看時身體跟著蹲一點」的來源；但在很多模型上
  （March 7th 就是）它是 **physics-output**，由 `ParamAngleY` 經物理算出來，
  動作寫在 `update()` 第 1 步、物理在第 7 步，寫了會被整個蓋掉。
  所以 `SlotSpec::allowPhysicsOutput` 對身體角度開，並由 `builtinMotionFor` 把那些 id
  列進 `carryPastPhysics`，`playSynthesizedMotion` 收下後在**第 1.5 步記值、第 9 步（物理之後）寫回**
  —— 於是「身體在跳、頭一格都不碰」才成立（照一般做法只能甩頭去帶身體，那是點頭不是彈跳）。
  記值必須 gate 在 `motionUpdated` 上：動作播完後那裡讀到的是上一幀的物理輸出，
  寫回去等於把身體凍在最後那個姿勢；清單也只在動作**真的起播**時才換掉
  （Reserve 被拒時還在播的是舊動作）。因此 `excited` 的 required 掛在 `BodyAngleY` 上，
  沒有身體角度的模型不提供 —— 退化成「只有一張笑臉」沒有意義；
  **頭部角度也開了 `allowPhysicsOutput`**（同一條路，理由不同）——
  `nod`／`shake`／`tilt`／`look_away`／`sigh`／`doze` 是「只綁一個 Idle 的模型至少也拿得到」的保底那一批，
  前提是 `ParamAngleX/Y/Z` 寫得進去。但有一類模型（實測 `ariu`，VTube Studio 出身的很多都是）
  把頭的慣性做成 `ParamAngle{X,Y,Z}` **同時是物理 Input 也是 Output** 的自我回授 rig
  （`ariu.physics3.json` 的 `PhysicsSetting1~3`：in `ParamAngleY` → out `ParamBodyAngleY` + `ParamAngleY`），
  三個角度因此全被判成 physics-output、三個槽位一起空掉，那六個動作一次全滅 ——
  實測那隻模型的 `list_motions` 只剩 `excited`／`grin`／`wink`／`yawn` 四個。
  開了之後回到十個。這一類模型走 `carryPastPhysics` 反而是最好的結果：物理**之前**寫進去的值
  照樣餵給頭髮那 11 組鏈（它們的 Input 就是 `ParamAngle*`），頭髮跟著甩；物理**之後**再寫回曲線值，
  頭本身不會被慣性回授拖成軟綿綿的半套動作。**對絕大多數模型這是完全的 no-op** ——
  `ParamAngle*` 不是 physics-output 時 `carryPastPhysics` 就是空的，一行行為都沒變。
  （`wave` 不在這一批裡：它要的是手臂參數，`ariu` 一個都沒有，跟物理無關。）
  **開了之後槽位表的順序就變成有意義的**：`BodyAngleY`／`BodyAngleZ` 必須排在
  `AngleX/Y/Z` **前面**。關鍵字比對是不分大小寫的**子字串**、槽位又先到先得，
  而 `"bodyangley"` 本身就含有 `"angley"` —— 排在後面的話，一隻「沒有標準 id
  `ParamBodyAngleY`、只有 cdi3 名稱寫成 `BodyAngleY`」的模型會被 `AngleY` 先搶走：
  `excited` 整個消失，`nod`／`sigh`／`doze` 還把點頭曲線寫到**身體**參數上，
  動作照播、部位全錯，一句錯誤訊息都沒有。（開 `allowPhysicsOutput` 之前這順序無所謂 ——
  那時 `AngleY` 遇到 physics-output 的身體參數會直接跳過，剛好讓 `BodyAngleY` 撿到。）
  另一個副作用刻意留著：越過物理的晚寫是 `SetParameterValue`（覆蓋），而視線加成與
  拖曳搖晃都是同一組 `ParamAngle*` 上的 `AddParameterValue` —— 所以在那一類模型上，
  那六個動作播放期間頭不跟游標（`doze` 是循環的，等於整段打瞌睡都不看人）。
  身體角度那條沒這問題（`ParamBodyAngle*` 上沒有視線加成）。
  **`doze` 是唯一循環的內建動作**（`MotionDef::loop`，一路傳到 `playSynthesizedMotion`）——
  它不自己結束，靠下一個動作蓋掉或閒置復原的 `stopLoopingAiMotion()` 收掉，
  所以它的關鍵影格首尾必須同值，否則每一圈的接點都會跳一下。
  **播動作一律走 `AppController::startMotionByName()`** —— 內建群組在 model3.json 裡不存在，
  直接呼叫 `startMotion()` 會被 `GetMotionCount` 當場回絕而靜默沒反應
  （`playRandomMotion` 就是踩這個的，點角色與閒置表演都經過它）。
  已知限制：**沒有 cdi3.json 的模型一個內建都不會有**，掃描期讀不到參數表。
- **天氣只在「壞天氣要來」時才讓桌寵開口**（`core/weather_alert.h`／`media/weather_service.h`）。
  資料源是 Open-Meteo（免金鑰、免註冊，一支 GET 就有逐小時降水機率 —— 任何要先去申請
  API key 的來源都會讓這個功能對九成的人不存在）；地點沒設定時用一次 IP 粗定位補上，
  結果寫回 config 就不再打。**服務本身不碰 `ConfigStore`**，定位成功發
  `locationResolved` 由 `main()` 寫回，否則「服務寫 config → changed → applyConfig →
  服務再寫」很容易在某個分支上真的繞起來。
  產品規則只有一條：**主動開口只留給「使用者會因此改變行動」的天氣**（雨／雪／雷雨／
  極端高低溫／強風），其餘時候天氣只當 LLM 的背景知識（`LlmBehaviorPromptInput::weather`，
  每一輪都帶）。四條決策規則寫在 `weather_alert.h` 的檔頭，其中兩條是一組的：
  **事件已經開始就不提醒**（窗外正在下的雨不需要人講）＋
  **去重鍵用事件起點**（`"rain@2026-09-02T15:00"`）—— 只留一條都會讓同一場雨被唸到停為止。
  講過的鍵是**一個集合**（`AppController::announcedWeatherKeys_`）而不是一個字串：
  只留最後一個時，「+1h 下雨」與「+3h 寒流」會互相把對方的記錄擠掉而每 5 秒無限乒乓；
  config 的 `weather.lastAlertKey` 只負責跨重啟。
  **鍵是在規劃回呼裡、確定產出了步驟之後才記的** —— LLM 失敗會退回規則版，
  而規則版在台詞區空著時整輪安靜，先記鍵等於把那一場天氣永遠跳過。
  觸發點只有一個：`AppController::checkWeatherAlert()` 掛在 5 秒的在席心跳上，
  **每次都重算、不排隊也不記狀態**，於是「離座期間產生的預警」自然變成
  「回座時若還沒開始就照樣講、已經開始就自己消失」，不需要任何過期邏輯。
  規則版的台詞來自角色卡的 `# Weather Alert` 區（行首前綴 `rain:` / `snow:` / `thunder:` /
  `hot:` / `cold:` / `wind:`，解析規則與 `# Greeting by Time` 一模一樣）。
  **啟動的歡迎詞會等天氣有結果才講**（`AppController::greetOnLaunch` 的 `weatherReady`
  掛鉤，成功與失敗都算有結果），這樣 LLM 才有天氣可以拿來打招呼；
  但**等待有 10 秒上限** —— IP 定位與抓預報是兩趟往返、逾時加起來最壞 25 秒，
  為了天氣讓桌寵開機後悶那麼久不打招呼比沒帶到天氣更糟。
  天氣沒啟用時那個掛鉤永遠為真，整段等於不存在。
  溫度**一律以攝氏抓取與判斷**，華氏只在組字串時換算 —— 讓門檻跟著單位走等於每個門檻分兩套。
- **TTS 引擎只交出音訊位元組，絕不自己播放**（`core/tts_types.h`）—— 嘴形同步需要波形。所有回呼都在 GUI 執行緒。
  介面是串流的：`synthesize(text, options, sink)` 回一個可 `cancel()` 的 handle，
  `sink.onChunk` 可以被呼叫很多次。`streams()` 為 false 的引擎由 `main.cpp` 包一層
  `SegmentedTtsEngine`（文字切句 → 逐句合成 → 邊播邊合成下一句）。
- **播放器是「PCM 環形緩衝 + 顯式 EOS」**（`media/audio_player.h`）。舊版把「解碼器短讀」
  直接當成「播完了」，串流之下短讀是 **underrun**（合成或網路跟不上），誤判會收掉氣泡、
  提早回覆 `speak(wait=true)`、還會跳去講下一句 —— 所以這兩件事拆成
  `producerEos_` × 「環是不是空的」。結束改用「已播出 frame 數 ≥ 最後一個真音訊 + 裝置尾端」
  判定，句尾不再被切掉一個 device period。`speaking()`（整句期間恆為 true）與
  `hasAudio()`（此刻環裡有沒有料）刻意分開：口型同步接的是前者，缺料時嘴巴才不會被
  AI 的 `set_parameters` 搶回去。
- **MP3 串流用 dr_mp3 的 push API**（`media/mp3_push_decoder.h` 的 shim）而不是高階的
  `ma_decoder`：後者資料不足時會把 `atEnd` 設成 **sticky** 的 true，而且補料門檻是 16 KB
  —— 在 Edge 的 24 kHz/48 kbps 上等於要先緩衝 2.7 秒才敢解第一個 frame。理由與行號寫在
  `miniaudio_impl.c`。
- **句段管線預設不預取**（`core/tts_segment_pipeline.h`）：第 N+1 句的請求在第 N 句
  「交給播放器」那一刻才送出，合成與播放仍然完全重疊，但同一時間只有一個請求在飛。
  實測本機 GPT-SoVITS 上同時送兩句會互相搶資源，第一句從 4.1 秒變 8.2 秒。
- **內建 LLM（第一期：自主行為大腦）**。`behaviorPlanner` 掛鉤有兩個腦：LLM 版
  （`src/llm/llm_behavior_planner.h`，`llm.enabled && llm.driveIdle` 且設定齊全時接手
  idle／welcome／breakReminder）與規則版 `IdleDirector`（永遠的保底）。產品鐵律：
  **LLM 永遠不准讓桌寵僵住** —— 失敗／逾時／解析不出／cooldown 中一律退回規則版；
  `petted` 一律不上 LLM（被摸的當下等網路往返很怪）。協定是 OpenAI-compatible
  （一個 baseUrl 同吃 Ollama／LM Studio／多數雲端）＋ Anthropic 原生，兩家共用
  `src/llm/llm_engine_http.*` 一份搬運碼；會錯的邏輯全在 core：請求組裝
  `core/llm_http.h`（`llmConfigIssue` 的錯誤句＝設定頁紅字，同一個真相來源）、
  SSE 增量解析 `core/sse_parser.h`（餵任意位元組切片）、delta 累積
  `core/llm_stream_accumulator.h`（tool-call 碎片重組最容易錯）、prompt 與回應解析
  `core/llm_behavior_prompt.h`（輸出就是 `PerformStep` 同一份 schema，走
  `parsePerformSteps` 同源驗證後再過濾 —— parameters/animate 不開放給自主表演，
  `move` 則看 `parseBehaviorSteps` 的 `allowMove`）。
  **移動的閘門是兩層，共用 `core/autonomy_gates.h` 的 `autonomousMoveAllowed()`**
  （`autonomy.move`／`!interaction.lockPosition`／`interaction.dragMove` 三者皆備）：
  規劃前算成 `IdleWorld::canMove` 餵給規劃器（LLM 那條再由 planner 一路帶成
  `allowMove`，重試也用同一份、不現讀，prompt 說的話才不會跟過濾規則對不起來），
  執行前由 `AppController::runAutonomous()` **現讀再擋一次**。
  第二層不能省：LLM 從送出到回覆隔了一整趟網路往返（暫時性失敗還多等 15 秒重試），
  表演本身又可能夾著 `wait` 拖上好幾分鐘，這中間使用者完全來得及把開關關掉；
  而 `runAutonomous` 是所有規劃器共用的唯一漏斗。
  第一層也不能省：`move` 步驟被 `parseBehaviorSteps` 補上 `glideMs` 之後走的是
  `PerformRunner` → **`glideTo()`**（只有 `preset` 形狀才落到 `moveTo()`），
  兩支都不看設定，光靠 prompt 裡的 `can_move` 拜託模型是擋不住的 ——
  症狀就是「隨機移動明明關著（甚至位置鎖著），桌寵還是自己散步」。
  規則版 `IdleDirector` 從第一天就守著 `world.canMove`，只有 LLM 那條路沒有。
  system prompt 裡 `move` 的格式宣告也跟著 `canMove` 條件式列出（沒看過格式就不太會
  生出來），但那只是省模型的 token，**不是**防線。
  **MCP 不走這條**：AI 明確下的 `move_to`／`perform` 是使用者的 AI 主動要求，
  不是自主行為，照舊直接進 `moveTo()`。
  `HttpJson::postStream` 的逾時是**停滯逾時**（每收到 chunk 重算），不是整體逾時 ——
  本機推論慢慢吐 token 十分鐘也不算逾時。config 的 `llm` section 欄位刻意全攤平
  （兩層 patch 限制），設定頁連線欄位走「套用」按鈕（`llmPatch` 整包送＋
  `llmFieldsEqual` 決定亮不亮，照抄 tts.custom 前例）。雲端計費節流：
  `idleLlmCooldownMs`（預設 5 分鐘）之間的輪次走規則版，調 0 = LLM 全接管。
  **踩過的坑：moc 的 lexer 解不動 raw string 裡的 `\"`** —— 測試檔裡含跳脫引號的
  JSON 得用傳統字串常值，否則整個測試類別被靜默吃掉（.moc 產出 0 位元組、
  連結時 metaObject 全數 unresolved）。
  **第二期（記憶與角色輔助）**：長期記憶是 `%APPDATA%/live2d_mate/memory/<角色名>.md`
  純文字檔（沿用 persona 的「記事本改得動」哲學；讀取直接重用 `readPersona` 換目錄），
  `clampPersonaMemory`（`core/persona_memory.h`）夾 2000 字後整段進行為大腦的
  system prompt；防重複是 planner 在記憶體裡記最近 8 句 LLM 台詞餵回 prompt
  （無狀態 API 不會自己記得）。角色分頁的「AI 擴寫」走 `core/persona_generate.h`
  的 **PersonaGenerateSession**：同一場對話分七階段各生成一區（描述→台詞→歡迎詞→
  時段問候→久坐提醒→摸摸反應→天氣預警），每階段只要求輸出純內容 —— 一次產整份 markdown
  的第一版死在小模型的標題五花八門上，分階段讓標題解析整個問題不存在。
  「同一場對話」靠把已完成階段的問答附回每次請求模擬；accept() 剝列表符號、
  丟超長行、描述超限報錯（與 `personaDocIssue` 同源）。進度框是確定進度
  （N/7，可取消），產出先進預覽對話框、再填編輯器，**存檔永遠是使用者按的那一下**。
  **角色分頁的暫存鐵律**：沒有輸入框的區塊要在 loadIntoEditor 暫存、
  docFromEditors 寫回 —— 漏一個，按一次儲存就把使用者手寫的那一區靜默砍掉
  （第一期加新區塊時 `# Break Reminder`／`# Petted` 真的踩過）。七個具名區塊
  現在都有輸入框（右半邊的編輯區包在 QScrollArea 裡才塞得下），只剩
  `reserved`（保留字清單裡還沒有對應欄位的區塊）還走這條暫存路徑。
- **「AI 留下的狀態」有兩條復原倒數，共用同一個 `resetToIdle()`**（`core/idle.h`）。
  `IdleReset` 是「沒人理它」的尺度（`idle.resetMs`，預設兩分鐘），使用者互動也算一次活動；
  `McpQuietReset` 只在 AI 真的下過指令時才倒數（`kMcpQuietResetMs`，5 秒），觸發一次就熄火。
  後者的進場點只有一個：`McpTools::invoke()` 開頭的 `notifyMcpCommand()`，所以 24 個工具
  （含唯讀的 `get_state`／`list_*`，還有 `schedule` 到期的 `invokeDetached`）都會重新給滿。
  **倒數必須從「講完」起算而不是「指令送出」**，AI 的典型序列就是 `set_expression` 接一句
  長台詞。靠兩層保險：① `SpeechController::finished`（**佇列真的清空才發**，連續 `speak`
  中間不會落下來）接到 `touchIdle()`，在那裡把倒數重新給滿；② 說話期間 `isBusy()` 為真，
  `IdleReset::fire()` 本來就是延後重排而不是硬清。只留①會被「合成中還沒開口」的空窗鑽過去，
  只留②則會用掉說話前剩下的殘值。`isBusy()` 也把「進行中的 `perform`」算成忙（`performActive_`），
  不然步驟之間的 `wait` 沒有任何活動訊號，復原會在表演中途把第一步的表情抽掉。
- **氣泡是獨立的 top-level QWidget**，不能畫在角色視窗裡（會被 alpha region 切掉，且常常超出 400×600 舞台）。
  字型要用 `QFont::setFamilies` 給候選清單並以 `QGuiApplication::font().family()` 收尾 ——
  寫死一個機器上不存在的家族會讓 DirectWrite 每次都走 fallback 並丟 first-chance 例外。
  **錨點是模型的頭頂，不是視窗頂端**（`core/bubble_placement.h` 的 `bubbleAnchorRect`）：
  角色視窗是固定 400×600 比例的舞台，模型在裡面上下留白多少由 moc3 畫布與
  `model3.json` 的 `Layout` 決定、app 蓋不掉 —— 拿視窗頂端當錨點的話，畫布上方
  留一大片空白的模型（全身立繪很常見）氣泡會浮在頭頂老遠的地方，只能靠
  `tts.bubbleOffsetY` 一隻一隻手動補。上下緣由 `AlphaHitMask` 的同一份 alpha 回讀量出來
  （`topNormalizedY()`／`bottomNormalizedY()`，掃描與換算在 `core/bottom_align.h`），
  於是 `offsetY = 0` 剛好是「氣泡底邊碰到頭頂」。三個刻意的決定：
  **只在載入模型時量一次**（`AppController::onMaskUpdated` 的 one-shot，與腳底對齊同一個入口）——
  遮罩最快每幀就更新一次（游標靠近時），跟著逐次重擺的話舉手、甩頭都會讓氣泡上下跳；
  **只動垂直方向**（水平仍是整個視窗的中心，模型本來就被 fit() 置中，水平量測改不了多少
  卻會讓角色左右擺動時氣泡跟著漂）；**量不到就退回整個視窗**（顛倒、太薄、遮罩還沒好），
  而且切模型量測失敗時要明寫補一次 (0, 1)，否則氣泡會一直貼著**上一隻模型**的頭頂高度。
  **`tts.bubbleOffsetY` 的語意跟著改了**：從前錨的是視窗的頂端與底端，所以那些
  「拿它一隻一隻手動補留白」的舊設定值升級後會**再往角色身上推一次**（例如 -180 會壓在臉上），
  要自己歸零；`placeBubble` 的下界 `-character.height` 也從視窗高度縮成模型的視覺高度。
  刻意不做一次性遷移 —— 預設值是 0，而寫進一個只用一次的 config 旗標比在這裡寫清楚更貴。
- **設定的 UI 是 `windows/settings/`**（`SettingsWindow` ＝ QTabWidget 七分頁：
  一般／模型／角色／語音／MCP／LLM／關於，
  骨架走 Qt Designer）。系統匣只剩高頻的一次性操作：大小／透明度／位置／設定／離開，
  仍然是每次 `stateChanged` 整份重建（150 ms 節流、選單可見時延後）。
  **設定視窗與系統匣是同一份設定的兩個 view**，兩邊都即時套用。**兩處例外，各有各的理由**：
  MCP 分頁的「套用並重新啟動」—— host 與 token 有跨欄位驗證（非 loopback 一定要 token），
  即時套用會在使用者還沒打 token 時就把控制權開到區網上，而且每個鍵擊都重啟一次 httplib 伺服器；
  語音分頁的「自定義語音設定」則是**自由文字欄位沒有可靠的提交點**（見下）。
- **設定分頁是延遲建立的**：`SettingsWindow::open()` 只 refresh 目前那一頁，其餘等 `currentChanged` 才建。
  MCP 的連線片段與命名區都是上百個 widget。分頁的 `refresh()` 沒有護欄（由視窗主動呼叫），
  掛在訊號上的是 `refreshIfActive()`（有 `isVisible()` 護欄）。
- **設定分頁的 `updating_` 旗標不能省**：`refresh()` 的 `setChecked()` 會發 `toggled` → patch →
  `ConfigStore::changed` → `refresh()` → … 繞不完。`ConfigStore`「序列化結果真的有變才 emit」
  只擋得住整數與布林，語速／音量是滑桿值除以 100，浮點往返誤差會讓它每一圈都「真的有變」。
- **GPT-SoVITS 與 Voicebox 的伺服器位址有輸入框**（設定 → 語音 → 伺服器位址）：
  兩個 `baseUrl` 填得下區網 IP 與埠，所以服務不必跑在本機（引擎顯示名也就不再帶
  「本機服務」尾巴）。兩個位址共用一顆「套用」按鈕、送同一份 patch
  （`core/config_patch.h` 的 `ttsLocalServerPatch`）—— 分兩包送會讓 ConfigStore
  在中途以半套設定重跑一輪。**內層一定要整包送**：漏掉 `gptsovits.presets`
  就是把使用者手寫的音色預設整組刪掉（`parseConfig` 補回的預設值是空陣列）。
  套用成功後一定要 `redetect()`，不然可用性與語音清單還是舊主機的答案。
  另外**引擎不可用不再 disable 下拉項目**，只加「（不可用）」後綴 —— 偵測只是
  「現在連不連得上」的快照，而使用者常常先填位址、再去把服務開起來。
- **「自定義語音」是 UI 最完整的引擎專屬設定**（設定 → 語音 → 自定義語音設定）：
  使用者自己填 URL／方法／參數樣板／標頭，參數裡的 `${TEXT}` 會被換成要唸的句子。
  幾個刻意的決定：組裝全在 `core/tts_http.h` 的純函式（URL 一律百分比編碼，
  **即使方法是 POST(JSON) 也一樣**；參數則依方法在百分比與 JSON 逃逸之間切換）；
  `isAvailable` **永遠回 true 且不打網路** —— 不只是因為任意端點沒有 health path
  （拿合成端點探測等於對計費 API 每次扣錢），更因為「沒填 URL 就不可用」會讓引擎在語音分頁
  **選不了**，使用者也就沒辦法試聽自己剛填的設定。
  `listVoices` 回空清單且**不算錯誤**。設定不完整的回報一律留到 `synthesize`。
  設定頁的紅字提示與合成失敗的訊息**是同一句**（都來自 `buildCustomTtsRequest().error`）。
  那組 GroupBox **常駐顯示**（不依 engine 切可見性），而且是這一頁唯一**不即時套用**的區塊：
  改動累積在表單裡，按右下角的「套用」才寫進 config。`customParams`／`customHeaders` 是
  `QPlainTextEdit`，沒有 `editingFinished` 這種可靠的提交點，原本的 400 ms 防抖＋`hideEvent`
  補寫會讓「改了不生效，要把引擎切走再切回來」變成常態。按鈕只在 `customDirty()`
  （表單 vs 設定，每次現算，用 `core/config_patch.h` 的 `customTtsFieldsEqual`）為真時亮起；
  設定不完整照樣可以套用，只出紅字。理由與三個具體症狀寫在 `voice_page.h` 的第 4 點。
  只吃「回應 body 直接就是音訊」的端點 —— miniaudio 只解得了 wav/mp3/flac，
  其餘在 `customTtsAudioError` 就擋掉，不讓它靜默失敗成「有氣泡沒聲音」。
- **`tts.custom` 的 patch 一定要整包送**（`ttsCustomPatch`）：`ConfigStore::patch()` 的合併只有兩層，
  內層是整個覆蓋，少送的欄位會被 `parseConfig` 補回預設值而不是保留原值。
- **語音清單只在三處重查**（第一次開語音分頁／換引擎／按「重新偵測」）。掛在 `refresh()` 或
  `ConfigStore::changed` 上的話，說一句話就打一輪網路（Edge 的可用性偵測要連線）。
  非同步回覆要比對送出時的引擎 id，否則快速換兩次引擎會讓舊清單蓋掉新的。
- **`ConfigStore` 寫入 debounce 300 ms**，離開前 `flush()`。`patch()` 吃兩層 JSON 物件字串（`{"model":{"scale":1.5}}`），
  合併後重新驗證，序列化結果真的有變才 emit `changed`。config.json 壞掉會改名成 `config.bak.json` 並重建預設值。
- **Cubism Framework 5-r.5 在執行期從磁碟載入 shader 原始碼**：開發時用編譯期的 `L2M_CUBISM_SHADER_DIR`
  指向 FetchContent 原始碼樹，部署時要把 shader 目錄複製成執行檔旁的 `FrameworkShaders/`。

### 新增 MCP 工具

目前**剛好 24 個**。要同時改三處，缺一不可：
`src/core/mcp_tool_specs.cpp`（描述與 schema）、`src/mcp/mcp_tools.cpp`（`if (tool == …)` 分派鏈）、
`tests/test_mcp_tools.cpp`（斷言總數為 24，並釘住 6 個唯讀工具的清單）。
`tests/test_persona.cpp` 另外釘住「沒有套用角色時，每個工具的 description 與 spec 一字不差」，
所以新工具的描述改動也會經過那裡。

### 角色描述（persona）送到 AI 的三條通道

`personas/<名稱>.md` 是純文字檔，套用中的那一份要送到 AI 面前，而三條通道的性質不同：

| 通道 | AI 一定讀得到 | 換角色後即時生效 |
|---|---|---|
| `initialize` 的 `instructions`（全文） | 是（進 host 的 system prompt） | 否，要重連 |
| `speak` / `perform` 的 description（角色名） | 是（工具表也在 context） | 伺服器端是即時的，但 client 通常整場只拉一次 |
| `resources/list` + `resources/read`（全文與清單） | 否，要 client 主動撈 | **是**（client 端發起） |

根因是這台伺服器是 **POST-only 的 httplib，沒有 SSE**，推不了 `notifications/*`。
所以角色分頁按「使用此角色」之後會明講「已連線的 AI 要重新連線才會讀到」。

`PersonaSnapshot` 由 GUI 執行緒在 `AppController::personaChanged` 時推給 `McpHttpServer::setPersona()`，
httplib 的 worker 只讀鎖內複製出來的複本 —— **`setPersona` 純寫狀態不發訊號**，
所以不會踩到 `setStatus()` 那個重入死鎖。

**產品規則**：失敗的 `CommandResult` 一定要附 `hint` 並列出有效選項（`core/command_result.h`）。
這是 AI 自我修正的機制 —— 讀完 hint 就能重試，不必再打一輪 `list_*`；系統匣的錯誤對話框吃同一組字串。

## 慣例

風格由根目錄的 `.clang-format` 定義（Google 為底、逐項實測校準過），VSCode 存檔時的
Format Code 走的就是它 —— **改動請照它的產出走**，手寫成別的樣子只會在下次存檔被改回來。
設定檔本身寫了每一項的理由與實測數字，尤其是「為什麼不重排註解」「為什麼不排序 include」。
以下是同一套規則的人話版：

- **縮排 2 空白**，無 tab；建構子初始化列表與續行都縮 2（不是 Google 的 4）；行寬 200。
- **註解一律繁體中文**（MSVC 靠 `/utf-8` 支撐）。標頭開頭的區塊註解要寫「這是什麼／
  為什麼這樣設計」，常常帶實測數字或某個具體 bug。**這就是本專案的文件，請維持同樣密度。**
- **面向使用者／AI 的字串一律英文**（`core/model_commands.h` 明文規定）。UI 字串走 `i18n::translate`，
  鍵值在 `i18n/{en,ja,ko,zh-CN,zh-TW}.json`（執行期 JSON，刻意不用 Qt Linguist）。
  `qDebug`/`qWarning` 則是中文並帶 `[mcp]`、`[tts]`、`[live2d]`、`[config]`、`[perf]` 之類的子系統前綴。
- 檔名 `snake_case.{h,cpp}`、`#pragma once`、平台實作用 `_win.cpp` 後綴。
- **Qt Designer（`.ui`）只用在 `src/windows/settings/`**，`AUTOUIC` 也只開在 `live2d_mate` 這個 target 上
  （`l2m_core` 與 84 個測試沒有 `.ui`，開全域只是每次 configure 多跑一次掃描）。
  `.ui` 與 `.cpp` **同層**放置，AUTOUIC 預設就找得到，不必設 `AUTOUIC_SEARCH_PATHS`；
  `.ui` 的 `<class>`、檔名、C++ 類別名三者一致。`.gitattributes` 把 `*.ui` 釘成 LF。
  **`.ui` 裡不寫註解**（Designer 重存會刪掉），說明寫在對應 `.h` 的區塊註解裡，
  並註明哪些控制項是 Designer 靜態的、哪些由程式碼動態產生。
  **`.ui` 的 `text`/`title` 一律填該項目的 i18n key 本身**（例如 `settings.general.alwaysOnTop`）：
  本專案的 i18n 是執行期 JSON 不是 Qt Linguist，沒有 `retranslateUi`；實際文字由手寫的
  `retranslate()` 覆蓋，漏寫時畫面上直接顯示 raw key，跑一次就發現。
  `Ui::X` 用 `std::unique_ptr` 持有，**解構子必須離線定義**（不完整型別）。
- 類別 `PascalCase`、成員**尾底線** `foo_`、常數 `kCamelCase`、新寫的列舉用 `enum class`。
- 命名空間 `l2m`（另有 `l2m::platform`、`l2m::i18n`、`l2m::jsonu`、`l2m::strutil`、`l2m::edge`），
  結尾大括號註記 `}  // namespace l2m`。**兩個例外在全域命名空間**：`CharacterWindow`、`SingleInstance`。
- include 順序：順序敏感的平台/GL 標頭最前面（`<GL/glew.h>`、`<httplib.h>` 必須在 Qt 標頭之前），
  然後自己的標頭、Qt、std、專案標頭。
- JSON 一律 yyjson，透過 `core/json_doc.h` 的 RAII 包裝。
- 應用層統一回傳 `CommandResult{ok, error, hint, dataJson}`。例外只用在 `ConfigStore::patch` 與 `buildMotion3`，
  且在呼叫端就地接住。
- 測試檔 `tests/test_<subject>.cpp`，單一 QObject 配 `private slots`，測試方法名用 camelCase 句子，
  上方以中文註解說明測試意圖。fixture 在 `tests/fixtures/models/`，`FakeTimers` 在 `tests/fake_timers.h`。
- **commit 訊息繁體中文**，`類別：一句摘要` 形式（`修正：`／`效能：`／`完成 M4-M6：`），
  內文分項說明「症狀 → 根因 → 修法」並帶 `file.cpp:line` 參照。

### 只有 clang 會擋的地雷（在 Windows 上改，到 macOS 才爆）

MSVC 放行、clang 直接報 error 的寫法，在 Windows 上編得過、CI 也只跑 Windows，
所以只能靠這一節記住。目前就一條，但已經踩了五次：

- **巢狀型別若有成員預設值（NSDMI），就不可以拿它當外層類別的預設引數。**

  ```cpp
  class AudioPlayer {
    struct Effects { bool echo = false; };                 // 有成員預設值
    bool beginUtterance(double v, const Effects& e = {});  // ← clang 報錯
  };
  ```

  錯誤訊息長這樣（macOS/clang）：`default member initializer for 'echo' needed within
  definition of enclosing class 'AudioPlayer' outside of member functions`。
  根因是**預設引數屬於外層類別的 complete-class context** —— 等於要求在外層類別還沒定義
  完成時就展開巢狀型別的成員預設值。

  **修法：把需要那個預設值的地方移出類別定義。** 拆成兩個多載或兩個建構子，`{}`／`Type{}`
  寫在 `.cpp` 的函式本體裡（函式本體是延後解析的，clang 允許）。已這樣修過的有
  `media/audio_player.h`（兩個多載）與 `core/behavior_pick.h`／`core/gesture_detector.h`／
  `core/mood.h`／`core/presence_tracker.h`（兩個建構子），每一處都留了同樣的註解。

  **不受影響的三種，不必跟著改**：參數型別是 `std::string`／`std::function` 之類的
  （`= {}` 不牽涉巢狀 NSDMI）、型別在**命名空間層級**（`DragSwingTuning`、
  `GazeDirectorTuning`、`SegmentOptions`）、以及**自由函式**的預設引數
  （根本不在任何類別的 complete-class context 裡）。
