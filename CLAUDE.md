# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# TotalControl — project context for Claude

## Project goal

Windows application for controlling Sony cameras via **Sony Camera Remote SDK (CrSDK)**.  
Operational goal: autonomous execution of an exposure bracket sequence during the TSE 2026-08-12 solar eclipse (Burgos/Lerma, totality 103.9s).

## Build

**Requirements:** CMake 4.3.3+, MSVC (Visual Studio 2026 / VS 18), Windows 10+

```
# Configure (once, or after CMakeLists.txt changes):
VsDevCmd.bat -arch=amd64
cmake -B out/build/x64-Debug -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Build:
VsDevCmd.bat -arch=amd64
cmake --build out/build/x64-Debug
```

VS Developer Prompt: `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat`

**IMPORTANT: always `-arch=amd64`** — without it VsDevCmd sets LIB=x86 and the linker won't find CRT (199 LNK2019).  
If link is blocked by a running SRV: first run `TotalControlCLI quit`, then build.

Executables land in `out/build/x64-Debug/`.  
Post-build copies CrSDK DLLs + `CrAdapter/` next to SRV, CLI next to SRV, GUI next to SRV, and `TotalControlData.db` next to SRV.

**CrSDK** is in `external/CrSDK/{include,lib,bin}`. CMake verifies `Cr_Core.dll`, `Cr_Core.lib`, `CameraRemote_SDK.h`, `IDeviceCallback.h`, and all four CrAdapter transport DLLs (`Cr_PTP_IP.dll`, `Cr_PTP_USB.dll`, `libusb-1.0.dll`, `libssh2.dll`) — any missing file = FATAL_ERROR at configure time.

## Architecture

```
src/
  main.cpp                   # TotalControlSRV — daemon: SDK init + pipe server + SequencerEngine
  CameraController.cpp       # CrSDK layer — the only place with #include CrSDK
  SequencerEngine.cpp        # UTC sequencer: Load/Start/Stop, dispatches to CommandHandler
  daemon/
    PipeServer.cpp           # named pipe \\.\pipe\TotalControl (JSON Lines, UTF-8)
    CommandHandler.cpp       # dispatcher: shoot/bracket/status/get/set/seq_*/...
  cli/
    main.cpp                 # TotalControlCLI — thin pipe client
  gui/
    main_gui.cpp             # WinMain: D3D11 device + ImGui setup + message loop
    App.h / App.cpp          # all GUI logic: columns, timeline, camera polling, eclipse calc
    PipeClient.h/.cpp        # synchronous JSON-Lines pipe client (thread-safe)
    Database.h/.cpp          # SQLite3 wrapper (Open/Exec/GetSetting/SetSetting)
    IqpClient.h/.cpp         # WinHTTP GET to dedicated BE REST API (x-api-key); fallback → local BesselCalc
    BesselCalc.h/.cpp        # C1/C2/Max/C3/C4 from NASA Besselian elements
    TzEntry.h / EclipseEntry.h   # data structs for DB rows
  visualization/             # stubs: Renderer3D, Overlay2D, CameraPreview

include/
  CameraController.h         # zero #include CrSDK (forward-declare + pimpl)
  SequencerEngine.h          # SeqStep, SeqState, SequencerEngine class

sequences/                   # JSON sequence files
  eclipse2026_example.json         # example sequence TSE 2026
  eclipse2026_240mm_f56.json       # production: 240mm f/5.6 outer corona
  eclipse2026_900mm_f10.json       # production: 900mm f/10 inner corona (Earthshine ISO=400)
  test_sequence.json               # test sequence; C1=2026-08-12T12:00:00Z; run with --test C1=...

data/
  TotalControlData.db        # read-only: 11 898 eclipses (Besselian elements), 598 IANA timezones

docs/
  solar_eclipse_exposure_model.md  # NASA/Espenak formula, Q-values, Python/C++ calculator

external/
  CrSDK/                     # Sony SDK — do not modify
  imgui/                     # Dear ImGui (Win32 + DX11 backends) — built as static lib
  sqlite/                    # SQLite3 amalgamation — built as static lib
```

### Key SDK isolation rule

All `#include "CameraRemote_SDK.h"` **exclusively in `CameraController.cpp`**.  
`CameraController.h` is clean — forward-declares `namespace SCRSDK`.

## GUI architecture (TotalControlGUI)

### Rendering stack

`main_gui.cpp` owns D3D11 device + swap chain + ImGui init. Every frame: `ImGui_ImplDX11_NewFrame` → `app.OnFrame()` → `ImGui::Render` → `Present(1, 0)` (vsync).

### Layout (App::OnFrame)

`OnFrame()` renders a single full-screen host window (`##host`) below the main menu bar:

```
┌─ Menu bar ─────────────────────────────────────────────────────────────────┐
├─ Col1: Hardware ─┬─ Col2: Eclipse ──┬─ Status (auto) ─┬─ Inspector (200) ─┤
│ 200px            │ 400px            │ remaining width  │                    │
│ TIME             │ ECLIPSE          │ IQP status msg   │ INSPECTOR          │
│ CONNECTION       │ CONTACTS         │ last pipe result │ (selected block)   │
│ CAMERA STATUS    │                  │                  │ SS/ISO/f/bracket/  │
│                  │                  │                  │ burst params       │
├──────────────────┴──────────────────┴──────────────────┴────────────────────┤
│ Timeline (full width, 380px)                                                 │
│  ruler / phase markers / GE/C1–C4 markers / track rows / drag-to-move       │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Timeline data model

```cpp
enum class BlockType { Single, Burst, Bracket, Audio };

struct TLBlock {
    BlockType type;
    int64_t   atMs;       // absolute UTC ms (timeline position)
    // Camera: ss, iso, fstop, count, ev, burstDrive, burstDurMs
    // Audio:  audioFile, audioDurMs
    bool      snapToPrev; // snap start to end of previous block
};

