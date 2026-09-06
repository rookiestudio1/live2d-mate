# Live2D Mate Android 移植 實作計畫

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 讓 Live2D Mate 以全螢幕、不透明的形式跑在 Android 手機上，並且 PC 上（或透過 Termux 的 cloudflared 隧道從遠端）的 AI 能用既有的 MCP 工具讓她說話。

**Architecture:** 不改 `src/app/main.cpp`，另開 `src/android/main.cpp` 當第二個進入點（照 `live2d_viewer` 的前例），共用 `l2m_core` / `l2m_live2d` / `l2m_media` / `src/mcp/` / `src/app/app_controller.*`。GL 從桌面 GL 換成 GLES 2.0（拿掉 glew、改用 Cubism 的 `CSM_TARGET_ANDROID_ES2` 分支）。資源（i18n、FrameworkShaders）編進 qrc，靠 `readFileUtf8()` 的一個 qrc 分支同時打通。MCP 伺服器原封不動綁在手機上，對外走 Termux 裡的 cloudflared。

**Tech Stack:** Qt 6.11.2（`android_arm64_v8a` kit）、Android NDK 27、CMake + Ninja、Cubism SDK for Native 5-r.5、cpp-httplib、miniaudio（AAudio/OpenSL ES）、yyjson、miniz。

**Spec:** `docs/superpowers/specs/2026-09-04-android-port-design.md`

---

## Global Constraints

這些是全專案既有的規則，**每一個 Task 都隱含適用**。值一字不差抄自 `CLAUDE.md`。

- **程式碼註解一律繁體中文**；標頭開頭的區塊註解要寫「這是什麼／為什麼這樣設計」，帶實測數字或具體 bug。**這就是本專案的文件，維持同樣密度。**
- **面向使用者／AI 的字串一律英文**。UI 字串走 `i18n::translate`，鍵值同步進 `i18n/{en,ja,ko,zh-CN,zh-TW}.json` 五份。`qDebug`/`qWarning` 是中文並帶 `[mcp]`、`[tts]`、`[live2d]`、`[config]` 之類子系統前綴。
- **縮排 2 空白**、無 tab、行寬 200。風格由根目錄 `.clang-format` 定義，**改動照它的產出走**。
- 檔名 `snake_case.{h,cpp}`、`#pragma once`、平台實作用 `_android.cpp` 後綴。類別 `PascalCase`、成員尾底線 `foo_`、常數 `kCamelCase`、新列舉用 `enum class`。
- **測試全部只連 `l2m_core`**（`l2m_add_test()`）。決策邏輯放 `src/core/`，Qt/GL/OS 互動放其他目錄 —— 這是能不能被測到的分界。
- **`serializeConfig` 的鍵順序是既有 config.json 的合約**：新欄位一律接在既有鍵後面。
- **`describeModel` 與 `scanModels` 一字不差是合約**（`tests/test_model_scanner.cpp` 釘住），這次一個字都不動。
- **巢狀型別若有成員預設值（NSDMI），不可拿它當外層類別的預設引數**（MSVC 放行、clang 報錯）。自由函式的預設引數不受影響。
- include 順序：順序敏感的平台/GL 標頭最前面（`<GL/glew.h>`、`<httplib.h>` 必須在 Qt 標頭之前），然後自己的標頭、Qt、std、專案標頭。
- JSON 一律 yyjson，透過 `core/json_doc.h` 的 RAII 包裝。應用層統一回傳 `CommandResult{ok, error, hint, dataJson}`，失敗一定要附 `hint` 並列出有效選項。
- **commit 訊息繁體中文**，`類別：一句摘要` 形式（`新增：`／`修正：`／`效能：`），內文分項說明「症狀 → 根因 → 修法」並帶 `file.cpp:line` 參照。
- **Android 專屬定值**：`applicationId` = `com.rookiestudio.live2dmate`；`minSdk` = 28；`targetSdk` = 35；ABI 只出 `arm64-v8a`。
- **回歸底線**：每個 Task 結束前，Windows 建置與全部測試必須仍然通過。指令見下方。

**Windows 回歸驗證指令**（每個 Task 的最後一步都要跑）：

```bash
cmake -S . -B build/rel -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/rel
ctest --test-dir build/rel --output-on-failure
```

---

## File Structure

### 新增

| 檔案 | 責任 |
|---|---|
| `src/core/asset_paths.h` / `.cpp` | 「i18n 與 FrameworkShaders 該去哪裡找」的候選順序。純函式，可測。 |
| `src/platform/window_effects_android.cpp` | 穿透／置頂在 Android 上不存在 → no-op（照 `crash_handler_linux.cpp` 的空殼前例）。 |
| `src/platform/autostart_android.cpp` | 開機自啟 → no-op。 |
| `src/platform/user_idle_android.cpp` | 全域閒置拿不到 → `std::nullopt`。 |
| `src/platform/console_android.cpp` | 沒有 GUI 子系統這回事 → no-op。 |
| `src/platform/crash_handler_android.cpp` | 空殼 + `appDataDirFromEnv()` 的 Android 版。 |
| `src/android/main.cpp` | Android 進入點與物件圖。**不含** splash／single-instance／tray／設定視窗。 |
| `src/android/android_root.h` / `.cpp` | 全螢幕版面：角色鋪滿、`BubbleWindow`（**不改**）當子 widget 浮在上面。 |
| `src/android/android_shell.h` / `.cpp` | 螢幕常亮、沉浸式全螢幕、生命週期 → JNI 的唯一出入口。 |
| `android/AndroidManifest.xml` | 權限、Activity、螢幕方向、splash theme。 |
| `resources/android_assets.qrc` | 把 `i18n/` 與 FrameworkShaders 編進 APK。 |
| `docs/ANDROID_SETUP.md` | 工具鏈、建置、adb push 模型、Termux + cloudflared 的操作手冊。 |
| `tests/test_asset_paths.cpp` | Task 3 的測試。 |

### 修改

| 檔案 | 改什麼 |
|---|---|
| `src/core/mcp_host.h` / `.cpp` | `requiresToken` / `validateMcpBinding` 加 `tunnelExposed`。 |
| `src/core/mcp_snippets.h` / `.cpp` | `SnippetInput` 加 `bool android`；Android 走 Termux 片段。 |
| `src/core/config_schema.h` / `.cpp` | `McpConfig::tunnelExposed`（接在既有鍵後面）。 |
| `src/core/json_doc.cpp` | `readFileUtf8()` 加 qrc（`":/"`）分支。 |
| `src/core/i18n.cpp` | `loadLocale` 改走 `readFileUtf8`（順帶得到 qrc 能力）。 |
| `src/core/zip_archive.cpp` | iconv 的 Android 分支。 |
| `src/live2d/cubism_runtime.cpp` | `loadFileBytes` 改用 `assetCandidates`。 |
| `src/mcp/mcp_http_server.h` / `.cpp` | `start()` 多收 `tunnelExposed`。 |
| `src/windows/settings/mcp_page.{ui,h,cpp}` | 隧道核取方塊。 |
| `src/windows/character_window.h` / `.cpp` | `setHitMaskEnabled()`。 |
| `src/windows/window_manager.h` / `.cpp` | `fixedFullscreen` 模式。 |
| `cmake/SetupCubismCore.cmake` | Android 分支。 |
| `cmake/FetchCubismFramework.cmake` | `CSM_TARGET_ANDROID_ES2`、不連 glew。 |
| `patches/cubism-framework-lazy-blend-shaders.patch` | 延遲產生涵蓋 Android ES2。 |
| `CMakeLists.txt` | glew 條件化、Android target、qrc、`l2m_add_test(test_asset_paths)`。 |
| `i18n/*.json`（5 份） | 新增鍵值。 |

---

# Phase 0 — 純 core 的準備工作（在 Windows 上做完，不需要 Android 工具鏈）

## Task 1: 隧道曝露時強制 token

修的是**桌面版現在就有的洞**：照 UI 印出來的 cloudflared 指令做，會得到一個沒有任何驗證的公開 MCP 端點。Android 版把這個邊角案例變成主要路徑（Termux 的 cloudflared 與桌寵共用同一個 loopback），所以先修。

**Files:**
- Modify: `src/core/mcp_host.h:44-48`
- Modify: `src/core/mcp_host.cpp:47-53`
- Modify: `src/core/config_schema.h`（`McpConfig` 尾端）
- Modify: `src/core/config_schema.cpp:274`（parse）、`:448`（serialize）
- Modify: `src/mcp/mcp_http_server.h`、`src/mcp/mcp_http_server.cpp:116`
- Modify: `src/windows/settings/mcp_page.ui`、`mcp_page.cpp:199,206,330`
- Modify: `src/app/main.cpp:522` 附近（`mcpServer.start(...)` 的呼叫）
- Modify: `i18n/{en,ja,ko,zh-CN,zh-TW}.json`
- Test: `tests/test_mcp_host.cpp`、`tests/test_config.cpp`

**Interfaces:**
- Produces: `bool l2m::requiresToken(const std::string& host, bool tunnelExposed = false)`
- Produces: `std::optional<std::string> l2m::validateMcpBinding(const std::string& host, const std::optional<std::string>& token, bool tunnelExposed = false)`
- Produces: `bool l2m::McpConfig::tunnelExposed`（預設 `false`）
- Produces: `void l2m::McpHttpServer::start(const std::string& host, int port, const std::optional<std::string>& token, bool tunnelExposed)`

- [ ] **Step 1: 寫失敗的測試**

在 `tests/test_mcp_host.cpp` 的 `=== validateMcpBinding ===` 區塊末尾加入：

```cpp
  // === tunnelExposed ===

  // 隧道行程就在本機，它把 127.0.0.1 原封不動端上公網 ——
  // 「loopback ⇒ 只有坐在這台機器前的人打得到」這個前提不成立
  void tunnelExposedLoopbackNeedsToken() {
    QVERIFY(requiresToken("127.0.0.1", true));
    QVERIFY(requiresToken("localhost", true));
    const auto refusal = validateMcpBinding("127.0.0.1", std::nullopt, true);
    QVERIFY(refusal.has_value());
    QVERIFY(refusal->find("token") != std::string::npos);
    QVERIFY(refusal->find("tunnel") != std::string::npos);
  }

  // 有 token 就放行，綁哪裡、有沒有隧道都一樣
  void tunnelExposedWithTokenIsAllowed() {
    QVERIFY(!validateMcpBinding("127.0.0.1", std::string("abc"), true).has_value());
    QVERIFY(!validateMcpBinding("0.0.0.0", std::string("abc"), true).has_value());
  }

  // 沒開隧道時答案與原本一字不差 —— 預設引數不得改變既有呼叫端的行為
  void tunnelFlagDefaultsToOldBehaviour() {
    QVERIFY(!requiresToken("127.0.0.1"));
    QVERIFY(!validateMcpBinding("127.0.0.1", std::nullopt).has_value());
    QVERIFY(validateMcpBinding("0.0.0.0", std::nullopt).has_value());
  }
```

在 `tests/test_config.cpp` 的 `private slots:` 加入：

```cpp
  // 隧道旗標：預設關閉，且序列化排在 mcp 區塊既有鍵的後面
  //（新欄位插在中間會讓既有 config.json 每次重寫都整份 diff）
  void mcpTunnelExposedDefaultsOffAndSerializesLast() {
    QCOMPARE(defaultConfig().mcp.tunnelExposed, false);
    const std::string json = serializeConfig(defaultConfig());
    const auto announce = json.find("\"announceSteps\"");
    const auto tunnel = json.find("\"tunnelExposed\"");
    QVERIFY(announce != std::string::npos);
    QVERIFY(tunnel != std::string::npos);
    QVERIFY(announce < tunnel);
  }
```

- [ ] **Step 2: 跑測試確認失敗**

```bash
cmake --build build/rel --target test_mcp_host test_config
build/rel/test_mcp_host.exe
```

Expected: 編譯失敗（`requiresToken` 不吃第二個參數 / `tunnelExposed` 不是 `McpConfig` 的成員）。

- [ ] **Step 3: 改 `src/core/mcp_host.h`**

把第 44-48 行那兩個宣告換成：

```cpp
// 綁到 loopback 以外就必須有 token。
//
// **tunnelExposed 是第二個理由，而且它推翻了第一個的前提**：
// cloudflared／ngrok／Termux 的隧道行程就在本機，它把 127.0.0.1 原封不動端上
// 公網，於是「loopback ⇒ 只有坐在這台機器前的人打得到」不再成立 ——
// 沒有 token 的設定會變成誰拿到網址都能操控角色的公開端點。
// 手機版尤其：Termux 的 cloudflared 與桌寵共用同一個 loopback，
// 那是**建議的正常設定**而不是邊角案例。
bool requiresToken(const std::string& host, bool tunnelExposed = false);

// 這組設定能不能安全地啟動；沒問題回 nullopt，否則回英文的原因字串
//（會顯示在狀態列，與伺服器的其他錯誤訊息同一風格）
std::optional<std::string> validateMcpBinding(const std::string& host, const std::optional<std::string>& token, bool tunnelExposed = false);
```

- [ ] **Step 4: 改 `src/core/mcp_host.cpp:47-53`**

```cpp
bool requiresToken(const std::string& host, bool tunnelExposed) { return tunnelExposed || !isLoopbackHost(host); }

std::optional<std::string> validateMcpBinding(const std::string& host, const std::optional<std::string>& token, bool tunnelExposed) {
  if (!requiresToken(host, tunnelExposed)) return std::nullopt;
  if (token.has_value() && !strutil::trim(*token).empty()) return std::nullopt;
  // 兩句話刻意不同：使用者需要知道是「哪一個理由」逼他填 token，
  // 不然綁 loopback 卻被要求 token 看起來像 bug
  if (tunnelExposed && isLoopbackHost(host)) return "A tunnel makes this port reachable from the public internet; an access token is required";
  return "Listening on " + host + " exposes the MCP server to your network; an access token is required";
}
```

- [ ] **Step 5: 加 config 欄位**

`src/core/config_schema.h` 的 `McpConfig` **最後**（`announceSteps` 之後）加：

```cpp
  // 這個埠會不會透過隧道（cloudflared / ngrok / Termux）對外。
  // 不影響伺服器怎麼綁，只讓 validateMcpBinding 把 token 從「選填」變成「必填」——
  // 理由見 core/mcp_host.h 的 requiresToken。
  bool tunnelExposed = false;
```

