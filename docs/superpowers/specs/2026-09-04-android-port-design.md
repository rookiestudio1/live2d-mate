# Live2D Mate 移植到 Android — 設計

日期：2026-09-04
狀態：設計已定案，待實作（計畫見 `docs/superpowers/plans/2026-09-04-android-port.md`）

---

## 1. 要做的東西

**使用者把手機放在桌旁，手機全螢幕顯示角色；PC 上（或遠端）的 AI 透過 MCP
讓她說話、換表情、做動作。**

兩個從一開始就定死的取捨，它們是這份設計成立的前提：

- **全螢幕** —— 沒有「視窗位置／大小／貼邊」這回事。
- **不透明背景** —— 沒有穿透點擊、沒有形狀視窗、沒有 alpha 命中遮罩。

對外連線走兩條，兩條都不需要我們寫網路程式碼：

| 路徑 | 手段 | 誰動手 |
|---|---|---|
| 區網 | PC 執行 `live2d_mate.exe --mcp-stdio http://<手機IP>:3777/mcp <token>` | 既有的 stdio 橋接，**零改動** |
| 遠端 | 手機的 **Termux** 裡 `pkg install cloudflared` 後開隧道，指向 `http://127.0.0.1:3777` | Termux（另一個 app），**零改動** |

## 2. 為什麼「全螢幕、不透明」讓這件事變簡單

被這兩個條件整組砍掉的，剛好就是全專案最不可攜的部分：

| 砍掉 | 原因 |
|---|---|
| `src/platform/*_win.cpp`（1,839 行含各平台） | 穿透、置頂、閒置偵測、console、crash handler 全是桌面概念 |
| `live2d/hit_mask.cpp`（`AlphaHitMask`） | PBO 非同步回讀求視窗 region，不透明就不需要 |
| `WindowManager` 的 move/scale/opacity | 全螢幕之下一件都不成立 |
| `windows/tray.cpp` | Android 沒有系統匣 |
| `app/single_instance.cpp` | Android 單 Activity 本來就是單實例 |
| `app/splash_process.cpp` | 它用 `QProcess` 重新啟動自己，Android 不允許；原生 splash theme 更好 |
| `platform/autostart_*` | 換成 Termux:Boot 或不做 |
| `media/tts_engine_sapi_win.cpp` | Windows 專有 |

順帶一個好處：`AlphaHitMask` 用的 `glMapBufferRange` / `glFenceSync` /
`glClientWaitSync` / `glBlitFramebuffer` 都要 **GLES 3.0**，而 Cubism 的 renderer
只需要 ES 2.0。不編那支，GLES 版本下限就掉回 2.0。

## 3. 已經就位的資產

- **Cubism Core 官方就附 Android 靜態庫**：`third_party/CubismCore/lib/android/{arm64-v8a,x86,x86_64}/libLive2DCubismCore.a`
  （沒有 `armeabi-v7a`，不影響 —— 只出 arm64）。
- **CubismNativeFramework 5-r.5 原生支援 Android**：`CSM_TARGET_ANDROID_ES2` 分支完整
  （`<GLES2/gl2.h>` + Tegra 擴充 shader 那一路）。
- **`l2m_core` 17,009 行（全專案 36,889 行的 46%）純邏輯**，只連 Qt Core/Network +
  yyjson + miniz。除了一處 iconv（見 §6）之外一行都不用改，70 支測試照跑。
- **`live2d_viewer` 已經是 Android 版的骨架**：`QOpenGLWidget`、不透明、單一視窗、
  不連 `l2m_media`／httplib／spdlog。共 846 行。
- **本機工具鏈**：`C:\SDKs\android-sdk` 有 NDK 26/27/28/29、build-tools 36、
  platform android-36、以及 `android_openssl`。

缺：Qt 的 `android_arm64_v8a` kit（`D:\Qt\6.11.2` 目前只有 `msvc2022_64`）、JDK 17。

## 4. 六個定案的決策

