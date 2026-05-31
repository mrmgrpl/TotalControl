# TotalControl — project context for Claude

## Project goal

Windows application for controlling Sony cameras via **Sony Camera Remote SDK (CrSDK)**.  
Operational goal: autonomous execution of an exposure bracket sequence during the TSE 2026-08-12 solar eclipse (Burgos/Lerma, totality 103.9s).

## Build

**Requirements:** CMake 3.20+, MSVC (Visual Studio 2026 / VS 18), Windows 10+

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
Post-build copies CrSDK DLLs + `CrAdapter/` next to SRV, and CLI next to SRV.

**CrSDK** is in `external/CrSDK/{include,lib,bin}`. CMake verifies the presence of key files — missing SDK = FATAL_ERROR.

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
  visualization/             # stubs: Renderer3D, Overlay2D, CameraPreview

include/
  CameraController.h         # zero #include CrSDK (forward-declare + pimpl)
  SequencerEngine.h          # SeqStep, SeqState, SequencerEngine class

sequences/                   # JSON sequence files
  eclipse2026_example.json         # example sequence TSE 2026
  eclipse2026_240mm_f56.json       # production: 240mm f/5.6 outer corona
  eclipse2026_900mm_f10.json       # production: 900mm f/10 inner corona (Earthshine ISO=400)
  test_sequence.json               # test sequence; C1=2026-08-12T12:00:00Z; run with --test C1=...

docs/
  solar_eclipse_exposure_model.md  # NASA/Espenak formula, Q-values, Python/C++ calculator

external/CrSDK/              # Sony SDK — do not modify
```

### Key SDK isolation rule

All `#include "CameraRemote_SDK.h"` **exclusively in `CameraController.cpp`**.  
`CameraController.h` is clean — forward-declares `namespace SCRSDK`.

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

**IMPORTANT:** `--test C2 now` with the production sequence (August 2026) gives `offset = +15 s` — it does NOT shift steps to the present; steps still wait until August 2026. Use `--test C2=2026-08-12T20:29:02.100Z` (which gives a large negative difference of ~−77 days) to actually test the sequence right now.

## Code conventions

- **C++23** (ISO/IEC 14882:2024), MSVC, Unicode (`wchar_t`, `std::wstring`)
- Namespace: `TotalControl`; SDK alias: `namespace SDK = SCRSDK;`
- Flags: `/W4 /WX /MP /utf-8`
- Use `std::expected<T,E>` for error propagation, `std::format` for string building
- `(void)expr` required when discarding `[[nodiscard]]` `std::expected` results

## Logging

- `TotalControlSRV.log` — daemon, next to exe, truncated on start
- `TotalControlCLI.log` — CLI, next to exe, append mode; disable with: `--nolog`
- `CameraController.cpp` → `OutputDebugStringW` (DebugView / VS debugger)

## Current status (2026-05-31)

| Module | Status |
|---|---|
| CMakeLists.txt | SRV + CLI + GUI; C++23; CMake 4.3.3; LANGUAGES C CXX |
| CameraController | Init/Connect/Disconnect + WarmCache + property cache + multi-cam |
| PipeServer | Working — named pipe, JSON Lines, persistent connection |
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
| **TotalControlGUI Phase 0–2b** | **DONE — see details below** |

### TotalControlGUI — Phase 2b complete (2026-05-31)

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

**New modules (Phase 2b):**
- `IqpClient.h/.cpp` — WinHTTP GET to maps.besselianelements.com; auto key-refresh from page JS; SetIqpLogger() for diagnostics
- `BesselCalc.h/.cpp` — C1/C2/Max/C3/C4 from NASA Besselian elements (Meeus/Miller algorithm, Newton iteration)
- `Database::LoadBesselianElements()` — loads all polynomial coefficients from TotalControlData.db

**Next: Phase 3** — Shoot panel, Sequencer panel, Timeline

### Known pitfalls in IqpClient (maps.besselianelements.com API)

- API returns pretty-printed JSON with spaces after colons: `"message1": "..."` — `JsonStr` must skip whitespace before the opening `"`
- Success response has **no** `"message"` field (empty from JsonStr) **or** `"message":"OK"` at the end — treat both as success
- `"message":"Limit Error"` = rate-limit (too many requests) — do NOT retry or refresh key
- `"message":"Wrong Key"` (contains "Key"/"key") = expired key — refresh from map page JS + retry once
- Key auto-refresh: fetch `/map/<eclipseId>/`, search inline scripts then JS files for standalone 128-hex string; fall back to `/map/TSE20260812/`
- `SetIqpLogger(fn)` — set once in App ctor; logs to TotalControlGUI.log from background thread

### Known pitfalls in the sequence JSON parser (SequencerEngine.cpp)

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
