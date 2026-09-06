<div align="center">

<img src="resources/splash.png" width="520" alt="Live2D Mate">

# Live2D Mate

**可以對話的 Live2D 桌面夥伴，也能讓 AI 工具透過 MCP 驅動。**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Qt 6](https://img.shields.io/badge/Qt-6-41CD52.svg)](https://www.qt.io/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](CMakeLists.txt)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey.svg)](#系統需求)

[English](README.md) · **繁體中文**

</div>

---

Live2D Mate 讓一位 Live2D 角色住在你的桌面上：透明、置頂、可穿透點擊的視窗，可以拖、可以摸、
可以聊天。它內建 **MCP 伺服器**，Claude Code、Claude Desktop、VS Code / Copilot、Cursor、
Gemini CLI 等 MCP 客戶端可以直接驅動角色 —— 播動作、換表情、帶嘴形同步說一句話，
或是跑一整段編排好的表演。

以 **Qt 6 / C++17** 實作，搭配 Live2D **Cubism SDK for Native 5-r.5**。

## 功能

| | |
|---|---|
| 🪟 **桌寵視窗** | 無邊框、透明、置頂。穿透點擊是**每幀從模型自己的 alpha 重算**的，所以點得到角色，其他地方一律穿透過去。 |
| 🎭 **Cubism 3/4/5 模型** | 把模型資料夾丟進模型目錄即可 —— 也吃 **`.zip` 壓縮包**，直接就地讀取，磁碟上不解壓縮。舊版的 Cubism 2.1（`.moc`）不支援。 |
| 🏷️ **幫動作與表情命名** | 用白話寫下每個動作、每個表情「是什麼意思」，AI 就能照語意挑，而不是對著 `motion_03.motion3.json` 猜。 |
| 🗣️ **語音合成與嘴形同步** | Microsoft Edge 線上語音、Windows SAPI（離線）、macOS `say`（離線）、GPT-SoVITS、Voicebox，或**任何你自己填的 HTTP 端點**。附對話氣泡。 |
| 🧠 **角色描述（persona）** | 一份純 Markdown 的角色設定（個性、口頭禪、閒聊台詞、問候語、久坐提醒、摸摸反應）會送到 AI 面前，另有每個角色各自的長期記憶。 |
| 🤖 **內建 LLM 大腦**（選用） | 接上 Ollama、LM Studio、任何 OpenAI 相容端點或 Anthropic，角色就會即席發揮自己的閒聊、歡迎詞與久坐提醒，而且照著角色設定講。規則版行為永遠墊在底下當保底，絕不會僵住。 |
| 🔌 **MCP 伺服器** | 21 個工具，走 HTTP JSON-RPC（`http://127.0.0.1:3777/mcp`），另附給只講 stdio 的客戶端用的橋接。 |
| 🌏 **5 種介面語言** | English、日本語、한국어、简体中文、繁體中文 —— 執行期切換，不必重開。 |
| ✨ **待機時的生命感** | 眨眼、呼吸、視線追著游標跑出視窗外、頭髮衣服的環境風、拖曳物理、隨機自主表演，還有「你坐太久了」的提醒。 |
| 🖥️ **系統匣** | 大小、透明度、貼齊角落、設定與離開 —— 另有開機自動啟動與 `--hidden` 啟動。 |

## 系統需求

- **Windows 10/11 x64** —— 主要平台。
- **macOS 26** —— 已編譯測試，執行正常：Objective-C++ 平台層、`.app` bundle、
  離線語音走系統的 `say`。只有打包（`cmake --install`）那一段仍是 Windows 專用，
  所以 macOS 目前直接從建置樹啟動。
- **Linux x64** —— **X11（或 XWayland）session**；在 Debian 13 / Xfce 上以 Qt 6.7.2
  （`gcc_64`）與 GCC 14 編譯測試通過。穿透點擊（XShape input region）、置頂（EWMH）
  與視線跟隨游標都是 X11 機制 —— 原生 Wayland session 下程式照常跑，但這三項會
  安靜退化。建置需要 GL/X11 開發標頭（Debian/Ubuntu：`libgl1-mesa-dev libx11-dev
  libxext-dev`）。已知缺口：還沒有離線 TTS 引擎（網路引擎全部可用）、
  系統閒置偵測尚未接上、GNOME 的系統匣需要 AppIndicator 擴充，
  打包（`cmake --install`）仍是 Windows 專用，請從建置樹啟動。
- **Qt 6** —— 開發環境為 Windows 上的 6.8.3 與 6.11.2（`msvc2022_64`）、
  Linux 上的 6.7.2（`gcc_64`）。要載入**貼圖是 WebP 的模型**還得加裝
  **Qt Image Formats** 模組：解碼走 `QImage`，少了 `qwebp` 外掛那種模型會載入失敗，
  並回報解不動的是哪個格式（而不是靜靜地什麼都不顯示）。
- Windows 用 **MSVC 2022 x64**、macOS 用 **Xcode / Apple Clang**、Linux 用 **GCC ≥ 12**；
  **CMake ≥ 3.21**、**Ninja**。
- **第一次 CMake configure 需要網路** —— 開源的 Cubism Framework 是從 GitHub 抓的。

## 建置

### 1. 放好 Cubism Core（做一次就好）

`third_party/CubismCore/` 放的是 Live2D 的**閉源**執行核心。授權不允許把它放進本 repo，
所以該目錄已列入 gitignore，而且**缺檔時 CMake 會直接 FATAL_ERROR**。

到[官方網站](https://www.live2d.com/sdk/download/native/)下載 **Cubism SDK for Native 5-r.5**，
依 **[docs/CUBISM_CORE_SETUP.md](docs/CUBISM_CORE_SETUP.md)** 的說明手動放置。
官方 SDK 本來就附三個平台的靜態庫 —— 包含 `lib/linux/x86_64/` ——
所以同一個複製步驟就涵蓋 Windows、macOS 與 Linux。

開源的 `CubismNativeFramework` 由 `cmake/FetchCubismFramework.cmake` 自動取得（釘在 tag `5-r.5`），
而且**每次 configure 都會套用 `patches/` 底下的 patch** —— 把混合模式 shader 改成延遲產生，
啟動時的 `CreateRenderer` 從 **2995 ms 降到 55 ms**。

### 2. Configure 與建置

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64 \   # Linux 例：~/Qt/6.7.2/gcc_64
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

兩個開關：

```bash
-DL2M_BUILD_TESTS=OFF   # 不建 63 支測試（預設 ON）。關掉之後連 Qt 的 Test 元件
                        # 都不會去找，沒裝的環境照樣 configure 得過。
-DL2M_BUILD_APP=OFF     # 不建主程式。這種建置**完全不需要 Cubism Core**，
                        # Qt 也只要 Core/Network/Test。
```

`L2M_BUILD_APP=OFF` 是為 CI 存在的。Cubism Core 閉源、不能進 repo，而缺檔是 `FATAL_ERROR`，
所以沒有這個開關的話，乾淨機器上連「只想跑測試」都會在 configure 當場死。
但測試全部只連 `l2m_core`、一行 Cubism 都沒碰 —— 這個開關只是把那個既有事實變成建置設定。

### 3. 測試

63 支測試，**全部只連 `l2m_core`**：

```bash
ctest --test-dir build --output-on-failure       # 全部
ctest --test-dir build -R test_mcp_host --output-on-failure
build/rel/test_mcp_host.exe <testFunctionName>   # 單一 QTest slot；-functions 可列出
```

### 4. 打包（Windows）

```bash
cmake --install build --config RelWithDebInfo --prefix dist/live2d_mate
```

版面刻意是**平的** —— 執行檔在根目錄，`i18n/`、`FrameworkShaders/`、Qt DLL 與授權文件全在旁邊。
這不是美觀問題：i18n 目錄與 Cubism shader 目錄都是「先試編譯期烤進去的路徑，不存在才退回
執行檔旁的同名目錄」，換成 `bin/` + `share/` 那種版面，打包出來的程式會變成
「介面全是 raw key、模型一動也不動」。

`.github/workflows/` 底下就是對應的兩個 workflow：`tests.yml` 在每次 push 與 PR
於 **Windows 與 Ubuntu** 兩種 runner 上跑單元測試
（用 `L2M_BUILD_APP=OFF`，所以不需要 Cubism Core）；`release.yml` 在推 `v*` tag 時建置並發布
Windows zip。release 那個需要在 repo 設一個**變數** `CUBISM_SDK_URL` 指向官方 SDK 的下載位址，
因為那份 SDK 不能進 repo、也不該透過 repo 散布。

## 執行

```bash
build/rel/live2d_mate.exe                             # 一般啟動（single-instance）
build/rel/live2d_mate.exe --hidden                    # 縮在系統匣啟動
build/rel/live2d_mate.exe --mcp-stdio [url] [token]   # stdio ↔ HTTP 橋接
```

第二次啟動只會叫醒第一個實例。macOS 與 Linux 的執行檔沒有 `.exe` 後綴，指令其餘相同。

診斷用環境變數：`L2M_PROFILE`（每 2 秒印各階段幀時間）、
`L2M_SAY`（啟動 3 秒後說一句話，一次驗證 synth → 播放 → 氣泡 → 嘴形）、
`L2M_STRAIGHT_ALPHA`、`L2M_FORCE_REGION`、`L2M_TEST_OPAQUE`、`L2M_DUMP_FRAME`。
stdio 橋接讀的是 `L2D_MCP_URL` / `L2D_MCP_TOKEN`（注意是 `L2D_` 前綴，不是 `L2M_`）。

## 加入模型

把下載回來的模型**整個資料夾**丟進模型目錄（*設定 → 一般 → 開啟模型資料夾*），
再按*設定 → 模型 → 重新掃描*。

```
models/
  Hiyori/
    Hiyori.model3.json      <- Cubism 3/4/5 入口檔
    Hiyori.moc3
    motions/...
  Marisa.zip                <- 壓縮起來的模型包，就地讀取
```

- **只支援 Cubism 3/4/5** —— 入口檔是 `*.model3.json`，旁邊放著 `*.moc3`。
  入口檔只叫 `model.json` / `index.json` 的舊包也吃，前提是內容是 Cubism 3/4/5。
- **Cubism 2.1 模型讀不了。** Cubism SDK for Native 5 沒有舊 `.moc` 格式的執行核心，
  所以 `*.model.json` 以及內容判定為 Cubism 2 的裸命名入口，掃描時一律略過、
  不會出現在模型清單裡。要用的話請在 Cubism Editor 裡重新輸出成 `.moc3`。
- 只掃 3 層資料夾。模型名稱取自放入口檔的那個資料夾。
- **`.zip` 是一等公民**：一個 zip 就是一個模型，磁碟上永不解壓縮。
  Windows 檔案總管「壓縮成 ZIP 檔」多包的那一層會自動吸收，
  查找一律大小寫不敏感，條目名稱會先判斷是不是 UTF-8，不是才用來源 Windows 的
  ANSI 碼頁解碼 —— Linux/macOS 沒有「本機 ANSI 碼頁」這個概念，
  改依介面語系的優先順序嘗試常見的 CJK 碼頁。
  所以 zip 裡的中日文檔名在每個平台都讀得到。

## 接上 AI（MCP）

打開*設定 → MCP*。選好監聽位址與連接埠，若監聽的不是 `127.0.0.1` 就產生一組存取權杖，
按**套用並重新啟動**，然後直接複製你要用的客戶端的現成片段 —— Claude Code、Claude Desktop、
VS Code / Copilot、Cursor / Windsurf / Cline、Gemini CLI，
或給只吃公開網址的雲端客戶端用的 Cloudflare / ngrok 通道。

預設端點：`http://127.0.0.1:3777/mcp`（另有 `GET /health`）。
支援的方法：`initialize`、`ping`、`tools/list`、`tools/call`、`resources/list`、`resources/read`。

以 Claude Code 為例：

```bash
claude mcp add --transport http live2d_mate http://127.0.0.1:3777/mcp --scope user
```

只講 stdio 的客戶端（Claude Desktop）走內附的橋接 —— 同一個執行檔，加上 `--mcp-stdio`。

### 21 個工具

| 分類 | 工具 |
|---|---|
| 唯讀 | `list_motions`、`list_expressions`、`list_parameters`、`list_voices`、`get_state` |
| 演出 | `play_motion`、`set_expression`、`set_parameters`、`reset_parameters`、`animate`、`look_at` |
| 說話 | `speak`、`stop_speaking` |
| 視窗 | `move_to`、`set_scale`、`set_opacity`、`set_visible`、`set_always_on_top` |
| 複合 | `perform`（一整段編排）、`schedule`（排程稍後執行）、`open_naming_editor` |

失敗的呼叫一定會附上 `hint` 並列出有效選項，AI 讀完就能自己改正，不必再打一輪 `list_*`。

### AI 不會自己開口說話時

`initialize` 已經會把一份互動守則交給每個客戶端（*設定 → MCP* 的三個開關就是在調整它：
**多話**、**任務完成時說一句**、**不等播放結束**）。

**在 AI 裡註冊完這個 MCP 不一定就夠。** 有些 AI 會讀那份 instructions 並主動用起來，
**Claude Code** 就是；有些 AI 則是「對話裡沒明講就不會主動叫工具」，不管伺服器講得多清楚 ——
**Claude Desktop** 是最常見的例子，21 個工具全部連上了，它還是一個都不會呼叫。
長對話也可能把伺服器給的 instructions 擠出視野。

碰到不會主動使用的 AI，就把規則明確寫進**該 AI 自己的 System Prompt／全域指令**
—— 那裡的優先權高過伺服器所能「建議」的任何東西。Claude Code 寫在 `CLAUDE.md`，
Claude Desktop / Cursor / Copilot 寫在自訂指示欄：

```text
若本次對話有 live2d_mate 工具可用，每次回覆開始及回覆結束時，
都用 live2d_mate 說出該段回覆的摘要，並依需要展現表情或動作。
speak 一律帶 wait: false，讓語音在背景播放，不要阻塞後續文字。
```

### 角色不必跟你講同一種語言

`speak` 只是把你交給它的文字唸出來，所以 **AI 書寫的語言與角色說出口的語言是各自獨立的**。
System Prompt 加一句就成立：

```text
用繁體中文回答我，但透過 live2d_mate 的 MCP 一律用日語說話。
```

然後到*設定 → 語音*挑一個該語言的語音（或在 `speak` 帶 `voice` 參數，id 由 `list_voices` 給）。
想用 Voicebox 這種只有日語的引擎、自己卻繼續用中文工作時，這就是正解。

## 角色描述與記憶

角色描述是 `personas/` 底下的純 Markdown 檔，可以在*設定 → 角色*編輯，也可以用任何文字編輯器改。
裡面除了送給 AI 的角色描述，還有閒聊台詞、歡迎詞、時段問候、久坐提醒與摸摸反應。

**AI 擴寫**會分六階段幫你寫一份（描述 → 台詞 → 歡迎詞 → 時段問候 → 久坐提醒 → 摸摸反應），
先給預覽，再填進編輯器 —— **存檔永遠是你自己按的那一下。**

長期記憶放在 `memory/<角色名>.md`，夾在 2000 字以內，整段併進行為大腦的 system prompt。

套用中的角色會透過三條通道送到 AI 面前：`initialize` 的 `instructions`（全文，要重連才更新）、
`speak` / `perform` 的工具描述（角色名），以及 `resources/list` + `resources/read`（全文，即時）。
這台伺服器是 POST-only、沒有 SSE，推不了 `notifications/*` ——
所以**已經連上的 AI 必須重新連線才會看到換過的角色**。

## 語音

| 引擎 | 說明 |
|---|---|
| **Microsoft Edge** | 線上、免金鑰、多語言。預設值。 |
| **Windows SAPI** | 離線，用 Windows 內建安裝的語音。 |
| **macOS `say`** | 離線，僅 macOS。 |
| **GPT-SoVITS** | 你自己架的本機伺服器。剪一小段參考音檔就能複製音色，而且合成很快。**推薦，詳見下方。** |
| **Voicebox** | 你自己架的本機伺服器。只有日語，而且合成速度慢。 |
| **自定義語音** | 任意 HTTP 端點：URL、GET / POST(form) / POST(JSON)、參數樣板（`${TEXT}` 會被換成要唸的句子）、自訂標頭。回應 body 必須直接就是 **wav、mp3 或 flac**。 |

引擎只負責交出音訊位元組 —— 播放、嘴形同步與氣泡都是主程式的事。
句子是串流處理的：目前這句還在播的時候，下一句就已經在合成了。

### 推薦：GPT-SoVITS

想讓角色用「某個特定的聲音」講話 —— 喜歡的聲優、遊戲裡的角色 —— **首選是 GPT-SoVITS**。
它只需要一小段參考音檔：把你想要的那把嗓子剪個幾秒鐘，模型就會用那個音色講話。

Voicebox 也能給角色一把有辨識度的嗓子，但它的**合成速度太慢** —— 就算有 GPU 還是慢，
慢到桌寵每講一句都要讓你乾等。GPT-SoVITS 的效果沒有 Voicebox 那麼好，但它**快得多**：
即使只用 CPU，等待時間仍在可以接受的範圍。對一隻會回你話的桌寵來說，
延遲比那最後幾個百分點的音質重要得多。

## 選用的 LLM 大腦

*設定 → LLM* 可以填 OpenAI 相容的 base URL（Ollama、LM Studio、多數雲端服務）或 Anthropic 原生 API。
打開**讓 LLM 即席發揮待機行為**之後，閒聊、歡迎詞與久坐提醒都會當場生成、照著角色設定講，
輸出用的是和 `perform` 同一份 schema。

有一條產品鐵律：**LLM 永遠不准讓桌寵僵住。**
失敗、逾時、解析不出來、還在 cooldown —— 一律退回規則版。
*最多每 N 秒即席發揮一次*（預設 300）用來節制雲端計費；設成 0 就讓本機模型全接管。

## 資料放在哪裡

| | Windows | macOS | Linux |
|---|---|---|---|
| 設定檔 | `%APPDATA%/live2d_mate/config.json` | `~/Library/Application Support/live2d_mate/config.json` | `~/.local/share/live2d_mate/config.json` |
| 模型 | `%APPDATA%/live2d_mate/models/` | `~/Library/Application Support/live2d_mate/models/` | `~/.local/share/live2d_mate/models/` |
| 角色描述 | `%APPDATA%/live2d_mate/personas/` | `~/Library/Application Support/live2d_mate/personas/` | `~/.local/share/live2d_mate/personas/` |
| 長期記憶 | `%APPDATA%/live2d_mate/memory/` | `~/Library/Application Support/live2d_mate/memory/` | `~/.local/share/live2d_mate/memory/` |

*設定 → 關於*會顯示你這台機器上實際解析出來的路徑。
`config.json` 壞掉時會被改名成 `config.bak.json` 並重建預設值，而不是讓程式掛掉。

## 專案結構

單一根 `CMakeLists.txt`、四個 target。`src/` 本身就是 include root。

| Target | 內容 | 連結 |
|---|---|---|
| `l2m_core` | `src/core/*` —— **純邏輯，不碰 GUI/GL** | Qt Core/Network、yyjson、miniz |
| `l2m_live2d` | `src/live2d/*` —— Cubism 包裝與 GL 渲染 | `l2m_core`、Cubism Framework、glew、Qt Gui |
| `l2m_media` | `src/media/*`（音訊與 TTS）、`src/llm/*`（LLM 引擎與行為大腦） | `l2m_core`、miniaudio、Qt Network/WebSockets |
| `live2d_mate` | `src/app/`、`src/windows/`、`src/mcp/`、`src/platform/` | 以上全部 + httplib、Qt Widgets/OpenGL |

**63 支測試全部只連 `l2m_core`。** 所以「決策邏輯放 `src/core/`，Qt/GL/OS 互動放其他目錄」
不是風格建議而是硬規則 —— 那是能不能被測到的分界。

執行緒只有三條：GUI 執行緒（渲染、模型、設定、TTS 回呼、MCP 工具執行）、
httplib 伺服器執行緒與它的 worker pool，以及 miniaudio 即時音訊執行緒 ——
後者只做「從環形緩衝 memcpy → 算 RMS → 寫五個 atomic」，
絕不碰 Qt、不配置記憶體、不上鎖，也不解碼。

完整的架構筆記、啟動順序與各子系統的來龍去脈都在 [CLAUDE.md](CLAUDE.md)。
標頭的區塊註解就是本專案的文件，請維持同樣的密度。

## 參與開發

- 縮排 2 空白、無 tab；行寬約 100；成員變數尾底線 `foo_`；常數 `kCamelCase`。
- **註解一律繁體中文**；面向使用者或 AI 的字串一律英文，UI 字串走 `i18n::translate`，
  鍵值放在 `i18n/*.json`。
- commit 訊息用繁體中文，`類別：一句摘要`，內文分項寫「症狀 → 根因 → 修法」。
- 新增一個 MCP 工具剛好要改三處：`src/core/mcp_tool_specs.cpp`、
  `src/mcp/mcp_tools.cpp`、`tests/test_mcp_tools.cpp`。


## 授權

Live2D Mate 的**原始碼**採 **[MIT License](LICENSE)** 釋出 —— © 2026 RookieStudio。

**但編譯出來的 binary 不是。** 它靜態連結了閉源專有的 Live2D Cubism Core，
也動態連結了 LGPL v3 的 Qt 6。完整條款與各家授權全文在
**[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**；`cmake --install` 會把它們一起放進
打包目錄，散布 binary 時這些文件必須同行。

| | |
|---|---|
| **Live2D Cubism Core** | 閉源專有。**不在**本專案授權範圍內，也不隨本 repo 散布，但會被靜態連結進執行檔。使用它即代表接受 [Live2D Proprietary Software License](https://www.live2d.com/sdk/license/)；年營收超過 1000 萬日圓的事業者發布時另需 Cubism SDK Release License。 |
| **CubismNativeFramework** | Live2D Open Software License —— `patches/` 底下那兩份 diff 也適用，因為它們的 context 行就是 Live2D 的原始碼。 |
| Qt 6 | LGPL v3，動態連結。 |
| GLEW、miniz、yyjson、cpp-httplib、miniaudio、spdlog | 寬鬆授權（BSD / MIT / MIT-0 / public domain）。 |

Live2D 與 Cubism 是 Live2D Inc. 的商標或註冊商標。
Live2D 模型的權利屬於各自的作者，使用前請先確認該模型自己的條款。
