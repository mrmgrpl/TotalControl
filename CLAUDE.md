# TotalControl — kontekst projektu dla Claude

## Cel projektu

Aplikacja Windows do sterowania aparatami Sony przez **Sony Camera Remote SDK (CrSDK)**.  
Cel operacyjny: autonomiczne wykonanie sekwencji bracketów eksponometrycznych podczas zaćmienia Słońca TSE 2026-08-12 (Burgos/Lerma, totality 103.9s).

## Build

**Wymagania:** CMake 3.20+, MSVC (Visual Studio 2026 / VS 18), Windows 10+

```
cmake -B out/build/x64-Debug -S . -G "Visual Studio 17 2022" -A x64
cmake --build out/build/x64-Debug
```

VS Developer Prompt: `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat`

Exeki lądują w `out/build/x64-Debug/Debug/`.  
Post-build kopiuje DLL-e CrSDK + `CrAdapter/` obok SRV, oraz CLI obok SRV.

**CrSDK** jest w `external/CrSDK/{include,lib,bin}`. CMake weryfikuje obecność kluczowych plików — brak SDK = FATAL_ERROR.

## Architektura

```
src/
  main.cpp                   # TotalControlSRV — daemon: SDK init + pipe server + SequencerEngine
  CameraController.cpp       # warstwa CrSDK — jedyne miejsce z #include CrSDK
  SequencerEngine.cpp        # sekwencer UTC: Load/Start/Stop, dispatch do CommandHandler
  daemon/
    PipeServer.cpp           # named pipe \\.\pipe\TotalControl (JSON Lines, UTF-8)
    CommandHandler.cpp       # dispatcher: shoot/bracket/status/get/set/seq_*/...
  cli/
    main.cpp                 # TotalControlCLI — cienki klient pipe
  visualization/             # stubs: Renderer3D, Overlay2D, CameraPreview

include/
  CameraController.h         # zero #include CrSDK (forward-declare + pimpl)
  SequencerEngine.h          # SeqStep, SeqState, SequencerEngine class

sequences/                   # pliki JSON sekwencji zdjęciowych
  eclipse2026_example.json   # przykładowa sekwencja TSE 2026
  test_sequence.json         # sekwencja testowa (edytuj "at" timestamps)

docs/
  solar_eclipse_exposure_model.md  # NASA/Espenak formuła, Q-values, Python/C++ kalkulator

external/CrSDK/              # Sony SDK — nie modyfikować
```

### Kluczowa zasada izolacji SDK

Wszystkie `#include "CameraRemote_SDK.h"` **wyłącznie w `CameraController.cpp`**.  
`CameraController.h` jest czysty — forward-declare `namespace SCRSDK`.

## Property cache (m_propSetCache)

Po `Connect()` wywoływana jest `WarmCache()` — wczytuje wszystkie aktualne wartości kamery do `m_propSetCache`. Kolejne `Set*` sprawdzają cache przed wywołaniem SDK:

- **SetPropCached** — hit → `Skip (cached)`, brak SDK call; miss → SetPropRaw + cache update
- **SetPropAndVerify** — hit → skip; miss → SetPropRaw + polling `GetPropRaw` do potwierdzenia + cache update po confirm; retry w połowie okresu

Używają: SetPCRemotePriority, SetExposureMode, SetISO, SetFNumber, SetFocusMode, SetStoreDestination → `SetPropCached`; SetShutterSpeed (3s), DriveMode bracket/burst/single (2s) → `SetPropAndVerify`.

## Protokół pipe (JSON Lines)

Każde żądanie = jedna linia JSON + `\n`. Odpowiedź = jedna linia JSON + `\n`.

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

Pole `"cam"` opcjonalne — bez niego komenda → kamera[0].  
`"cam"` akceptuje: pełny GUID, prefiks GUID, indeks numeryczny.

### Format pliku sekwencji

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

`interval_ms` + `until` → krok jest ekspandowany do wielu powtórzeń przy Load().  
Krok pominięty (SKIP) gdy spóźnienie > 30s. Drift logowany przy każdym kroku.

## Konwencje kodu