struct TLTrack {
    std::string type;     // "camera" | "audio"
    std::string cameraId; // e.g. "ILCE-7RM4A"
    std::vector<TLBlock> blocks;
};
```

`App::ExportTimelineJson()` serialises tracks/blocks to a sequence JSON file (same format as `sequences/*.json`), which can then be fed to `TotalControlCLI seq_start`.

`BlockDurMs()` computes block duration from SS + overhead (kCamOverheadMs = 350ms measured USB latency). Used both for Inspector display and timeline rendering.

### Three SQLite databases (next to exe)

- `TotalControlDefaultConfig.db` — factory defaults, created by app on first run
- `TotalControlConfig.db` — active user config; keys: `show_home_clock`, `home_tz_iana`, `ecl_tz_iana`, `obs_lat`, `obs_lon`, `obs_alt_m`, `iqp_api_key`
- `TotalControlData.db` — read-only: 11 898 eclipses (Besselian elements), 598 IANA timezones (source: `data/TotalControlData.db`, copied by CMake post-build)

### GUI log

`TotalControlGUI.log` — next to exe, append mode. `App::LogLine()` is mutex-protected; safe to call from background IQP thread.

## Property cache (m_propSetCache)

After `Connect()`, `WarmCache()` is called — it reads all current camera values into `m_propSetCache`. Subsequent `Set*` calls check the cache before calling the SDK:

- **SetPropCached** — hit → `Skip (cached)`, no SDK call; miss → SetPropRaw + cache update
- **SetPropAndVerify** — hit → skip; miss → SetPropRaw + polling `GetPropRaw` for confirmation + cache update after confirm; retry at mid-interval

Used by: SetPCRemotePriority, SetExposureMode, SetISO, SetFNumber, SetFocusMode, SetStoreDestination → `SetPropCached`; SetShutterSpeed (3s), DriveMode bracket/burst/single (2s) → `SetPropAndVerify`.

## Pipe protocol (JSON Lines)

Each request = one JSON line + `\n`. Response = one JSON line + `\n`.

```json
{"cmd":"list_cameras"}
{"cmd":"shoot","ss":"1/100","iso":100,"f":8.0,"mode":"M"}
{"cmd":"shoot","drive":"single","ss":"7s","timeout_ms":15000}
{"cmd":"shoot","ss":"1/100","cam":"<guid>"}
{"cmd":"shoot","count":10,"drive":"cont-hi-plus"}
{"cmd":"bracket","ev":"1ev","count":5,"mode":"cont","ss":"1/100"}
{"cmd":"arm","ss":"1/100","iso":100,"f":"8.0","ev":"1ev","count":5,"mode":"cont"}
{"cmd":"status"}
{"cmd":"status","cam":"<guid-prefix>"}
{"cmd":"get","prop":"shutter_speed"}
{"cmd":"get","prop":"slot1_writing"}
{"cmd":"set","prop":"drive_mode","val":"cont-hi"}
{"cmd":"seq_start","file":"D:\\sequences\\eclipse.json"}
{"cmd":"seq_stop"}
{"cmd":"seq_status"}
{"cmd":"quit"}
```

The `"cam"` field is optional — without it the command targets camera[0].  
`"cam"` accepts: full GUID, GUID prefix, numeric index.

### Sequence file format

```json
{
  "steps": [
    {
      "at": "2026-08-12T20:29:02.100Z",
      "cmd": "bracket", "ev": "1ev", "count": 5, "ss": "1/1000",
      "label": "C2-chromosphere"
    },
    {
      "at": "2026-08-12T20:29:10.000Z",
      "until": "2026-08-12T20:30:46.000Z",
      "interval_ms": 10000,
      "cmd": "bracket", "ev": "1ev", "count": 5, "ss": "1/250",
      "label": "corona"
    }
  ]
}
```

`interval_ms` + `until` → the step is expanded into multiple repetitions at Load().  
Step skipped (SKIP) when late by > 30s. Drift is logged for each step.

### Test mode — `--test Cx=<utc>`

Shifts all sequence times by `simOffsetMs = (now + 15 000) − contactUtcMs` so that the specified contact fires ~15 s after start. The JSON file is not modified — the offset is injected as `sim_offset_ms` in the pipe command.

```
# Production sequence test — C2 triggers in 15 s from now:
TotalControlCLI seq_start sequences/eclipse2026_240mm_f56.json --test C2=2026-08-12T20:29:02.100Z

# Test from test file — step-1 triggers in 15 s:
TotalControlCLI seq_start sequences/test_sequence.json --test C1=2026-08-12T12:00:00.000Z

# "now" — current time as contact (effect: shift = +15 s, useful when sequence is already close to now):
TotalControlCLI seq_start sequences/test_sequence.json --test C1=now
```

**IMPORTANT:** `--test C2=now` with the production sequence (August 2026) gives `offset = +15 s` — it does NOT shift steps to the present; steps still wait until August 2026. Use `--test C2=2026-08-12T20:29:02.100Z` (which gives a large negative difference of ~−77 days) to actually test the sequence right now.

## NASA Power of Ten — coding rules

Adapted from Gerard J. Holzmann (JPL/NASA) for this C++23 codebase. All ten rules apply; violation requires explicit justification in a comment.

1. **Simple control flow.** No `goto`, no `setjmp`/`longjmp`, no direct or indirect recursion. Use iteration; if recursion seems necessary, convert to an explicit stack.

2. **Bounded loops.** Every loop must have a statically provable upper bound. Add a compile-time constant or `assert` to make the bound explicit; an unbounded spin loop is a defect.

3. **No dynamic allocation after init.** Heap allocation is allowed during startup (constructors, `OnInit()`). After the camera is connected and the sequencer is running, no `new`/`delete`/`malloc`/`free`. Use `std::array`, pre-allocated `std::vector`, or stack objects.

4. **Function length: max 600 lines.** Every function should remain within 600 lines. If a function exceeds this, extract a named helper. Long render blocks like `OnFrame()` / `RenderTimelineBottom()` should be split into focused `Render*()` methods.

5. **Minimum two assertions per function.** Use `assert()` to guard preconditions and postconditions. In release-critical paths use `static_assert` for compile-time invariants. Assertions document intent and catch logic errors early.

6. **Minimal scope for data.** Declare variables at the innermost scope where they are used. Prefer local variables over member variables; prefer member variables over globals. The only file-scope state in GUI code is D3D11 device globals in `main_gui.cpp`.

7. **Check all return values.** Every non-`void` return value must be checked. `std::expected<T,E>` results tagged `[[nodiscard]]` must not be silently discarded — use `(void)` only with an explicit comment explaining why the error is intentionally ignored.

8. **Preprocessor limited to includes and simple constants.** No token pasting, no variadic macros, no recursive macros. Prefer `constexpr`, `enum class`, and templates over `#define`. CrSDK headers are exempt (third-party).

9. **Restrict pointer use.** Maximum one level of indirection per expression. Prefer references and `std::span` over raw pointers. No function pointers — use `std::function` or template parameters. Raw pointers to CrSDK objects (`ICrCameraObjectInfo*`) are exempt where the SDK requires them.

10. **Zero-warning build, static analysis daily.** All project code compiles clean under `/W4 /WX`. Third-party code (CrSDK, ImGui, SQLite) is isolated via `target_include_directories(... SYSTEM PRIVATE ...)` which suppresses warnings from external headers. Run the MSVC static analyser (`/analyze`) before every non-trivial commit; address or explicitly suppress every finding with a rationale comment.

## Code conventions

- **C++23** (ISO/IEC 14882:2024), MSVC, Unicode (`wchar_t`, `std::wstring`)
- Namespace: `TotalControl`; SDK alias: `namespace SDK = SCRSDK;`
- Flags: `/W4 /WX /MP /utf-8`
- Use `std::expected<T,E>` for error propagation, `std::format` for string building
- `(void)expr` required when discarding `[[nodiscard]]` `std::expected` results
- JSON parsing in both SRV and GUI uses hand-written mini-parsers (no external library)

## Logging

- `TotalControlSRV.log` — daemon, next to exe, truncated on start
- `TotalControlCLI.log` — CLI, next to exe, append mode; disable with: `--nolog`
- `TotalControlGUI.log` — GUI, next to exe, append mode
- `CameraController.cpp` → `OutputDebugStringW` (DebugView / VS debugger)

## Current status (2026-07-12)

| Module | Status |
|---|---|
| CMakeLists.txt | SRV + CLI + GUI; C++23; CMake 4.3.3; LANGUAGES C CXX |
| CameraController | Init/Connect/Disconnect + WarmCache + property cache + multi-cam |
| PipeServer | Working — named pipe, JSON Lines, persistent connection, multi-client |
| CommandHandler | shoot/bracket/burst/arm/movie/af/get/set/cmd/quit/list_cameras/seq_* |
| Multi-camera | Enumerate + Connect(guid) + routing by "cam":guid/index |
| Graceful shutdown | SetConsoleCtrlHandler → RequestShutdown → Shutdown() |
| Singleton mutex | `TotalControl_DaemonRunning` — SRV rejects second instance |
| SequencerEngine | Load/Start/Stop, UTC ms, repeat steps, seq_start/stop/status |
| --test Cx-Ns | Reads contact time from JSON "contacts", lead = N seconds |
| Live monitor | Countdown ms to step after seq_start; Ctrl+C = exit |
| Startup status table | battery/mode/SS/ISO/f/focus/drive/cards/store/WB/time |
| sequences/ | eclipse2026_{240mm_f56,900mm_f10}.json — production TSE 2026 |
| docs/ | solar_eclipse_exposure_model.md |
| Renderer3D / Overlay2D / CameraPreview | Empty stubs |
| **TotalControlGUI Phase 0–2b** | **DONE** |
| **TotalControlGUI Phase 3a–3c** | **DONE** |
| **TotalControlGUI Phase 3d** | **DONE** |
| **TotalControlGUI Solar Simulator** | **DONE 2026-06-08** |
| **TotalControlGUI GOES-19 SUVI Fe171** | **DONE 2026-06-27** |
| **TotalControlGUI TLTrack focalMm** | **DONE 2026-06-28** |
| **TotalControlGUI Camera Config menu** | **DONE 2026-06-28** |
| **TotalControlGUI Live View overlay** | **DONE 2026-06-29** |
| **TotalControlGUI Corona gradient** | **DONE 2026-06-29** |
| **TotalControlGUI About modal (sources)** | **DONE 2026-06-29** |
| **TotalControlGUI Options menu + IQP API key** | **DONE 2026-06-29** |
| **IqpClient — stary scraping usunięty** | **DONE 2026-06-29** |
| **Camera Track Mode (Sun/Moon/Horizon)** | **DONE 2026-06-29** |
| **Card remaining shots false alarm fix** | **DONE 2026-06-29** |
| **Card capacity display (shots/max/%)** | **DONE 2026-06-29** |
| **GUI layout refactor — 3 kolumny** | **DONE 2026-06-30** |
| **Burst block fix** | **DONE 2026-06-30** |
| **BlockDurMs bracket-width fix (BracketSumMultiplier)** | **DONE 2026-07-12** |
| **CrSDK property-change event wiring (OnPropertyChanged)** | **DONE 2026-07-12** |
| **SetPropAndVerify — event-driven wait, honest false on timeout** | **DONE 2026-07-12** |
| **CommandHandler — armed-check before Shoot() (shoot/bracket/arm)** | **DONE 2026-07-12** |
| **Locale bug fix — CrSDK clobbers C locale, broke fractional EV parsing** | **DONE 2026-07-12** |
| **"Add All Bracket Variants" calibration preset (Photo Sequence menu)** | **DONE 2026-07-12** |
| **Bracket + ARM timing calibration (measured, not guessed)** | **DONE 2026-07-12** |
| **CLI `arm` subcommand** | **DONE 2026-07-12** |
| **Buffer-depletion model (repeated identical brackets)** | **IN PROGRESS — see Change log** |
| **Multi-camera true concurrency (SRV per-camera locks + acceptor pool, GUI per-camera pipe/threads)** | **DONE 2026-07-21** |
| **GUI/SRV hang-on-stop fix (PipeClient overlapped I/O + timeout)** | **DONE 2026-07-21** |
| **DriveModeNames — shared hex→string decoder (SRV status + CommandHandler)** | **DONE 2026-07-21** |
| **Live Delta-T (IERS bulletin fetch/cache/24h refresh)** | **DONE 2026-07-21** |
| **Per-model ARM latency calibration (arm_calibration table, ArmEstMs)** | **DONE 2026-07-21** |
| **ISO 8601 log timestamps (4-decimal fraction, all 3 executables)** | **DONE 2026-07-21** |
| **"Bracket ARM Calibration" + "Bracket SS Sweep" presets (Photo Sequence menu)** | **DONE 2026-07-21** |
| **BracketExposureSumMs — shutter-speed 30s ceiling clamp** | **DONE 2026-07-22** |
| **bracket_calibration corruption from SS Sweep runs (missing `ss` tag on samples)** | **OPEN — diagnosed, not fixed, see Change log 2026-07-22** |
| **Camera USB connect speedup — eliminate redundant re-enumeration** | **DONE 2026-07-22, verified on 4-camera hardware: ~27s/cam → ~0.3-1.0s/cam** — see Change log |

### TotalControlGUI — Phase 2b (complete)

**Column 1 — Hardware (200px):**
- **TIME**: UTC HH:MM:SS.mmm + Home TZ + Local/Eclipse TZ (598 IANA zones, DST); settings in SQLite; GE/C1–C4 countdown
- **CONNECTION**: ●/○ status + Connect cameras / Test picture / Disconnect cameras
- **CAMERA STATUS**: multi-camera polling ~2s; model, battery bar+%, Mode/SS/ISO/f/Focus/Drive, C1/C2 card `rem/max (%)`, `Shot Nms` latency
  - `CamStatus::slot1MaxRem / slot2MaxRem` — max remaining seen this session; carried forward between polls like `lastShotMs`; resets on app restart
  - Format: `"994/1024  97%"` when max known; `"994 shots"` otherwise; `"reading..."` when remaining==0 && status ok (SDK async startup)

**Column 2 — Eclipse (400px):**
- **ECLIPSE**: combo 11 898 eclipses (ImGuiListClipper); type+duration; GE lat/lon; observer Lat/Lon DMS + Alt (m)
- **[Calculate Contacts]**: triggers IQP background fetch + Besselian sync calc
- **CONTACTS**: side-by-side IQP (blue) and BE (green) — C1/C2/Max/C3/C4 in UTC and local TZ

**Three SQLite databases** (next to exe):
- `TotalControlDefaultConfig.db` — factory defaults, created by app
- `TotalControlConfig.db` — active user config; keys: show_home_clock, home_tz_iana, ecl_tz_iana, obs_lat, obs_lon, obs_alt_m, iqp_api_key
- `TotalControlData.db` — read-only: 11 898 eclipses (Besselian elements), 598 IANA timezones

### TotalControlGUI — Phase 3a–3c (complete)

- Full dark theme (`ApplyStyleDark()` — 58 colour tokens)
- Main menu bar: File (New/Export JSON), View, Help/About
- Inspector column: SS/ISO/f-stop per block; bracket combo (16 modes matching CrSDK); burst drive (HI+/HI/MID/LO) + duration
- Timeline bottom (380px): ruler with UTC ticks, eclipse phase markers (C1–C4/GE), track rows, drag-to-move blocks, Delete key removes selected block, Ctrl+D duplicates
- `ExportTimelineJson()`: saves TLTrack/TLBlock data as sequences/*.json via OPENFILENAME dialog
- `SaveTimeline()` / `LoadTimeline()` — `tl_tracks` + `tl_blocks` tables in `TotalControlConfig.db`; `m_tlDirty` flag triggers auto-save each frame

### TotalControlGUI — Phase 3d (in progress)

**PipeServer multi-client** (`PIPE_UNLIMITED_INSTANCES`):
- `Run()` spawns a detached `std::thread` per accepted client
- `ServeClient(HANDLE pipe)` — per-client JSON Lines loop; locks `m_handlerMtx` per call
- `m_handlerMtx` serialises concurrent calls to CommandHandler (not thread-safe)

**Background status thread** (`m_statusThread`):
- `StartStatusThread()` / `StopStatusThread()` / `StatusThreadProc()`
- Polls camera status every ~2s (200 × 10ms interruptible sleep)
- Writes `m_cameras` under `m_camerasMutex`; render thread snapshots under mutex
- Preserves `lastShotMs` across polls; replaces old on-frame `PollCameraStatus()`

**GUI sequencer thread** — runs independently of SRV `SequencerEngine`:
- `GuiSeqMode`: `Idle → TestRunning ↔ TestPaused; Idle → Running`
- `m_guiSeqMode`: `std::atomic<GuiSeqMode>` — written by main/seqThread, read by both
- `m_tlPlayheadMs`: `std::atomic<int64_t>` — updated 10ms by seqThread; drag/init by main
- `static constexpr int kMaxCamTracks = 4` — up to 4 simultaneous camera tracks
- `int m_seqNextBlock[kMaxCamTracks]` — per-track "next unfired" index; persists across pause/resume
- `SeqThreadProc(mode, playheadStartMs, realStartMs)`: 10ms tick; TestRun = `simMs = playheadStart + (realNow - realStart)`; Run = `simMs = realNow`; fires `BuildBlockCmd()` via pipe when `blk.atMs ≤ simMs`
- `BuildBlockCmd(blk, camIdx)` — static; returns JSON pipe cmd for Single/Bracket/Burst; `"cam":"N"` routing; Audio → `{}`

**Sequencer buttons** (`RenderSequencerButtons()` in Col1):
- Playhead time display (cyan = running, amber = idle/paused)
- **TEST RUN / RESUME TEST** (yellow/amber), **STOP TEST** — pause keeps position; second STOP = idle
- **RUN** (red palette), **STOP RUN** — fires on real UTC time (test: change system clock)
- Commands go **directly GUI → SRV via pipe**, no intermediate JSON file

**Playhead on timeline**:
- Cyan vertical line + triangle + time label in `RenderTimelineBottom()`
- Defaults to C2 − 45s when contacts first become available
- Click on ruler (Idle or TestPaused) sets playhead + recomputes `m_seqNextBlock`

**Named timeline snapshots** (`tl_snapshots` DB table):
- `CreateCalibrationSnapshot()` — seeds built-in calibration timeline (idempotent)
- `RenderSnapshotModal()` — open / save-as / delete modal
- File > Open / Save As in menu bar

**Bracket calibration** (measured latency → DB):
- `SeqCalibSample { count, ev, latMs }` — collected during TEST RUN / RUN (Bracket only)
- `m_seqCalibBuf` written by seqThread, read by main after join
- `SaveCalibFromBuf(camModel)` → `bracket_calib` table in Config.db; reloads `m_calibCache`
- `BlockDurMs()` uses calibrated latency if available; falls back to formula
- `SeedBuiltinCalib()` — seeds ILCE-7RM4A with measured data if table absent

**ARM deduplication + timeline visualization**:
- `BlockParamsDiffer(a, b)` — static helper; returns `true` if ARM is needed between blocks a and b
  (compares type / ss / iso / fstop; for Bracket: ev + count; for Burst: burstDrive)
- `ArmEstMs(b)` — conservative estimate: `min(2100, 1000 + count×300)` ms for Bracket; 1800ms for Single/Burst
- `SeqThreadProc` replaces `ArmParams`/`lastArmed[]` with `BlockParamsDiffer(fired, next)` check
- Post-block `m_tlPlayheadMs.store(simPost)` before ARM fires — reduces apparent freeze from 2270ms to ARM-only (~1800ms)
- Timeline pass 1 draws dim amber "ARM" extension bar after blocks that trigger ARM (width = `ArmEstMs`)
- Hit-testing and selection border extended to cover the ARM zone
- Snap-to-prev includes `ArmEstMs` when `BlockParamsDiffer(prev, cur)` is true

### TotalControlGUI — Solar Simulator (complete 2026-06-08)

**RenderSolarView** — rendered in Col2 (Eclipse column), below CONTACTS:

**Ephemeris integration** (EphClient + Database):
- `EphClient.h/.cpp` — JPL Horizons HTTPS fetch for Sun/Moon/5 planets over eclipse day
- Results cached in `TotalControlConfig.db` (eph_meta + eph_rows tables)
- `EphThreadProc` → `m_ephSamples[]`; `interpSnap(rows, simMs)` → EphRow per frame
- Playhead time (`m_tlPlayheadMs`) drives simulator — scrubbing moves Moon across sky

**SDO live solar image** (SdoClient.h/.cpp):
- WinHTTP HTTPS GET `sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_HMIIC.jpg`
- WIC (Windows Imaging Component) JPEG decode → RGBA; circular alpha mask (r=0.5)
- `m_sdoThread` writes pixels buffer + sets `m_sdoNewData`; render thread calls `CreateSdoTexture()`
- D3D11 Texture2D → SRV; rendered with `AddImageQuad` rotated by `P_rad = (P₀-q)*π/180`
- Offline cache: `TotalControlSDO.jpg` next to exe
- `OnInit(ID3D11Device*, ID3D11DeviceContext*)` — device passed from main_gui.cpp globals
- **PITFALL**: `TriggerSdoFetch()` must be called AFTER `m_d3dDev = d3dDev` in OnInit (not in ctor)
- **PITFALL**: `ImTextureID = ImU64` since ImGui 1.91.4 → cast: `(ImTextureID)(uintptr_t)srv`

**Angles and orientation**:
- `ComputeP0(ra, dec)` → P₀: solar N pole PA from celestial N (IAU pole RA=286.13°, Dec=63.87°)
- `ComputeQ(ra, dec, lat, lon, ms)` → q: parallactic angle (from GMST + hour angle)
- `ComputeMoonV(ra, dec)` → V: Moon N pole PA from celestial N (IAU pole RA=269.9949°, Dec=66.5392°)
- **rot = P₀ − q**: effective field rotation from zenith — displayed in status bar, used for 900mm frame label
- P₀ changes < 0.1°/h during eclipse; rot changes significantly with time (via q)
- **PITFALL**: ☉ (U+2609) not in Consolas — draw as `AddCircle` + `AddCircleFilled` manually

**Moon rotation axis**:
- `moonVrad = (V − q) * π/180` — Moon N pole direction from screen-up (zenith)
- Full axis line through Moon disc + arrowhead at N + "N" label + drawn circle (Moon symbol)
- Moon disc: gray normally; **100% black** (`IM_COL32(0,0,0,255)`) during totality (`dist + sunR < moonR`)

**Other features**:
- C2/C3: dynamic from Moon-Sun relative angular velocity between adjacent ephemeris samples
- Alt/Az grid (adaptive 0.5°/1°/5°), horizon, zenith marker, 24h trajectories
- Camera frame overlays: 240mm (dashed, 0°), 900mm (solid, rotated P_rad)
- Playhead drag: grab within 8px of triangle apex only (no click-to-teleport)
- Zoom: mouse wheel, range 0.2×–20×

**Next (after Solar Simulator)**: audio playback block, obscuration % display

### TotalControlGUI — GOES-19 SUVI Fe171 animation (complete 2026-06-27)

**Source**: `cdn.star.nesdis.noaa.gov/GOES19/SUVI/FD/Fe171/` — 1200×1200 JPEGs, cadence ~4 min

**SuviThreadProc** (background, one instance):
- Fetches CDN Apache directory listing (~10 MB HTML) → parses actual filenames (14-digit timestamp + `_GOES19-SUVI-Fe171-1200x1200.jpg` suffix)
- Sorts chronologically, takes last 300 frames
- **Smart cache** (`suvi_cache/` next to exe): loads cached files, downloads only missing, deletes files outside last-300
- WIC JPEG decode → RGBA (alpha = max(R,G,B)); D3D11 texture per frame
- Status in solar view: `SUVI N/300` (cyan, loading) or `SUVI N fr` (green, done)

**Animation**: 30 fps, 300 frames = 10 s loop; `m_suviAnimFps = 30.f`

**Image geometry** (1200×1200):
- Solar disc: 768×768 px → radius 384 px; centre at X=600, Y=590 (10 px above image centre)
- `kSuviHalfQ = image_half / disc_radius = 600/384` (base value)
- Rendered rotated by `P_rad = (P₀ − q) × π/180`; alpha-blended over simulator background

**SUVI ALIGNMENT panel** (Inspector column, below PALETTE):
- 4 × `InputFloat` with +/− buttons, step 0.005 or 1 px
- `m_suviHalfQ` — Skala dysku (disc scale relative to sunR)
- `m_suviFooterPx` — Offset stopki (disc centre Y offset from image centre, image px)
- `m_suviCorrRightPx` — Korekta prawo (shift right in image space, image px)
- `m_suviCorrUpPx` — Korekta góra (shift up in image space, image px)
- All 4 persisted to `TotalControlConfig.db` keys `suvi_half_q / suvi_footer_px / suvi_corr_right_px / suvi_corr_up_px` — survive restarts and GOES-19 orbital drift

**Red alignment circle**: `AddCircle(cx, cy, sunR)` always drawn on top of SUVI image for visual verification

**Periodic re-fetch**: every 1 minute measured from **completion** of previous fetch (not from start) — avoids
immediate re-fetch loop when download takes > 1 min. `m_suviFetchedAtMs` set at end of `SuviThreadProc`.

**PITFALLs**:
- SUVI filenames: SS and trailing digit vary with NOAA schedule — cannot be guessed; must fetch directory listing
- CDN directory = ~10 MB Apache autoindex HTML; parse `href="NNNNNNNNNNNNNN_GOES19-SUVI-Fe171-1200x1200.jpg"`
- `ImTextureID = ImU64` since ImGui 1.91.4 → cast: `(ImTextureID)(uintptr_t)srv`
- `Database::SetSetting` takes `const char*` — use `.c_str()` on `std::to_string()` result
- `m_suviPending.clear()` must be called in `TriggerSuviFetch()` to avoid stale frame accumulation across cycles
- Calibration defaults stored in DB with `suvi_calib_ver` key; v2 defaults: halfQ=1.5250, foot=20, corrR=-24, corrU=0

### TotalControlGUI — TLTrack focal length (complete 2026-06-28)

- `TLTrack::focalMm` (int, default 0) — lens focal length in mm per timeline camera track
- DB: `focal_mm` column in `tl_tracks` and `tl_snap_tracks`; `ALTER TABLE ADD COLUMN` migration for existing DBs
- Inspector column: `InputInt "Ogniskowa"` shown when any camera track is selected (above block params)
- Timeline track header: second line shows `"240 mm"` in green when focalMm > 0

### TotalControlGUI — Camera Config menu (complete 2026-06-28)

**Menu bar → Camera Config** — per-camera configuration windows.

**Data model** (`Database::CamConfig`):
```cpp
struct CamConfig {
    std::string guid;       // primary key — permanent, never updated after first detection
    std::string model;      // e.g. "ILCE-7RM4A" — recorded once on first detection
    int         focalMm = 0;   // configured lens focal length (0 = no frame drawn)
    bool        applyP  = true; // true → rotate solar frame by P_rad; false → horizontal (0°)
};
```
DB table: `camera_config (guid TEXT PK, model TEXT, focal_mm INTEGER, apply_p INTEGER)`

**Discovery**: `MergeCamerasIntoCamConfigs()` called every frame — merges `m_cameras` snapshot into
`m_camConfigs`; new cameras auto-registered in DB on first connection (model+GUID recorded once, immutable).

**Camera Config window** (floating, per camera):
- Section 1 "Detected Parameters": Model, GUID, Battery bar+%, Mode, SS, ISO, f/, Focus, Drive, Slot1/Slot2 (live from `m_cameras`; "Not connected" when offline)
- Section 2 "Configuration": `Ogniskowa` InputInt (step=0, free text entry — supports zoom lenses) + `Apply P` checkbox (true=corona framing rotated by P_rad, false=horizontal landscape)
- Changes saved immediately to DB via `SaveCamConfig()`

**Solar simulator frames** — dynamic, replace hardcoded 240mm/900mm:
- Loop over `m_camConfigs`; skip if `focalMm == 0`
- FOV: `fovW = 2·atan2(35.9/2, f)`, `fovH = 2·atan2(24.0/2, f)` (Sony full-frame 35.9×24.0mm hardcoded for now)
- Rotation: `applyP ? P_rad : 0.f`
- Color palette: niebieski/zielony/czerwony/bursztyn; first camera dashed, rest solid
- Labels: stacked top-left, format `"ILCE-7RM4A  240mm  8.6°×5.7°  (P=15°)"`

**PITFALL**: `std::vector<bool>` returns proxy objects, not `bool*` — `ImGui::Begin` requires `bool*`.
Use local `bool wndOpen = m_showCamCfgWnd[ci]; ImGui::Begin(..., &wndOpen); m_showCamCfgWnd[ci] = wndOpen;`

### TotalControlGUI — Live View overlay (complete 2026-06-29)

**Purpose**: final alignment check before totality — overlay camera's real live image on the solar simulator to compare actual vs. model framing. Opacity slider blends between pure model and pure LV.

**IPC mechanism: Named Shared Memory**
- SRV writes: `TotalControl_LV_<camIdx>` (2 MB + 8 B header)
- Layout (identical in `CameraController.cpp` and `App.cpp`):
  ```cpp
  struct LvShmLayout {
      volatile LONG frameNo;   // incremented after each JPEG write
      uint32_t      jpegSize;
      uint8_t       data[2*1024*1024];
  };
  ```
- Write: `std::memcpy(data) → _WriteBarrier() → InterlockedIncrement(frameNo)`
- Read: volatile read of `frameNo`; copy when changed; decode JPEG via WIC

**SRV side (CameraController)**:
- `StartLiveView(camIdx)`: `SetDeviceSetting(Setting_Key_EnableLiveView=0, value=1)` + `CreateFileMapping` + `MapViewOfFile`; pre-allocated `m_lvBuf` (2 MB, in `Init()`)
- `StopLiveView()`: `m_lvActive=false` → `SetDeviceSetting(0,0)` → `Sleep(50)` → `UnmapViewOfFile`
- `DeviceCallback::OnNotifyMonitorUpdated(CrMonitorUpdated_LiveView)`: `GetLiveViewImageInfo` → `GetLiveViewImage` → write SHM
- `m_lvActive` (atomic bool): guards callback against writing to unmapped SHM during StopLiveView

**GUI side (App)**:
- `m_lvThread` polls SHM every 200ms (5 fps); WIC JPEG decode → `m_lvPending[ci]` under `m_lvMutex`
- `m_lvNewFrames` (atomic bool): set by thread, cleared by `CreateLvTextures()` on render thread
- `CreateLvTextures()`: D3D11 Texture2D + SRV per camera (same pattern as SUVI)
- **Rendering**: in `RenderSolarView()` after Moon disc, before solar axis:
  - `AddImageQuad` centered on sun position (`cx,cy`), sized by camera FOV, rotated by `applyP ? P_rad : 0`
  - Alpha = `m_lvOpacity × 255`; then redraws white frame outline on top
- **Inspector UI**: `SeparatorText("LIVE VIEW")` → checkbox per camera (●/○ + model) → `SliderInt` 0-100% opacity
- Toggle ON: pipe `{"cmd":"lv_start","cam":"N"}` + start `m_lvThread` (lazy)
- Toggle OFF: pipe `{"cmd":"lv_stop","cam":"N"}` + release SRV

**Pipe commands**: `{"cmd":"lv_start","cam":"0"}` / `{"cmd":"lv_stop","cam":"0"}`

**PITFALLs**:
- `m_lvBuf` must be pre-allocated in `Init()` (NASA rule 3: no heap after init)
- `StopLiveView`: set `m_lvActive=false` BEFORE `SetDeviceSetting(0)`, then `Sleep(50)` before `UnmapViewOfFile` — prevents race with in-flight callbacks
- Focus magnifier (camera zoom) reflects automatically in LV stream — no extra command needed
- LV aspect ratio when camera uses focus magnifier differs from sensor (camera crops) — `AddImageQuad` stretches to frame rect; operator sees zoomed view stretched to full frame (acceptable for focus check)
- CommandHandler: `lv_start`/`lv_stop` use the outer-scope `cam` pointer (already routed at line 592) — do NOT redeclare `cam` inside these branches (C4456 shadow warning)

### TotalControlGUI — Corona gradient (complete 2026-06-29)

8 `AddCircleFilled` circles, all rendered BEFORE SUVI layer in `RenderSolarView()`:
```cpp
dl->AddCircleFilled({cx,cy}, sunR*9.0f,  IM_COL32(220,140, 50,  2));
dl->AddCircleFilled({cx,cy}, sunR*6.5f,  IM_COL32(235,160, 60,  3));
dl->AddCircleFilled({cx,cy}, sunR*4.5f,  IM_COL32(245,175, 75,  6));
dl->AddCircleFilled({cx,cy}, sunR*3.0f,  IM_COL32(250,190, 90, 11));
dl->AddCircleFilled({cx,cy}, sunR*2.1f,  IM_COL32(252,208,115, 18));
dl->AddCircleFilled({cx,cy}, sunR*1.5f,  IM_COL32(254,225,155, 26));
dl->AddCircleFilled({cx,cy}, sunR*1.2f,  IM_COL32(255,245,210, 36));
dl->AddCircleFilled({cx,cy}, sunR*1.06f, IM_COL32(255,255,245, 50));
```
Pearl-white at limb (`sunR*1.06`) → deep amber outer corona (`sunR*9`). Replaces old 3-circle flat amber.

### TotalControlGUI — About modal (complete 2026-06-29)

`RenderAboutModal()` — `BeginChild` scroll area 580×480px, 4 sections:
- **Application** — name, version, author, goal
- **Libraries & SDKs** — ImGui, CrSDK, SQLite, WinHTTP, Direct3D 11, WIC
- **Eclipse & Solar Data** — NASA JPL Horizons, NASA SDO, NOAA GOES-19 SUVI, besselianelements.com IQP, NASA Espenak
- **Acknowledgements** — individual library/data authors

`S::Link` local struct renders blue `TextColored` with URL in gray beneath. Non-clickable (ImGui limitation), user copies manually.

### TotalControlGUI — Options menu + IQP API key (complete 2026-06-29)

`RenderOptionsWindow()` — floating window, opened via **Menu bar → Options**:
- Password-masked `InputText` for IQP API key (40-char hex)
- Show/Hide toggle button
- Apply button: `SetBeApiKey(newKey)` + `m_configDb.SetSetting("be_api_key", newKey.c_str())`
- Instructions: contact besselianelements.com for key; without key → local BE model; with key → IQP API
- `m_showOptions` flag, `m_beApiKeyBuf[48]`, `m_beKeyVisible` in `App.h`

### TotalControlGUI — GUI layout refactor 3-column (complete 2026-06-30)

Layout zmieniony z 4-kolumnowego na **3-kolumnowy**: Left(300px) | Center(auto) | Right(270px).

**Lewa kolumna — `RenderLeftColumn()`** (sekcje kolejno):
- **ECLIPSE**: combo + pola read-only z niebieskim tłem (domyślny `ImGuiCol_FrameBg`): Eclipse type / Duration at GE / GE location (każde 100px, opis po prawej). Wybór zaćmienia → auto-load GE lat/lon → TriggerIqpFetch.
- **LOCATION**: DMS Latitude / Longitude (opis po prawej od N/S button); Altitude 100px (opis po prawej); centrowalny status IN TOTALITY ZONE / OUTSIDE TOTALITY.
- **TIME**: przycisk "Calculate contact times"; 3 zegary (UTC/Home/Loc z gear); tabela 5-kolumn (IQP/BE/Loc/GE) × C1/C2/Max/C3/C4/Rise/Set.
- **SERVER** (był HARDWARE): przycisk trójstanowy — "Connect cameras" → pomarańczowy "Connection in progress..." (m_connecting=true) → zielony "Cameras are connected"; Test picture; Disconnect; CAMERAS.

**Prawa kolumna** — Inspector + EXECUTE (RenderSequencerButtons przeniesiony pod Block Inspector).

**SKY VIEW SIMULATOR — pasek statusu**: po `Obs=XX.XX%` wyświetlane countdowns w jednej linii:
`C1: Xd HH:MM:SS  C2: …  Max: …  C3: …  C4: …` — kolor zgodny z typem zaćmienia.

**PITFALLs**:
- `FormatCountdown` używana w `RenderSolarView` (wcześniej niż definicja) → forward declaration przed `RenderStatusColumn`
- `m_connecting` (bool w App.h) reset automatyczny gdy `connected=true` w każdym frame

### TotalControlGUI — Burst block fix (complete 2026-06-30)

**Przyczyna**: `BuildBlockCmd(Burst)` nie wysyłał `"count"` → `CommandHandler` widział `count=1` → `Shoot()` zwalniał migawkę po 1 klatce (275ms zamiast burstDurMs).

**Fix w `CommandHandler.cpp`**:
- `isContinuousDrive = driveStr.contains("cont") || driveStr.contains("burst")` → wchodzi w ścieżkę burst niezależnie od count
- Gdy `isContinuousDrive && count <= 1`: `count = 9999` (sentinel) → `Shoot(count=9999, holdForBurst=true)` trzyma migawkę przez pełny `timeout_ms = burstDurMs + 2000ms`

## Change log

### 2026-07-12 — Timeline accuracy, ARM race condition, and calibration

Started from a bug report: GUI timeline blocks visibly shorter than the real
camera sequence — the playhead lagged behind real time after every bracket.
Root-caused and fixed in stages; each stage surfaced the next issue.

**1. `BlockDurMs()` bracket-width formula (`App.cpp`)**
The old formula scaled the ss-correction by `count` (`count × (ss − baseline)`),
but an N-shot 1EV bracket spans N *stops* of exposure time, not N × base-ss —
e.g. a 9-shot 1EV bracket at 0.2s base fires 3.2s, 1.6s, 0.8s, 0.4s, 0.2s, 0.1s,
1/20, 1/40, 1/80 (sum ≈ 6.39s), not `9 × 0.2s = 1.8s`. Added
`BracketSumMultiplier(count, evStep)` — sums `2^(k·ev)` over the bracket's
symmetric stop range — and used it in place of the flat `count` multiplier.
Verified against real hardware: old formula predicted 3094ms for a 9-shot/1EV/
ss=1/5 bracket that actually took 7485–7693ms (2.4× under); new formula
predicts 7452ms.

**2. CrSDK busy-state race (`CameraController.cpp`)**
Investigating why a fast automated benchmark occasionally shot the *wrong*
exposure (silently) led to three findings:
- `DeviceCallback::OnPropertyChanged/OnPropertyChangedCodes` were empty stubs —
  the SDK already notifies the app the instant a property change is confirmed
  (including media-writing-state going idle after a capture), but nothing was
  listening. Now bumps an atomic generation counter and notifies a
  `condition_variable`.
- `SetPropAndVerify` polled on a fixed 200ms/3-attempt cadence instead of
  waiting on that signal, and — critically — returned `true` even when the
  verify loop timed out without ever confirming the value. Rewrote it to wait
  on the property-change signal (500ms fallback cap per iteration, bounded to
  64 attempts), retry the write on each wake, and return `false` on genuine
  timeout so callers can no longer assume an unconfirmed property applied.
- `CommandHandler`'s `shoot`/`bracket`/`arm` handlers ignored that return value
  and fired the shutter regardless. Added an `armed` check in each handler:
  on a real verify failure, they now return `{"ok":false,"err":"arm_failed"}`
  instead of shooting on unconfirmed settings.

**3. Locale bug — fractional EV brackets silently broken**
The new "Add All Bracket Variants" preset (below) was the first thing to ever
exercise EV values other than `1ev`, and every fractional one (`0.3ev`,
`0.5ev`, `0.7ev`) failed with `unsupported_bracket`. Root cause: Sony's
`SCRSDK::Init()` resets the process-wide C locale to the system default
(Polish → comma decimal) sometime after `main()`'s own `setlocale(LC_ALL,"C")`
call. `std::stof(L"0.3")` under a comma locale stops parsing at the `.` and
silently returns `0.0` — `1.0ev`/`2.0ev`/`3.0ev` happened to still work because
their integer part alone gives the right answer. Fixed by re-asserting
`setlocale(LC_ALL,"C")` at the top of `CommandHandler::Handle()` (once per
request, cheap, immune to whatever CrSDK does between calls) rather than only
at process startup.

**4. "Add All Bracket Variants" preset (`App.cpp`, Photo Sequence menu)**
Adds one Bracket block per entry in the Inspector's bracket-mode dropdown (16
ev/count combinations), 2s gap from each block's ARM-end to the next block's
start — a one-click calibration/verification run exercising every supported
bracket variant. This is what exposed bug #3 above.

**5. Bracket duration + ARM timing calibration (measured, not guessed)**
- Re-ran all 16 bracket variants × 10 reps (160 shots, `calibration/
  bracket_calibration_10x.csv`) to get real mean/min/max/stddev per (count,ev).
  `LoadCalibCache()` now caches `latAvgMs + 50` (was `latMaxMs + 10`) —
  average + fixed margin instead of worst-observed-rep + margin, per explicit
  request.
- ARM-timing gap sweep (5000ms → 250ms per bracket count, `calibration/
  arm_sweep_final.csv`): found `gap + confirm_ms` converges to a near-constant
  total once the gap drops below the true buffer-clear threshold — i.e. this
  *is* a measurement of real buffer-clear time, not noise. Measured floors:
  count=3 → 4100ms, count=5 → 4350ms, count=9 → 5500ms (max+200ms margin).
  `ArmEstMs()` rewritten as `max(settings-change≈300ms, buffer-clear[count])`
  — replaces the old guess (`min(2100, 1000+count×300)`, which was under half
  the real value for count=9). `kDriveModeVerifyMs` in `CommandHandler.cpp`
  raised from a hardcoded 2000ms (at all 6 call sites) to 6000ms — the old
  budget was shorter than the real buffer-clear time for a 9-shot bracket,
  which was the actual cause of most `arm_failed` responses, not just tight
  pacing.
- Added a CLI `arm` subcommand (mirrors `bracket`; sets exposure/drive without
  firing) — needed as a zero-shot probe for the timing sweeps, kept as a
  general debugging tool.

**6. Buffer-depletion model — in progress**
Sweep of repeated *identical* brackets (`calibration/buffer_depletion_sweep.csv`,
1029 shots) to find how buffer-clear time grows under sustained shooting
(not just single-bracket-then-arm). Key finding: **total accumulated shot
count alone does not predict clear time** — a single 9-shot burst clears
slower than three 3-shot bursts totalling the same 9 shots (+13–28% at matched
T). Bracket size has an effect independent of total shots. Fitted count=3's
curve (9 points, T=3→60): flat ~4000ms up to T≈5, then linear at
~160.5ms/shot. Count=9 (3 points only): same ~3200ms baseline, but a steeper
~231.7ms/shot. Count=5 has no dedicated sweep yet (only the single-bracket
N=1 anchor, ~4138ms). Not yet wired into `ArmEstMs()` — needs a per-count
N-sweep (count=5 fully, count=9 more N-levels) before it can replace the
current single-bracket-only model.

<br>

<details>
<summary>Session data artifacts</summary>

- `calibration/bracket_calibration_10x.csv` — 160 shots, all 16 bracket
  variants × 10 reps, used to fix `LoadCalibCache()`.
- `calibration/arm_sweep_final.csv` — 72 trials, gap sweep 5000→250ms ×
  3 counts, used to fix `ArmEstMs()` and `kDriveModeVerifyMs`.
- `calibration/buffer_depletion_sweep.csv` — 1029 shots, repeated-identical-
  bracket sweep (count=3 primary, count=9 cross-validation); analysis above,
  not yet applied to code.

</details>

### 2026-07-21 — Multi-camera concurrency, live Delta-T, per-model ARM calibration

Started from a hardware report: with more than one camera connected, only
one camera ever fired at a time and Timeline scheduling drifted badly.
Root-caused to two separate single-instance bottlenecks, one per side of the
pipe:

**1. SRV serialization (`CommandHandler.h/.cpp`, `PipeServer.h/.cpp`)**
`CommandHandler` had one global `m_handlerMtx` serializing every command
across all cameras — replaced with per-camera `std::vector<std::mutex>
m_camLocks` (indexed via `CamIndex()`) plus a separate `m_globalLock` for
non-camera commands (`quit`/`list_cameras`/`seq_*`). `PipeServer::Run()` was
also a single-instance accept loop — a first attempt at fixing GUI-side
concurrency (below) made this *worse* by having all camera threads hammer
`CreateFileW` against it simultaneously (3 of 4 cameras failed a live test).
Fixed with an 8-thread acceptor pool (`AcceptorLoop()` × `kAcceptorThreads`).
Separately found and fixed a real UB bug while in this code: pipes were
created with `FILE_FLAG_OVERLAPPED` but `ServeClient()`'s `ReadFile`/
`WriteFile` passed `nullptr` for the OVERLAPPED param — added proper
`OverlappedReadByte`/`OverlappedWriteAll` helpers.

**2. GUI serialization (`App.h/.cpp`, `PipeClient.h/.cpp`)**
One shared blocking pipe connection and one sequencer thread served all
camera tracks. Rewrote to `PipeClient m_seqPipe[kMaxCamTracks]` (dedicated
connection per camera) and `std::thread m_seqThread[kMaxCamTracks]`
(`SeqCamThreadProc`, was `SeqThreadProc`) — each camera's bracket/burst
commands now fire independently instead of queueing behind the others.
Verified live: 4 cameras firing brackets within single-digit-to-tens-of-ms
of each other, down from seconds of drift.

**3. GUI hang on Stop, requiring the process to be killed**
`PipeClient::SendRequest` had no I/O timeout — a single stuck request could
block forever, and Stop couldn't cancel it. Added `PipeError::Timeout`,
`kIoTimeoutMs=10000`, and an `OverlappedIo()` helper using
`GetOverlappedResultEx` + `CancelIoEx` on timeout.

**4. DriveMode raw hex display**
`GetStatus()` (SRV) and CommandHandler's generic property decoder had two
independently-diverging copies of a DriveMode hex→string table, and neither
was complete — unmapped codes fell back to raw hex. Consolidated into one
shared `DecodeDriveMode()` (`DriveModeNames.h/.cpp`, ~55 entries).

**5. Live Delta-T (IERS bulletin), replacing the static Espenak catalog value**
Prompted by an email report (Alessandro Pessi) of a ~6s discrepancy against
besselianelements.com traced to a stale ΔT constant. New
`IersDeltaTClient.h/.cpp` fetches `finals.all.iau2000.txt` from
`datacenter.iers.org`, computes `ΔT = 32.184 + leap_seconds − UT1UTC`, and
caches the result in `delta_t_cache` (Config.db) with a fetch timestamp.
Background thread refreshes at most once per 24h
(`kDeltaTRefreshMs`); on fetch failure, falls back to the last successfully
cached value (never the static catalog) per explicit requirement — a stale
*measured* value beats a wrong *guessed* one. Displayed in the Solar
Simulator status bar between ALT and OBS.

**6. Per-model ARM latency calibration**
`ArmEstMs()` used one formula for every camera model regardless of its
actual DriveMode-change latency. Hardware testing found this varies hugely
by image-processor generation (BIONZ XR: ~300–550ms stable on ILCE-7SM3/
ILCE-7M4; BIONZ X: up to 5.4s and far less consistent on ILCE-7C/
ILCE-7RM4A) — see `docs/arm_latency_bionz_whitepaper.md`. Added
`arm_calibration` table (`cam_model, count → lat_avg_ms` etc., mirroring the
existing `bracket_calibration` pattern) and a member `ArmEstMs(TLBlock,
camModel)`. Found and fixed a second bug while wiring this up: none of the
6–9 real call sites actually passed `camModel` — every one defaulted to
`{}`, so the per-model lookup was dead code from the day it was written.
Threaded `camModel` through all of them (presets, snap-to-prev, hit-testing,
timeline drawing). `kDriveModeVerifyMs` explicitly kept flat at 6000ms
(not per-model) — a large constant gap is *wanted* here so the calibration
measurement environment stays uniform; calibrating the calibration would be
circular.

**7. New calibration presets (Photo Sequence menu)**
"Bracket ARM Calibration (5x, interleaved)" — 16 bracket variants × 5 reps,
feeds the per-model ARM/bracket "SAVE CALIB" buttons with a stable mean+max.
"Bracket SS Sweep" — validates `BlockDurMs()`'s exposure-time scaling across
the shutter-speed range (count=3 full sweep, count=5/9 spot checks); this is
the preset that exposed the shutter-speed ceiling bug fixed the next day
(2026-07-22, below).

**8. ISO 8601 log timestamps**
All three executables' log timestamp generators (`App::LogLine`, SRV's
`LogLine`/`LogFileOnly`/`LogWarning`, CLI's `Log()`) rewritten to full
`YYYY-MM-DDTHH:MM:SS.ffff` (4-digit fraction) via
`GetSystemTimePreciseAsFileTime` — was previously inconsistent (3-digit ms,
space separator) across the three binaries.

**Paused, not implemented**: camera USB connection speedup (~25s/camera).
Investigated `SDK::CreateCameraObjectInfoUSBConnection` as a way to skip the
redundant re-enumeration `main.cpp` currently does (once globally, then
again inside every `CameraController::Connect()` call) and parallelize the
per-camera connect loop — paused to prioritize calibration work, not yet
resumed.

### 2026-07-22 — BracketSumMultiplier() shutter-speed ceiling fix

The new "Bracket SS Sweep" preset (above) exposed a physically-impossible
prediction: a count=9/ev=1.0/base=8s block was estimated at ~200s. Root
cause: `BracketSumMultiplier()` summed `2^(k·evStep)` for each bracket stop
as a pure theoretical exponential, with no upper bound — for that block the
last stop computes to `8s × 2^4 = 128s`, a shutter speed that doesn't exist
on any of our camera bodies or in the SDK's own valid-value list (mechanical
shutter tops out at 30s; longer needs bulb, which brackets don't use). We
only ever set the bracket's *base* ss + DriveMode code — the camera's own
firmware computes each stop's actual exposure internally and snaps it to its
real value list (same mechanism as `CameraController::NearestShutterSpeed()`,
which queries `SDK::GetSelectDeviceProperties` and does log-space nearest
match), so an unclamped formula can only ever be a theoretical upper bound,
not a real prediction.

Fixed by replacing the dimensionless multiplier with `BracketExposureSumMs()`
(`App.cpp`), which computes each stop's nominal exposure time and snaps it
to the nearest entry in a generated standard 1/3-stop shutter-speed table
(`StandardShutterSpeedsSec()`, 30s → 1/8000s, ratio 2^(1/3)/step — the same
industry-standard geometric series Sony bodies use) before summing, via
`SnapToRealShutterSpeedSec()` (log-space nearest-match, mirroring
`NearestShutterSpeed()`). This makes the sum correctly plateau at the 30s
ceiling instead of growing without bound.

The calibrated path in `BlockDurMs()` previously scaled a fixed multiplier
by `(ssMs − kCalibBaselineSsMs)` — a linear correction that assumed the
sum-vs-ss relationship was linear, which the clamping breaks. Changed to
compute `BracketExposureSumMs()` directly at both the requested ss and the
calibration baseline ss, and subtract the two sums — correct at any base ss
including near/at the ceiling, with zero behavior change at short ss (where
clamping never triggers, matching all existing calibration data).

The "Bracket SS Sweep" preset's own SS range (kFullSs, `AddShutterSpeedSweepPreset()`)
was independently capped at 8s during the same investigation to keep sweep
runtime practical (30s at count=9 alone took ~16 min for one block) — that
part of the fix was already in place; only the duration-prediction formula
needed the ceiling fix above.

**Known open bug, not fixed today**: the SS Sweep run that exposed this also
exposed a pre-existing bug — `SeqCalibSample` doesn't track which `ss` a
sample was taken at, so if "SAVE CALIB" is pressed after an SS-Sweep run, its
across-many-shutter-speeds samples get averaged into `bracket_calibration`'s
`(count,ev)`-keyed rows as if they were all one baseline measurement,
corrupting them (confirmed via direct DB query: ILCE-7C count=3/ev=1.0ev
jumped from ~437ms/5 reps to 1119ms/14 reps after this happened once
already). Diagnosed, not yet fixed — don't press SAVE CALIB after an SS
Sweep run; re-run "Bracket ARM Calibration (5x)" to restore clean data if it
happens.

**Hardware note from the same session**: one camera (ILCE-7RM4A) fully
disconnected mid-test at 0% battery (`OnDisconnected 0x00008209`), then ran
a subsequent SS Sweep at only 8% after reconnecting — several of its ARM/
shoot commands failed outright (`arm_failed`, zero photos for that step),
and successful ones were 4-8x slower than its normal ~500-700ms. Confirmed
in code that failed attempts are correctly excluded from calibration sample
buffers (`if (ok && ...)` guards in both `sendArm` and the main shoot path
in `App.cpp`) — but successful-but-battery-degraded samples are NOT
excluded, so don't trust or save calibration data gathered from a
low-battery camera. This is also a general reminder that Timeline block
durations are always a forward *prediction*, computed before a block fires
— there is no feedback loop from actual shoot success/failure back into
what's displayed or scheduled.

### 2026-07-22 — Bracket calibration architecture: (model,count,ev) → (model,count)

Following the shutter-speed-table fix above, re-examined what
`bracket_calibration` was actually measuring. Queried the live DB and
computed `residual = measured_total_ms − BracketExposureSumMs(count, ev,
ss)` for every stored row: once the now-exact analytic exposure-time sum is
subtracted, the residual (pure per-shot mirror/shutter/buffer/USB/SDK
overhead) is ev- and ss-independent for a given (model, count) — confirmed
on real data, spread of a few percent at most (e.g. ILCE-7SM3 count=3:
346-360ms residual across 5 different ev variants). The `ev` key in the old
schema was measuring nothing real; it was also the direct cause of the
still-open SS-Sweep corruption bug (Change log above), since a table that's
supposed to vary by ev looks like a plausible target for SS-Sweep's
constant-ev/varying-ss samples to silently corrupt.

Restructured `bracket_calibration` from `(cam_model, count, ev)` to
`(cam_model, count)` — identical shape to `arm_calibration`. `Database.cpp`'s
`CreateCalibTables()` detects the old schema (presence of an `ev` column via
`PRAGMA table_info`) and drops+recreates automatically. `SeqCalibSample`
gained an `ss` field so `SaveCalibFromBuf()` can subtract each sample's own
analytic exposure sum (its own ev *and* ss) before averaging — this is what
actually fixes the SS-Sweep corruption bug: a sample from any ss is now a
valid calibration input instead of a risk, because the physics is subtracted
per-sample before anything gets averaged. `BlockDurMs()`'s calibrated path
simplified from a baseline-delta correction to a plain
`overhead[model][count] + BracketExposureSumMs(...)`.

Removed `SeedBuiltinCalib()` and `SeedArmCalibFromWhitepaper()` (hardcoded
C++ seed functions, ILCE-7RM4A-only and 2026-07-21-whitepaper-only
respectively) — factory-default calibration now ships as real database
content instead: `data/TotalControlDefaultConfig.db` (new, tracked in git,
same pattern as `data/TotalControlData.db`) is pre-populated via a direct
SQL write (not app source code) with fresh measured rows for all 4 tracked
models (`bracket_calibration` + `arm_calibration`, 3 reps/variant — see
"Bracket ARM Calibration" reps reduction below). New CMake `POST_BUILD` step
copies it next to the SRV output, same as `TotalControlData.db`.
`App::EnsureDefaultConfig()` needed zero code changes — its existing
`if (exists) return` guard means a shipped file is simply adopted as-is.

"Bracket ARM Calibration" preset reps reduced 5→3 (`kReps` in
`AddBracketArmCalibrationPreset()`): a repeat 5x run produced results
statistically indistinguishable from the first, so 3 has margin to spare
without unnecessary shutter wear.

### 2026-07-22 — Camera connect speedup: eliminate redundant re-enumeration

`main.cpp` enumerated cameras once via `CameraController::Enumerate()` to
build the sorted GUID list, then looped over them calling
`CameraController::Connect(guid, enumTimeoutSec, connectTimeoutMs)` — but
`Connect()` did its *own* fresh `SDK::EnumCameraObjects()` internally to
re-find the same GUID, for every single camera. For N cameras that's 1+N
full USB scans (each with a 5s ceiling) where 1 would do.

Added `CameraController::EnumerateRaw(timeoutSec, out&) -> void*` (same scan
as `Enumerate()`, but returns the raw SDK enum result as an opaque handle
instead of releasing it immediately) and `ReleaseEnum(void*)`. Added a new
`Connect(guid, void* preEnumeratedHandle, connectTimeoutMs)` overload that
searches the passed-in handle instead of re-scanning; both overloads now
share their post-resolve logic (`SDK::Connect`, wait for `OnConnected`,
`PopulateSupportedCodes` stabilization, `WarmCache`) via a new private
`ConnectToTarget()` helper, so nothing about the actual connect sequence
itself changed. `main.cpp`'s connect loop stays sequential exactly as
before — explicitly scoped to *not* touch parallelism, per discussion; only
the redundant-enumeration part was in scope. Opaque `void*` handle at the
`CameraController.h` boundary keeps the SDK-isolation rule intact (`main.cpp`
never touches an SDK type directly).

**Measured on real hardware same day, 4 cameras** (`TotalControlSRV.log`):
connect phase went from ~27s/camera (108s total, one full redundant USB
re-scan per camera) to ~0.3-1.0s/camera (2.9s total) — cameras now connect
essentially back-to-back. The search/enumeration phase itself (finding
cameras in the first place, `SDK::EnumCameraObjects`) is untouched by this
fix and stayed at its usual ~26.3-26.8s across every run, old and new alike
— that cost is internal to the SDK call, not something this change reaches.

**Progress bar follow-up**: both phases previously shared one hardcoded
`kExpectedPhaseMs = 25000` (25s = 100%) — not an SDK constant, just this
app's own guess, written before this fix existed. After the connect-phase
speedup that guess became wildly wrong for that bar specifically (bar would
sit at ~12% when the phase was actually already done). Split into
`kExpectedSearchMs = 27000` (tuned to the observed, SDK-bound ~26.3-26.8s)
and `kExpectedConnectPerCamMs = 1000` — the connect bar's 100% target is now
`cameraList.size() * kExpectedConnectPerCamMs`, since total connect time is
genuinely proportional to camera count post-fix, not a fixed guess.
`RenderProgress()` takes the expected-duration as a parameter instead of
reading a single module-level constant.

**Follow-up same day**: the connect-phase bar was removed entirely (not just
retimed) — at ~1s/camera it's too fast for an animated bar to convey
anything; replaced with a single `"Connecting to N camera(s)..."` log line.
Only the search-phase bar remains (`kExpectedSearchMs`), since that phase is
still genuinely ~27s and benefits from visual feedback.

## Known pitfalls in IqpClient (BE REST API / besselianelements.com)

IqpClient previously scraped `maps.besselianelements.com`. This has been REPLACED by a dedicated
REST API endpoint provided by the besselianelements.com team (2026-06-29).

### Dedicated REST API (current)

- **Endpoint**: `https://tryjhlq5f5.execute-api.eu-west-1.amazonaws.com/v1/eclipse`
- **Auth**: `x-api-key: <40-char-hex>` header — key stored ONLY in `TotalControlConfig.db` key `be_api_key`; NEVER in source code
- **No key** → `FetchContactTimes` returns `{}` immediately; App uses local `CalcBesselian` model
- **Response format** (confirmed from log 2026-06-29):
  ```json
  {
    "eclipse_type": "TOTAL ECLIPSE",
    "duration": "1m 44.2s",
    "eclipse_events": [
      {"event_type":"c1",  "utc_date":"2026-08-12", "utc_time":"17:33:54"},
      {"event_type":"c2",  "utc_date":"2026-08-12", "utc_time":"18:28:53.700000"},
      {"event_type":"max", "utc_date":"2026-08-12", "utc_time":"18:29:46"},
      {"event_type":"c3",  "utc_date":"2026-08-12", "utc_time":"18:30:37.900000"},
      {"event_type":"c4",  "utc_date":"2026-08-12", "utc_time":"19:22:12"}
    ]
  }
  ```
  **PITFALL**: Format is NOT flat `"c1":"ISO8601"` — times are in nested `eclipse_events` array,
  `utc_date` and `utc_time` are SEPARATE fields (not ISO 8601 combined). Seconds have decimal.
- `JsonStr` requires `"key":` (no space before colon) — API response has `: ` after colon which is
  handled because `JsonStr` skips whitespace AFTER the colon
- Error response: `{"message": "Wrong Key"}` or `{"message": "Limit Error"}`
- `SetIqpLogger(fn)` — set once in App ctor; logs to TotalControlGUI.log from background thread
- Log lines: `IQP-API fetch:`, `HTTP GET /v1/eclipse... -> 200 (N bytes)`, `body[0:600]:`, `IQP-API raw:`, `IQP-API: done — valid=...`

### Options window (App)

- **Menu bar → Options** → floating window with IQP API key field
- `m_beApiKeyBuf[48]` — InputText password-masked + Show/Hide toggle + Apply button
- Apply: `SetBeApiKey(newKey)` + `m_configDb.SetSetting("be_api_key", newKey.c_str())`
- OnInit: loads `be_api_key` from DB and calls `SetBeApiKey()` + copies to `m_beApiKeyBuf`

## Known pitfalls in the sequence JSON parser (SequencerEngine.cpp)

Hand-written mini-parser (no external libraries). Three pitfalls encountered during tests:

- `ExtractSteps`: search for `"steps"` then skip whitespace + `:` + skip whitespace + `[` (not `"steps":[` directly)
- `SJStr`: search for `"key":` then skip whitespace then check `"` (indented JSON has spaces between `:` and value)
- `SJInt64`: already tolerates whitespace after `:`

## Known CrSDK pitfalls

- `DeviceConnectionVersioin` — typo in SDK (double `i`), intentional
- `Connect()` is **asynchronous** — wait for `OnConnected`, not `err==0`
- `ICrCameraObjectInfo*` requires `const_cast` when passed to `Connect()`
- `EnumCameraObjects` — second arg = timeout in seconds; omitting it may hang the thread
- ShutterSpeed and DriveMode may be deferred while camera is writing RAW — poll via GetPropRaw
- `StoreDestination = HostPC (0x01)` → CrNotify_Captured_Event does NOT fire
- Transport DLLs must be in `CrAdapter/` subdirectory — CMake does this in POST_BUILD
- GetGuid() empty for USB → GuidOrIdHex() fallback to GetId() as UTF-16LE
- `GetTimeZoneSetting()` returns `0x8003` (CrError_Generic_NotSupported) for USB — camera time unavailable over USB; host UTC from `GetSystemTimePreciseAsFileTime()` is always available and sufficient for drift correction (compare with EXIF DateTimeOriginal)
- `CrSlotStatus_OK = 0x0000` (card present), `CrSlotStatus_NoCard = 0x0001` — old wrong ordering produced false "no-card" with card present
- `SCRSDK::Init()` resets the process-wide C locale to the system default (e.g. Polish → comma decimal) sometime after startup — any `std::stof`/`std::stod` on a JSON number after that point can silently misparse (`"0.3"` → `0.0`) unless the locale is re-asserted. `CommandHandler::Handle()` calls `setlocale(LC_ALL,"C")` on every request specifically because of this — don't remove it, and don't assume `main()`'s own startup `setlocale()` call is enough
- `MediaSLOT1_WritingState`/`MediaSLOT2_WritingState` are not reliably populated by `GetDeviceProperties()`'s bulk snapshot over USB on the ILCE-7RM4A — reads as an empty string even mid-write. Don't use it to gate property changes; poll via `GetPropRaw` + retry instead (see `SetPropAndVerify`)
- DriveMode changes are rejected (`err=0x8402`) while the camera is still clearing its capture buffer — this is a real, measurable delay (not just SDK/USB round-trip latency), see Change log 2026-07-12 and `calibration/arm_sweep_final.csv`. Don't assume a short fixed timeout is safe; it scales with recent shot count and bracket size

## Cleanup order (important)

```cpp
Cr::Disconnect(hDev);
Cr::ReleaseDevice(hDev);
Cr::Release();      // stops internal SDK threads
delete cb;          // only after Release()
```

## Exposure — TSE 2026 (NASA/Espenak, f/8, ISO 100, h=8°, ΔEV=0.94)

```
t = f² / (ISO × 2^Q_eff)    Q_eff = Q_NASA - ΔEV

Phenomenon        Q   Q_eff   t@ISO100,f8
Chromosphere     11   10.06   ~1/2000s
Prominences       9    8.06   ~1/500s
Corona <0.1R☉     7    6.06   ~1/100s
Corona <0.2R☉     5    4.06   ~1/25s
Corona <0.5R☉     3    2.06   ~1/6s
Corona <1.0R☉     1    0.06   ~0.6s
Corona <2.0R☉     0   -0.94   ~1.2s
Corona <4.0R☉    -1   -1.94   ~2.4s
Corona <8.0R☉    -3   -3.94   ~10s
Earthshine       -5   -5.94   ~40s  ← ISO 400 = ~10s, ISO 1600 = ~2.5s
```

Earthshine at ISO 100 requires ~40s. For a 7s exposure use ISO 1600 (~2.5s) or ISO 400 (~10s).