`src/core/config_schema.cpp` 的 parse 區塊（`:274` 之後）加：

```cpp
    readBool(ctx, mcp, "tunnelExposed", config.mcp.tunnelExposed, "mcp.tunnelExposed");
```

serialize 區塊（`:448`，`announceSteps` 之後）加：

```cpp
    putBool(doc, mcp, "tunnelExposed", config.mcp.tunnelExposed);
```

- [ ] **Step 6: 跑測試確認通過**

```bash
cmake --build build/rel --target test_mcp_host test_config
build/rel/test_mcp_host.exe && build/rel/test_config.exe
```

Expected: 兩支都 PASS。

- [ ] **Step 7: 接上伺服器與呼叫端**

`src/mcp/mcp_http_server.h` 的 `start` 宣告改成：

```cpp
  // 啟動；失敗時 status().error 會有原因。已經在跑會先停掉。
  // tunnelExposed：這個埠會不會透過隧道對外（見 core/mcp_host.h 的 requiresToken）
  void start(const std::string& host, int port, const std::optional<std::string>& token, bool tunnelExposed);
```

`src/mcp/mcp_http_server.cpp` 的定義同步改簽名，並把 `:116` 改成：

```cpp
  if (const auto refusal = validateMcpBinding(host, token, tunnelExposed)) {
```

`src/app/main.cpp` 裡 `mcpServer.start(...)` 的呼叫（`:522` 附近）補上 `config.get().mcp.tunnelExposed`。

`src/windows/settings/mcp_page.cpp` 的三處（`:199`、`:206`、`:330`）補上表單上核取方塊的目前值。

- [ ] **Step 8: 加 UI 核取方塊**

`src/windows/settings/mcp_page.ui`：把 `applyRow` 那個 `<item row="6" ...>` 改成 `row="7"`，並在它前面插入：

```xml
          <item row="6" column="0" colspan="3">
           <widget class="QCheckBox" name="tunnelExposed">
            <property name="text">
             <string>settings.mcp.tunnelExposed</string>
            </property>
           </widget>
          </item>
```

`src/windows/settings/mcp_page.cpp` 的 `retranslate()` 加一行（照鄰近的寫法）：

```cpp
  ui_->tunnelExposed->setText(QString::fromStdString(i18n::translate("settings.mcp.tunnelExposed")));
```

`reloadForm()` 裡照 host/port/token 的寫法把值填進去（記得 `updating_` 旗標），
`apply()` 裡把它一起送進 patch。**不要**加進 `speechBoxes()` —— 它屬於
host/port/token 那組走「套用並重新啟動」的連線設定，不是即時套用的發話節奏。

- [ ] **Step 9: 五份 i18n**

在五份 `i18n/*.json` 的 `settings.mcp.announceSteps` 後面各加一行：

```json
  "settings.mcp.tunnelExposed": "This port is exposed through a tunnel (cloudflared / ngrok / Termux)",
```

日文：`"このポートはトンネル（cloudflared / ngrok / Termux）で外部に公開されている"`
韓文：`"이 포트는 터널(cloudflared / ngrok / Termux)로 외부에 공개됨"`
簡中：`"此端口通过隧道（cloudflared / ngrok / Termux）对外公开"`
繁中：`"這個埠會透過隧道（cloudflared / ngrok / Termux）對外公開"`

- [ ] **Step 10: 全套回歸**

```bash
cmake --build build/rel
ctest --test-dir build/rel --output-on-failure
```

Expected: 全部 PASS。手動開一次設定 → MCP，確認核取方塊顯示的是翻譯而不是 raw key。

- [ ] **Step 11: Commit**

```bash
git add src/core/mcp_host.h src/core/mcp_host.cpp src/core/config_schema.h src/core/config_schema.cpp src/mcp/mcp_http_server.h src/mcp/mcp_http_server.cpp src/app/main.cpp src/windows/settings/mcp_page.ui src/windows/settings/mcp_page.cpp i18n tests/test_mcp_host.cpp tests/test_config.cpp
git commit -m "修正：隧道之下 loopback 也必須有 token

症狀：照設定頁印出來的 cloudflared 指令做，會得到一個沒有任何驗證的公開
MCP 端點 —— 拿到網址的人都能叫角色說話、讀 persona 全文。

根因：mcp_host.cpp:47 的 requiresToken 只看綁在哪裡，前提是「loopback ⇒
只有本機打得到」。隧道行程就在本機，它把 127.0.0.1 原封不動端上公網，
前提不成立。而 mcp_snippets.cpp:107 是無條件推出那個片段的。

修法：requiresToken / validateMcpBinding 各加 tunnelExposed 參數，
config 加 mcp.tunnelExposed，MCP 分頁加核取方塊（歸連線那組，走套用按鈕）。"
```

---

## Task 2: Termux / cloudflared 的設定片段

Android 上使用者沒辦法在 live2d_mate 裡執行 `cloudflared tunnel --url ...`，那行指令要在 Termux 裡打。而且隧道端點固定是 `127.0.0.1`（Termux 與桌寵共用同一個 loopback），不是設定頁 advertise 出來的區網位址。

**Files:**
- Modify: `src/core/mcp_snippets.h`、`src/core/mcp_snippets.cpp:107-108`
- Modify: `i18n/{en,ja,ko,zh-CN,zh-TW}.json`
- Test: `tests/test_mcp_snippets.cpp`

**Interfaces:**
- Consumes: 無（Task 1 的改動不影響這裡）
- Produces: `bool l2m::SnippetInput::android`（預設 `false`）
- Produces: snippet id `"termux-cloudflared"`（僅 `android == true` 時出現）

- [ ] **Step 1: 寫失敗的測試**

在 `tests/test_mcp_snippets.cpp` 的 `build()` helper 後面加一個 Android 版：

```cpp
std::vector<McpSnippet> buildAndroid(const std::optional<std::string>& token = std::nullopt) {
  SnippetInput input;
  input.host = "0.0.0.0";
  input.port = 3777;
  input.token = token;
  input.exePath = kExePath;
  input.android = true;
  return buildMcpSnippets(input);
}
```

`private slots:` 加：

```cpp
  // 桌面版沒有 Termux 片段，Android 版才有
  void termuxSnippetIsAndroidOnly() {
    QVERIFY(find(build(), "termux-cloudflared").text == std::nullopt);
    QVERIFY(find(buildAndroid(), "termux-cloudflared").text.has_value());
  }

  // Android 版不推桌面那兩個 —— 使用者沒地方執行它們
  void androidDropsDesktopTunnelSnippets() {
    const auto snippets = buildAndroid();
    QVERIFY(!find(snippets, "cloudflared").text.has_value());
    QVERIFY(!find(snippets, "ngrok").text.has_value());
  }

  // 隧道一定指向 127.0.0.1：Termux 與桌寵共用同一個 loopback，
  // 而設定頁 advertise 出來的區網位址在 Termux 裡繞遠路、換 Wi-Fi 還會失效
  void termuxTunnelTargetsLoopback() {
    const std::string& text = *find(buildAndroid(), "termux-cloudflared").text;
    QVERIFY(text.find("http://127.0.0.1:3777") != std::string::npos);
    QVERIFY(text.find("0.0.0.0") == std::string::npos);
    QVERIFY(text.find("pkg install") != std::string::npos);
    // 少了 wake lock，螢幕一關 Termux 就被 Doze 凍結，隧道會時好時壞
    QVERIFY(text.find("termux-wake-lock") != std::string::npos);
  }

  // Android 版仍然要給 stdio 橋接（PC 端走區網，不必繞公網）
  void androidKeepsStdioSnippet() { QVERIFY(find(buildAndroid(), "claude-desktop").text.has_value()); }
```

- [ ] **Step 2: 跑測試確認失敗**

```bash
cmake --build build/rel --target test_mcp_snippets
build/rel/test_mcp_snippets.exe
```

Expected: 編譯失敗（`SnippetInput` 沒有 `android` 成員）。

- [ ] **Step 3: `src/core/mcp_snippets.h` 加欄位**

在 `SnippetInput` 的 `exePath` 之後加：

```cpp
  // 這份設定是要給手機上的桌寵看的。
  // 影響的只有隧道那一組：Android 上使用者沒辦法在 app 裡執行 cloudflared，
  // 那行指令要在 **Termux**（另一個 app）裡打，而且端點固定是 127.0.0.1
  // —— Termux 與桌寵共用同一個 network namespace，走區網位址只是繞遠路，
  // 換一次 Wi-Fi 還會失效。
  bool android = false;
```

- [ ] **Step 4: `src/core/mcp_snippets.cpp:107-108` 換成分支**

```cpp
  // 隧道指向來源，不是 /mcp；路徑由 client 自己接上去
  if (input.android) {
    // Termux 是另一個 app，靠 targetSdk 28 保有 exec 權限 —— 那正是它跑得動
    // cloudflared 而我們跑不動的原因（Android 10+ 的 W^X 只放行
    // nativeLibraryDir 底下的執行檔）。
    // 端點寫死 127.0.0.1：兩個 app 共用同一個 network namespace。
    const std::string termux =
      "pkg install cloudflared\n"
      "termux-wake-lock\n"
      "cloudflared tunnel --url http://127.0.0.1:" + std::to_string(input.port);
    snippets.push_back({"termux-cloudflared", McpSnippetGroup::Tunnel, "mcp.snippet.termuxCloudflared", "mcp.snippet.termuxHint", termux, std::nullopt});
  } else {
    snippets.push_back({"cloudflared", McpSnippetGroup::Tunnel, "mcp.snippet.cloudflared", "mcp.snippet.tunnelHint", "cloudflared tunnel --url " + origin, std::nullopt});
    snippets.push_back({"ngrok", McpSnippetGroup::Tunnel, "mcp.snippet.ngrok", "", "ngrok http " + std::to_string(input.port), std::nullopt});
  }
```

- [ ] **Step 5: 跑測試確認通過**

```bash
cmake --build build/rel --target test_mcp_snippets
build/rel/test_mcp_snippets.exe
```

Expected: PASS。

- [ ] **Step 6: 五份 i18n**

在 `mcp.snippet.ngrok` 後面各加兩行（英文版；其餘語系照譯）：

```json
  "mcp.snippet.termuxCloudflared": "Termux + Cloudflare Tunnel",
  "mcp.snippet.termuxHint": "Install Termux from F-Droid (the Play Store build is abandoned and cannot install packages). Run these in Termux, then paste the https URL it prints (with /mcp appended) into your AI client. Exempt Termux from battery optimisation, or the tunnel drops when the screen turns off.",
```

- [ ] **Step 7: 全套回歸並 commit**

```bash
cmake --build build/rel && ctest --test-dir build/rel --output-on-failure
git add src/core/mcp_snippets.h src/core/mcp_snippets.cpp i18n tests/test_mcp_snippets.cpp
git commit -m "新增：Android 版的 Termux + cloudflared 連線片段

桌面版那行 cloudflared 指令在手機上沒地方執行 —— 隧道要在 Termux 裡開，
而且端點固定 127.0.0.1（Termux 與桌寵共用同一個 network namespace，
走區網位址只是繞遠路，換 Wi-Fi 還會失效）。

SnippetInput 加 android 旗標；true 時以 termux-cloudflared 取代
桌面的 cloudflared / ngrok 兩則。"
```

---

## Task 3: 資源路徑打通 qrc

APK 裡沒有「執行檔旁邊」這種地方，`applicationDirPath()` 指向一個列不出內容的位置。i18n 訊息表與 FrameworkShaders 都必須編進 qrc。**判斷放在 `readFileUtf8()` 這個最底層的一支**，讀 JSON／shader 的路徑全部經過它，一處改完兩邊同時通。

**Files:**
- Create: `src/core/asset_paths.h`、`src/core/asset_paths.cpp`
- Create: `tests/test_asset_paths.cpp`
- Modify: `src/core/json_doc.cpp:8-21`
- Modify: `src/core/i18n.cpp:35`
- Modify: `src/live2d/cubism_runtime.cpp:36-74`
- Modify: `CMakeLists.txt`（`l2m_core` 來源、`l2m_add_test`）
- Test: `tests/test_asset_paths.cpp`、既有的 `tests/test_persona_default.cpp`（已證明 qrc 在測試裡可用）

**Interfaces:**
- Produces: `enum class l2m::AssetKind { I18n, Shader }`
- Produces: `std::vector<std::string> l2m::assetDirCandidates(AssetKind kind, const std::string& devDir, const std::string& exeDir, bool bundled)`
- Produces: `readFileUtf8()` 現在也吃 `":/..."` 開頭的 qrc 路徑

- [ ] **Step 1: 寫失敗的測試**

建立 `tests/test_asset_paths.cpp`：

```cpp
// 執行期資源（i18n、FrameworkShaders）的候選順序（core/asset_paths.h）。
//
// 這一段錯掉的症狀**沒有錯誤訊息**：介面全是 raw key、模型一動也不動。
#include <QtTest>

#include <string>
#include <vector>

#include "core/asset_paths.h"
#include "core/json_doc.h"

using namespace l2m;

class TestAssetPaths : public QObject {
  Q_OBJECT

private slots:
  // 桌面：開發樹優先，找不到才退回執行檔旁的同名目錄
  void desktopPrefersDevDirThenExeDir() {
    const auto dirs = assetDirCandidates(AssetKind::I18n, "E:/repo/i18n", "C:/app", false);
    QCOMPARE(dirs.size(), std::size_t(2));
    QCOMPARE(dirs[0], std::string("E:/repo/i18n"));
    QCOMPARE(dirs[1], std::string("C:/app/i18n"));
  }

  // 出貨建置沒有編譯期路徑，只剩執行檔旁邊
  void emptyDevDirIsSkipped() {
    const auto dirs = assetDirCandidates(AssetKind::Shader, "", "C:/app", false);
    QCOMPARE(dirs.size(), std::size_t(1));
    QCOMPARE(dirs[0], std::string("C:/app/FrameworkShaders"));
  }

  // Android：qrc 是唯一真的存在的來源，必須排第一
  void bundledPutsQrcFirst() {
    const auto dirs = assetDirCandidates(AssetKind::I18n, "", "", true);
    QCOMPARE(dirs.size(), std::size_t(1));
    QCOMPARE(dirs[0], std::string(":/i18n"));
  }

  // qrc 前綴與部署目錄同名，兩邊心智模型才一致
  void shaderDirNameMatchesDeployedLayout() {
    QCOMPARE(assetDirCandidates(AssetKind::Shader, "", "", true)[0], std::string(":/FrameworkShaders"));
  }

  // readFileUtf8 要開得動 qrc —— l2m_core 上掛著 resources/personas.qrc，
  // 拿它現成的檔案來驗（見 tests/test_persona_default.cpp）
  void readFileUtf8OpensQrc() {
    const auto text = jsonu::readFileUtf8(":/personas/Default.en.md");
    QVERIFY(text.has_value());
    QVERIFY(!text->empty());
  }

  // 不存在的 qrc 路徑要回 nullopt，不能丟例外
  void missingQrcReturnsNullopt() { QVERIFY(!jsonu::readFileUtf8(":/personas/NoSuchFile.md").has_value()); }
};

QTEST_MAIN(TestAssetPaths)
#include "test_asset_paths.moc"
```