- C++17, MSVC, Unicode (`wchar_t`, `std::wstring`)
- Namespace: `TotalControl`; alias SDK: `namespace SDK = SCRSDK;`
- Flagi: `/W4 /WX /MP /utf-8`

## Logowanie

- `TotalControlSRV.log` — daemon, obok exe, trunc przy starcie
- `TotalControlCLI.log` — CLI, obok exe, append; wyłącz: `--nolog`
- `CameraController.cpp` → `OutputDebugStringW` (DebugView / VS debugger)

## Stan aktualny (2026-05-25)

| Moduł | Stan |
|---|---|
| CMakeLists.txt | Gotowy — SRV + CLI targets |
| CameraController | Init/Connect/Disconnect + WarmCache + property cache + multi-cam |
| PipeServer | Działa — named pipe, JSON Lines |
| CommandHandler | shoot/bracket/burst/movie/af/get/set/cmd/quit/list_cameras/**seq_start/seq_stop/seq_status** |
| Multi-camera | Enumerate + Connect(guid) + routing po "cam":guid/index |
| Graceful shutdown | SetConsoleCtrlHandler → RequestShutdown na wszystkich kamerach → Shutdown() |
| GetId fallback | GuidOrIdHex() — USB kamera: GetId() jako UTF-16LE string |
| CountCapture CAS | Ochrona m_capturedCount przed stray late events |
| WritingState | slot1_writing / slot2_writing w status + get |
| DriveMode single | "drive":"single" w shoot → SetPropAndVerify(CrDrive_Single=0x01) |
| **SequencerEngine** | **Zaimplementowany** — Load/Start/Stop, UTC ms, repeat steps, seq_* pipe API |
| sequences/ | eclipse2026_example.json + test_sequence.json |
| docs/ | solar_eclipse_exposure_model.md |
| Renderer3D / Overlay2D / CameraPreview | Puste stubs |
| TotalControlGUI | Placeholder — nie zaimplementowane |

## Znane pułapki CrSDK

- `DeviceConnectionVersioin` — literówka w SDK (podwójne `i`), tak ma być
- `Connect()` jest **asynchroniczne** — czekaj na `OnConnected`, nie na `err==0`
- `ICrCameraObjectInfo*` wymaga `const_cast` przy przekazaniu do `Connect()`
- `EnumCameraObjects` — drugi arg = timeout w sekundach, bez niego może zawiesić wątek
- ShutterSpeed i DriveMode mogą być odroczone gdy kamera zapisuje RAW — polling GetPropRaw
- `StoreDestination = HostPC (0x01)` → CrNotify_Captured_Event NIE odpala
- Transport DLL-e muszą być w podkatalogu `CrAdapter/` — CMake robi to w POST_BUILD
- GetGuid() puste dla USB → GuidOrIdHex() fallback na GetId() jako UTF-16LE

## Kolejność cleanup (ważne)

```cpp
Cr::Disconnect(hDev);
Cr::ReleaseDevice(hDev);
Cr::Release();      // zatrzymuje wewnętrzne wątki SDK
delete cb;          // dopiero po Release()
```

## Ekspozycja — TSE 2026 (NASA/Espenak, f/8, ISO 100, h=8°, ΔEV=0.94)

```
t = f² / (ISO × 2^Q_eff)    Q_eff = Q_NASA - ΔEV

Zjawisko          Q   Q_eff   t@ISO100,f8
Chromosfera      11   10.06   ~1/2000s
Protuberancje     9    8.06   ~1/500s
Korona <0.1R☉     7    6.06   ~1/100s
Korona <0.2R☉     5    4.06   ~1/25s
Korona <0.5R☉     3    2.06   ~1/6s
Korona <1.0R☉     1    0.06   ~0.6s
Korona <2.0R☉     0   -0.94   ~1.2s
Korona <4.0R☉    -1   -1.94   ~2.4s
Korona <8.0R☉    -3   -3.94   ~10s
Earthshine       -5   -5.94   ~40s  ← ISO 400 = ~10s, ISO 1600 = ~2.5s
```

Earthshine przy ISO 100 wymaga ~40s. Dla 7s ekspozycji użyć ISO 1600 (~2.5s) lub ISO 400 (~10s).