| # | 決策 | 選擇 | 理由 |
|---|---|---|---|
| 1 | 進入點 | **新開 `src/android/main.cpp`**，不改 `src/app/main.cpp` | 那 677 行裡 Android 用不到的（splash 子行程、single instance、tray、七分頁設定視窗、autostart）將近一半，而且彼此交纏。它本來就是「順序敏感，改動前先讀註解」的檔案，最不該塞第二條控制流。照 `live2d_viewer` 的前例：新進入點、共用所有函式庫。 |
| 2 | 角色視窗 | **沿用 `CharacterWindow` + `WindowManager`**，加兩個開關讓它們退化 | `AppController` 的建構子吃這兩個具體型別（14 個成員 + 10 個成員的用量）。抽介面等於重構一個能跑的桌面 app，風險不成比例。退化的做法照 `window_effects_linux.cpp` 在 Wayland 下的既有前例。 |
| 3 | 氣泡 | **畫在同一張 GL 畫布上**，不開第二個 top-level | 原本獨立 top-level 的理由是「會被 alpha region 切掉」，不透明全螢幕之下這個理由消失。`QOpenGLWindow` 繼承 `QPaintDeviceWindow`，`paintGL()` 裡 `QPainter p(this)` 直接可用。繪製邏輯（`bubble_shape`／`ellipse_text_fit`／`bubble_font`）全在 core 且有測試，換的只有呈現層。 |
| 4 | 資源存取 | **`readFileUtf8()` 加 qrc 分支**，不是每個呼叫端各自改 | APK 裡沒有「執行檔旁邊」；`applicationDirPath()` 指向列不出內容的位置。i18n 與 FrameworkShaders 都得編進 qrc。讀 JSON／shader 的路徑全部經過 `readFileUtf8()`，一處改完兩邊都通。 |
| 5 | 對外連線 | **Termux + cloudflared，我們不寫任何隧道程式碼** | 自己 exec cloudflared 會撞 Android 10+ 的 W^X（只有 `nativeLibraryDir` 底下能 exec），還要處理 Go 的 `/etc/resolv.conf`。Termux 靠 `targetSdk 28` 保有 exec 權限，這是它能做而我們不能的原因。 |
| 6 | 安全 | **隧道曝露時強制 token**（新的 `tunnelExposed` 概念） | 見 §5 —— 這是目前桌面版就有的洞。 |

## 5. 必須修的安全洞（桌面版現在就中）

`src/core/mcp_host.cpp:47`：

```cpp
bool requiresToken(const std::string& host) { return !isLoopbackHost(host); }
```

`validateMcpBinding` 只在**非** loopback 時要求 token，而 `config_schema.h:167` 的
預設 host 就是 `127.0.0.1`、`token` 預設無值。

同時 `src/core/mcp_snippets.cpp:107` **無條件**推出 cloudflared 片段，內容是
`cloudflared tunnel --url <origin>`，而 `origin` 直接取綁定的 host。

於是**照著 UI 印出來的指令做，就會得到一個沒有任何驗證的公開 MCP 端點** ——
拿到 `*.trycloudflare.com` 網址的任何人都能叫角色說話、換表情、讀 persona 全文。

`mcp_host.h` 開頭那條規則的前提是「loopback ⇒ 只有坐在這台機器前的人打得到」。
隧道行程就在本機，它把 `127.0.0.1` 原封不動端上公網，前提不成立。
手機版尤其：Termux 的 cloudflared 與桌寵**共用同一個 loopback**，這是建議的正常
設定而不是邊角案例。

**修法**：`requiresToken` / `validateMcpBinding` 各加一個 `bool tunnelExposed`
參數，config 加 `mcp.tunnelExposed`，MCP 分頁加一個核取方塊（歸在
host/port/token 那組走「套用並重新啟動」，不是 `mcpToggles()` 的即時套用那組）。

**推薦設定**（文件要寫進去）：綁 `0.0.0.0` + token。`0.0.0.0` 本來就涵蓋
loopback，cloudflared 照樣打得到 `127.0.0.1`，而 `requiresToken("0.0.0.0")`
本來就回 true —— 區網與隧道兩條路同時可用，且 token 自動被逼出來。

已知範圍限制：`isAllowedOrigin()`（`mcp_host.cpp:104`）在 boundHost 是 loopback
時對任何非 loopback 的 `Origin` 回 false，所以**瀏覽器型**的 MCP client 透過隧道
會被擋。Claude／ChatGPT 的 connector 是伺服器端發起、不送 `Origin`，不受影響。
這一條這次不處理，留給「要接瀏覽器型 client」的那天。