- [ ] **Step 2: 註冊測試並確認失敗**

`CMakeLists.txt` 的測試清單加 `l2m_add_test(test_asset_paths)`（放在 `l2m_add_test(test_i18n)` 後面）。

```bash
cmake -S . -B build/rel -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/rel --target test_asset_paths
```

Expected: 編譯失敗（找不到 `core/asset_paths.h`）。

- [ ] **Step 3: 建立 `src/core/asset_paths.h`**

```cpp
#pragma once

// 執行期資源（i18n 訊息表、Cubism 的 FrameworkShaders）該去哪裡找。
//
// **為什麼是一支純函式而不是各自寫在三個呼叫點**：原本 src/app/main.cpp 的
// resolveI18nDir()、src/viewer/main.cpp 的同名函式、live2d/cubism_runtime.cpp 的
// loadFileBytes() 各寫一份「先試編譯期烤進去的開發路徑，不存在才退回執行檔旁的
// 同名目錄」。三份分岔的症狀是**完全沒有錯誤訊息的** —— 介面全是 raw key、
// 模型一動也不動（同 CMakeLists 尾端 install() 那段註解說的壞法）。
//
// Android 又多一條規則：APK 裡沒有「執行檔旁邊」這種地方，
// applicationDirPath() 指向一個列不出內容的位置，兩個資源目錄都必須編進 qrc。
// 所以候選清單多一個 ":/" 開頭的項目，而且**排在最前面**。
//
// 這裡只算候選順序，開檔由呼叫端做 —— 但 core/json_doc.h 的 readFileUtf8()
// 已經吃得下 ":/" 開頭的路徑，所以呼叫端照舊即可。

#include <string>
#include <vector>

namespace l2m {

enum class AssetKind {
  I18n,    // i18n/<locale>.json
  Shader,  // FrameworkShaders/<名稱>.vert|.frag
};

// 依序要嘗試的目錄。以 ":/" 開頭的是 qrc。
//
// devDir  ：編譯期烤進去的開發樹路徑（L2M_DEV_I18N_DIR / L2M_CUBISM_SHADER_DIR）。
//           空字串＝這份建置沒有（出貨建置）。**這一層直接就是那個目錄本身**，
//           不再多包一層同名目錄。
// exeDir  ：QCoreApplication::applicationDirPath()。Android 上給空字串。
// bundled ：資源編進執行檔（Android）。true 時 qrc 排第一。
std::vector<std::string> assetDirCandidates(AssetKind kind, const std::string& devDir, const std::string& exeDir, bool bundled);

}  // namespace l2m
```

- [ ] **Step 4: 建立 `src/core/asset_paths.cpp`**

```cpp
#include "asset_paths.h"

namespace l2m {

namespace {

// qrc 前綴與部署目錄同名，兩邊的心智模型才一致
const char* dirNameOf(AssetKind kind) { return kind == AssetKind::I18n ? "i18n" : "FrameworkShaders"; }

}  // namespace

std::vector<std::string> assetDirCandidates(AssetKind kind, const std::string& devDir, const std::string& exeDir, bool bundled) {
  const std::string dir = dirNameOf(kind);
  std::vector<std::string> out;
  if (bundled) out.push_back(":/" + dir);
  if (!devDir.empty()) out.push_back(devDir);
  if (!exeDir.empty()) out.push_back(exeDir + "/" + dir);
  return out;
}

}  // namespace l2m
```

- [ ] **Step 5: 讓 `readFileUtf8` 吃 qrc**

`src/core/json_doc.cpp:8` 的 `readFileUtf8` 開頭插入：

```cpp
std::optional<std::string> readFileUtf8(const std::filesystem::path& path) {
  // qrc（":/..."）只有 QFile 開得動 —— APK 裡沒有「執行檔旁邊」，i18n 與
  // FrameworkShaders 兩個目錄都是編進資源的，路徑會長成 ":/i18n/en.json"。
  //
  // **判斷放在最底層這一支而不是各個呼叫端**：讀 JSON 與 shader 的路徑全部
  // 經過這裡，一處改完，i18n 與 Cubism 的 shader 載入同時就通了。
  // 用 generic_string() 而不是 native()：Windows 上 ':' 是磁碟機分隔符號，
  // native 表示法可能把它正規化掉（這條路只有 Android 會走到，但寫對成本為零）。
  const std::string generic = path.generic_string();
  if (!generic.empty() && generic.front() == ':') {
    QFile file(QString::fromStdString(generic));
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray bytes = file.readAll();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
  }
  // ...以下維持原本的 ifstream 路徑不動...
```

同檔頂端補 `#include <QFile>`（照 include 順序：Qt 標頭在自己的標頭之後）。

- [ ] **Step 6: 跑測試確認通過**

```bash
cmake --build build/rel --target test_asset_paths
build/rel/test_asset_paths.exe
```

Expected: 六個測試全 PASS。

- [ ] **Step 7: 三個呼叫點改用**

`src/core/i18n.cpp:35` 原本是 `jsonu::Doc::parseFile(messagesDir / ...)`，
`messagesDir` 現在可能是 `":/i18n"` —— `std::filesystem::path(":/i18n") / "en.json"`
在 POSIX 上得到 `":/i18n/en.json"`，`readFileUtf8` 會走 qrc 分支。**這一支不用改**，
但要在 `setMessagesDir` 上方補一行註解說明它現在也吃 qrc 路徑。

`src/live2d/cubism_runtime.cpp` 的 `loadFileBytes` 把 `candidates` 那一段換成：

```cpp
  std::vector<fs::path> candidates;
  const std::string devDir =
#ifdef L2M_CUBISM_SHADER_DIR
    L2M_CUBISM_SHADER_DIR;
#else
    std::string();
#endif
  const std::string exeDir = QCoreApplication::instance() ? QCoreApplication::applicationDirPath().toStdString() : std::string();
  for (const auto& dir : assetDirCandidates(AssetKind::Shader, devDir, exeDir, kAssetsBundled)) {
    candidates.push_back(fs::u8path(dir) / fs::u8path(name));
  }
  candidates.push_back(fs::u8path(filePath));  // 原樣路徑（最後手段）
```

同檔加 `#include "core/asset_paths.h"`，並在 `namespace {` 裡定義：

```cpp
// 資源編進執行檔（Android 的 APK 裡沒有「執行檔旁邊」，見 core/asset_paths.h）
#ifdef Q_OS_ANDROID
constexpr bool kAssetsBundled = true;
#else
constexpr bool kAssetsBundled = false;
#endif
```

`src/app/main.cpp` 與 `src/viewer/main.cpp` 的 `resolveI18nDir()` 各換成：

```cpp
// i18n 訊息表目錄：候選順序見 core/asset_paths.h
fs::path resolveI18nDir() {
  const std::string devDir =
#ifdef L2M_DEV_I18N_DIR
    L2M_DEV_I18N_DIR;
#else
    std::string();
#endif
  const std::string exeDir = QCoreApplication::applicationDirPath().toStdString();
  for (const auto& dir : l2m::assetDirCandidates(l2m::AssetKind::I18n, devDir, exeDir, false)) {
    const fs::path candidate = fs::u8path(dir);
    if (l2m::jsonu::readFileUtf8(candidate / "en.json")) return candidate;
  }
  return fs::u8path(exeDir) / "i18n";
}
```

- [ ] **Step 8: 全套回歸並 commit**

```bash
cmake --build build/rel && ctest --test-dir build/rel --output-on-failure
build/rel/live2d_mate.exe   # 手動確認介面不是 raw key、模型會動
```

```bash
git add src/core/asset_paths.h src/core/asset_paths.cpp src/core/json_doc.cpp src/core/i18n.cpp src/live2d/cubism_runtime.cpp src/app/main.cpp src/viewer/main.cpp CMakeLists.txt tests/test_asset_paths.cpp
git commit -m "重構：資源目錄的候選順序收進 core，readFileUtf8 吃得下 qrc

原本 app/main.cpp、viewer/main.cpp、live2d/cubism_runtime.cpp 各寫一份
「開發路徑優先、退回執行檔旁邊」，三份分岔的症狀完全沒有錯誤訊息
（介面全是 raw key、模型一動也不動）。收進 core/asset_paths.h 並補測試。

順帶讓 json_doc.cpp:8 的 readFileUtf8 認得 ':/' 開頭的 qrc 路徑 ——
Android 的 APK 裡沒有「執行檔旁邊」，i18n 與 FrameworkShaders 都要編進資源，
改最底層這一支，兩邊同時就通了。"
```

---

# Phase 1 — Android 建置打通

## Task 4: 工具鏈就緒 + `l2m_core` 在 Android 上編得過

第一個可驗證的產出。順帶把 iconv 那顆地雷排掉 —— 它只會在這一步炸出來。

**Files:**
- Modify: `src/core/zip_archive.cpp:14-16`、`:93-111`
- Create: `docs/ANDROID_SETUP.md`
- Test: 無新測試（`test_zip_archive` 既有的行為必須維持）

**Interfaces:**
- Produces: 可用的 configure 指令（寫進 `docs/ANDROID_SETUP.md`）
- Produces: `build/android/libl2m_core.a`

- [ ] **Step 1: 裝 Qt for Android kit 與 JDK 17**

```bash
D:/Qt/MaintenanceTool.exe
```

勾選 `Qt 6.11.2 → Android`（產出 `D:/Qt/6.11.2/android_arm64_v8a`）。
JDK 17 用 Android Studio 內附的 JBR，或另裝 Temurin 17。
（現在的 `java -version` 是 1.8.0_441，Gradle 跑不動。）

- [ ] **Step 2: 設環境變數並確認**

```bash
export ANDROID_SDK_ROOT=/c/SDKs/android-sdk
export ANDROID_NDK_ROOT=/c/SDKs/android-sdk/ndk/27.0.12077973
export JAVA_HOME=/c/Program\ Files/Eclipse\ Adoptium/jdk-17
"$JAVA_HOME/bin/java" -version
ls "$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
ls D:/Qt/6.11.2/android_arm64_v8a/bin/qt-cmake.bat
```

Expected: java 17、兩個檔案都在。

- [ ] **Step 3: 第一次 configure（只建 core，故意不碰 Live2D）**

```bash
D:/Qt/6.11.2/android_arm64_v8a/bin/qt-cmake.bat -S . -B build/android -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DL2M_BUILD_APP=OFF -DL2M_BUILD_VIEWER=OFF -DL2M_BUILD_TESTS=OFF \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
cmake --build build/android
```

Expected: **編譯失敗**，`zip_archive.cpp` 找不到 `<iconv.h>`
（或連結期 `iconv_open` 未定義 —— bionic 的 iconv 是 API 28 才有，
而且只認 UTF-8／UTF-16／ASCII／Latin-1）。

- [ ] **Step 4: `src/core/zip_archive.cpp` 的 Android 分支**

第 14-16 行改成：

```cpp
#if !defined(_WIN32) && !defined(__ANDROID__)
#include <iconv.h>
#endif
```

第 39 行與第 79 行那對 `#ifndef _WIN32` / `#endif`（包住 `ansiCodepageCandidates`
與 `iconvToUtf8`）同步改成 `#if !defined(_WIN32) && !defined(__ANDROID__)`。

第 93 行起的解碼分支中間插入 Android 那一段：

```cpp
#ifdef _WIN32
  // ...維持原本 fromLocal8Bit 那一段不動...
#elif defined(__ANDROID__)
  // **Android（bionic）沒有 CP950／GBK／Shift-JIS** —— iconv 本身要 API 28 才有，
  // 而且只認 UTF-8／UTF-16／ASCII／Latin-1，iconv_open("UTF-8", "CP950") 必然失敗。
  // 連結進來只是白背一個相依，所以整段排除掉，直接走下面 Linux 版的保底路徑。
  //
  // 代價很明確且有界：Windows 檔案總管壓的、非 UTF-8 中文檔名的 zip，
  // 檔名會顯示成 U+FFFD 亂碼。但**往返一致**（查找用的也是同一個字串），
  // 模型照樣讀得到、也不會 crash —— 只是清單上的名字不好看。
  const QString text = QString::fromLocal8Bit(raw.data(), static_cast<qsizetype>(raw.size()));
  const QByteArray utf8 = text.toUtf8();
  return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
#else
  // ...維持原本 iconv 那一段不動...
#endif
```

- [ ] **Step 5: 重新建置確認通過**

```bash
cmake --build build/android
ls build/android/libl2m_core.a
```

Expected: 建置成功，`libl2m_core.a` 存在。

- [ ] **Step 6: Windows 回歸（iconv 分支不得影響桌面）**

```bash
cmake --build build/rel --target test_zip_archive test_model_assets
build/rel/test_zip_archive.exe && build/rel/test_model_assets.exe
```

Expected: PASS。

- [ ] **Step 7: 寫 `docs/ANDROID_SETUP.md` 的第一節**

建立檔案，寫入 Step 1-3 的內容（工具鏈需求、環境變數、configure 指令），
標題 `## 1. 工具鏈`。後續 Task 會往這份文件追加章節。

- [ ] **Step 8: Commit**

