# TotalControl — kontekst projektu dla Claude

## Cel projektu

Aplikacja Windows do sterowania aparatami Sony przez **Sony Camera Remote SDK (CrSDK)**.  
Docelowo: wyzwalanie migawki, sekwencjonowanie zdjęć (SequencerEngine), podgląd live-view oraz wizualizacja 3D/2D (D3D11 + D2D1).

## Build

**Wymagania:** CMake 3.20+, MSVC (Visual Studio 2022), Windows 10+

```
cmake -B out/build/x64-Debug -S . -G "Visual Studio 17 2022" -A x64
cmake --build out/build/x64-Debug
```

Exeki lądują w `out/build/x64-Debug/Debug/`.  
Post-build kopiuje DLL-e CrSDK + `CrAdapter/` obok SRV, oraz CLI obok SRV.

**CrSDK** jest w `external/CrSDK/{include,lib,bin}`. CMake weryfikuje obecność kluczowych plików — brak SDK = FATAL_ERROR.

## Architektura

```
src/
  main.cpp                   # TotalControlSRV — daemon: SDK init + pipe server
  CameraController.cpp       # warstwa CrSDK — jedyne miejsce z #include CrSDK
  daemon/
    PipeServer.cpp           # named pipe \\.\pipe\TotalControl (JSON Lines, UTF-8)
    CommandHandler.cpp       # dispatcher komend: shoot/bracket/status/get/set/...
  cli/
    main.cpp                 # TotalControlCLI — cienki klient pipe
  SequencerEngine.cpp        # stub
  visualization/             # stubs: Renderer3D, Overlay2D, CameraPreview

include/
  CameraController.h         # zero #include CrSDK (forward-declare + pimpl)

external/CrSDK/              # Sony SDK — nie modyfikować
```

### Kluczowa zasada izolacji SDK

Wszystkie `#include "CameraRemote_SDK.h"` **wyłącznie w `CameraController.cpp`**.  
`CameraController.h` jest czysty — forward-declare `namespace SCRSDK`.

## Property cache (m_propSetCache)

Po `Connect()` wywoływana jest `WarmCache()` — wczytuje wszystkie aktualne wartości kamery do `m_propSetCache`. Kolejne `Set*` sprawdzają cache przed wywołaniem SDK:

- **SetPropCached** — hit → `Skip (cached)`, brak SDK call; miss → SetPropRaw + cache update
- **SetPropAndVerify** — hit → skip; miss → SetPropRaw + polling `GetPropRaw` do potwierdzenia + cache update po confirm; retry w połowie okresu

Używają: SetPCRemotePriority, SetExposureMode, SetISO, SetFNumber, SetFocusMode, SetStoreDestination → `SetPropCached`; SetShutterSpeed (3s), DriveMode bracket/burst (2s) → `SetPropAndVerify`.

## Protokół pipe (JSON Lines)

Każde żądanie = jedna linia JSON + `\n`. Odpowiedź = jedna linia JSON + `\n`.

```json
{"cmd":"list_cameras"}
{"cmd":"shoot","ss":"1/100","iso":100,"f":2.8,"mode":"M"}
{"cmd":"shoot","ss":"1/100","cam":"<guid>"}           // konkretna kamera po GUID
{"cmd":"shoot","ss":"1/100","cam":1}                  // lub po indeksie (0-based)
{"cmd":"bracket","ev":"1ev","count":5,"mode":"cont","ss":"1/100"}
{"cmd":"shoot","count":10,"drive":"cont-hi-plus"}
{"cmd":"status"}
{"cmd":"status","cam":"<guid-prefix>"}                // prefix GUID też działa
{"cmd":"get","prop":"shutter_speed"}
{"cmd":"set","prop":"drive_mode","val":"cont-hi"}
{"cmd":"quit"}
```

Pole `"cam"` jest opcjonalne — bez niego komenda trafia do kamery 0 (compat tryb jednej kamery).  
`"cam"` akceptuje: pełny GUID, prefiks GUID, indeks numeryczny.

## Konwencje kodu

- C++17, MSVC, Unicode (`wchar_t`, `std::wstring`)
- Namespace: `TotalControl`; alias SDK: `namespace SDK = SCRSDK;`
- Flagi: `/W4 /WX- /MP /utf-8`

## Logowanie

- `TotalControlSRV.log` — daemon, obok exe, trunc przy starcie
- `TotalControlCLI.log` — CLI, obok exe, append; wyłącz: `--nolog`
- `CameraController.cpp` → `OutputDebugStringW` (DebugView / VS debugger)

## Stan aktualny (2026-05-24)

| Moduł | Stan |
|---|---|
| CMakeLists.txt | Gotowy — SRV + CLI targets |
| CameraController | Init / Connect / Disconnect + WarmCache + property cache + multi-cam |
| PipeServer | Działa — named pipe, JSON Lines |
| CommandHandler | shoot / bracket / burst / movie / af / get / set / cmd / quit / list_cameras |
| Multi-camera | CameraController::Enumerate + Connect(guid) + routing po "cam":guid/index |
| Graceful shutdown | SetConsoleCtrlHandler → RequestShutdown na wszystkich kamerach → Shutdown() |
| SequencerEngine | Szkielet — do implementacji |
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

## Kolejność cleanup (ważne)

```cpp
Cr::Disconnect(hDev);
Cr::ReleaseDevice(hDev);
Cr::Release();      // zatrzymuje wewnętrzne wątki SDK
delete cb;          // dopiero po Release()
```
