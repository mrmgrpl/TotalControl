# Tooltips — tabela przeglądowa (do akceptacji)

Robocza lista pól GUI kandydujących do `ImGui::SetItemTooltip()` (hover) +
proponowana treść opisu, wynikająca z logiki kodu (nie z domysłu). Do
przejrzenia i redakcji przed implementacją — patrz `ROADMAP.md` → "Tooltips
(on-hover) dla wszystkich pól interfejsu".

Legenda kolumny "Uwagi":
- **widget** — zwykły element ImGui (Button/InputText/Combo/...), `SetItemTooltip()`
  działa bezpośrednio po wywołaniu.
- **draw-list** — tekst rysowany bezpośrednio na `ImDrawList` (np. etykiety na
  Solar Simulator w przestrzeni ekranu), tooltip wymaga ręcznego hit-testu
  względem pozycji myszy — więcej roboty niż zwykły widget.
- **readonly** — pole tylko do odczytu (status/obliczenie), tooltip wyjaśnia
  skąd wartość pochodzi, nie jak ją edytować.

---

## ECLIPSE (lewa kolumna)

| Pole (etykieta w UI) | Zmienna w kodzie | Proponowany opis | Uwagi |
|---|---|---|---|
| Combo wyboru zaćmienia (`##ecl_sel`) | `m_eclipseIdx`, `m_eclipses` | Wybór zaćmienia z bazy 11 898 pozycji (`TotalControlData.db`), elementy Besselian NASA/Espenak. | widget |
| Eclipse type (readonly) | `ec.type` | Typ zaćmienia w Greatest Eclipse — Total / Annular / Partial / Hybrid, wg elementów Besselian. | readonly |
| Duration at GE (readonly) | `ec.duration`/pochodna | Maksymalny czas trwania fazy całkowitej/obrączkowej w punkcie Greatest Eclipse (nie w Twojej lokalizacji — tam czas może być krótszy). | readonly |
| GE location (readonly) | GE lat/lon z rekordu eklipsy | Współrzędne punktu Greatest Eclipse — maksimum czasu trwania totalności, zwykle NIE pokrywają się z Twoją lokalizacją obserwacji. | readonly |

## LOCATION (lewa kolumna)

| Pole (etykieta w UI) | Zmienna w kodzie | Proponowany opis | Uwagi |
|---|---|---|---|
| Latitude (deg/min/sec + N/S) | `m_latDms` | Szerokość geograficzna obserwatora (stopnie/minuty/sekundy). Edycja ręczna albo automatycznie z wklejonego linku Google Maps. | widget |
| Longitude (deg/min/sec + E/W) | `m_lonDms` | Długość geograficzna obserwatora. | widget |
| Altitude (m) | `m_obsAltM` | Wysokość n.p.m. obserwatora. Uzupełniana automatycznie z Open-Elevation API po wklejeniu linku Google Maps (który sam w sobie NIE zawiera wysokości) — można nadpisać ręcznie. | widget |
| Paste Google Maps URL (`##gmaps_url`) | `m_gmapsUrlBuf` | Wklej link Google Maps (z aplikacji mobilnej lub przeglądarki) — aplikacja wyciąga z niego szerokość/długość geograficzną, a wysokość dociąga osobno z Open-Elevation API. Współrzędne można też wpisać ręcznie powyżej, bez linku. | widget |
| Calculate contact times (przycisk) | triggers `TriggerIqpFetch`/BE calc | Liczy czasy kontaktów C1–C4 dla bieżącej lokalizacji: przez IQP API (jeśli ustawiony klucz w Options) i lokalny model Besselian jako porównanie/fallback. | widget |
| IN TOTALITY ZONE / NOT IN TOTALITY ZONE | `ct.valid`, `ct.c2Ms` | Czy bieżąca lokalizacja leży w pasie całkowitości wg obliczonych kontaktów (C2 istnieje = jest totalność w tym miejscu). | readonly |

## TIME (lewa kolumna)

| Pole (etykieta w UI) | Zmienna w kodzie | Proponowany opis | Uwagi |
|---|---|---|---|
| Zegar UTC | `UtcNowMs()` | Czas uniwersalny (UTC) z zegara systemowego hosta — to na nim opiera się cała sekwencja. | readonly |
| Zegar "Home" (+ ikona koła zębatego) | `m_homeTzIana` | Czas w Twojej macierzystej strefie czasowej (do wyboru: 598 stref IANA, z DST). | widget |
| Zegar "Loc/Eclipse" (+ ikona koła zębatego) | `m_eclTzIana` | Czas lokalny w miejscu obserwacji zaćmienia (strefa IANA, z DST). | widget |
| Tabela kontaktów — wiersze IQP/BE/Loc/GE | `m_contacts`, `m_beResult` | Porównanie dwóch niezależnych silników obliczeniowych: IQP (API besselianelements.com, dokładniejszy model) i BE (lokalny model elementów Besselian, Meeus/Miller). Rozbieżności rosną przy skrajnych lokalizacjach (blisko krawędzi pasa) — to oczekiwane, nie błąd (patrz ROADMAP). | readonly |
| Countdown do najbliższego kontaktu | pochodna `ct.c1Ms..c4Ms` | Odliczanie czasu do najbliższego nadchodzącego kontaktu (C1/C2/Max/C3/C4). | readonly |