```bash
git add src/core/zip_archive.cpp docs/ANDROID_SETUP.md
git commit -m "修正：Android 沒有可用的 iconv，zip 檔名解碼走保底路徑

症狀：Android 建置在 zip_archive.cpp:15 找不到 <iconv.h>。

根因：bionic 的 iconv 要 API 28 才有，而且只認 UTF-8／UTF-16／ASCII／
Latin-1 —— 沒有 CP950／GBK／Shift-JIS，iconv_open 必然失敗。

修法：__ANDROID__ 整段排除，走既有的逐位元組替換保底路徑。代價有界：
檔案總管壓的非 UTF-8 中文檔名會是 U+FFFD 亂碼，但往返一致、模型讀得到。"
```

---

## Task 5: Cubism Core / Framework / glew 的 Android 分支

**Files:**
- Modify: `cmake/SetupCubismCore.cmake:32-56`
- Modify: `cmake/FetchCubismFramework.cmake:100-115`
- Modify: `CMakeLists.txt`（glew 那一段，約 `:131-146`）

**Interfaces:**
- Consumes: Task 4 的 Android configure 指令
- Produces: `build/android/libl2m_live2d.a`

- [ ] **Step 1: 確認現況會失敗**

```bash
D:/Qt/6.11.2/android_arm64_v8a/bin/qt-cmake.bat -S . -B build/android -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DL2M_BUILD_APP=OFF -DL2M_BUILD_VIEWER=ON -DL2M_BUILD_TESTS=OFF \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
```

Expected: `SetupCubismCore.cmake` 的 `此平台的 Cubism Core 尚未設定` FATAL_ERROR。

- [ ] **Step 2: `cmake/SetupCubismCore.cmake` 加 Android 分支**

在 `if(WIN32)` 之前插入（Android 也是 UNIX，所以必須排在 `elseif(UNIX)` 之前；
放最前面最不容易被日後的分支順序改動弄壞）：

```cmake
if(ANDROID)
  # 官方 SDK 本來就附 Android 靜態庫（arm64-v8a / x86 / x86_64；
  # 沒有 armeabi-v7a，本專案也只出 arm64）。
  # ANDROID_ABI 由 NDK 的 toolchain 檔設定，直接拿來當目錄名。
  set(_core_lib "${CUBISM_CORE_DIR}/lib/android/${ANDROID_ABI}/libLive2DCubismCore.a")
  if(NOT EXISTS "${_core_lib}")
    message(FATAL_ERROR "找不到 ${ANDROID_ABI} 的 Cubism Core：${_core_lib}")
  endif()
  set_target_properties(Live2DCubismCore PROPERTIES IMPORTED_LOCATION "${_core_lib}")
elseif(WIN32)
```

並把原本的 `if(WIN32)` 那一行刪掉（已被上面的 `elseif(WIN32)` 取代）。

- [ ] **Step 3: `cmake/FetchCubismFramework.cmake` 的平台巨集與 glew**

把檔尾的平台巨集與連結那一段換成：

```cmake
# 平台巨集：Framework 的 GL renderer 靠它決定要 include 哪組 GL 標頭
if(ANDROID)
  # ES2 分支：<GLES2/gl2.h> + Tegra 擴充那一路。**Android 沒有 GLEW** ——
  # GLES 的函式是靜態符號，不需要執行期取函式指標。
  target_compile_definitions(Framework PUBLIC CSM_TARGET_ANDROID_ES2)
  target_link_libraries(Framework PUBLIC Live2DCubismCore GLESv2 EGL)
  target_include_directories(Framework PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CubismCore/include")
else()
  if(WIN32)
    target_compile_definitions(Framework PUBLIC CSM_TARGET_WIN_GL)
  elseif(APPLE)
    target_compile_definitions(Framework PUBLIC CSM_TARGET_MAC_GL)
  elseif(UNIX)
    target_compile_definitions(Framework PUBLIC CSM_TARGET_LINUX_GL)
  endif()
  # Windows / macOS / Linux 桌面 GL 都要 GLEW（靜態連結）
  target_compile_definitions(Framework PUBLIC GLEW_STATIC)
  target_link_libraries(Framework PUBLIC Live2DCubismCore glew)
  target_include_directories(Framework PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CubismCore/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glew/include")
endif()
```

- [ ] **Step 4: `CMakeLists.txt` 不在 Android 上建 glew**

把 `add_library(glew STATIC third_party/glew/src/glew.c)` 那一整段（含
`if(WIN32)/elseif(APPLE)/else()` 的連結）包進 `if(NOT ANDROID)`，並在上方加註解：

```cmake
  # ---------------------------------------------------------------------------
  # 第三方：GLEW（靜態；CubismNativeFramework 的桌面 GL renderer 需要）
  #
  # **Android 不建**：GLES 的函式是靜態符號，不需要執行期取函式指標，
  # 而且 glew.c 本身就編不過（它 include 的是桌面的 <GL/gl.h>）。
  # Framework 那邊由 CSM_TARGET_ANDROID_ES2 改走 <GLES2/gl2.h>。
  # ---------------------------------------------------------------------------
  if(NOT ANDROID)
```

- [ ] **Step 5: 建置確認**

```bash
cmake --build build/android --target l2m_live2d
```

Expected: 可能仍失敗於 `src/live2d/hit_mask.cpp` 與 `model_controller.cpp` 的
`#include <GL/glew.h>`。下一步處理。

- [ ] **Step 6: 兩個 GL 標頭的 Android 分支**

`src/live2d/model_controller.cpp:3` 換成：

```cpp
// GL 標頭必須在任何會引入 gl.h 的標頭（含 Qt 的 OpenGL 標頭）之前。
// Android 走 GLES2 + gl2ext（Cubism 的 renderer 本來就是 ES2 風格），
// 桌面走 glew 取函式指標。
#ifdef __ANDROID__
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#else
#include <GL/glew.h>
#endif
```

`src/live2d/hit_mask.cpp` **整支不在 Android 上編**。改 `CMakeLists.txt` 的
`l2m_live2d` 來源清單，把那兩行包起來：

```cmake
  # 命中遮罩只有「貼在桌面上的透明角色」需要（穿透點擊）。Android 版是
  # 不透明全螢幕，一件都用不到 —— 順帶避開它用的 glMapBufferRange /
  # glFenceSync / glClientWaitSync / glBlitFramebuffer 全是 **GLES 3.0**
  # 才有的（Cubism 的 renderer 只要 ES 2.0）。
  if(NOT ANDROID)
    target_sources(l2m_live2d PRIVATE src/live2d/hit_mask.h src/live2d/hit_mask.cpp)
  endif()
```

（同時把 `hit_mask.h` / `hit_mask.cpp` 從 `add_library(l2m_live2d STATIC ...)`
的清單裡移除。）

- [ ] **Step 7: 建置確認通過**

```bash
cmake --build build/android --target l2m_live2d
ls build/android/libl2m_live2d.a
```

Expected: 成功。

- [ ] **Step 8: Windows 回歸並 commit**

```bash
cmake -S . -B build/rel -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/rel && ctest --test-dir build/rel --output-on-failure
```

```bash
git add cmake/SetupCubismCore.cmake cmake/FetchCubismFramework.cmake CMakeLists.txt src/live2d/model_controller.cpp
git commit -m "新增：Cubism 的 Android（GLES2）建置分支

SetupCubismCore 接上官方 SDK 本來就附的 lib/android/<ABI>；
Framework 改用 CSM_TARGET_ANDROID_ES2（<GLES2/gl2.h>），不連 GLEW ——
GLES 的函式是靜態符號，而且 glew.c 本身 include 的是桌面的 <GL/gl.h>。

hit_mask.cpp 在 Android 上整支不編：命中遮罩是透明桌寵才需要的，
而它用的 glMapBufferRange / glFenceSync / glBlitFramebuffer 全要 GLES 3.0，
Cubism 的 renderer 只需要 ES 2.0。"
```

---

## Task 6: shader 延遲產生涵蓋 Android ES2

`patches/cubism-framework-lazy-blend-shaders.patch` 的檔頭明寫「Android ES2 分支維持原本的預先產生」。那 474 支在桌面 NVIDIA 上量到 **2,995 ms**；手機 GPU 上會是 ANR。patch 的索引逆推邏輯與平台無關。

**Files:**
- Modify: `patches/cubism-framework-lazy-blend-shaders.patch`

**Interfaces:**
- Consumes: Task 5 的 Android 建置
- Produces: Android 上 `CreateRenderer` 不再預編 474 支 shader

- [ ] **Step 1: 量出現況（先有數字再改）**

暫時在 `src/live2d/model_controller.cpp` 的 `CreateRenderer` 呼叫前後加
`QElapsedTimer` 並 `qInfo() << "[live2d] CreateRenderer" << ms`，
建置 Task 7 的 APK 後從 logcat 讀。

> 若 Task 7 尚未完成，這一步延後到 Task 7 之後補做，但 patch 本身照 Step 2 先改。

- [ ] **Step 2: 改 patch —— `GenerateShaders()` 的 Android 預生成迴圈**

patch 中對應 `@@ -515,6 +502,9 @@` 的那一段，把新增的
`#ifdef CSM_TARGET_ANDROID_ES2 ... #endif` 註解與整段預生成迴圈改成**刪除**
（即讓 Android 也不預生成）。具體：把 patch 裡那段以 `+` 開頭、內容為
`#ifdef CSM_TARGET_ANDROID_ES2` 到對應 `#endif` 的行整組移除，
並把原本以空白開頭（context）的預生成迴圈改成以 `-` 開頭（刪除）。

- [ ] **Step 3: 改 patch —— `EnsureBlendShaderSet()` 的平台護欄**

patch 中新增 `EnsureBlendShaderSet()` 的那一段（`@@ -539,6 +529,80 @@`），
把函式本體開頭的 `#ifndef CSM_TARGET_ANDROID_ES2` 與結尾對應的 `#endif`
兩行刪掉，並把函式上方的註解補一句：

```
// **Android ES2 走同一條路**：手機 GPU 編 474 支 shader 的時間遠比桌面糟，
// 那正是 ANR watchdog 會殺 app 的量級。索引逆推與平台無關（純算術），
// static_assert 也一樣保護得到。
```

- [ ] **Step 4: 更新 patch 檔頭的說明**

把檔頭「Android ES2 分支維持原本的預先產生，完全不受影響。」那一行改成：

```
桌面與 Android ES2 走同一條延遲路徑。原版把 Android 排除在外，但手機 GPU 編
474 支 shader 的耗時遠比桌面的 2,858 ms 更糟，那是 ANR watchdog 的量級。
```

- [ ] **Step 5: 確認 patch 仍套得上（兩邊都要）**

```bash
rm -rf build/rel/_deps/cubismnativeframework-src
cmake -S . -B build/rel -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

Expected: `CubismNativeFramework patch：已套用`，無 FATAL_ERROR。

```bash
rm -rf build/android/_deps/cubismnativeframework-src
D:/Qt/6.11.2/android_arm64_v8a/bin/qt-cmake.bat -S . -B build/android -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DL2M_BUILD_APP=OFF -DL2M_BUILD_VIEWER=ON \
  -DL2M_BUILD_TESTS=OFF -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
cmake --build build/android --target l2m_live2d
```

Expected: 兩邊都成功。

- [ ] **Step 6: Windows 上驗證行為沒變**

```bash
cmake --build build/rel && build/rel/live2d_mate.exe
```

手動確認模型正常顯示（含有用到混合模式的模型，第一次畫到時會就地編一支）。

- [ ] **Step 7: Commit**

```bash
git add patches/cubism-framework-lazy-blend-shaders.patch
git commit -m "效能：混合模式 shader 的延遲產生也涵蓋 Android ES2

原版 patch 刻意把 Android 排除在外。但那 474 支在桌面 NVIDIA 上就要
2,858 ms，手機 GPU 只會更糟 —— 那是 ANR watchdog 殺 app 的量級。

索引逆推是純算術、與平台無關，static_assert 也一樣保護得到，
所以拿掉 EnsureBlendShaderSet() 的 #ifndef CSM_TARGET_ANDROID_ES2
與 GenerateShaders() 裡那段 Android 專用的預生成迴圈即可。"
```

---

## Task 7: `live2d_viewer` 出 APK 並在真機顯示模型

第一個看得到東西的里程碑。用 Viewer 當載體是因為它已經滿足所有 Android 的結構條件（QOpenGLWidget、不透明、單一視窗、不連 `l2m_media`／httplib／spdlog），總共只有 846 行。

**Files:**
- Create: `android/AndroidManifest.xml`
- Create: `resources/android_assets.qrc`
- Modify: `src/viewer/main.cpp:48-53`（QSurfaceFormat）
- Modify: `src/viewer/viewer_canvas.cpp:1-2, 84-90`（glew）
- Modify: `CMakeLists.txt`（viewer 的 Android 設定）
- Modify: `docs/ANDROID_SETUP.md`

**Interfaces:**
- Consumes: Task 3 的 `assetDirCandidates`、Task 5 的 GLES 建置
- Produces: `build/android/android-build/live2d_viewer.apk`

- [ ] **Step 1: `QSurfaceFormat` 的 Android 分支**

`src/viewer/main.cpp:48-53` 換成：

```cpp
  QSurfaceFormat fmt;
  fmt.setAlphaBufferSize(8);
#ifdef Q_OS_ANDROID
  // GLES **沒有 profile 這回事** —— setProfile 在 ES 上是無意義的，
  // 而桌面那個 CompatibilityProfile 的理由（Cubism 用 client-side vertex
  // array，core profile 完全禁止）在這裡自動消失：那本來就是 ES2 的原生寫法。
  fmt.setRenderableType(QSurfaceFormat::OpenGLES);
  fmt.setVersion(2, 0);
#else
  fmt.setRenderableType(QSurfaceFormat::OpenGL);
  fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
#endif
  fmt.setSwapInterval(1);  // vsync
  QSurfaceFormat::setDefaultFormat(fmt);
