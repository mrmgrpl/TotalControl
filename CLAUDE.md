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
    IqpClient.h/.cpp         # WinHTTP GET to maps.besselianelements.com + key-refresh
    BesselCalc.h/.cpp        # C1/C2/Max/C3/C4 from NASA Besselian elements
    TzEntry.h / EclipseEntry.h / Timezones.h   # data structs for DB rows
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

## Current status (2026-06-27)

| Module | Status |
|---|---|
| CMakeLists.txt | SRV + CLI + GUI; C++23; CMake 4.3.3; LANGUAGES C CXX |
| CameraController | Init/Connect/Disconnect + WarmCache + property cache + multi-cam |
| PipeServer | Working — named pipe, JSON Lines, persistent connection, multi-client |
| CommandHandler | shoot/bracket/burst/movie/af/get/set/cmd/quit/list_cameras/seq_* |
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

### TotalControlGUI — Phase 2b (complete)

**Column 1 — Hardware (200px):**
- **TIME**: UTC HH:MM:SS.mmm + Home TZ + Local/Eclipse TZ (598 IANA zones, DST); settings in SQLite; GE/C1–C4 countdown
- **CONNECTION**: ●/○ status + Connect cameras / Test picture / Disconnect cameras
- **CAMERA STATUS**: multi-camera polling ~2s; model, battery bar+%, Mode/SS/ISO/f/Focus/Drive, C1/C2 shot count, `Shot Nms` latency

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

**PITFALLs**:
- SUVI filenames: SS and trailing digit vary with NOAA schedule — cannot be guessed; must fetch directory listing
- CDN directory = ~10 MB Apache autoindex HTML; parse `href="NNNNNNNNNNNNNN_GOES19-SUVI-Fe171-1200x1200.jpg"`
- `ImTextureID = ImU64` since ImGui 1.91.4 → cast: `(ImTextureID)(uintptr_t)srv`
- `Database::SetSetting` takes `const char*` — use `.c_str()` on `std::to_string()` result

## Known pitfalls in IqpClient (maps.besselianelements.com API)

- API returns pretty-printed JSON with spaces after colons: `"message1": "..."` — `JsonStr` must skip whitespace before the opening `"`
- Success response has **no** `"message"` field (empty from JsonStr) **or** `"message":"OK"` at the end — treat both as success
- `"message":"Limit Error"` = rate-limit (too many requests) — do NOT retry or refresh key
- `"message":"Wrong Key"` (contains "Key"/"key") = expired key — refresh from map page JS + retry once
- Key auto-refresh: fetch `/map/<eclipseId>/`, search inline scripts then JS files for standalone 128-hex string; fall back to `/map/TSE20260812/`
- `SetIqpLogger(fn)` — set once in App ctor; logs to TotalControlGUI.log from background thread

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
