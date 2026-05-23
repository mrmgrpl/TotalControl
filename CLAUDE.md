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

Exe ląduje w `out/build/x64-Debug/Debug/TotalControl.exe`.  
Post-build automatycznie kopiuje DLL-e CrSDK do katalogu exe i podkatalogu `CrAdapter/`.

**CrSDK** jest w `external/CrSDK/{include,lib,bin}`. CMake weryfikuje obecność kluczowych plików przy konfiguracji — jeśli brakuje SDK, konfiguracja pada z FATAL_ERROR.

## Architektura

```
src/
  main.cpp                  # aktualnie: diagnostyczny harness (połączenie + test migawki)
  CameraController.cpp      # warstwa abstrakcji nad CrSDK — jedyne miejsce z #include CrSDK
  SequencerEngine.cpp       # stub — planowane sekwencje wyzwolenia
  visualization/
    Renderer3D.cpp          # stub — D3D11
    Overlay2D.cpp           # stub — D2D1/DWrite
    CameraPreview.cpp       # stub — live-view

include/
  CameraController.h        # zero #include CrSDK w nagłówku (forward-declare + pimpl)
  SequencerEngine.h         # szkielet
  Renderer3D.h              # szkielet

external/CrSDK/             # Sony SDK — nie modyfikować
```

### Kluczowa zasada izolacji SDK

Wszystkie `#include "CameraRemote_SDK.h"` i pozostałe nagłówki CrSDK **wyłącznie w `CameraController.cpp`**.  
Nagłówek `CameraController.h` jest czysty — forward-declare `namespace SCRSDK` bez żadnych include CrSDK.

## Konwencje kodu

- C++17, MSVC, Unicode (`wchar_t`, `std::wstring`)
- Namespace aplikacji: `TotalControl`
- Alias SDK w plikach `.cpp`: `namespace SDK = SCRSDK;`
- Flagi: `/W4 /WX- /MP /utf-8` — warningi jako błędy wyłączone świadomie (faza dewelopmentu)
- Definicje: `WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00`
- Brak konsoli: `add_executable(TotalControl WIN32 ...)` — entry point to `WinMain`

## Logowanie (debug)

`main.cpp` zapisuje do `C:\Temp\tc_log.txt` + `OutputDebugStringW`.  
`CameraController.cpp` używa wyłącznie `OutputDebugStringW` (widoczne w debuggerze VS lub w Sysinternals DebugView).

## Stan aktualny

| Moduł | Stan |
|---|---|
| CMakeLists.txt | Gotowy |
| CameraController | Działa — Init / Enum / Connect / Disconnect + pomiar latencji |
| main.cpp | Test harness: połączenie + wyzwolenie migawki + pomiar latencji |
| SequencerEngine | Szkielet — do implementacji |
| Renderer3D / Overlay2D / CameraPreview | Puste pliki — do implementacji |

## Znane pułapki CrSDK

- `DeviceConnectionVersioin` — literówka w SDK (podwójne `i`), tak ma być, nie naprawiać
- `Connect()` jest **asynchroniczne** — `err == 0` nie znaczy połączono; czekaj na callback `OnConnected`
- `ICrCameraObjectInfo*` z `GetCameraObjectInfo()` zwraca `const` — wymaga `const_cast` przy przekazaniu do `Connect()`
- `EnumCameraObjects` przyjmuje opcjonalny timeout w sekundach (drugi arg) — bez niego może zawiesić wątek
- Transport DLL-e (`Cr_PTP_IP.dll`, `Cr_PTP_USB.dll`, `libusb-1.0.dll`, `libssh2.dll`) muszą być w podkatalogu `CrAdapter/` obok exe — CMake robi to automatycznie w POST_BUILD

## Kolejność cleanup (ważne)

```cpp
Cr::Disconnect(hDev);
Cr::ReleaseDevice(hDev);
Cr::Release();      // zatrzymuje wewnętrzne wątki SDK
delete cb;          // dopiero po Release() — pewność, że SDK nie wywoła już callbacków
```