```

- [ ] **Step 2: `viewer_canvas.cpp` 的 glew 分支**

檔頭第 1-2 行換成：

```cpp
// GL 標頭必須在任何會引入 gl.h 的標頭（含 Qt 的 OpenGL 標頭）之前
#ifdef __ANDROID__
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#else
#include <GL/glew.h>
#endif
```

`initializeGL()` 的前六行換成：

```cpp
void ViewerCanvas::initializeGL() {
#ifndef __ANDROID__
  // Framework 的 GL renderer 用 glew 取函式指標。
  // Android 上 GLES 的函式是靜態符號，沒有這一步。
  glewExperimental = GL_TRUE;
  const GLenum glewResult = glewInit();
  if (glewResult != GLEW_OK) {
    qWarning() << "[live2d] glewInit 失敗:" << reinterpret_cast<const char*>(glewGetErrorString(glewResult));
  }
  glGetError();  // 清掉 glewInit 可能留下的無害錯誤
#endif
```

- [ ] **Step 3: 資源 qrc**

建立 `resources/android_assets.qrc`：

```xml
<RCC>
  <qresource prefix="/i18n">
    <file alias="en.json">../i18n/en.json</file>
    <file alias="ja.json">../i18n/ja.json</file>
    <file alias="ko.json">../i18n/ko.json</file>
    <file alias="zh-CN.json">../i18n/zh-CN.json</file>
    <file alias="zh-TW.json">../i18n/zh-TW.json</file>
  </qresource>
</RCC>
```

FrameworkShaders 的路徑是 FetchContent 才知道的，所以那一份要在 configure 期產生。
`CMakeLists.txt` 的 `if(ANDROID)` 區段裡加：

```cmake
  # FrameworkShaders 的來源目錄要等 FetchContent 跑完才知道，所以這份 qrc
  # 在 configure 期產生。Android 的 APK 裡沒有「執行檔旁邊」，兩個資源目錄
  # 都必須編進資源（見 core/asset_paths.h）。
  file(GLOB l2m_shader_files
       "${cubismnativeframework_SOURCE_DIR}/src/Rendering/OpenGL/Shaders/Standard/*")
  set(l2m_shader_qrc_body "")
  foreach(l2m_shader IN LISTS l2m_shader_files)
    get_filename_component(l2m_shader_name "${l2m_shader}" NAME)
    string(APPEND l2m_shader_qrc_body
           "    <file alias=\"${l2m_shader_name}\">${l2m_shader}</file>\n")
  endforeach()
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/android_shaders.qrc"
       "<RCC>\n  <qresource prefix=\"/FrameworkShaders\">\n${l2m_shader_qrc_body}  </qresource>\n</RCC>\n")
```

- [ ] **Step 4: `AndroidManifest.xml`**

建立 `android/AndroidManifest.xml`：

```xml
<?xml version="1.0"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
          package="com.rookiestudio.live2dmate"
          android:versionCode="1"
          android:versionName="1.0.0"
          android:installLocation="auto">
  <uses-permission android:name="android.permission.INTERNET"/>
  <uses-feature android:glEsVersion="0x00020000" android:required="true"/>
  <application android:label="Live2D Mate"
               android:hardwareAccelerated="true"
               android:extractNativeLibs="true">
    <activity android:name="org.qtproject.qt.android.bindings.QtActivity"
              android:label="Live2D Mate"
              android:screenOrientation="portrait"
              android:configChanges="orientation|uiMode|screenLayout|screenSize|smallestScreenSize|layoutDirection|locale|fontScale|keyboard|keyboardHidden|navigation|mcc|mnc|density"
              android:exported="true">
      <intent-filter>
        <action android:name="android.intent.action.MAIN"/>
        <category android:name="android.intent.category.LAUNCHER"/>
      </intent-filter>
      <meta-data android:name="android.app.lib_name" android:value="-- %%INSERT_APP_LIB_NAME%% --"/>
      <meta-data android:name="android.app.arguments" android:value="-- %%INSERT_APP_ARGUMENTS%% --"/>
    </activity>
  </application>
</manifest>
```

- [ ] **Step 5: `CMakeLists.txt` 的 viewer Android 設定**

在 `if(L2M_BUILD_VIEWER)` 區段內、`target_link_libraries` 之後加：

```cmake
  if(ANDROID)
    # 資源編進 APK（Android 沒有「執行檔旁邊」，見 core/asset_paths.h）
    target_sources(live2d_viewer PRIVATE resources/android_assets.qrc
                                         "${CMAKE_CURRENT_BINARY_DIR}/android_shaders.qrc")
    set_target_properties(
      live2d_viewer
      PROPERTIES QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
                 QT_ANDROID_MIN_SDK_VERSION 28
                 QT_ANDROID_TARGET_SDK_VERSION 35
                 QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
                 QT_ANDROID_VERSION_CODE 1)
  endif()
```

同時把 `qt_add_executable(live2d_viewer WIN32 MACOSX_BUNDLE ...)` 的
`WIN32 MACOSX_BUNDLE` 保留即可（Android 上這兩個關鍵字無作用）。

- [ ] **Step 6: 建 APK**

```bash
D:/Qt/6.11.2/android_arm64_v8a/bin/qt-cmake.bat -S . -B build/android -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DL2M_BUILD_APP=OFF -DL2M_BUILD_VIEWER=ON \
  -DL2M_BUILD_TESTS=OFF -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
cmake --build build/android --target live2d_viewer_make_apk
```

Expected: 產出 `build/android/android-build/build/outputs/apk/debug/android-build-debug.apk`。

- [ ] **Step 7: 安裝並推一個模型上去**

```bash
adb install -r build/android/android-build/build/outputs/apk/debug/android-build-debug.apk
adb shell am start -n com.rookiestudio.live2dmate/org.qtproject.qt.android.bindings.QtActivity
adb logcat -c && adb logcat | grep -E "live2d|cubism|Qt"
```

Viewer 需要一個模型路徑。先確認 app 可寫目錄：在 `src/viewer/main.cpp` 的
`QApplication app(...)` 之後暫時加

```cpp
  qInfo() << "[live2d] AppDataLocation:" << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
```

從 logcat 讀出實際路徑，再：

```bash
adb push "path/to/Model.zip" /sdcard/Android/data/com.rookiestudio.live2dmate/files/Model.zip
```

（若 logcat 印出的是 `/data/user/0/...` 這種內部路徑，改用
`adb push` 到 `/data/local/tmp/` 再 `adb shell run-as com.rookiestudio.live2dmate cp ...`。
把實際可行的那一條寫進 `docs/ANDROID_SETUP.md`。）

- [ ] **Step 8: 真機驗收**

用 Viewer 的「開啟模型…」選那個 zip。

Expected:
- 模型顯示在畫布上，會呼吸、會眨眼
- 手指在畫布上移動時視線跟隨
- logcat 沒有 `[cubism] 讀不到檔案` 或 `[live2d] 模型載入失敗`
- **量 `CreateRenderer` 的耗時**（Task 6 Step 1 加的那行），確認遠低於 2,995 ms

- [ ] **Step 9: 補文件並 commit**

把 Step 6-8 寫進 `docs/ANDROID_SETUP.md` 的 `## 2. 建置與安裝`、`## 3. 推模型上去`。

```bash
git add android resources/android_assets.qrc src/viewer/main.cpp src/viewer/viewer_canvas.cpp CMakeLists.txt docs/ANDROID_SETUP.md
git commit -m "新增：live2d_viewer 的 Android 建置（第一個能在手機上跑的版本）

QSurfaceFormat 走 OpenGLES 2.0 且不設 profile（GLES 沒有 profile 這回事，
而桌面那個 CompatibilityProfile 的理由 —— Cubism 用 client-side vertex
array —— 在 ES2 上本來就是原生寫法）；initializeGL 的 glewInit 在 Android
上整段跳過。

i18n 與 FrameworkShaders 編進 qrc：APK 裡沒有「執行檔旁邊」。
shader 的 qrc 在 configure 期產生，因為來源目錄要等 FetchContent 才知道。"
```

---

# Phase 2 — 手機上的桌寵與 MCP

## Task 8: `src/platform/*_android.cpp` 五支

`live2d_mate` 在非 Windows／非 Apple 時走 `else()` 分支連 X11。Android 需要自己的一組，全部是 no-op —— 照 `src/platform/crash_handler_linux.cpp` 的既有前例（那支本來就是空殼，檔頭寫明理由）。

**Files:**
- Create: `src/platform/window_effects_android.cpp`
- Create: `src/platform/autostart_android.cpp`
- Create: `src/platform/user_idle_android.cpp`
- Create: `src/platform/console_android.cpp`
- Create: `src/platform/crash_handler_android.cpp`
- Modify: `CMakeLists.txt`（`live2d_mate` 的平台來源分支）

**Interfaces:**
- Produces: `l2m::platform::{applyClickableRegion, clearClickableRegion, keepVisibleWhenAppInactive, stripLayeredStyle, isTopmost, applyTopmost}` 的 Android no-op
- Produces: `l2m::platform::{setOpenAtLogin, isOpenAtLogin}`、`{userIdleMs}`、`{attachParentConsole}`
- Produces: `l2m::platform::{appDataDirFromEnv, installCrashHandler, installThreadCrashSupport, triggerCrashTestFromEnv}`

> 下面五段的函式清單是照五份標頭逐一核對過的（`window_effects.h` 6 支、
> `autostart.h` 2 支、`user_idle.h` 1 支、`console.h` 1 支、`crash_handler.h` 4 支）。
> **少一支就是連結期一串 undefined symbol** —— 那些標頭是無條件 include 的。

- [ ] **Step 1: `window_effects_android.cpp`**

```cpp
// Android 的視窗效果：全部是 no-op。
//
// 對應 window_effects_win.cpp / window_effects_linux.cpp。這裡什麼都不做
// 不是「還沒實作」，而是**這些概念在 Android 上不存在**：
//   穿透點擊 —— 桌寵是全螢幕不透明的，底下沒有別的視窗可以穿透過去
//   置頂     —— Activity 要嘛在前景要嘛不在，沒有 z 序這回事
//   WS_EX_LAYERED —— Windows 專有的問題，Qt 在 Android 上不會掛那種東西
//
// 退化成 no-op 的做法照 window_effects_linux.cpp 在 Wayland session 下的前例：
// 功能失效，但程式照常跑。
//
// isTopmost 回 false 而不是 true：它的語意是「**原生實況**是不是置頂」
// （topmost_watchdog 存在的理由）。Android 上沒有這個概念，回 false 等於
// 「我沒有把它設成置頂」，與這裡 applyTopmost 什麼都不做是一致的。

#include "window_effects.h"

namespace l2m::platform {

void applyClickableRegion(QWindow*, const QRegion&) {}
void clearClickableRegion(QWindow*) {}
void keepVisibleWhenAppInactive(QWindow*) {}
void stripLayeredStyle(QWindow*) {}
bool isTopmost(QWindow*) { return false; }
void applyTopmost(QWindow*, bool) {}

}  // namespace l2m::platform
```

- [ ] **Step 2: `autostart_android.cpp`**

```cpp
// Android 的開機自啟：no-op。
//
// 對應的機制是 BOOT_COMPLETED 的 BroadcastReceiver，而那要 Java 端配合、
// 還要使用者手動放行自啟權限（各廠 ROM 規則不同）。**這次不做** ——
// 使用者的情境是「手機放在桌旁、插著電、螢幕常亮」，那是手動開一次的用法。
// 介面保留是為了讓 app 那邊不必到處 #ifdef。

#include "autostart.h"

namespace l2m::platform {

void setOpenAtLogin(bool) {}
bool isOpenAtLogin() { return false; }

}  // namespace l2m::platform
```

- [ ] **Step 3: `user_idle_android.cpp`**

```cpp
// Android 拿不到「使用者閒置了多久」：no-op（回 nullopt）。
//
// 對應 user_idle_win.cpp（GetLastInputInfo）。Android 沒有等價的全域 API，
// 而且真正該用的訊號是不一樣的東西 —— Activity 的 onPause/onResume
// （見 src/android/android_shell.h）比「多久沒動」更準。
//
// 介面已保證呼叫端能在 nullopt 下照常運作（PresenceTracker 維持 Active，
// 寧可多演也不要誤判成「沒人」），同 user_idle_linux.cpp。

#include "user_idle.h"

namespace l2m::platform {

std::optional<double> userIdleMs() { return std::nullopt; }

}  // namespace l2m::platform
```

- [ ] **Step 4: `console_android.cpp` 與 `crash_handler_android.cpp`**

```cpp
// Android 沒有「GUI 子系統」這回事：no-op（同 console_linux.cpp / console_mac.mm）。
// qDebug 走 Qt 的 Android message handler，直接進 logcat。

#include "console.h"

namespace l2m {
namespace platform {

void attachParentConsole() {}

}  // namespace platform
}  // namespace l2m
```

```cpp
// Android 的當機處理：空殼（同 crash_handler_linux.cpp / crash_handler_mac.mm）。
//
// 現有實作整份是 Windows 專有的（MSVC CRT 的三個進場點 + DbgHelp 擷取堆疊）。
// Android 上系統的 tombstone 與 logcat 已經記了原生堆疊，
// 而且 adb 就拿得到 —— 自己再做一份的收益遠低於桌面。
//
// appDataDirFromEnv() 必須與 QStandardPaths::AppDataLocation 在 Android 上
// 算出來的路徑一致（存在的理由就是「QApplication 還沒建立時算出同一個目錄」）。
// Android 上那個路徑由 JNI 的 Context.getFilesDir() 決定，環境變數推不出來，
// 所以這裡回空 path —— 呼叫端（src/android/main.cpp 根本不呼叫
// installCrashHandler，這一支留著只是為了讓標頭的宣告有實作）。

#include "crash_handler.h"

namespace l2m {
namespace platform {

std::filesystem::path appDataDirFromEnv() { return {}; }
void installCrashHandler(const std::filesystem::path&, const char*) {}
void installThreadCrashSupport() {}
void triggerCrashTestFromEnv() {}

}  // namespace platform
}  // namespace l2m
```

- [ ] **Step 5: `CMakeLists.txt` 的平台分支**

`live2d_mate` 的 `if(WIN32)/elseif(APPLE)/else()` 鏈中，在 `else()` 之前插入：

```cmake
  elseif(ANDROID)
    # 五支全部是 no-op，理由寫在各自的檔頭。**必須列進來**，
    # 少一支就是連結期一串 undefined symbol（那些標頭是無條件 include 的）。
    target_sources(live2d_mate PRIVATE src/platform/window_effects_android.cpp
                                       src/platform/autostart_android.cpp
                                       src/platform/user_idle_android.cpp
                                       src/platform/console_android.cpp
                                       src/platform/crash_handler_android.cpp)
```

- [ ] **Step 6: 驗證與 commit**

這一步還不能建 `live2d_mate`（`src/android/main.cpp` 還沒有）。先驗證
Windows 回歸沒被 CMake 的分支改動影響：