## 6. Android 平台的六個具體障礙

| # | 障礙 | 處置 |
|---|---|---|
| 1 | GLEW 在 Android 不存在 | 不建 `glew` target；GLES 函式是靜態符號，直接連 `GLESv2`。`FetchCubismFramework.cmake` 改用 `CSM_TARGET_ANDROID_ES2`。 |
| 2 | `QSurfaceFormat` 的 `CompatibilityProfile` | GLES 沒有 profile 概念。Android 上設 `OpenGLES` + 2.0，**不設 profile**。Cubism 的 client-side vertex array 在 GLES 本來就是原生寫法，桌面上那個限制在這裡自動消失。 |
| 3 | **474 支混合模式 shader 會在啟動時全部編譯** | `patches/cubism-framework-lazy-blend-shaders.patch` 檔頭明寫「Android ES2 分支維持原本的預先產生」。桌面 NVIDIA 上量到 2,995 ms，手機 GPU 會是 ANR。patch 的索引逆推邏輯與平台無關，把 `EnsureBlendShaderSet()` 的 `#ifndef CSM_TARGET_ANDROID_ES2` 與 `GenerateShaders()` 裡那段 Android 專用迴圈一起拿掉即可。 |
| 4 | bionic 的 iconv 沒有 CP950／GBK／Shift-JIS，且 API 28 才有 | `core/zip_archive.cpp:15` 在非 Windows 一律 include `<iconv.h>`。Android 分支整段排除，走既有的逐位元組替換保底路徑。**症狀降級為「檔案總管壓的非 UTF-8 中文檔名 zip 顯示成亂碼」**，往返一致、模型照樣讀得到、不會 crash。 |
| 5 | `aboutToQuit` 不保證跑到 | Android 可以直接殺行程。`ConfigStore::flush()` 另外掛在 `QGuiApplication::applicationStateChanged == Qt::ApplicationSuspended`（Qt 的 `onPause`）。 |
| 6 | Qt for Android 不內建 OpenSSL | `https://` / `wss://` 會靜默失敗。M3 先不接（GPT-SoVITS 走區網 `http://` 就有聲音），要用 Edge TTS／LLM／天氣時再掛 `C:\SDKs\android-sdk\android_openssl`。 |

**已經是對的、不要動的一條**：`mcp_http_server.h` 的「worker 執行緒去阻塞等結果，
GUI 執行緒永遠不阻塞」。Android 的 ANR watchdog 在主執行緒卡 5 秒就殺 app，而
`speak(wait=true)` 的期限是 185 秒。這條原本是為了 Windows 的 UI 流暢度，在
Android 上直接變成能不能活的分界。

## 7. 不在這次範圍內

- **觸控版設定 UI**（`SettingsWindow` 七分頁）。M3 的設定靠手改
  `config.json`（路徑由啟動時 `qInfo()` 印出來）。
- **Foreground Service / WifiLock**。情境 A（螢幕常亮、插電、app 在前景）不需要，
  前景 Activity 不會被殺、Doze 不啟動。螢幕關掉也要能被叫醒是後續的事。
- **模型匯入 UI（SAF）**。M3 靠 `adb push` 到 app 的 files 目錄。
- **OLED 燒屏對策**、電量調校（25 Hz 游標輪詢在手機上沒有游標可輪詢，
  視線輸入源改成觸控點就順帶解掉一半）。
- **cloudflared 打包進 app**。Termux 全包了。

## 8. 驗收條件

1. `cmake --build` 產出 `live2d_mate.apk`（arm64-v8a），安裝到實機。
2. 全螢幕顯示模型，待機動作／呼吸／眨眼與桌面版一致；觸控時視線跟隨手指。
3. 啟動到出現模型 **不超過 5 秒**（shader 延遲產生那一條的驗收）。
4. PC 上 `live2d_mate.exe --mcp-stdio http://<手機IP>:3777/mcp <token>` 掛進
   Claude Desktop，`speak` 讓手機出聲並顯示氣泡。
5. Termux 開 cloudflared 隧道後，同一組工具從公網 URL 也叫得動。
6. 沒有 token 時，綁 loopback + `tunnelExposed=true` 會被 `validateMcpBinding` 擋下。
7. Windows 建置與 70 支測試全部維持通過（Android 的改動不得回歸桌面版）。
