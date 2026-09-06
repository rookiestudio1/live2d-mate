<div align="center">

<img src="resources/splash.png" width="520" alt="Live2D Mate">

# Live2D Mate

**A Live2D desktop companion you can talk to — and that AI tools can drive over MCP.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Qt 6](https://img.shields.io/badge/Qt-6-41CD52.svg)](https://www.qt.io/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](CMakeLists.txt)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey.svg)](#requirements)

**English** · [繁體中文](README.zh-TW.md)

</div>

---

Live2D Mate puts a Live2D character on your desktop — a transparent, always-on-top,
click-through window that you can drag around, pet, and talk with. It ships a built-in
**MCP server**, so Claude Code, Claude Desktop, VS Code / Copilot, Cursor, Gemini CLI and
other MCP clients can drive the character directly: play a motion, change the expression,
speak a line with lip-sync, or run a whole scripted performance.

Written in **Qt 6 / C++17** with the Live2D **Cubism SDK for Native 5-r.5**.

## Features

| | |
|---|---|
| 🪟 **Desktop pet window** | Frameless, transparent, always-on-top. Click-through is recomputed from the model's own alpha every frame, so clicks land on the character and pass through everywhere else. |
| 🎭 **Cubism 3/4/5 models** | Drop a model folder into the models directory — or a **`.zip` package**, which is read in place, without unpacking. Legacy Cubism 2.1 models (`.moc`) are not supported. |
| 🏷️ **Name motions & expressions** | Write down what each motion and expression *means* in plain words; the AI then picks by meaning instead of guessing from `motion_03.motion3.json`. |
| 🗣️ **Text to speech with lip-sync** | Microsoft Edge online voices, Windows SAPI (offline), macOS `say` (offline), GPT-SoVITS, Voicebox, or **any custom HTTP endpoint** you configure yourself. Speech bubble included. |
| 🧠 **Persona** | A plain-Markdown character sheet (personality, catchphrases, idle lines, greetings, break reminders, petting reactions) that is handed to the AI, plus per-persona long-term memory. |
| 🤖 **Built-in LLM brain** *(optional)* | Point it at Ollama, LM Studio, any OpenAI-compatible endpoint, or Anthropic, and the character improvises its own idle chatter, greetings and break reminders — in persona. The rule-based behavior always stays underneath as a fallback, so it never freezes. |
| 🔌 **MCP server** | 21 tools over HTTP JSON-RPC (`http://127.0.0.1:3777/mcp`), plus a stdio bridge for clients that only speak stdio. |
| 🌏 **5 UI languages** | English, 日本語, 한국어, 简体中文, 繁體中文 — switched at runtime, no restart. |
| ✨ **Idle life** | Blinking, breathing, gaze that follows the cursor beyond the window, ambient wind in hair and clothes, drag physics, random idle performances, and a "you have been sitting too long" reminder. |
| 🖥️ **System tray** | Size, opacity, corner snapping, settings and quit — plus launch-at-login and a `--hidden` start. |

## Requirements

- **Windows 10/11 x64** — the primary target.
- **macOS 26** — built and tested, runs correctly: Objective-C++ platform layer, `.app`
  bundle, `say` as the offline voice. Only the packaging step (`cmake --install`) is still
  Windows-only, so a macOS build is launched from the build tree.
- **Linux x64** — **X11 (or XWayland) sessions**; built and tested on Debian 13 / Xfce with
  Qt 6.7.2 (`gcc_64`) and GCC 14. Click-through (XShape input regions), always-on-top (EWMH)
  and the cursor-following gaze are all X11 mechanisms — under a native Wayland session the
  app still runs, but those three degrade gracefully. Building needs the GL/X11 dev headers
  (Debian/Ubuntu: `libgl1-mesa-dev libx11-dev libxext-dev`). Known gaps: no offline TTS
  engine yet (all the network engines work), system-idle detection is not wired up, the tray
  on GNOME needs an AppIndicator extension, and `cmake --install` is still Windows-only, so
  launch from the build tree.
- **Qt 6** — developed against 6.8.3 and 6.11.2 (`msvc2022_64`) on Windows, 6.7.2 (`gcc_64`)
  on Linux. Add the **Qt Image Formats** module if you want models whose textures are WebP:
  decoding goes through `QImage`, and without the `qwebp` plug-in such a model fails to load
  and reports the unsupported format instead of quietly showing nothing.
- **MSVC 2022 x64** on Windows, **Xcode / Apple Clang** on macOS, **GCC ≥ 12** on Linux;
  **CMake ≥ 3.21**, **Ninja**.
- Network access on the **first CMake configure** — the open-source Cubism Framework is
  fetched from GitHub.

## Build

### 1. Place the Cubism Core (once)

`third_party/CubismCore/` holds Live2D's **closed-source** runtime. Its license forbids
redistributing it here, so the directory is gitignored and **CMake fails hard when it is
missing**.

Download **Cubism SDK for Native 5-r.5** from the
[official site](https://www.live2d.com/sdk/download/native/) and copy the files as described
in **[docs/CUBISM_CORE_SETUP.md](docs/CUBISM_CORE_SETUP.md)**. The official SDK already ships
the static libraries for all three platforms — including `lib/linux/x86_64/` — so the same
copy step covers Windows, macOS and Linux.

The open-source `CubismNativeFramework` is fetched automatically by
`cmake/FetchCubismFramework.cmake` (pinned to tag `5-r.5`), and every configure applies the
patches in `patches/` — a deferred blend-mode shader build that cuts `CreateRenderer` from
**2995 ms to 55 ms** at startup.

### 2. Configure and build

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64 \   # Linux: ~/Qt/6.7.2/gcc_64
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Two switches:

```bash
-DL2M_BUILD_TESTS=OFF   # skip the 63 unit tests (ON by default). Turning them off also
                        # stops CMake from looking for the Qt Test component at all.
-DL2M_BUILD_APP=OFF     # skip the desktop app itself — this build needs NO Cubism Core
                        # at all, and only requires Qt Core/Network/Test.
```

`L2M_BUILD_APP=OFF` exists for CI. Cubism Core is closed-source and cannot live in the
repository, and a missing Core is a hard `FATAL_ERROR`, so without this switch even a
tests-only build fails at configure time on a clean machine. Since every test links only
`l2m_core` and never touches Cubism, the switch simply turns that existing fact into a build
configuration.

### 3. Test

63 test binaries, every one of them linking **only** `l2m_core`:

```bash
ctest --test-dir build --output-on-failure       # everything
ctest --test-dir build -R test_mcp_host --output-on-failure
build/rel/test_mcp_host.exe <testFunctionName>   # a single QTest slot; -functions lists them
```

### 4. Package (Windows)

```bash
cmake --install build --config RelWithDebInfo --prefix dist/live2d_mate
```

The layout is deliberately **flat** — the executable sits at the root with `i18n/`,
`FrameworkShaders/`, the Qt DLLs and the license files beside it. That is not cosmetic: the
i18n directory and the Cubism shader directory are both resolved as "try the path baked in at
compile time, fall back to the folder next to the executable", so a `bin/` + `share/` layout
produces a build that starts with raw translation keys and a model that never moves.

`.github/workflows/` has the two workflows this maps onto: `tests.yml` runs the unit tests on
every push and pull request, on **Windows and Ubuntu** runners (with `L2M_BUILD_APP=OFF`, so
it needs no Cubism Core), and
`release.yml` builds and publishes a Windows **NSIS installer** (`-setup.exe`), a portable
**zip** and the full **PDB** when a `v*` tag is pushed. Both packages carry the same two
executables — `live2d_mate.exe` and the `live2d_viewer.exe` model inspector — and settings,
models and logs always live in `%APPDATA%\live2d_mate\`, so the installed and the portable
copy are interchangeable. The release workflow needs a repository **variable** `CUBISM_SDK_URL`
pointing at the official Cubism SDK download, because the SDK cannot be committed or
redistributed through the repository.

## Running

```bash
build/rel/live2d_mate.exe                             # normal launch (single instance)
build/rel/live2d_mate.exe --hidden                    # start minimized to the tray
build/rel/live2d_mate.exe --mcp-stdio [url] [token]   # stdio <-> HTTP bridge
```

Launching a second time just wakes the first instance up. On macOS and Linux the binary has
no `.exe` suffix — same commands otherwise.

Diagnostic environment variables: `L2M_PROFILE` (per-stage frame timings every 2 s),
`L2M_SAY` (say one line 3 s after startup — a one-shot check of synth to playback to bubble
to lip-sync), `L2M_STRAIGHT_ALPHA`, `L2M_FORCE_REGION`, `L2M_TEST_OPAQUE`, `L2M_DUMP_FRAME`.
The stdio bridge reads `L2D_MCP_URL` / `L2D_MCP_TOKEN` (note the `L2D_` prefix, not `L2M_`).

## Adding models

Drop the **whole folder** of a downloaded model into the models directory
(*Settings → General → Open Models Folder*), then hit *Settings → Models → Rescan*.

```
models/
  Hiyori/
    Hiyori.model3.json      <- Cubism 3/4/5 entry file
    Hiyori.moc3
    motions/...
  Marisa.zip                <- a zipped model package, read as-is
```

- **Cubism 3/4/5 only** — the entry file is a `*.model3.json` sitting next to a `*.moc3`. A
  package whose entry file is just `model.json` / `index.json` works too, as long as its
  contents are Cubism 3/4/5.
- **Cubism 2.1 models cannot be read.** The Cubism SDK for Native 5 carries no runtime for the
  old `.moc` format, so `*.model.json` entries — and bare entry files whose contents turn out
  to be Cubism 2 — are skipped during the scan and never appear in the model list. Re-export
  such a model as `.moc3` from Cubism Editor if you want to use it.
- Only 3 folder levels are scanned. The model name comes from the folder holding the entry file.
- **`.zip` packages are first-class**: one zip is one model, never unpacked to disk. The extra
  wrapper folder that Explorer's *Compress to ZIP file* produces is handled, lookups are
  case-insensitive, and entry names are decoded as UTF-8 or the originating Windows ANSI code
  page — on Linux/macOS, where "the local ANSI code page" does not exist, the common CJK code
  pages are tried in the order suggested by your UI language. Chinese and Japanese file names
  inside a zip resolve correctly on every platform.

## Connecting an AI (MCP)

Open *Settings → MCP*. Pick the listen address and port, generate an access token if you
listen anywhere other than `127.0.0.1`, press **Apply & Restart**, then copy the ready-made
snippet for your client — Claude Code, Claude Desktop, VS Code / Copilot, Cursor / Windsurf /
Cline, Gemini CLI, or a Cloudflare / ngrok tunnel for cloud-only clients.

Default endpoint: `http://127.0.0.1:3777/mcp` (plus `GET /health`).
Supported methods: `initialize`, `ping`, `tools/list`, `tools/call`, `resources/list`,
`resources/read`.

Claude Code, for example:

```bash
claude mcp add --transport http live2d_mate http://127.0.0.1:3777/mcp --scope user
```

Clients that only speak stdio (Claude Desktop) go through the bundled bridge — the same
executable, started with `--mcp-stdio`.

### The 21 tools

| Group | Tools |
|---|---|
| Read-only | `list_motions`, `list_expressions`, `list_parameters`, `list_voices`, `get_state` |
| Acting | `play_motion`, `set_expression`, `set_parameters`, `reset_parameters`, `animate`, `look_at` |
| Speech | `speak`, `stop_speaking` |
| Window | `move_to`, `set_scale`, `set_opacity`, `set_visible`, `set_always_on_top` |
| Composite | `perform` (a whole scripted sequence), `schedule` (run something later), `open_naming_editor` |

A failed call always comes back with a `hint` listing the valid options, so an AI can correct
itself without another round of `list_*`.

### When the AI does not speak on its own

`initialize` already hands every client an interaction protocol — *announce the task, speak
your replies, stay quiet in between* — and *Settings → MCP* has the three switches that shape
it (**Talkative**, **Say something when a task is done**, **Don't wait for playback**).

**Registering the server is not always enough.** Some clients read those instructions and start
driving the character by themselves — **Claude Code** does. Others never touch a tool unless the
conversation explicitly tells them to, however clearly the server asked: **Claude Desktop** is the
usual example, and it will happily sit there with all 21 tools connected and never call a single
one. A long conversation can also push the server's instructions out of view.

For those clients the fix is to state the rule in the AI's **own system prompt / global
instructions**, where it outranks anything a server can merely suggest — `CLAUDE.md` for Claude
Code, the custom-instructions box for Claude Desktop / Cursor / Copilot:

```text
If the live2d_mate tools are available in this conversation, use live2d_mate to speak a short
summary at the start of every reply and again when the reply is finished, and show a fitting
expression or motion whenever it helps. Always pass wait: false so the audio plays in the
background and never blocks your next message.
```

### The character does not have to speak your language

`speak` simply voices the text handed to it, so **the language the AI writes in and the
language the character speaks are independent.** One line in the system prompt is all it
takes:

```text
Answer me in Traditional Chinese, but always speak Japanese through the live2d_mate MCP.
```

Then pick a voice for the spoken language in *Settings → Voice* — or pass `voice` to `speak`,
with the ids from `list_voices`. This is also how you use a Japanese-only engine such as
Voicebox while you keep working in your own language.

## Persona and memory

A persona is a plain Markdown file under `personas/`, editable in *Settings → Persona* or in
any text editor. It carries the character description sent to the AI, plus idle lines, welcome
lines, time-of-day greetings, break reminders and petting reactions.

**AI Draft** writes one for you in six stages (description → lines → welcome → greetings →
break reminders → petting), shows a preview, then fills the editor — **saving is always the
button you press.**

Long-term memory lives in `memory/<persona name>.md`, is clamped to 2000 characters, and is
folded into the behavior brain's system prompt.

The active persona reaches an AI through three channels: the `instructions` field of
`initialize` (full text, needs a reconnect), the `speak` / `perform` tool descriptions (the
character's name), and `resources/list` + `resources/read` (full text, live). The server is
POST-only with no SSE, so it cannot push `notifications/*` — **an already-connected AI has to
reconnect before it sees a persona change.**

## Voice

| Engine | Notes |
|---|---|
| **Microsoft Edge** | Online, no key, many languages. The default. |
| **Windows SAPI** | Offline, uses the voices installed in Windows. |
| **macOS `say`** | Offline, macOS only. |
| **GPT-SoVITS** | Your own local server. Clones a voice from a short reference clip, and it is fast. **Recommended — see below.** |
| **Voicebox** | Your own local server. Japanese only, and slow to synthesize. |
| **Custom Voice** | Any HTTP endpoint: URL, GET / POST(form) / POST(JSON), a params template in which `${TEXT}` is replaced with the sentence, and your own headers. The response body must be raw **wav, mp3 or flac**. |

Engines only hand back audio bytes — playback, lip-sync and the bubble are the app's job.
Sentences are streamed: the next one is synthesized while the current one is still playing.

### Recommended: GPT-SoVITS

If you want your character to speak in a *particular* voice — a voice actor you like, a
character from a game — **GPT-SoVITS is the one to reach for**. All it needs is a short
reference clip: cut a few seconds of the voice you want and the model speaks in it.

Voicebox can also give your character a distinctive voice, but its synthesis is **slow** —
slow even with a GPU, slow enough that the pet leaves you waiting between lines. GPT-SoVITS
does not sound quite as good, but it is *extremely* fast: even on CPU the wait stays
acceptable. For a desktop pet that talks back to you, latency matters more than the last few
percent of quality.

## Optional LLM brain

*Settings → LLM* takes an OpenAI-compatible base URL (Ollama, LM Studio, most cloud providers)
or Anthropic's native API. Turn on **Let the LLM improvise idle behavior** and the idle
chatter, welcome lines and break reminders are written on the spot, in persona, using the same
schema as `perform`.

One product rule is absolute: **the LLM is never allowed to make the pet freeze.** A failure,
a timeout, an unparsable answer or a cooldown round all fall back to the built-in rules.
*Improvise at most every N seconds* (default 300) keeps cloud billing in check — set it to 0
to let a local model handle every round.

## Where your data lives

| | Windows | macOS | Linux |
|---|---|---|---|
| Settings | `%APPDATA%/live2d_mate/config.json` | `~/Library/Application Support/live2d_mate/config.json` | `~/.local/share/live2d_mate/config.json` |
| Models | `%APPDATA%/live2d_mate/models/` | `~/Library/Application Support/live2d_mate/models/` | `~/.local/share/live2d_mate/models/` |
| Personas | `%APPDATA%/live2d_mate/personas/` | `~/Library/Application Support/live2d_mate/personas/` | `~/.local/share/live2d_mate/personas/` |
| Long-term memory | `%APPDATA%/live2d_mate/memory/` | `~/Library/Application Support/live2d_mate/memory/` | `~/.local/share/live2d_mate/memory/` |

*Settings → About* shows the resolved paths for the machine you are actually on.
A corrupted `config.json` is renamed to `config.bak.json` and rebuilt from defaults rather
than taking the app down.

## Project layout

Four targets from a single root `CMakeLists.txt`. `src/` itself is the include root.

| Target | Contents | Links |
|---|---|---|
| `l2m_core` | `src/core/*` — **pure logic, never touches GUI or GL** | Qt Core/Network, yyjson, miniz |
| `l2m_live2d` | `src/live2d/*` — Cubism wrapper and GL rendering | `l2m_core`, Cubism Framework, glew, Qt Gui |
| `l2m_media` | `src/media/*` (audio and TTS), `src/llm/*` (LLM engines and the behavior brain) | `l2m_core`, miniaudio, Qt Network/WebSockets |
| `live2d_mate` | `src/app/`, `src/windows/`, `src/mcp/`, `src/platform/` | all of the above + httplib, Qt Widgets/OpenGL |

**All 63 tests link only `l2m_core`.** That is why "decision logic goes in `src/core/`,
Qt/GL/OS interaction goes everywhere else" is a hard rule here rather than a style preference:
it is the line between testable and untestable.

Three threads, and only three — the GUI thread (rendering, model, settings, TTS callbacks,
MCP tool execution), the httplib server thread with its worker pool, and the miniaudio
realtime thread, which only memcpys from a ring buffer, computes RMS and writes five atomics.
It never touches Qt, never allocates, never locks, and never decodes.

See [CLAUDE.md](CLAUDE.md) for the full architecture notes, the startup order and the
subsystem-by-subsystem rationale. Header block comments are the documentation in this
project — please keep them at the same density.

## Contributing

- 2-space indent, no tabs; roughly 100-column lines; members end with `foo_`; constants are
  `kCamelCase`.
- **Comments are written in Traditional Chinese**; strings shown to users or to the AI are
  English, and UI strings go through `i18n::translate` with keys in `i18n/*.json`.
- Commit messages are Traditional Chinese, `category: one-line summary`, with the body laid
  out as symptom → root cause → fix.
- A new MCP tool touches exactly three places: `src/core/mcp_tool_specs.cpp`,
  `src/mcp/mcp_tools.cpp` and `tests/test_mcp_tools.cpp`.


## License

The **source code** of Live2D Mate is released under the
**[MIT License](LICENSE)** — © 2026 RookieStudio.

**A built binary is not.** It statically links Live2D Cubism Core, which is proprietary, and
dynamically links Qt 6 under the LGPL v3. The full terms and the complete license texts are in
**[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**; `cmake --install` copies them into the
package, and they have to travel with any binary you distribute.

| | |
|---|---|
| **Live2D Cubism Core** | Proprietary. **Not** covered by this project's license and not redistributed here, but statically linked into the executable. Using it means accepting the [Live2D Proprietary Software License](https://www.live2d.com/sdk/license/); publishing an application built on it is additionally subject to the Cubism SDK Release License if your business revenue exceeds 10,000,000 JPY. |
| **CubismNativeFramework** | Live2D Open Software License — which also covers the two diffs under `patches/`, since their context lines are Live2D's source. |
| Qt 6 | LGPL v3, linked dynamically. |
| GLEW, miniz, yyjson, cpp-httplib, miniaudio, spdlog | Permissive (BSD / MIT / MIT-0 / public domain). |

Live2D and Cubism are trademarks or registered trademarks of Live2D Inc.
Live2D models belong to their respective creators — check each model's own terms before
using it.