```bash
cmake -S . -B build/rel -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/rel && ctest --test-dir build/rel --output-on-failure
```

```bash
git add src/platform CMakeLists.txt
git commit -m "新增：src/platform 的五支 Android 實作（全部是 no-op）

穿透、置頂、閒置偵測、console、crash handler 在 Android 上要嘛不存在、
要嘛該用別的訊號。退化成 no-op 的做法照 window_effects_linux.cpp 在
Wayland 下的既有前例：功能失效，程式照常跑。每支的理由寫在檔頭。"
```

---

## Task 9: `CharacterWindow` 與 `WindowManager` 的兩個退化開關

`AppController` 的建構子吃 `CharacterWindow&` 與 `WindowManager&` 兩個具體型別（用到 14 + 10 個成員）。抽介面等於重構一個能跑的桌面 app，風險不成比例 —— 改成讓這兩個類別自己有一個「全螢幕、不做遮罩」的模式。**桌面路徑一個字不動。**

**Files:**
- Modify: `src/windows/character_window.h`、`character_window.cpp`
- Modify: `src/windows/window_manager.h`、`window_manager.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `void CharacterWindow::setHitMaskEnabled(bool)`（預設 `true`）
- Produces: `WindowManager(QWindow&, ConfigStore&, bool fixedFullscreen, QObject* parent = nullptr)`

- [ ] **Step 1: `CharacterWindow` 的遮罩開關**

`src/windows/character_window.h` 的 public 區加：

```cpp
  // 命中遮罩（alpha 回讀求視窗形狀）的總開關。**必須在 initializeGL 之前設定**
  //（Android 版在建構後、show() 之前就呼叫）。
  //
  // 關掉的效果是 hitMask_ 根本不建立，於是 isOpaqueAt() 一律回 true、
  // modelBottomNormalized() 回 nullopt —— 那兩支在 character_window.cpp:92 與 :97
  // **本來就有 `!hitMask_` 的護欄**，所以這裡只要不建它、並跳過 paintGL 裡那一段。
  //
  // Android 版關掉：不透明全螢幕沒有穿透可言，而且遮罩用的 glMapBufferRange /
  // glFenceSync 全是 GLES 3.0 才有的（Cubism 的 renderer 只需要 ES 2.0）。
  void setHitMaskEnabled(bool enabled);
```

`character_window.cpp`：
- `setHitMaskEnabled` 存進 `hitMaskEnabled_`（新成員，預設 `true`），false 時 `hitMask_.reset()`
- `:125` 的 `hitMask_ = std::make_unique<l2m::AlphaHitMask>();` 包上 `if (hitMaskEnabled_)`
- `paintGL()` 裡 `:182-211` 那整段遮罩與 region 的處理包上 `if (hitMask_)`
- `isOpaqueAt()`（`:92`）與 `modelBottomNormalized()`（`:97`）**不用改** —— 它們本來就有 `!hitMask_` 的護欄

- [ ] **Step 2: `WindowManager` 的全螢幕模式**

`src/windows/window_manager.h` 的建構子改成：

```cpp
  // fixedFullscreen：視窗永遠佔滿螢幕，位置／大小／透明度全部不接受變更。
  // Android 用 —— 那邊沒有「視窗」這個概念，所有 move/scale/opacity 的
  // 要求（含 MCP 的 move_to、系統匣的大小選單）都該安靜地無效化而不是報錯。
  // **桌面路徑一個字不動**：預設 false 時所有行為與原本完全相同。
  WindowManager(QWindow& window, ConfigStore& config, bool fixedFullscreen, QObject* parent = nullptr);
```

`window_manager.cpp`：
- `applyBounds()` 在 `fixedFullscreen_` 時改成 `window_.showFullScreen()` 後
  `emit boundsChanged(window_.geometry())` 並 return
- `moveBy` / `moveTo` / `moveToRatio` / `moveToPreset` 在 `fixedFullscreen_` 時
  直接回傳 `window_.geometry().topLeft()`（不寫 config）
- `applyScale` / `setOpacity` / `setAlwaysOnTop` 在 `fixedFullscreen_` 時直接 return
- `setVisible` / `isVisible` / `bounds` 維持原本行為（AppController 會用）

`src/app/main.cpp` 建構 `WindowManager` 的那一行補上 `false`。

- [ ] **Step 3: `CMakeLists.txt` 讓 Android 也編這兩支**

Android 版的來源清單（Task 11 會建立）要包含
`src/windows/character_window.{h,cpp}` 與 `src/windows/window_manager.{h,cpp}`。
這一步先確認它們沒有 Windows 專屬的 include：

```bash
grep -n "#include" src/windows/character_window.cpp src/windows/window_manager.cpp | grep -iv "^.*:.*#include [\"<]\(Q\|core/\|live2d/\|\.\./\|GL/glew\|functional\|memory\|algorithm\|cmath\|optional\)"
```

Expected: 沒有輸出（或只有 `platform/window_effects.h`，那支 Task 8 已經有 Android 版）。
`character_window.cpp:1-2` 的 glew include 照 Task 7 Step 2 的寫法加 Android 分支。

- [ ] **Step 4: Windows 回歸**

```bash
cmake --build build/rel && ctest --test-dir build/rel --output-on-failure
build/rel/live2d_mate.exe
```

手動確認：拖曳、滾輪縮放、穿透點擊、切模型的腳底對齊全都與改動前相同
（`hitMaskEnabled_` 預設 true、`fixedFullscreen_` 預設 false）。

- [ ] **Step 5: Commit**

```bash
git add src/windows/character_window.h src/windows/character_window.cpp src/windows/window_manager.h src/windows/window_manager.cpp src/app/main.cpp
git commit -m "新增：CharacterWindow 的遮罩開關、WindowManager 的全螢幕模式

AppController 的建構子吃這兩個具體型別（用到 14 + 10 個成員），抽介面
等於重構一個能跑的桌面 app。改成讓它們自己有一個退化模式，Android 版
建構時打開，桌面路徑一個字不動（兩個旗標的預設值就是原本的行為）。

setHitMaskEnabled(false) 的效果是 hitMask_ 根本不建立 —— isOpaqueAt 與
modelBottomNormalized 在 character_window.cpp:92/:97 本來就有 !hitMask_
的護欄，所以那兩支不必改。"
```

---

## Task 10: 氣泡 —— 把角色與氣泡裝進一個全螢幕的 root widget

**這一個 Task 的重點是「不要重寫氣泡」。**

`BubbleWindow` 的 `paintEvent`（`bubble_window.cpp:356-420`）用了六個私有 helper
（`buildLayout`／`ellipseRect`／`runLayoutPass`／`speechTail`／`thoughtDots`／
`fillShadowPath`／`boxBlur`）與快取好的 `QTextLayout`，重寫等於把
`core/ellipse_text_fit.h` 的收斂規則、尾巴相切的接縫處理、陰影不能進位的
dpr 陷阱全部再踩一遍。而且 `SpeechController` 的建構子吃的就是
`BubbleWindow&`（`speech_controller.h:67`，只用到 `showText` 與
`hideBubble` 兩支，`speech_controller.cpp:223, 237`）。

所以做法是：**`BubbleWindow` 一個字都不改，只是不讓它當 top-level**。
Android 上開一個全螢幕的 root `QWidget`，把 `CharacterWindow`（QWindow）
用 `QWidget::createWindowContainer` 包成子 widget 鋪滿，`BubbleWindow`
當它的兄弟浮在上面。

**這裡有一個必須先驗證的假設**：Qt for Android 把所有視窗畫進同一個
Activity，`createWindowContainer` 產生的原生子視窗在那個模型下行不行得通，
沒有實測過。所以 Step 1 是 spike，plan B 寫在 Step 2。

**Files:**
- Create: `src/android/android_root.h`、`src/android/android_root.cpp`

**Interfaces:**
- Consumes: `CharacterWindow`（Task 9 的 `setHitMaskEnabled`）
- Consumes: `l2m::BubbleWindow`（既有，不改）
- Produces: `class AndroidRoot : public QWidget`，持有 container 與 `BubbleWindow&`，`resizeEvent` 讓 container 鋪滿

- [ ] **Step 1: Spike —— 先驗證 `createWindowContainer` 在 Android 上能用**

在 `src/viewer/main.cpp` 裡暫時做一個最小測試（Viewer 已經有可用的 APK 流程）：

```cpp
  // === 暫時的 spike，驗證完整段刪掉 ===
  QWidget root;
  auto* canvasWindow = new QOpenGLWindow();  // 只要能顯示就好
  QWidget* container = QWidget::createWindowContainer(canvasWindow, &root);
  container->setGeometry(root.rect());
  QLabel* overlay = new QLabel(QStringLiteral("overlay"), &root);
  overlay->setStyleSheet(QStringLiteral("background: rgba(255,255,255,200); font-size: 40px;"));
  overlay->move(60, 200);
  overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
  root.showFullScreen();
```

建 APK、裝上真機。

Expected: GL 內容顯示，`overlay` 的文字浮在上面。

- [ ] **Step 2: 依 spike 結果選路**

- **成功** → 照 Step 3 往下走。
- **失敗**（GL 一片黑、overlay 不見、或 container 佔不滿）→ **plan B**：
  Android 版的畫布改用 `QOpenGLWidget`（Viewer 已經證明它在 Android 上可行），
  也就是把 `CharacterWindow` 從 `QOpenGLWindow` 改成同時提供一個
  `QOpenGLWidget` 版本。那是比較大的改動，**成本估計多 3~5 天**，
  要先回報再往下做，不要自行決定。

無論走哪條，**把 spike 的程式碼從 `viewer/main.cpp` 刪掉**，結果寫進
`docs/ANDROID_SETUP.md` 的「已知限制」。

- [ ] **Step 3: 寫 `src/android/android_root.h`**

```cpp
#pragma once

// Android 的頂層版面：角色鋪滿、氣泡浮在上面。
//
// **為什麼需要這一層**：桌面版的角色視窗與氣泡是兩個獨立的 top-level
//（氣泡要逃出 alpha region，而且常常超出 400×600 的舞台）。Android 上
// 第二個 top-level window 會變成同一個 Activity 裡另一層 surface，行為不可靠。
//
// **但氣泡本體一個字都不改**：BubbleWindow 的繪製用了六個私有 helper 與
// 快取好的 QTextLayout（bubble_window.cpp:189-420），重寫等於把
// ellipse_text_fit 的收斂規則、尾巴相切的接縫、陰影不能進位的 dpr 陷阱
// 全部再踩一遍。而且 SpeechController 的建構子吃的就是 BubbleWindow&
//（speech_controller.h:67）。所以這裡只是**換一個 parent**：
// top-level → 這個 root 的子 widget。
//
// CharacterWindow 是 QWindow，用 createWindowContainer 包成子 widget。
// 氣泡要 WA_TransparentForMouseEvents，否則它蓋住的那一塊觸控不到角色。

#include <QWidget>

namespace l2m {
class BubbleWindow;
}
class CharacterWindow;

namespace l2m {

class AndroidRoot : public QWidget {
  Q_OBJECT

public:
  AndroidRoot(CharacterWindow& character, BubbleWindow& bubble, QWidget* parent = nullptr);

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  QWidget* container_ = nullptr;
  BubbleWindow& bubble_;
};

}  // namespace l2m
```

- [ ] **Step 4: 寫 `src/android/android_root.cpp`**

```cpp
#include "android_root.h"

#include <QResizeEvent>

#include "windows/bubble_window.h"
#include "windows/character_window.h"

namespace l2m {

AndroidRoot::AndroidRoot(CharacterWindow& character, BubbleWindow& bubble, QWidget* parent) : QWidget(parent), bubble_(bubble) {
  container_ = QWidget::createWindowContainer(&character, this);
  container_->setGeometry(rect());
  // 氣泡改當這個 root 的子 widget（原本是 top-level）。
  // WA_TransparentForMouseEvents：不然氣泡蓋住的那一塊觸控不到角色 ——
  // 而氣泡出現時角色多半正在說話，那正是使用者最想戳它的時候。
  bubble_.setParent(this);
  bubble_.setAttribute(Qt::WA_TransparentForMouseEvents);
  bubble_.raise();
}

void AndroidRoot::resizeEvent(QResizeEvent* event) {
  if (container_) container_->setGeometry(rect());
  bubble_.raise();
  QWidget::resizeEvent(event);
}

}  // namespace l2m
```

- [ ] **Step 5: 驗證（併進 Task 11 的真機測試）**

這一步沒有獨立的驗證方式 —— `AndroidRoot` 要等 Task 11 的 `main.cpp`
把它組起來才跑得動。**先 commit，驗證在 Task 11 Step 6 一併做**：
氣泡出現在角色上方、文字不溢出、氣泡蓋住的地方仍然觸控得到角色。

- [ ] **Step 6: Commit**

```bash
git add src/android/android_root.h src/android/android_root.cpp docs/ANDROID_SETUP.md
git commit -m "新增：Android 的頂層版面（角色鋪滿、氣泡浮在上面）

Android 上第二個 top-level window 會變成同一個 Activity 裡另一層 surface，
行為不可靠，所以氣泡要改當子 widget。

**但 BubbleWindow 一個字都不改** —— 它的繪製用了六個私有 helper 與快取的
QTextLayout（bubble_window.cpp:189-420），重寫等於把 ellipse_text_fit 的
收斂規則、尾巴相切的接縫、陰影不能進位的 dpr 陷阱全部再踩一遍；
而且 SpeechController 的建構子吃的就是 BubbleWindow&。這裡只換 parent。

氣泡加 WA_TransparentForMouseEvents：不然它蓋住的那一塊觸控不到角色，
而氣泡出現時正是使用者最想戳它的時候。"
```

---

## Task 11: Android 進入點 —— 角色出現在手機上

**Files:**
- Create: `src/android/main.cpp`
- Create: `src/android/android_shell.h`、`src/android/android_shell.cpp`
- Modify: `CMakeLists.txt`
- Modify: `docs/ANDROID_SETUP.md`

**Interfaces:**
- Consumes: Task 9 的兩個開關、Task 10 的 `AndroidRoot`、Task 8 的 platform no-op
- Produces: `void l2m::android::keepScreenOn(bool)`、`void l2m::android::enterImmersiveFullscreen()`、`std::string l2m::android::appFilesDir()`
- Produces: APK `com.rookiestudio.live2dmate`，啟動後全螢幕顯示角色

- [ ] **Step 1: `src/android/android_shell.h`**

```cpp
#pragma once