## SERVER (lewa kolumna)

| Pole (etykieta w UI) | Zmienna w kodzie | Proponowany opis | Uwagi |
|---|---|---|---|
| Connect cameras / Connection in progress.../ Cameras are connected | `m_connecting`, `connected` | Uruchamia TotalControlSRV.exe (jeśli nie działa) i łączy się ze wszystkimi wykrytymi aparatami Sony przez CrSDK. Wymaga wcześniejszego ustawienia aparatu w tryb zdalnego sterowania (patrz ROADMAP — instrukcja how-to). | widget |
| Test picture | pipe `{"cmd":"shoot","drive":"single","ss":"1/8000","iso":100,"f":8.0}` | Robi jedno testowe zdjęcie z **utrwalonymi** parametrami (1/8000s, ISO 100, f/8) — niezależnie od aktualnych ustawień aparatu — żeby zweryfikować samo połączenie/spust migawki, a nie bieżącą ekspozycję. | widget |
| Disconnect | — | Rozłącza wszystkie aparaty (SDK `Disconnect`/`ReleaseDevice`), SRV zostaje uruchomiony. | widget |
| CAMERA STATUS — Batt | `CamStatus::batteryPct`, `batteryLevel` | Procent baterii + poziom (kolor: zielony/żółty/pomarańczowy/czerwony wg progu 3/4, 1/2, 1/4); "U" = zasilanie/ładowanie przez USB. | readonly |
| CAMERA STATUS — Mode | `CamStatus::mode` | Tryb ekspozycji aparatu (np. M = Manual). Ustawiany na aparacie, nie w apce. | readonly |
| CAMERA STATUS — SS | `CamStatus::ss` | Czas naświetlania (shutter speed) aktualnie ustawiony na aparacie. | readonly |
| CAMERA STATUS — ISO | `CamStatus::iso` | Czułość ISO aktualnie ustawiona na aparacie. | readonly |
| CAMERA STATUS — f/ | `CamStatus::fnum` | Przysłona (f-number) aktualnie ustawiona na aparacie. | readonly |
| CAMERA STATUS — Focus | `CamStatus::focus` | Tryb ostrości (np. MF = Manual Focus). | readonly |
| CAMERA STATUS — Drive | `CamStatus::drive` | Tryb pracy migawki (pojedyncza klatka / seria / bracket) — kod SDK, nie zawsze czytelny wprost. | readonly |
| CAMERA STATUS — C1 / C2 | `slot1/2Remaining`, `slot1/2MaxRem`, `slot1/2Status` | Karta pamięci w slocie 1/2: pozostałe/maksymalne miejsce na zdjęcia (%) i status karty. "reading..." = SDK jeszcze nie zwrócił wartości po połączeniu. | readonly |
| CAMERA STATUS — Shot | `CamStatus::lastShotMs` | Czas ostatniego spustu migawki w ms (pełny stos: SDK+USB+migawka+potwierdzenie) — kolor: zielony <500ms, żółty <1500ms, czerwony powyżej. | readonly |

## BLOCK INSPECTOR (prawa kolumna, dla zaznaczonego bloku Timeline)

