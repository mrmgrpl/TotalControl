# Sterownik USB kamery Sony — instalacja i uruchomienie TotalControl

Ten katalog zawiera sterownik Windows wymagany do połączenia z kamerą Sony przez
USB przy użyciu Camera Remote SDK. Pochodzi z oficjalnej paczki Sony Camera
Remote SDK v2.02.00 (`Driver.zip`), sekcja instrukcji:
`RemoteSampleApp_IM_v2.02.00.pdf → "0. Preparation → Installation of device driver"`.

## 1. Instalacja sterownika (jednorazowo, na każdym komputerze)

1. Podłącz kamerę do komputera kablem USB.
2. Na kamerze, gdy pojawi się okno wyboru trybu USB, wybierz
   **"Remote Shoot (PC Remote)"**.
   (Można to też ustawić na stałe: `Setup > USB > USB Connection Mode`.)
3. W tym katalogu kliknij prawym przyciskiem myszy na właściwy plik `.inf`:
   - `srcameradriver.inf` — dla większości modeli (np. ILCE-7RM4A)
   - `srcameradriver_dscrx0m2.inf` — tylko dla modelu DSC-RX0M2
4. Wybierz z menu **"Zainstaluj" / "Install"**.
5. Potwierdź okno kontroli konta użytkownika (UAC) — **Tak**.
6. W oknie "Zabezpieczenia systemu Windows" (wydawca: **Sony Corporation**)
   kliknij **Zainstaluj / Install**.
7. Po komunikacie "Operacja zakończona pomyślnie" kliknij **OK**.
8. Weryfikacja: otwórz Menedżera urządzeń — powinna pojawić się gałąź
   **"libusbK Usb Devices" → "Sony Remote Control Camera"**.

Sterownik trzeba zainstalować tylko raz na danym komputerze — kolejne
uruchomienia programu go nie wymagają ponownie.

## 2. Uruchomienie programu

1. Uruchom `TotalControlSRV.exe` (katalog nadrzędny) — serwer/daemon musi
   działać w tle przez cały czas pracy z kamerą.
2. Uruchom `TotalControlGUI.exe` — interfejs graficzny łączy się z SRV przez
   named pipe `\\.\pipe\TotalControl`.
3. W GUI: sekcja **SERVER** (lewa kolumna) → **"Connect cameras"**.

## Rozwiązywanie problemów

- Jeśli kamera nie jest wykrywana mimo zainstalowanego sterownika, sprawdź w
  Menedżerze urządzeń, czy kamera nie widnieje jako "Portable Devices" (MTP)
  zamiast "libusbK Usb Devices" — w takim wypadku sterownik nie został
  poprawnie przypisany do urządzenia; zainstaluj go ponownie przy podłączonej
  i włączonej kamerze.
- Sterownik jest podpisany cyfrowo przez Sony Corporation (pliki `.cat`) —
  jeśli Windows odmawia instalacji z powodu niezaufanego wydawcy, upewnij się,
  że system ma włączone standardowe zasady weryfikacji podpisów sterowników.

## Licencje zewnętrzne

Sterownik korzysta z komponentów projektu **libusbK** (`libusbK.dll`,
`libusbK.sys`) — implementacji generycznego sterownika WinUSB używanego przez
Sony Camera Remote SDK do komunikacji USB. Powiązana biblioteka
`libusb-1.0.dll` (dostarczana osobno wraz z CrSDK, kopiowana do
`CrAdapter/` obok plików wykonywalnych) jest oprogramowaniem Open Source na
licencji **GNU Lesser General Public License v2.1** — pełny tekst licencji
znajduje się w `THIRD_PARTY_LICENSES/libusb_COPYING.txt` (wskazany wprost
przez dokumentację Sony jako obowiązujący dla tych komponentów).