// Android 平台互動的**唯一**出入口（JNI 全部關在這裡）。
//
// 刻意不放 src/platform/：那個目錄的五支是「桌面既有介面的 Android 空殼」，
// 這裡則是 Android 才有的能力，沒有對應的桌面介面可以實作。
// 混在一起會讓 platform/ 的標頭長出一半在別的平台永遠是 no-op 的宣告。

#include <string>

namespace l2m::android {

// 螢幕常亮（FLAG_KEEP_SCREEN_ON）。桌寵放在桌上的整個前提。
void keepScreenOn(bool on);

// 沉浸式全螢幕：藏掉狀態列與導覽列。
void enterImmersiveFullscreen();

// app 的可寫資料目錄（Context.getExternalFilesDir(null)，拿不到時退回
// getFilesDir()）。**優先用 external** —— 那個路徑 adb push 進得去，
// 而模型檔在做出匯入 UI 之前只能靠 adb 推上來。
std::string appFilesDir();

}  // namespace l2m::android
```

- [ ] **Step 2: `src/android/android_shell.cpp`**

用 `QJniObject` / `QNativeInterface::QAndroidApplication::context()` 實作三支。
`keepScreenOn` 走 `activity.getWindow().addFlags(0x00000080)`
（`WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON`），
**必須用 `QNativeInterface::QAndroidApplication::runOnAndroidMainThread`**
包起來（那些 API 只能在 Android 的 UI thread 上呼叫）。

- [ ] **Step 3: `src/android/main.cpp`**

照 `src/app/main.cpp` 的物件圖，但砍掉 splash 子行程、single instance、
tray、設定視窗、autostart、crash handler。檔頭要寫清楚為什麼是新開一份：

```cpp
// Live2D Mate 的 Android 進入點。
//
// **為什麼不是在 src/app/main.cpp 裡加 #ifdef**：那 677 行裡 Android 用不到的
// 東西（splash 子行程、single-instance 鎖、系統匣、七分頁設定視窗、開機自啟、
// crash handler）將近一半，而且彼此交纏（splash 的 QLocalSocket 接線、
// Tray::Deps、SettingsWindow 的 SettingsContext）。那個檔案本來就是
// 「順序敏感，改動前先讀註解」的檔案 —— 最不該塞第二條控制流的地方。
// 照 src/viewer/main.cpp 的前例：新進入點、共用所有函式庫。
//
// 物件圖（解構是建構的反序）：
//   ConfigStore → CharacterWindow → WindowManager(fullscreen)
//     → AppController → TTS 一組 → McpTools → McpHttpServer
//
// 順序敏感的三件事與桌面版相同，理由見 src/app/main.cpp：
//   1. QSurfaceFormat 必須在 QApplication 之前（這裡是 OpenGLES 2.0，
//      **不設 profile** —— GLES 沒有 profile 這回事）
//   2. setApplicationName 必須在任何 QStandardPaths 查詢之前
//   3. aboutToQuit 裡 mcpServer.stop() 排第一
//
// **Android 多一條**：aboutToQuit 不保證跑得到（系統可以直接殺行程），
// 所以 ConfigStore::flush() 另外掛在 applicationStateChanged 的
// Qt::ApplicationSuspended（Qt 在 Android 的 onPause）。
```

主體要做的事：

```cpp
int main(int argc, char* argv[]) {
  QSurfaceFormat fmt;
  fmt.setRenderableType(QSurfaceFormat::OpenGLES);
  fmt.setVersion(2, 0);
  fmt.setSwapInterval(1);
  QSurfaceFormat::setDefaultFormat(fmt);

  QCoreApplication::setApplicationName(QStringLiteral("live2d_mate"));
  QApplication app(argc, argv);

  // APK 裡沒有「執行檔旁邊」，兩個資源目錄都在 qrc（見 core/asset_paths.h）
  l2m::i18n::setMessagesDir(std::filesystem::path(":/i18n"));

  const fs::path filesDir = fs::u8path(l2m::android::appFilesDir());
  qInfo() << "[config] 資料目錄:" << QString::fromStdString(filesDir.string());
  // ...建立 models/ personas/ memory/ 三個子目錄（照 app/main.cpp:163-183）...

  l2m::ConfigStore config(filesDir / "config.json");
  CharacterWindow window;
  window.setHitMaskEnabled(false);           // 不透明全螢幕，沒有穿透可言
  l2m::WindowManager windowManager(window, config, /*fixedFullscreen=*/true);

  // 氣泡是**沒有改過的** BubbleWindow，只是 parent 換成 AndroidRoot（見 Task 10）。
  // 這樣 SpeechController 的建構子（吃 BubbleWindow&）也完全不必動。
  l2m::BubbleWindow bubble;
  l2m::AndroidRoot root(window, bubble);

  // AppController、TTS、SpeechController、McpTools、McpHttpServer 照
  // src/app/main.cpp 的「── 語音 ──」「── 接線 ──」「── MCP ──」三節逐段搬過來。
  // **不要重寫，逐行對照著搬** —— 那三節的每一條 connect 都有理由，
  // 漏掉的症狀多半是靜默的（例如少了 SpeechController::finished → touchIdle()
  // 那條，AI 留下的狀態就再也不會復原）。
  // 唯一的差別：engines 那個 vector 不含 SapiEngine（Windows 專有），見 Task 12。

  l2m::android::keepScreenOn(true);
  l2m::android::enterImmersiveFullscreen();
  root.showFullScreen();
  windowManager.setVisible(true);

  QObject::connect(&app, &QGuiApplication::applicationStateChanged, &app, [&config](Qt::ApplicationState state) {
    // Android 可以不經 aboutToQuit 直接殺行程 —— 切出去的那一刻就把設定寫下去，
    // 否則「改了設定切出去」會靜默掉整份改動（ConfigStore 的寫入有 300ms 防抖）
    if (state == Qt::ApplicationSuspended) config.flush();
  });

  QObject::connect(&app, &QCoreApplication::aboutToQuit, [&] {
    mcpServer.stop();  // **必須排第一**，理由見 src/app/main.cpp 的同一行
    config.flush();
  });

  return app.exec();
}
```

- [ ] **Step 4: `CMakeLists.txt` 的 Android target**

在 `if(L2M_BUILD_APP)` 區段裡，把 `set(PROJECT_SOURCES ...)` 之後加一段
Android 的來源替換（保留同一個 target 名 `live2d_mate`）：

```cmake
  if(ANDROID)
    # Android 版是另一份物件圖：沒有 splash 子行程、single-instance、系統匣、
    # 設定視窗。理由寫在 src/android/main.cpp 的檔頭。
    set(PROJECT_SOURCES
        src/android/main.cpp
        src/android/android_shell.h
        src/android/android_shell.cpp
        src/android/android_root.h
        src/android/android_root.cpp
        src/windows/bubble_window.h
        src/windows/bubble_window.cpp
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
        src/mcp/pending_call.h
        src/mcp/pending_call.cpp
        src/mcp/mcp_tools.h
        src/mcp/mcp_tools.cpp
        src/mcp/mcp_http_server.h
        src/mcp/mcp_http_server.cpp
        src/platform/window_effects.h
        src/platform/autostart.h
        src/platform/user_idle.h
        src/platform/console.h)
  endif()
```

並在 `target_link_libraries(live2d_mate ...)` 之後加：

```cmake
  if(ANDROID)
    target_sources(live2d_mate PRIVATE resources/android_assets.qrc
                                       "${CMAKE_CURRENT_BINARY_DIR}/android_shaders.qrc")
    # AUTOUIC 只有設定視窗需要，Android 版沒有 .ui
    set_target_properties(live2d_mate PROPERTIES AUTOUIC OFF)
    set_target_properties(
      live2d_mate
      PROPERTIES QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
                 QT_ANDROID_MIN_SDK_VERSION 28
                 QT_ANDROID_TARGET_SDK_VERSION 35
                 QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
                 QT_ANDROID_VERSION_CODE 1)
  endif()
```

同時把 `install()` 那一整段的條件從 `if(WIN32)` 確認為不含 Android（已是）。
`spdlog` 與 `src/app/logging.*` 在 Android 上不連（logcat 已足夠）——
把 `logging.h/.cpp` 從 Android 的來源清單裡排除（上面已排除），
並在 `target_link_libraries` 用 `$<$<NOT:$<BOOL:${ANDROID}>>:spdlog>` 條件化。

- [ ] **Step 5: 建 APK 並安裝**

```bash
D:/Qt/6.11.2/android_arm64_v8a/bin/qt-cmake.bat -S . -B build/android -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DL2M_BUILD_APP=ON -DL2M_BUILD_VIEWER=OFF \
  -DL2M_BUILD_TESTS=OFF -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
cmake --build build/android --target live2d_mate_make_apk
adb install -r build/android/android-build/build/outputs/apk/debug/android-build-debug.apk
```

- [ ] **Step 6: 推模型並驗收**

```bash
adb logcat -c
adb shell am start -n com.rookiestudio.live2dmate/org.qtproject.qt.android.bindings.QtActivity
adb logcat | grep -E "\[config\]|\[live2d\]|\[mcp\]"
# 從 [config] 那一行讀出資料目錄，把模型推進 <資料目錄>/models/
adb push "path/to/Model" /sdcard/Android/data/com.rookiestudio.live2dmate/files/models/Model
adb shell am force-stop com.rookiestudio.live2dmate
adb shell am start -n com.rookiestudio.live2dmate/org.qtproject.qt.android.bindings.QtActivity
```

Expected:
- 全螢幕顯示角色，沒有狀態列與導覽列
- 待機動作、呼吸、眨眼正常
- 螢幕不會自己暗掉
- 觸控角色時視線跟隨、會播點擊動作
- 啟動到出現模型 < 5 秒
- **Task 10 的驗收也在這裡一併做**：氣泡出現在角色上方、文字不溢出橢圓、
  氣泡蓋住的那一塊仍然觸控得到角色（`WA_TransparentForMouseEvents` 有生效）。
  沒有 TTS 的話先用 `mutter`（只出氣泡、不經 TTS）驗證

- [ ] **Step 7: 補文件並 commit**

```bash
git add src/android CMakeLists.txt docs/ANDROID_SETUP.md
git commit -m "新增：Android 進入點，角色以全螢幕不透明顯示

新開 src/android/main.cpp 而不是在 app/main.cpp 加 #ifdef —— 那 677 行裡
Android 用不到的（splash 子行程、single-instance、系統匣、七分頁設定視窗、
開機自啟）將近一半而且彼此交纏，那個檔案本來就是順序敏感的。
照 src/viewer/main.cpp 的前例：新進入點、共用所有函式庫。

android_shell 是 JNI 的唯一出入口（螢幕常亮、沉浸式全螢幕、資料目錄）。
ConfigStore::flush() 另外掛在 applicationStateChanged 的 Suspended：
Android 可以不經 aboutToQuit 直接殺行程。"
```

---

## Task 12: 音訊與 TTS

**Files:**
- Modify: `CMakeLists.txt`（miniaudio 的 Android 連結）
- Modify: `src/android/main.cpp`（TTS 引擎陣列）
- Modify: `src/android/android_shell.h`、`.cpp`（audio focus）
- Modify: `docs/ANDROID_SETUP.md`

**Interfaces:**
- Consumes: Task 11 的物件圖
- Produces: `void l2m::android::requestAudioFocus(bool)`
- Produces: 手機上 `L2M_SAY` 能出聲並顯示氣泡

- [ ] **Step 1: miniaudio 的 Android 連結**

`CMakeLists.txt` 的 miniaudio 那一段，把 `if(UNIX AND NOT APPLE)` 改成：

```cmake
  if(ANDROID)
    # AAudio（API 26+）優先，miniaudio 會自己退回 OpenSL ES。
    # log 是 miniaudio 的除錯輸出要的。
    target_link_libraries(miniaudio PUBLIC OpenSLES android log)
  elseif(UNIX AND NOT APPLE)
    # miniaudio 在 Linux 用 dlopen 動態載入 ALSA/PulseAudio，連結期只要這三個
    target_link_libraries(miniaudio PUBLIC dl pthread m)
  endif()
```

- [ ] **Step 2: TTS 引擎陣列**

`src/android/main.cpp` 的語音那一節建立引擎 vector，**順序就是 fallback 順序**：

類別名與建構子參數照 `src/app/main.cpp:257-282` 的實際寫法（`EdgeTtsEngine`、
`GptSovitsEngine`、`VoiceboxEngine`、`CustomTtsEngine`，後三個吃 config lambda）：

```cpp
  // ── 語音 ──
  l2m::HttpJson http;
  std::vector<std::unique_ptr<l2m::TtsEngine>> engines;
  // **陣列順序就是 fallback 順序**，而且 TtsManager::defaultEngineId() 取的是
  // 第一個 —— custom 不能排第一（同 src/app/main.cpp 的規則）。
  //
  // 與桌面版的兩個差別：
  //  · **沒有 SapiEngine** —— Windows 專有（tts_engine_sapi_win.cpp）。
  //  · **Edge 排在最後而不是第一** —— 它走 wss://，Qt for Android 不內建
  //    OpenSSL，這一版沒掛（見 spec §6 #6），連得上才怪。排最前面會讓
  //    defaultEngineId() 指到一個必然失敗的引擎。GPT-SoVITS 排第一：
  //    使用者的服務跑在 PC 上，baseUrl 欄位本來就填得下區網 IP。
  engines.push_back(std::make_unique<l2m::GptSovitsEngine>(http, [&config] { return config.get().tts.gptsovits; }));
  engines.push_back(std::make_unique<l2m::VoiceboxEngine>(http, [&config] { return config.get().tts.voicebox; }, [&controller] { return controller.uiLocale(); }));
  engines.push_back(std::make_unique<l2m::CustomTtsEngine>(http, [&config] { return config.get().tts.custom; }));
  engines.push_back(std::make_unique<l2m::EdgeTtsEngine>(http));

  // 不會分塊回傳的引擎全部包上句段管線（照抄 src/app/main.cpp:276-281 的理由與寫法）
  for (auto& engine : engines) {
    if (engine->streams()) continue;
    engine = std::make_unique<l2m::SegmentedTtsEngine>(std::move(engine));
  }

  l2m::TtsManager tts(std::move(engines));
  l2m::AudioPlayer player;
  l2m::SpeechController speech(config, tts, player, bubble);