| Pole (etykieta w UI) | Zmienna w kodzie | Proponowany opis | Uwagi |
|---|---|---|---|
| Type (readonly) | `blk.type` | Typ bloku: Single / Burst / Bracket / Audio. Zmienia się przez menu Photo Sequence, nie edytowalny wprost. | readonly |
| At (readonly) | `blk.atMs` | Bezwzględny czas UTC wyzwolenia bloku na osi Timeline (drag na osi czasu, żeby zmienić). | readonly |
| SS combo | `blk.ss` | Czas naświetlania dla tego bloku — ustawiany na aparacie tuż przed strzałem (ARM), jeśli różni się od poprzedniego bloku. | widget |
| ISO combo | `blk.iso` | Czułość ISO dla tego bloku. | widget |
| f-stop combo | `blk.fstop` | Przysłona dla tego bloku. | widget |
| Bracket mode combo | `blk.ev`, `blk.count` | Liczba klatek × krok EV dla bracketu (16 wariantów wspieranych przez serię ILCE) — np. "5×1.0ev" = 5 klatek co 1 EV. | widget |
| Burst drive combo | `blk.burstDrive` | Tryb serii zdjęć: cont-hi-plus / cont-hi / cont-mid / cont-lo — różna liczba klatek/sekundę. | widget |
| Burst duration | `blk.burstDurMs` | Jak długo trzymać wyzwolony spust w trybie seryjnym (sekundy). | widget |
| Audio file combo | `blk.audioFile` | Plik MP3 do odtworzenia w tym bloku (np. zapowiedzi fazy zaćmienia). | widget |
| Audio duration (readonly) | `blk.audioDurMs` | Długość wybranego pliku audio w ms (sondowana przez MCI przy skanowaniu biblioteki). | readonly |
| Label | `blk.label` | Własna nazwa bloku widoczna na Timeline i w logu — czysto opisowa, nie wpływa na wykonanie. | widget |
| Snap to previous | `blk.snapToPrev` | Blok zaczyna się dokładnie w momencie zakończenia poprzedniego bloku na tym samym torze (+ czas ARM, jeśli ustawienia się zmieniają) — zamiast trzymać stały czas UTC. | widget |
| Snap to Seconds | `blk.snapToSec` | Zaokrągla czas startu bloku do pełnej sekundy UTC (ułatwia planowanie "co dokładnie 10s" itp.). | widget |
| Ogniskowa (Inspector, przy zaznaczonym torze kamery) | `TLTrack::focalMm` | Ogniskowa obiektywu na tym torze (mm) — steruje rysowaniem ramki kadru na Solar Simulator; 0 = brak ramki. | widget |

## EXECUTE / sekwencer (prawa kolumna)

| Pole (etykieta w UI) | Zmienna w kodzie | Proponowany opis | Uwagi |
|---|---|---|---|
| Wyświetlacz czasu playheada | `m_tlPlayheadMs` | Bieżąca pozycja głowicy odtwarzania na osi Timeline — cyan gdy sekwencja działa, bursztyn gdy idle/pauza. | readonly |
| TEST RUN / RESUME TEST | `m_guiSeqMode` | Odtwarza sekwencję w symulowanym czasie od pozycji playheada (realny upływ czasu, ale "teraz" = playhead, nie zegar systemowy) — do prób bez czekania na prawdziwe kontakty. | widget |
| STOP TEST | — | Pauzuje TEST RUN — pozycja playheada zostaje zachowana, drugie kliknięcie wraca do stanu Idle. | widget |
| RUN | `m_guiSeqMode` | Odpala sekwencję na **prawdziwym** czasie UTC — blok wykona się dokładnie o zaplanowanej godzinie zegarowej. | widget |
| STOP RUN | — | Zatrzymuje aktywne wykonanie RUN. | widget |

## SOLAR SIMULATOR — pasek statusu (Col2, pod widokiem)

| Pole (etykieta w UI) | Zmienna w kodzie | Proponowany opis | Uwagi |
|---|---|---|---|
| P | `m_solarP` | Kąt pozycyjny bieguna północnego Słońca względem północy niebieskiej (od bieguna IAU Słońca) — prawie stały w trakcie zaćmienia (<0.1°/h). | widget |
| q | `m_solarQ` | Kąt paralaktyczny — jak bardzo "zenit" jest obrócony względem północy niebieskiej w danym miejscu/czasie/wysokości Słońca nad horyzontem. | widget |
| rot | `m_solarP - m_solarQ` | Efektywny obrót pola widzenia względem zenitu (P−q) — to o tyle obraca się kadr aparatu z `Apply P = true` na Solar Simulator. | widget |
| Alt | `sunEph.alt_deg` | Wysokość Słońca nad horyzontem w stopniach, dla bieżącej pozycji playheada. | widget |
| Obs | `obscuration` | Procent powierzchni tarczy Słońca zasłonięty przez Księżyc w danym momencie (0% = brak zaćmienia, 100% = totalność/pierścień). | widget |
| zoom Nx (dolny prawy róg) | `m_solarZoom` | Aktualny poziom przybliżenia widoku (rolka myszy, zakres 0.2×–20×). | draw-list |

## SIMULATOR CONFIG — SUVI ALIGNMENT (Inspector, pod PALETTE)

| Pole (etykieta w UI) | Zmienna w kodzie | Proponowany opis | Uwagi |
|---|---|---|---|
| Moon opacity slider | lokalny `moonPct` | Przezroczystość tarczy Księżyca na Solar Simulator (kosmetyka widoku, nie wpływa na obliczenia). | widget |
| GOES-19 SUVI opacity slider | lokalny `suviPct` | Przezroczystość nakładki obrazu korony EUV (GOES-19 SUVI Fe171) na tarczy Słońca. | widget |
| SUVI channel combo | `curCh`, `kSuviChannels` | Wybór kanału obrazowania SUVI (obecnie tylko Fe171 — 171Å, górna korona ~600 000 K). | widget |
| Scale (suvi_disc) | `m_suviHalfQ` | Skala średnicy tarczy słonecznej na obrazie SUVI względem promienia Słońca w symulatorze — kalibracja dopasowania obrazu. | widget |
| V-offset (suvi_foot) | `m_suviFooterPx` | Przesunięcie środka tarczy w pionie (w pikselach oryginalnego obrazu SUVI 1200×1200) — koryguje przesunięcie obrazu źródłowego. | widget |
| H-offset (suvi_horz) | `m_suviCorrRightPx` | Przesunięcie obrazu SUVI w poziomie (px w przestrzeni obrazu źródłowego) — kompensuje dryf orbitalny GOES-19. | widget |

Uwaga: 4. pole (`m_suviCorrUpPx`) zostało świadomie usunięte z UI (komentarz w
kodzie App.cpp ~3486: "Vertical offset control intentionally removed from the
UI — variable and its use in SUVI positioning/persistence stay untouched.") —
zmienna nadal istnieje w DB/logice, ale nie ma kontrolki do edycji. Pomijam ją
w tooltipach.

## LIVE VIEW (Inspector)

| Pole (etykieta w UI) | Zmienna w kodzie | Proponowany opis | Uwagi |
|---|---|---|---|
| Checkbox włączenia podglądu na żywo (per kamera) | `m_lvActive[ci]`/podobne | Włącza strumień Live View z aparatu (SHM 5 fps) nałożony na Solar Simulator — do finalnej weryfikacji kadrowania przed totalnością. | widget |
| Slider przezroczystości (0–100%) | `m_lvOpacity` | Jak mocno widoczny jest rzeczywisty obraz z aparatu na tle modelu symulatora — 0% = czysty model, 100% = czysty Live View. | widget |

## CAMERA CONFIG (osobne okno, per aparat)

| Pole (etykieta w UI) | Zmienna w kodzie | Proponowany opis | Uwagi |
|---|---|---|---|
| Model / GUID (readonly) | `CamConfig::model`, `guid` | Model i unikalny identyfikator aparatu — zapisywane raz przy pierwszym wykryciu, nie zmieniają się później. | readonly |
| Sekcja "Detected Parameters" (Battery/Mode/SS/ISO/f/Focus/Drive/Slot1/Slot2) | `m_cameras` snapshot | Aktualne, na żywo odczytywane parametry aparatu — identyczne źródło co tabela CAMERA STATUS w lewej kolumnie. | readonly |
| Ogniskowa (InputInt) | `cc.focalMm` | Ogniskowa obiektywu przypisanego do tego aparatu (mm) — steruje rysowaniem ramki kadru w Solar Simulator; wolny tekst (obsługuje też zoomy typu "100-400"). | widget |
| Apply P (checkbox) | `cc.applyP` | Czy ramka kadru tego aparatu ma być obracana o kąt pola (P−q) w Solar Simulator (true = kadr "podąża" za obrotem nieba, np. teleskop na montażu paralaktycznym) czy zostać pozioma (false, np. statyw fotograficzny bez śledzenia obrotu pola). | widget |

## OPTIONS (osobne okno)

| Pole (etykieta w UI) | Zmienna w kodzie | Proponowany opis | Uwagi |
|---|---|---|---|
| IQP API key (InputText, maskowane) | `m_beApiKeyBuf` | Klucz API do besselianelements.com (40 znaków hex) — bez klucza aplikacja liczy kontakty wyłącznie lokalnym modelem Besselian (BE), z kluczem dociąga też dokładniejsze wyniki IQP do porównania. Klucz zapisywany tylko lokalnie w `TotalControlConfig.db`, nigdy w repo/kodzie. | widget |
| Show/Hide (przycisk obok klucza) | `m_beKeyVisible` | Odsłania/maskuje wpisany klucz API na ekranie. | widget |

---

## Otwarte pytania do Andrzeja

1. Czy tooltips mają obejmować też pola tylko-do-odczytu (status kamery,
   tabela kontaktów), czy tylko rzeczywiście edytowalne kontrolki? Powyższa
   tabela zakłada "wszystko", ale to zwiększa zakres 2–3×.
   [answer? — Andrzej decyduje]
2. Pola rysowane na `ImDrawList` (P/q/rot/Alt/Obs — potwierdzone jako zwykłe
   `TextColored`, więc OK; ale zoom-hint i etykiety osi N☉/N☾ na samym
   diagramie są czystym `AddText` na canvasie) wymagają ręcznego hit-testu
   myszy zamiast `SetItemTooltip()` — czy warto je obejmować w pierwszej
   iteracji, czy zostawić na później?
3. Czy komunikaty błędów (np. "Wysokosc: blad pobierania (Open-Elevation) -
   ustaw recznie") też powinny mieć tooltip z wyjaśnieniem, czy sam tekst
   wystarcza?