```

- [ ] **Step 3: audio focus**

`android_shell.h` 加：

```cpp
// 向系統要／放音訊焦點。不要的話會跟音樂 app 同時出聲，
// 而且來電時我們不會自動閃避。
void requestAudioFocus(bool acquire);
```

`android_shell.cpp` 用 `QJniObject` 取得 `AudioManager`
（`context.getSystemService("audio")`）並呼叫 `requestAudioFocus` /
`abandonAudioFocus`。在 `src/android/main.cpp` 裡接到
`SpeechController` 的開始／結束。

- [ ] **Step 4: 真機驗證**

```bash
cmake --build build/android --target live2d_mate_make_apk
adb install -r build/android/android-build/build/outputs/apk/debug/android-build-debug.apk
adb shell am start -n com.rookiestudio.live2dmate/org.qtproject.qt.android.bindings.QtActivity --es L2M_SAY "テスト"
adb logcat | grep -E "\[tts\]|\[live2d\]"
```

（若 `--es` 傳環境變數不通，改成在 config.json 裡設一個測試用的
`personas` 歡迎詞，或暫時在 main.cpp 裡硬寫一次 `controller.speak(...)`。
把可行的那條寫進 `docs/ANDROID_SETUP.md`。）

Expected：
- 手機出聲
- 氣泡顯示文字
- 嘴形跟著音量動
- logcat 沒有 `ma_device_init` 失敗

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/android docs/ANDROID_SETUP.md
git commit -m "新增：Android 的音訊與 TTS

miniaudio 連 OpenSLES/android/log（AAudio 優先，會自己退回 OpenSL ES），
ma_device_init(nullptr, ...) 自己挑後端，C++ 一行都不用改。

引擎陣列不含 SAPI（Windows 專有）也暫不含 Edge（wss:// 要 OpenSSL）——
gptsovits 排第一：使用者的服務跑在 PC 上，baseUrl 本來就填得下區網 IP。

順帶接上 AudioManager 的 audio focus，否則會跟音樂 app 同時出聲、
來電時也不會閃避。"
```

---

## Task 13: MCP 伺服器在手機上跑起來

**Files:**
- Modify: `src/android/main.cpp`（MCP 一節）
- Modify: `docs/ANDROID_SETUP.md`

**Interfaces:**
- Consumes: Task 1 的 `tunnelExposed`、Task 2 的 `SnippetInput::android`、Task 11 的物件圖、Task 12 的 TTS
- Produces: 手機上 `http://<手機IP>:3777/mcp` 可用，連線指令印在 logcat

- [ ] **Step 1: 接上 MCP**

照 `src/app/main.cpp` 的「── MCP ──」節（`:488-525`）把
`McpTools` / `McpHttpServer` / `pushPersona` / `pushSpeechProtocol` 接起來。
`mcpServer.start()` 的四個參數取自 config。加一段註解：

```cpp
  // **綁 0.0.0.0 而不是 127.0.0.1**（Android 版的預設，寫進 docs/ANDROID_SETUP.md）：
  //  · 0.0.0.0 本來就涵蓋 loopback，Termux 裡的 cloudflared 照樣打得到 127.0.0.1
  //  · requiresToken("0.0.0.0") 回 true，token 自動被逼出來 ——
  //    而綁 loopback + 隧道那個組合正是 core/mcp_host.h 講的那個洞
  //  · 順帶讓 PC 端能用 --mcp-stdio 走區網直連，省掉一趟公網來回
```

- [ ] **Step 2: 首次啟動自動產生 token**

Android 版沒有設定 UI，token 得自己生。在建立 `ConfigStore` 之後加：

```cpp
  // Android 版沒有設定 UI，token 由第一次啟動自己生一個並寫回 config.json
  //（使用者要改的話直接編輯那個檔案，路徑在上面的 [config] 那一行）。
  // 綁 0.0.0.0 沒有 token 會被 validateMcpBinding 擋下來而整個 MCP 不啟動，
  // 那對使用者是「什麼都沒發生」的靜默失敗。
  if (!config.get().mcp.token.has_value() || config.get().mcp.token->empty()) {
    config.patch("{\"mcp\":{\"token\":\"" + l2m::generateToken() + "\"}}");
  }
  qInfo() << "[mcp] token:" << QString::fromStdString(config.get().mcp.token.value_or(""));
```

並在 `mcpServer.start(...)` 之後把連線片段印進 logcat —— **這是 Task 2 那組
Android 片段唯一的呼叫端**（手機上沒有設定 UI，使用者只能從 logcat 抄）：

```cpp
  // Android 沒有設定 UI，連線指令只能從 logcat 抄。印的是 Task 2 加的
  // android 分支：Termux 那三行 + PC 端的 --mcp-stdio 那一則。
  l2m::SnippetInput snippetInput;
  snippetInput.host = l2m::resolveAdvertisedHost(config.get().mcp.host, l2m::systemNetworkAddresses());
  snippetInput.port = config.get().mcp.port;
  snippetInput.token = config.get().mcp.token;
  snippetInput.exePath = "live2d_mate.exe";  // PC 端那支的路徑，使用者自己換
  snippetInput.android = true;
  for (const auto& snippet : l2m::buildMcpSnippets(snippetInput)) {
    if (snippet.text) qInfo().noquote() << "[mcp]" << QString::fromStdString(snippet.id) << "\n" << QString::fromStdString(*snippet.text);
  }
```

（`patch()` 吃兩層 JSON 物件字串，而且**內層是整個覆蓋** ——
`mcp` 內層只有純量欄位，所以這樣送是安全的；若日後 `mcp` 長出巢狀物件要改用
`core/config_patch.h` 的整包送法。）

- [ ] **Step 3: 建置、安裝、從 PC 連進去**

```bash
cmake --build build/android --target live2d_mate_make_apk
adb install -r build/android/android-build/build/outputs/apk/debug/android-build-debug.apk
adb shell am start -n com.rookiestudio.live2dmate/org.qtproject.qt.android.bindings.QtActivity
adb logcat | grep "\[mcp\]"      # 讀出 token
adb shell ip route | head -1     # 讀出手機的區網 IP
```

在 PC 上：

```bash
curl -s http://<手機IP>:3777/health
curl -s -X POST http://<手機IP>:3777/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <token>" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | head -c 400
```

Expected: `/health` 有回應；`tools/list` 回 24 個工具。

- [ ] **Step 4: 掛進 Claude Desktop（stdio 橋接，零改動）**

```json
{
  "mcpServers": {
    "live2d_mate": {
      "command": "E:\\Works\\Qt_Project\\live2d_mate_qt\\build\\rel\\live2d_mate.exe",
      "args": ["--mcp-stdio", "http://<手機IP>:3777/mcp", "<token>"]
    }
  }
}
```

Expected: Claude Desktop 看得到工具；`speak` 讓**手機**出聲並顯示氣泡。

- [ ] **Step 5: 驗證 ANR 沒有被觸發**

```bash
adb logcat -c
# 在 Claude Desktop 裡連續下 speak(wait=true) 的長句
adb logcat | grep -iE "ANR|Application Not Responding|Skipped [0-9]+ frames"
```

Expected: 沒有 ANR。`speak(wait=true)` 的期限是 185 秒，而
`mcp_http_server.h` 的設計是「worker 執行緒去阻塞，GUI 執行緒永遠不阻塞」
—— 這一條在 Android 上直接決定 app 活不活得下來，**不要動它**。

- [ ] **Step 6: 補文件並 commit**

`docs/ANDROID_SETUP.md` 加 `## 4. 從 PC 連進去（區網）`。

```bash
git add src/android/main.cpp docs/ANDROID_SETUP.md
git commit -m "新增：Android 版接上 MCP 伺服器

綁 0.0.0.0（不是 127.0.0.1）：它本來就涵蓋 loopback，Termux 的 cloudflared
照樣打得到，而 requiresToken(\"0.0.0.0\") 回 true 會把 token 逼出來 ——
綁 loopback + 隧道正是 core/mcp_host.h 講的那個洞。順帶讓 PC 端能用
--mcp-stdio 走區網直連，省掉一趟公網來回。

沒有設定 UI，所以 token 由第一次啟動自己生並寫回 config.json，
路徑與 token 都印在 logcat 的 [config]/[mcp] 兩行。"
```

---

## Task 14: Termux + cloudflared 端到端

**Files:**
- Modify: `docs/ANDROID_SETUP.md`
- Modify: `src/android/main.cpp`（`tunnelExposed` 的說明註解）

**Interfaces:**
- Consumes: Task 2 的 snippet 內容、Task 13 的伺服器

- [ ] **Step 1: 裝 Termux（F-Droid 版）**

從 <https://f-droid.org/packages/com.termux/> 或 GitHub releases 安裝。
**不可以用 Play 商店那個** —— 它停在 2020 年的 0.101 早已停止維護，
`pkg install` 會直接壞掉。

- [ ] **Step 2: 確認 cloudflared 在哪個 repo**

```bash
pkg update
pkg search cloudflared
```

- 主 repo 有 → `pkg install cloudflared`
- 沒有 → `pkg install tur-repo && pkg update && pkg install cloudflared`
- 都沒有 → 抓官方的 `cloudflared-linux-arm64`，`chmod +x` 後從 `$PREFIX/bin` 執行

**把實際可行的那一條寫進 `docs/ANDROID_SETUP.md`。**

- [ ] **Step 3: 開 quick tunnel 驗證**

```bash
termux-wake-lock
cloudflared tunnel --url http://127.0.0.1:3777
```

從輸出讀出 `https://xxxx.trycloudflare.com`。在 PC 上：

```bash
curl -s -X POST https://xxxx.trycloudflare.com/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <token>" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | head -c 400
```

Expected: 回 24 個工具。

- [ ] **Step 4: 驗證 token 真的擋得住**

```bash
curl -s -X POST https://xxxx.trycloudflare.com/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

Expected: 401 / 拒絕。**這一步是 Task 1 的最終驗收。**

- [ ] **Step 5: 驗證螢幕關掉之後的行為**

```bash
adb shell input keyevent KEYCODE_POWER   # 關螢幕
sleep 60
curl -s https://xxxx.trycloudflare.com/health
```

記錄結果。預期會**失敗或不穩定** —— 那是 Foreground Service 的工作，
不在這次範圍內（見 spec §7）。把觀察到的行為寫進
`docs/ANDROID_SETUP.md` 的「已知限制」，並註明使用者的正常用法是
「插著電、螢幕常亮」。

- [ ] **Step 6: named tunnel（固定網址）**

quick tunnel 的網址每次重啟都會變，AI 那邊的 connector 設定就要跟著改。
在 Termux 裡：

```bash
cloudflared tunnel login
cloudflared tunnel create live2d-mate
# 產生 ~/.cloudflared/config.yml：
#   tunnel: <UUID>
#   credentials-file: /data/data/com.termux/files/home/.cloudflared/<UUID>.json
#   ingress:
#     - hostname: mate.<你的網域>
#       service: http://127.0.0.1:3777
#     - service: http_status:404
cloudflared tunnel route dns live2d-mate mate.<你的網域>
cloudflared tunnel run live2d-mate
```

- [ ] **Step 7: 開機自啟（選用）**

裝 Termux:Boot，把啟動腳本放 `~/.termux/boot/tunnel.sh`：

```bash
#!/data/data/com.termux/files/usr/bin/sh
termux-wake-lock
cloudflared tunnel run live2d-mate
```

並把 Termux 移出電池最佳化白名單（系統設定 → 應用程式 → Termux → 電池）。

- [ ] **Step 8: 完成文件並 commit**

`docs/ANDROID_SETUP.md` 補完 `## 5. 遠端存取（Termux + cloudflared）`
與 `## 6. 已知限制`。至少要寫進去的三條：

1. Termux 一定要 F-Droid 版（Play 版停在 0.101，`pkg install` 會壞）
2. `termux-wake-lock` + 電池最佳化白名單缺一不可，否則螢幕一關隧道就掉
3. Termux 靠 `targetSdk 28` 保有 exec 權限（Android 10+ 的 W^X 只放行
   `nativeLibraryDir`）—— 那是它能跑 cloudflared 而我們的 app 不能的原因，
   也是一個會隨 Android 版本升高而失效的長期外部依賴

```bash
git add docs/ANDROID_SETUP.md src/android/main.cpp
git commit -m "文件：Termux + cloudflared 的遠端存取步驟與已知限制

隧道跑在 Termux（另一個 app），我們的 build pipeline 一個字都不用動。
端點固定 127.0.0.1 —— 兩個 app 共用同一個 network namespace。

記下三條會咬人的：F-Droid 版才裝得了套件、termux-wake-lock 與電池
最佳化白名單缺一不可、Termux 的 exec 權限靠 targetSdk 28 而那是會隨
Android 版本失效的外部依賴。"
```

---

## 完成後的狀態

對照 spec §8 的驗收條件逐條確認：

- [ ] APK 產出並安裝到實機（arm64-v8a）
- [ ] 全螢幕顯示模型，待機動作／呼吸／眨眼與桌面版一致，觸控時視線跟隨
- [ ] 啟動到出現模型 < 5 秒
- [ ] PC 的 `--mcp-stdio` 掛進 Claude Desktop，`speak` 讓手機出聲並顯示氣泡
- [ ] Termux 的 cloudflared 隧道下，同一組工具從公網 URL 也叫得動
- [ ] 沒有 token 時，隧道那條路被 `validateMcpBinding` 擋下（curl 驗證）
- [ ] Windows 建置與 70+ 支測試全部通過

## 不在這份計畫內（後續各自獨立成一份）

- **觸控版設定 UI** —— `SettingsWindow` 七分頁的手機版面。`core/settings_layout.h`
  那層「分頁 id + 開關表」是純邏輯且有測試，可以整份照抄，只換呈現層。
- **Foreground Service + WifiLock** —— 螢幕關掉也要能被叫醒。純 Java/Manifest，
  不影響 C++。
- **模型匯入（SAF）** —— 目前靠 `adb push`。
- **OpenSSL** —— 要用 Edge TTS／LLM／天氣時再掛 `C:\SDKs\android-sdk\android_openssl`。
- **OLED 燒屏與電量調校** —— 角色會動，但氣泡與任何常駐 UI 不會。
