# Roadmap

Pomysły i feature requesty zgłoszone przez beta testerów (korespondencja e-mail),
jeszcze niezaimplementowane. Po wdrożeniu wpis przenosi się do `CHANGELOG.md`
z kredytem dla zgłaszającego.

Status: `proponowane` / `w trakcie` / `wstrzymane`

Format wpisu:

## Tytuł pomysłu
- Status: proponowane
- Typ: feature / bug / pytanie / demo
- Zgłosił: Imię Nazwisko
- Opis: krótki opis czego dotyczy i jaki problem rozwiązuje

(`pytanie` = wymaga odpowiedzi mailowej, nie zmiany w kodzie; `demo` = sugestia
dot. prezentacji/YouTube, nie kodu — oba trzymane tu, żeby nie zgubić wątku)

---

## Focus viewer
- Status: **naprawione i przeniesione do `CHANGELOG.md` → 2026-07-20**
- Typ: feature
- Zgłosił: John Melson
- Opis: podgląd/narzędzie do oceny ostrości (focus) na żywo.
  **Pierwsza próba (błędna)**: uznano, że nic nie trzeba dodawać, bo natywny
  obiektyw Sony sam włącza lupę Live View, gdy operator kręci pierścieniem
  ostrości (sygnał elektroniczny obiektyw→aparat) — TotalControl ma gotową
  ścieżkę Live View (SHM→JPEG→panel w Solar Simulatorze) od czerwca, więc
  wystarczyłoby włączyć Live View i kręcić ostrością na aparacie.
  **Andrzej poprawił za**: to działa TYLKO z natywnym obiektywem Sony z
  elektronicznym pierścieniem. Jego setup na TSE 2026 to m.in. teleskop
  (Bresser AR90 900mm f/10, obiektyw pasywny/manualny) — taka optyka nigdy
  nie wysyła sygnału obrotu pierścienia do aparatu, więc lupa nigdy się nie
  włącza automatycznie. Trzeba sterować nią zdalnie przez SDK.
  **Rozwiązanie (zmierzone na sprzęcie, ILCE-7RM4A, 2026-07-20)**:
  `CrDeviceProperty_Focus_Magnifier_Setting` (kod `0x011D`) istnieje i steruje
  lupą zdalnie, ale wartość NIE jest prostym enumem 1/2/3, jak można by
  założyć z nazw w menu aparatu (x1.0/x5.9/x11.9) — to złożona 40-bitowa
  wartość (`CrDataType_UInt64`, nie `UInt8`), gdzie bity [32:39] to
  powiększenie×10 (`0x0A`=x1,0, `0x3B`=x5,9, `0x77`=x11,9, `0x00`=lupa
  wyłączona), a bity [0:31] to inny, zmienny stan aparatu, który trzeba
  zachować przy zapisie (odczyt-modyfikacja-zapis, nie zwykły zapis).
  Namierzone empirycznie: baseline dump wszystkich 93 właściwości aparatu
  przez nowe narzędzie diagnostyczne `dump_props` (SRV+CameraController),
  porównanie przed/po fizycznym ustawieniu lupy na aparacie przez Andrzeja —
  dokładnie to znalazło poprawny kod i format, zamiast dalszego zgadywania.
  Potwierdzone w obie strony na żywo: zapis z aplikacji faktycznie zmienia
  powiększenie widoczne na ekranie aparatu (x1.0/x5.9/x11.9/wyłączone), nie
  tylko odczyt.
  Zaimplementowane: `focus_magnifier` w `kProps` (CommandHandler.cpp),
  specjalny handler w `set` z odczyt-modyfikacja-zapis (zachowuje dolne 32
  bity), `DumpAllProps()` w CameraController + `dump_props` w pipe protocol
  (trwałe narzędzie diagnostyczne do przyszłych niezbadanych właściwości).
  GUI: 4 przyciski **Off / x1.0 / x5.9 / x11.9** w panelu SIMULATOR CONFIG,
  przy suwaku Live View każdej kamery, podświetlający aktywny wybór.

## Weryfikacja przypisania kamer (sortowanie po GUID, dynamiczne tory) na 2+ kamerach
- Status: zaplanowane na 2026-07-21 (wtorek) — Andrzej wejdzie w posiadanie
  dodatkowych kamer testowych
- Typ: bug (weryfikacja fixa)
- Zgłosił: John Melson (pierwotne zgłoszenie — Timeline pokazywał kamerę pod
  cudzą, zaszytą na sztywno nazwą modelu; **naprawione i przeniesione do
  `CHANGELOG.md` → 2026-07-19**)
- Opis: sama poprawka (pozycyjne przypisanie kamera↔tor, sortowanie po GUID
  w SRV, dynamiczny przyrost liczby torów) jest zaimplementowana i
  zweryfikowana build+log+zrzutem ekranu na **1 fizycznej kamerze** — to
  wystarcza, żeby potwierdzić brak regresji i poprawność logiki dla
  najczęstszego przypadku (1 kamera), ale nie testuje samego sedna zgłoszenia
  Johna (dwie różne kamery, poprawne rozróżnienie która jest którą) ani
  sortowania po GUID (efekt widoczny dopiero przy 2+ urządzeniach). Do
  zrobienia we wtorek: podłączyć 2+ kamery, potwierdzić że (1) każdy tor
  pokazuje właściwy, inny model+GUID, (2) kolejność slotów jest stabilna
  między restartami SRV niezależnie od kolejności podłączania USB, (3) nowy
  tor pojawia się automatycznie przy podłączeniu dodatkowej kamery.

## Instrukcja "how-to" dla operatora: jak przygotować i podłączyć aparat
- Status: **naprawione i przeniesione do `CHANGELOG.md` → 2026-07-19**
- Typ: feature
- Zgłosił: John Melson
- Opis: John nie pamiętał, jak ustawić aparat przed połączeniem z aplikacją.
  U niego zadziałało ustawienie w menu aparatu "Remote Shoot/Trn" + Set —
  ale to jego domysł, że ustawienie różni się między modelami, nie
  potwierdzony fakt. Realny problem to brak jakiejkolwiek instrukcji dla
  operatora w aplikacji — kwestia edukacyjna, nie logiki zależnej od modelu.
  Zaimplementowano **obie** zasugerowane opcje naraz (nie tylko jedną):
  1. Osobne okno "Camera Setup" (`RenderCameraSetupModal()`, App.cpp),
     otwierane przez menu **About → Camera Setup** (osobna pozycja obok
     "About TotalControl" i "What's New", ten sam wzorzec co
     `RenderWhatsNewModal()`) — 5-punktowa lista krok-po-kroku (Manual/RAW/
     MF/USB Camera Control by PC/podłącz kabel USB — punkt 5. dodany na
     prośbę Andrzeja, oczywisty, ale łatwy do pominięcia krok). Pierwsza
     wersja osadziła tę listę na górze modala "About TotalControl"; Andrzej
     poprosił o wydzielenie do własnego okna dla lepszej odnajdywalności —
     poprawione tego samego dnia.
  2. Tooltip na przycisku "Connect camera" (SERVER, lewa kolumna) — ten sam
     zestaw ustawień skrócony do jednego zdania, dodany już wcześniej przy
     okazji pełnego rollout'u tooltipów (`IsItemHovered(AllowWhenDisabled)`
     + `SetTooltip`, bo przycisk bywa disabled).
  **Domysł Johna o zależności od modelu POTWIERDZONY** oficjalną
  dokumentacją Sony (support.d-imaging.sony.co.jp/app/imagingedge/en/
  instruction/4_1_connection.php, link wklejony też w oknie Camera Setup):
  krok "USB Connection Mode" (MENU → Setup → [USB] → [USB Connection Mode])
  faktycznie ma różne etykiety per seria aparatu — "Remote Shooting"
  (ILCE-1M2/1/9M3, ILCE-7SM3/7RM5/7M4), "Remote Shoot (PC Remote)"
  (ILCE-7CR/7CM2/6700, ZV-1F/1M2), "Remote Shoot/Trn." (ILCE-7RM6/7M5,
  DSC-RX10M5 — to dokładnie ten wariant, którego użył John). Dodana tabela
  w oknie Camera Setup + uwaga o rozłączeniu Wi-Fi/smartfona przed
  podłączeniem USB na aparatach z Wi-Fi. Zweryfikowane zrzutem ekranu i
  bezpośrednio przez Andrzeja na żywo.

## Reset stanu połączenia do "świeżo uruchomiona aplikacja" bez restartu
- Status: proponowane
- Typ: feature
- Zgłosił: John Melson
- Opis: John próbował kilku typów połączenia zanim trafił na właściwy i chciał
  zresetować bieżący stan/próby połączenia do stanu początkowego bez Quit +
  ponownego uruchamiania aplikacji. Prośba: pozycja w menu, która czyści
  bieżący stan połączenia (disconnect + reset) bez zamykania SRV/GUI.

## Network error przy geokodowaniu linku Google Maps (lokalizacja)
- Status: **naprawione i przeniesione do `CHANGELOG.md` → 2026-07-19 (John
  Melson)** — patrz wyjaśnienie niżej
- Typ: bug
- Zgłosił: John Melson
- Opis: John wkleił link Google Maps do Mallorki — aplikacja poprawnie
  rozpoznała kraj (Hiszpania), ale wystąpił "network error" (zrzut ekranu
  załączony w mailu, nieprzeniesiony do repo).
  **Znalezione przy okazji sesji 2026-07-19**: literalny string "network error"
  występował w całym kodzie GUI w dokładnie jednym miejscu — banner strefy
  totalności (`RenderLeftColumn`, warunek `!ct.apiOk`) renderowany bezpośrednio
  pod adresem/lokalizacją, czyli dokładnie tam, gdzie John musiał go zobaczyć
  po wklejeniu linku. To NIE był błąd sieci ani `GeocodeClient`/`ElevationClient`
  — `ct.apiOk` jest fałszywe zawsze, gdy nie ustawiono klucza IQP API (patrz
  IqpClient.h: "No key → FetchContactTimes returns {} immediately"), co jest
  normalnym, oczekiwanym stanem bez klucza, nie awarią. Etykieta była po
  prostu myląca. Naprawione: tekst zmieniony na "To get IQP timings, set an
  API key in the Options menu" — jasno tłumaczy przyczynę i akcję zamiast
  sugerować awarię sieci. Nie wymaga już potwierdzenia logiem od Johna.

## "shoot failed" — sporadyczne błędy strzału niezdiagnozowane (log SRV nie objął zdarzenia)
- Status: proponowane (blokowane tylko na nowy materiał od Johna — log-truncation
  fix poniżej już wdrożony 2026-07-19)
- Typ: bug
- Zgłosił: John Melson
- Opis: w logu GUI Johna 4× wystąpił "ERROR: shoot failed" (test picture) mimo
  że kamera była połączona i większość strzałów w tej samej sesji się udawała.
  Nie udało się ustalić przyczyny z przesłanych logów — SRV.log obcinał się
  przy każdym starcie (patrz CLAUDE.md: "next to exe, truncated on start"), a
  plik dostarczony przez Johna nie obejmuje momentu żadnego z 4 błędów
  (widoczne w nim tylko ruch `list_cameras`/`status` z wątku statusu, zero
  `shoot` w tych oknach czasowych) — log z faktycznej awarii został nadpisany
  kolejnym restartem SRV. Wpis o obcinaniu logu poniżej **naprawiony
  2026-07-19** (append + separator sesji) — teraz potrzebny tylko nowy
  materiał od Johna po tej poprawce, żeby móc zdiagnozować sam "shoot failed".

## SRV.log obcina się przy każdym restarcie — niszczy dowody diagnostyczne
- Status: **naprawione i przeniesione do `CHANGELOG.md` → 2026-07-19**
- Typ: bug
- Zgłosił: John Melson (namierzone podczas analizy jego logów, nie zgłoszone
  wprost przez niego)
- Opis: `TotalControlSRV.log` był truncated on start (CLAUDE.md, sekcja
  Logging). Beta testerzy w praktyce restartują SRV wielokrotnie podczas
  rozwiązywania problemów z połączeniem — każdy restart kasował log z
  poprzedniej, potencjalnie właśnie nieudanej, próby. To wprost uniemożliwiło
  zdiagnozowanie zgłoszenia "shoot failed" powyżej. Naprawione drugą z
  proponowanych opcji: `main.cpp` otwiera log w trybie append zamiast trunc,
  z separatorem sesji `=== SRV start YYYY-MM-DD HH:MM:SS ===` (analogicznie
  do `TotalControlGUI.log`). BOM zapisywany tylko gdy plik jest faktycznie
  nowy/pusty (sprawdzone przez `GetFileAttributesExW` przed otwarciem) — nie
  duplikuje się przy każdym restarcie w środku pliku. Zweryfikowane:
  uruchomiono SRV dwa razy pod rząd, log zawiera oba banery startowe pod
  osobnymi separatorami zamiast nadpisania pierwszej sesji.
  **Uwaga**: to naprawia utratę dowodów, ale NIE naprawia samego zgłoszenia
  "shoot failed" — na to wciąż potrzebny nowy materiał od Johna po tej
  poprawce (patrz wpis wyżej).

## Wyjaśnić w UI, że lokalizację można wpisać też bezpośrednio jako współrzędne
- Status: **naprawione i przeniesione do `CHANGELOG.md` → 2026-07-19 (Alessandro
  Pessi)**
- Typ: feature
- Zgłosił: Alessandro Pessi
- Opis: Alessandro nie był pewien, jak działa pole na link Google Maps i pytał,
  czy da się wpisać współrzędne bezpośrednio. Pole DMS Latitude/Longitude do
  ręcznego wpisania już istnieje (sekcja LOCATION, lewa kolumna) — dodano
  `ImGui::SetItemTooltip()` na polu `##gmaps_url` wyjaśniający obie ścieżki
  (link Google Maps vs ręczne współrzędne powyżej).

## Pytanie: model czasu trwania Baily's Beads / Diamond Ring
- Status: proponowane
- Typ: pytanie
- Zgłosił: Alessandro Pessi
- Opis: Alessandro pyta, czy czas trwania efektów Baily's Beads / Diamond Ring
  jest oparty na stałym modelu, na profilu brzegu Księżyca (limb profile / DEM),
  czy na czymś innym. To wiąże się z memory "Phase 4 — 3D Simulator" (ray-tracing
  DiamondRing/Baily's, DEM Ziemi+Księżyca — zaplanowane post-2026, jeszcze
  niezaimplementowane). Wymaga odpowiedzi mailowej wyjaśniającej aktualny (brak)
  stan tej funkcji, nie zmiany w kodzie.

## Stress-test rozbieżności BE vs IQP przy krawędzi pasa całkowitego
- Status: proponowane
- Typ: demo
- Zgłosił: Alessandro Pessi
- Opis: Alessandro zasugerował lokalizację bliżej krawędzi pasa całkowitego
  zaćmienia zamiast blisko linii centralnej. To nie jest tylko kwestia
  prezentacji/YouTube — chodzi o stress-test dwóch silników obliczeniowych:
  różnice między modelem lokalnym (BE/Bessel) a IQP API są najwyraźniejsze
  właśnie przy skrajnych wartościach (blisko krawędzi pasa), bo IQP używa
  dokładniejszego modelu (uwzględnia więcej czynników niż uproszczone
  elementy Besselian) — rozbieżność w tym miejscu **nie jest błędem** żadnego
  z silników, tylko oczekiwaną konsekwencją różnej dokładności modeli. Warto
  jednak przetestować lokalizację blisko krawędzi jako case zwiększający
  widoczną rozbieżność BE/IQP — zarówno pod kątem demo, jak i sprawdzenia, czy
  UI/tabela kontaktów jasno komunikuje, że to spodziewana różnica modeli, a nie
  niespójność/błąd aplikacji.

## Nazwy kolumn w tabeli kontaktów niejasne (brak tooltipa/legendy)
- Status: **naprawione i przeniesione do `CHANGELOG.md` → 2026-07-19**
- Typ: feature (UI clarity)
- Zgłosił: Alexandru Barbovschi
- Opis: Alexandru nie rozumiał znaczenia nagłówków kolumn w tabeli kontaktów
  (IQP/BE/Loc/GE — zrzut ekranu załączony). Część jego zgłoszenia — brak
  precyzji sekund — naprawiona 2026-07-19 (razem z tożsamym zgłoszeniem
  Alessandra "Precyzja C1/C2 do dziesiątych sekundy"). Druga część — same
  nazwy kolumn nie były wyjaśnione w UI — naprawiona w ramach pełnego wdrożenia
  tooltipów (patrz "Tooltips (on-hover) dla wszystkich pól interfejsu" niżej):
  `##ct_tbl` (TIME) ma teraz manualny header row z `SetItemTooltip()` per
  kolumna (IQP/BE/Loc/T-) tłumaczącym źródło danych, plus tooltip per wiersz
  (C1/C2/Max/C3/C4/Rise/Set) wyjaśniający samo zjawisko.

## Rozwijana lista (dropdown) się otwiera, ale nie da się nic wybrać
- Status: proponowane (brak wystarczających danych do diagnozy)
- Typ: bug
- Zgłosił: Alexandru Barbovschi
- Opis: "Drop down opens, but I can't choose anything" — nie sprecyzował, który
  dropdown (kandydaci: combo 11 898 zaćmień, combo trybu bracket w Inspectorze,
  combo Track Mode Sun/Moon/Horizon). Do wyjaśnienia z Alexandru zanim da się
  to zdiagnozować/naprawić.

## Stopka obrazu GOES-19 SUVI rozprasza uwagę
- Status: **naprawione i przeniesione do `CHANGELOG.md` → 2026-07-19**
- Typ: feature
- Zgłosił: Alexandru Barbovschi
- Opis: obraz Fe171 pobierany z NOAA CDN ma widoczną stopkę (data/źródło
  wypalone w obrazie) nakładającą się na symulator. Zaimplementowane wg
  podjętej decyzji: w `decodeJpeg()` (`SuviThreadProc`, App.cpp) alpha
  ostatnich **20 wierszy** obrazu (dokładna wartość ustalona przez Andrzeja)
  jest teraz zerowane zaraz po standardowym `alpha = max(R,G,B)` — stopka
  staje się w pełni przezroczysta. Celowo **maskowanie przez alpha, nie
  fizyczny crop/resize** — obraz źródłowy zostaje 1200×1200, więc stałe
  kalibracyjne SUVI ALIGNMENT (`m_suviHalfQ` itd., skalibrowane względem
  pełnego kanwasu) pozostają ważne bez przeliczania. Podziękowanie/źródło w
  sekcji Acknowledgements modala About już istniało wcześniej ("GOES-19 SUVI
  Fe171 NOAA/NESDIS" + "NASA SDO i NOAA GOES-19" — nie wymagało zmian).
  Zweryfikowane buildem + zrzutem ekranu symulatora w przybliżeniu (widoczna
  czysta korona/protuberancje, bez artefaktów tekstu).

## Kolory symulatora słabo czytelne w jasnym świetle dziennym
- Status: proponowane
- Typ: bug (UX/accessibility)
- Zgłosił: Alexandru Barbovschi
- Opis: obecna paleta (ciemny motyw + subtelne odcienie korony/UI) jest słabo
  czytelna pod silnym światłem słonecznym — realny scenariusz użycia w polu
  podczas obserwacji zaćmienia (operator patrzy na ekran w pełnym słońcu, nie
  w zaciemnionym pomieszczeniu). Wymaga przeglądu kontrastu kluczowych
  elementów (nie tylko subiektywnej zmiany "ładniejszych" kolorów) — możliwe
  rozwiązanie: alternatywny "jasny/wysoki kontrast" motyw albo wzmocnienie
  kontrastu istniejących elementów.

## CMakeLists.txt i komentarze w kodzie — polskie napisy zamiast angielskich
- Status: proponowane
- Typ: feature (code quality / globalny przegląd)
- Zgłosił: Alexandru Barbovschi
- Opis: `CMakeLists.txt` zawiera polskie komunikaty/komentarze (np. `COMMENT
  "Copy ... -> ..."` w POST_BUILD są już po angielsku, ale część innych
  komentarzy w CMake i w kodzie C++ jest po polsku). Potrzebny globalny
  przegląd repo i tłumaczenie wszystkich polskich zapisów (komentarze, stringi
  diagnostyczne, komunikaty CMake) na angielski — projekt ma być czytelny dla
  zagranicznych beta testerów/kontrybutorów. Duży zakres — zrobić jako osobny
  przegląd, nie przy okazji innej zmiany.

## Rozbieżność czasów kontaktów TotalControl vs SEM/SEW — hipoteza deltaT
- Status: proponowane (WYSOKI priorytet — dokładność czasów kontaktów to
  funkcja krytyczna; hipoteza źródłowa częściowo obalona, patrz niżej)
- Typ: bug
- Zgłosił: Alexandru Barbovschi
- Opis: Alexandru zauważył rozbieżność ~6-7s między czasami kontaktów w
  TotalControl a Solar Eclipse Maestro / Solar Eclipse Timer (SEM/SEW),
  z hipotezą że przyczyną jest pominięcie/błędne ΔT — zasugerował sprawdzenie,
  że ΔT = 69.11s.
  **Zweryfikowane w kodzie/danych (2026-07-19)**: `dt` NIE jest pominięte — jest
  czytane per-zaćmienie z `eclipse_besselian.dt` (`Database::LoadEclipses`,
  `Database::LoadBesselianElements`) i używane w `BesselCalc.cpp` zarówno przy
  liczeniu kąta godzinnego (`o.H = mu + lonDeg − 0.00417807×e.dt`) jak i UT
  (`ut_h = t0 + tau − e.dt/3600`). Wartość w bazie dla 2026-08-12 to **75.4 s**,
  nie 69.11 s. Policzony niezależnie oficjalny wzór długoterminowy Espenaka
  (NASA, ΔT = 62.92 + 0.32217t + 0.005589t², t = rok−2000, dla połowy sierpnia
  2026) daje **≈75.46 s** — niemal idealna zgodność z wartością w bazie.
  **Wniosek**: hipoteza "brakującego ΔT = 69.11s" wygląda na nieprawdziwą —
  75.4s w bazie jest zgodne z kanonicznym źródłem NASA, więc realna przyczyna
  różnicy 6-7s vs SEM/SEW leży najprawdopodobniej gdzie indziej: SEM/SEW mogą
  używać starszej/innej wartości ΔT (te narzędzia bywają rzadziej aktualizowane
  niż baza NASA), albo różnica jest w innym miejscu formuł/parametrach
  Besselian. Do zrobienia: porównać wprost wartość ΔT używaną przez
  zainstalowaną wersję SEM/SEW Alexandru z 75.4s, zamiast zakładać który
  program ma rację.

## Jawny wybór modelu obliczeń — IQP vs Besselian Elements
- Status: **naprawione i przeniesione do `CHANGELOG.md` → 2026-07-19**
- Typ: feature
- Zgłosił: Alexandru Barbovschi (kontekst: powyższa rozbieżność SEM/SEW)
- Opis: zamiast (albo obok) biernego pokazywania obu kolumn IQP i BE w tabeli
  kontaktów, dać użytkownikowi jawny wybór/przełącznik, który model ma być
  traktowany jako "główny" (np. do generowania bloków Timeline / countdown
  T-). Powiązane z wpisem Alessandra o stress-teście BE vs IQP przy krawędzi
  pasa — obie sugestie wskazują na potrzebę jaśniejszej komunikacji w UI,
  że to dwa niezależne silniki, nie jeden "prawdziwy" wynik.
  Zaimplementowane: nowy `App::PrimaryContacts()` (App.cpp) — jedno miejsce
  decydujące, który silnik jest "główny" (`m_primaryContactSrc`, 0=IQP/
  1=BE, persystowane jako `primary_contact_src` w Config.db), z fallbackiem
  na drugi silnik gdy wybrany jest niedostępny. Podłączone do WSZYSTKICH
  miejsc generujących bloki/timing z kontaktów: `SnapMsToRelativeSecond`,
  `LoadAudioPreset`, `AddPhotoPreset` (One Picture Per Minute),
  `AddTotalityBracketPreset`, `RenderTimelineBottom` (zakres/markery
  Timeline) oraz kolumna T- w `##ct_tbl`. Celowo NIE podłączone do bannera
  strefy totalności (`RenderLocationSection`) — ten sprawdza specyficznie
  stan API IQP (klucz ustawiony czy nie), to inna sprawa niż "który silnik
  jest główny". Kolumny IQP/BE w tabeli kontaktów nadal pokazują oba wyniki
  jawnie obok siebie, niezależnie od wyboru (bez zmian). UI: para
  `RadioButton` "Primary: (•) IQP ( ) BE" nad `##ct_tbl` w sekcji TIME, z
  tooltipami tłumaczącymi co dokładnie wybór zmienia. Zweryfikowane zrzutem
  ekranu + bezpośrednio: przełączenie na BE widocznie przeliczyło znaczniki
  Timeline (np. "C2-1247s" → "C2-1241s").
  **Bug znaleziony przez Andrzeja od razu po wdrożeniu**: kolumna Loc
  (`FmtLoc`) była zaszyta na sztywno na `r.iqp` — nie reagowała na wybór
  Primary w ogóle, mimo że reprezentuje te same dane co T-, tylko w innej
  strefie czasowej. Naprawione: `FmtLoc(r.cd > 0 ? r.cd : r.iqp, ...)` —
  `r.cd` to już wartość silnika Primary (ten sam mechanizm co T-); `r.iqp`
  jako fallback tylko dla wierszy Rise/Set (nie mają silnika, `r.cd == -1`).
  Zweryfikowane zrzutami ekranu w obie strony: Primary=BE → Loc = BE + offset
  strefy; Primary=IQP → Loc = IQP + offset strefy.

## Zabezpieczenie przed przypadkowym zamknięciem GUI/SRV
- Status: **naprawione i przeniesione do `CHANGELOG.md` → 2026-07-19**
- Typ: feature
- Zgłosił: Alexandru Barbovschi
- Opis: przypadkowe zamknięcie TotalControlGUI lub TotalControlSRV podczas
  zaćmienia byłoby katastrofą (utrata sekwencji w trakcie totality).
  **GUI**: X/Alt+F4/File→Quit teraz przechodzą przez `App::OnCloseRequest()`
  (hak już istniał w `main_gui.cpp` WndProc, nigdy niepodłączony pod realną
  logikę — zwracał `true` bezwarunkowo) → modal `RenderCloseConfirmModal()`:
  "Restarting the GUI takes about 5 seconds." + (jeśli SRV podłączony)
  "The camera server keeps running -- it is NOT closed by this." Potwierdzenie
  re-wysyła `WM_CLOSE` (`RequestRealWindowClose()` w `main_gui.cpp`), więc
  `OnCloseRequest()` przepuszcza za drugim razem. Uwaga: File→Quit wcześniej
  wywoływał `PostQuitMessage(0)` wprost, całkiem z pominięciem potwierdzenia —
  też naprawione. **SRV**: `CtrlHandler` (main.cpp) pokazuje `MessageBoxW`
  z "Reconnecting to the cameras after this can take up to 60 seconds." dla
  CTRL_C_EVENT/CTRL_CLOSE_EVENT (bezpieczne — te sygnały dostają własny wątek
  od OS, blokujący dialog jest tu udokumentowaną, poprawną techniką); domyślny
  przycisk to "No" (`MB_DEFBUTTON2`). CTRL_LOGOFF_EVENT/CTRL_SHUTDOWN_EVENT
  **celowo pominięte** — to system każący procesowi zejść (wylogowanie/
  shutdown), blokujący dialog byłby tam nie na miejscu. Zweryfikowane
  automatyzacją: Cancel zostawia GUI uruchomione, potwierdzenie faktycznie
  kończy proces.
  **Poprawka odkryta przez Andrzeja tuż po wdrożeniu**: `App::~App()`
  (App.cpp) wysyłał `{"cmd":"quit"}` do SRV bezwarunkowo przy KAŻDYM
  zamknięciu GUI (nie tylko przez przycisk "Disconnect") — czyli samo
  potwierdzenie zamknięcia GUI i tak zabijało SRV razem z połączeniem do
  aparatów (~60s na ponowne połączenie), dokładnie to, czego miał chronić ten
  wpis, i wprost zaprzeczające tekstowi w nowym modalu ("The camera server
  keeps running"). Zachowanie było regresją względem wcześniejszego
  doświadczenia Andrzeja (zawieszenie GUI → restart samego GUI → SRV/aparaty
  zostawały podłączone). Naprawione: destruktor GUI już nie wysyła `quit`,
  tylko rozłącza własny pipe (`m_pipe.Disconnect()`) — SRV żyje dalej,
  obsługując innych klientów. Jedyne miejsce wysyłające `quit` to teraz
  wyłącznie przycisk "Disconnect" (świadoma akcja operatora). Zweryfikowane
  logami: SRV kontynuował aktywność (status kamery, ostrzeżenia) długo po
  `=== TotalControlGUI exit ===` w logu GUI, bez żadnego `{"cmd":"quit"}`
  między nimi.

## "TotalControlCLI quit" nie zamyka faktycznie procesu SRV
- Status: **naprawione i przeniesione do `CHANGELOG.md` → 2026-07-19**
- Typ: bug
- Zgłosił: Andrzej Nowak (znalezione przy okazji sesji 2026-07-19, podczas
  próby zbudowania projektu przy uruchomionym SRV)
- Opis: `tc quit` wysyła `{"cmd":"quit"}`, SRV odpowiada `{"ok":true}`, ale
  sam proces `TotalControlSRV.exe` nie kończył działania, dopóki inny klient
  (np. GUI z trwałym połączeniem) był podłączony. Przyczyna: w
  `PipeServer::ServeClient()` (`PipeServer.cpp`) `false` zwrócone z handlera
  (sygnał "quit") kończyło tylko pętlę obsługi TEGO JEDNEGO klienta, nigdy nie
  wołając `server.Stop()`. Naprawa: `ServeClient()` teraz jawnie woła
  `Stop()` przed przerwaniem swojej pętli, gdy handler zwróci `false` — to
  ustawia `m_running=false` i budzi `Run()`'s `WaitForMultipleObjects` przez
  `m_stopEvent`, więc główna pętla accept kończy się i `main()` przechodzi do
  pełnego sprzątania (`return 0`) niezależnie od tego, ilu innych klientów
  było podłączonych. Zweryfikowane automatyzacją: uruchomiono SRV, otworzono
  drugie, trwałe połączenie pipe symulujące GUI (`{"cmd":"status"}` +
  30s idle), następnie `tc quit` z osobnego procesu — proces SRV faktycznie
  zniknął z listy procesów mimo aktywnego drugiego klienta.
## Tooltips (on-hover) dla wszystkich pól interfejsu
- Status: **naprawione i przeniesione do `CHANGELOG.md` → 2026-07-19**
- Typ: feature
- Zgłosił: Andrzej Nowak (projekt)
- Opis: dodano `ImGui::SetItemTooltip()`/`IsItemHovered()+SetTooltip()` na
  praktycznie wszystkich eksponowanych polach GUI (Andrzej zaakceptował pełny
  zakres z `docs/tooltips_review.md`, w tym pola tylko-do-odczytu — pytanie 1
  z tego dokumentu rozstrzygnięte na "tak, wszystko"): ECLIPSE (combo
  zaćmienia, Type/Duration/GE location, tabela GE contact times),
  LOCATION (Latitude/Longitude/Altitude, pole URL Google Maps, przycisk SET
  LOCATION & CALCULATE, banner strefy totalności), TIME (nagłówki i wiersze
  `##ct_tbl` — patrz wpis o nazwach kolumn wyżej), SERVER (Connect/
  Connecting/Test picture/Disconnect, cała tabela CAMERA STATUS),
  BLOCK INSPECTOR (Type/Start UTC/SS/ISO/f-number/Bracket/Drive speed/Burst
  duration/File/Duration/Label/Snap to previous/Snap to Seconds), EXECUTE
  (TEST RUN/RESUME/STOP TEST/RUN/STOP RUN/SAVE CALIB), Solar Simulator status
  bar (P/q/rot/Alt/Obs/UTC/Home/Loc/PlayHead), zoom-hint i etykiety osi N☉/N☾
  (draw-list, hit-test ręczny na pozycję myszy — oba przypadki wymienione w
  pytaniu 2 dokumentu), SUVI ALIGNMENT (opacity/channel/Scale/V-offset/
  H-offset), LIVE VIEW (per-kamera opacity slider), ACTION LIBRARY (Single/
  Burst/Bracket/Audio), CAMERA CONFIG (Model/GUID, Focal Len, Apply P, Track
  Sun/Moon/Horizon), OPTIONS (klucz IQP API, Show/Hide, Apply). Przyciski
  wyłączone (BeginDisabled) używają jawnego `IsItemHovered(AllowWhenDisabled)`
  zamiast samego `SetItemTooltip()`, żeby tooltip nadal się pokazywał, gdy np.
  "Connecting..." albo "STOP RUN" jest nieaktywne. Pytanie 3 z dokumentu
  (tooltipy na komunikatach błędów) pominięte — poza zakresem tej rundy.
  Nieobjęte tym wdrożeniem: bloki/tory/ruler na samym Timeline (osobny,
  bardzo duży obszar rysowany na `ImDrawList`, nieujęty w
  `docs/tooltips_review.md` w ogóle) — do rozważenia jako osobny wpis, jeśli
  potrzebny.

## Moduł optymalizacji ekspozycji/bracketingu (Q-based) — nowe okno/przełącznik obok Solar Simulator
- Status: proponowane (duży zakres — do rozbicia na etapy przed startem)
- Typ: feature
- Zgłosił: Andrzej Nowak (projekt)
- Opis: nowe okno GUI albo przełącznik w Col2 obok "SKY VIEW SIMULATOR"
  ("SIMULATOR" ↔ "EXPOSURE OPTIMIZER"), liczący optymalny bracketing i czasy
  naświetlania per faza zaćmienia na podstawie wartości Q (NASA/Espenak) i
  modelu szumu Hasinoffa.

  **Zasada nadrzędna dla całego wdrożenia**: `eclipse_photo_optimization_
  context.md` jest znacznie starszy od obecnego stanu kodu — w momencie jego
  powstania wiele wartości było niewiadomych i celowo zgadywanych (sam
  dokument to przyznaje w sekcji 13 "Otwarte pytania/TODO"). Tam, gdzie
  udokumentowana/zmierzona wiedza z samego TotalControl (CLAUDE.md, kalibracje
  w `calibration/*.csv`, kod) jest dostępna i zaprzecza założeniu z tego
  dokumentu, **wygrywa kod/pomiar, nie dokument planistyczny**. Przykład niżej
  (overhead 0.15s vs zmierzone 4100-5500ms) nie jest wyjątkiem — to wzorzec do
  stosowania wszędzie w tym dokumencie: traktować go jako punkt startowy do
  zweryfikowania, nie jako gotowe dane wejściowe do wdrożenia wprost.

  Materiały źródłowe:
  1. `external/Q/Solar Eclipse Time Exposure Calculator - Xavier Jubier.htm` —
     lokalna kopia kalkulatora Xaviera Jubiera (v1.0.2), referencyjnego
     narzędzia w społeczności fotografów zaćmień (strona źródłowa jest tylko
     pod `http://xjubier.free.fr/...` — nie serwuje HTTPS — a narzędzie do
     pobierania stron w tej sesji wymusza upgrade http→https, stąd
     `ECONNREFUSED` na porcie 443 i konieczność kopii lokalnej od Andrzeja).
     Przeanalizowana:
     - To kalkulator **pojedynczego ujęcia**, nie sekwencji/bracketingu: user
       wybiera 1 zjawisko (radio button), ISO, f/number, wysokość Słońca,
       wysokość obserwatora n.p.m. → dostaje 2 sugerowane czasy (bez i z
       ekstynkcją atmosferyczną) + Q + EV. Instrukcja na stronie wprost mówi
       "bracket ±1EV lub więcej" — bracketing zostawiony użytkownikowi
       ręcznie, **nie ma optymalizacji sekwencji/ISO jak u Hasinoffa ani jak
       w naszym modelu**.
     - Zawiera bazę konkretnych ciał (Nikon/Canon/Sony/Pentax/Fuji/Panasonic/
       Olympus/Sigma, dziesiątki modeli z realnym pixel pitch) do policzenia
       pixel scale / FOV / **"Exposure Limit (without tracking)"** — limit
       czasu naświetlania zanim ruch Ziemi rozmyje gwiazdy/koronę bez trackera.
       **Tego nie mamy w TotalControl** — potencjalnie warto dodać jako
       dodatkowy alert (np. przy earthshine ~4s na 900mm bez montażu
       paralaktycznego pole może już zacząć się rozmywać).
     - Wartości Q/EV zaszyte w kodzie strony (radio "value") **nie pokrywają
       się 1:1 z naszą tabelą NASA Table 40** z `eclipse_photo_optimization_
       context.md` — np. jego "Chromosphere"=2048 (2^11, zgadza się z Q=11),
       ale "Prominences"=1024 (2^10, u nas Q=9→512) i "Diamond Rings"=80
       (~2^6.3, u nas Q=8→256) — rozbieżność do wyjaśnienia/zdecydowania,
       której tabeli Q ufać per zjawisko, zamiast zakładać że są identyczne.
     - Wniosek: nasz planowany moduł to **jakościowo więcej** niż kalkulator
       Jubiera (on daje 1 liczbę + rada "bracketuj ręcznie"; my chcemy
       policzyć całą optymalną sekwencję z ISO per zjawisko i budżetem czasu
       jak Hasinoff) — Jubier jest dobrym punktem odniesienia/sanity-check
       dla pojedynczych wartości Q, ale nie architekturą do skopiowania.
  2. `external/Q/Noise-Optimal_Capture.pdf` — Hasinoff, Durand, Freeman (CVPR
     2010), "Noise-Optimal Capture for HDR Photography": pokazuje, że
     optymalny bracketing dla worst-case SNR w danym budżecie czasu **nie**
     jest geometrycznym stopniowaniem przy stałym niskim ISO — używa
     zmiennego, często wysokiego ISO (bo szum ADC/readout maleje z ISO,
     tzw. "high-ISO potential", do ~19dB dla niektórych aparatów), i
     formułuje dobór (czas ekspozycji, ISO, liczba klatek) jako mixed-integer
     programming (Theorem 1/2, str. 4-5) maksymalizujące worst-case SNR przy
     limicie czasu (lub minimalizujące czas przy zadanym SNR).
  3. `external/Q/eclipse_photo_optimization_context.md` — gotowy, w pełni
     wypracowany model dla konkretnego setupu Andrzeja (TSE 2026, Burgos/Lerma,
     Bresser AR90 900mm f/10 + Sony SEL24-240 @240mm f/6.3, Sony A7R IVA):
     - tabela Q-values (NASA Table 40) per zjawisko (koraliki Bailey'ego,
       chromosfera, protuberancje, pierścień diamentowy, korona 0.1-8 R☉,
       earthshine)
     - wzór ekspozycji `t = f²/(ISO×2^(Q-ΔEV))`, ekstynkcja atmosferyczna
       `ΔEV = k×airmass(h)`
     - model SSNR po stackowaniu (`SSNR_dB = SNR1_dB + 10·log10(N_BRK×M)`),
       SNR1 per ISO z DxOMark dla A7R IV, i **optymalne ISO per zjawisko**
       wynikające z modelu Hasinoffa (nie zawsze ISO100! np. Q≤-4 → ISO
       1600-3200)
     - budżety czasowe per faza (C2 kontakt 5s, korona ~90s, earthshine,
       C3 kontakt 5s) z alertami przekroczenia i limitami migawki
       (mechanicznej 1/8000s, elektronicznej 1/32000s)
     - gotowe funkcje C++ (`calc_t`, `seq_time`, `ssnr_db`, `phase_M`,
       `optimal_iso`, `airmass`, `extinction`) — do przeportowania niemal
       wprost, matematyka nie zależy od SDK/UI
     - szkic formatu JSON (setups + phases) zbliżony do istniejącego formatu
       sekwencji `sequences/*.json`
     - własna lista otwartych TODO (m.in. "zweryfikować overhead CrSDK
       empirycznie — założono 150ms")

  **Kluczowe zastrzeżenie przed implementacją**: dokument #3 zakłada overhead
  `OV=0.15s` jako *nieznaną, niezweryfikowaną* wartość ("CrSDK estimate") i
  sam to flaguje jako TODO do zmierzenia. TotalControl **już to zmierzył** —
  patrz CLAUDE.md, sekcja "Change log 2026-07-12" i `calibration/
  bracket_calibration_10x.csv` + `calibration/arm_sweep_final.csv`: rzeczywisty
  czas serii bracketingu liczy `BracketSumMultiplier(count,ev)` (nie prosty
  `N×OV`), a czyszczenie bufora między seriami (ARM) to **4100-5500ms**, nie
  150ms. Naiwne przeportowanie wzorów z dokumentu #3 z placeholderem OV=0.15s
  dałoby drastycznie zawyżone `M` (liczbę pętli mieszczących się w budżecie
  fazy) i fałszywe poczucie bezpieczeństwa co do liczby klatek w 90s fazy
  korony. Optymalizator MUSI czytać rzeczywiste dane z `bracket_calib`
  (Config.db)/`m_calibCache`, nie stałą OV z dokumentu.

  Synergia z istniejącym kodem: wysokość Słońca h° per-moment już liczy Solar
  Simulator (`sunEph.alt_deg`, patrz `docs/tooltips_review.md` → pole "Alt"),
  czasy kontaktów C1-C4 już liczy IQP/BE (`ContactTimes`), a generowanie
  bloków Bracket na Timeline ma już wzorzec w `AddAllBracketVariantsPreset`-
  stylu presetach (App.cpp) — optymalizator mógłby **automatycznie
  wygenerować** tor Timeline z blokami zamiast wymagać ręcznego wpisywania.

  Proponowane etapy (duży zakres, nie robić na raz):
  1. Przeportować czystą matematykę (`calc_t`/`seq_time`/`ssnr_db`/
     `optimal_iso`/`airmass`/`extinction`) do nowego pliku, bez zależności od
     SDK/UI — łatwe do zweryfikowania niezależnie.
  2. Podłączyć realny overhead z `bracket_calib`/`m_calibCache` zamiast
     placeholdera OV z dokumentu.
  3. Zaprojektować UI (nowe okno albo przełącznik w Col2) — per-setup
     (tor kamery, już mamy `TLTrack::focalMm`), per-faza budżet czasu
     (auto z C1-C4), wybór zjawisk do pokrycia.
  4. Generowanie bloków Bracket na Timeline z wyniku optymalizacji.
  5. Rozstrzygnąć rozbieżność wartości Q między naszą tabelą (NASA Table 40,
     `eclipse_photo_optimization_context.md`) a wartościami zaszytymi w
     kalkulatorze Jubiera (patrz wyżej — Prominences/Diamond Rings różnią się)
     — zdecydować, której tabeli ufać per zjawisko, zamiast zakładać zgodność.
  6. Rozważyć dodanie alertu "Exposure Limit (without tracking)" (limit czasu
     naświetlania bez rozmycia polem gwiazd/korony przy braku trackera) —
     funkcja obecna u Jubiera, u nas jeszcze nieobsłużona.

## Wykres wartości Q na osi czasu Timeline (cienka linia w wierszu UTC)
- Status: proponowane (nie robić od razu — patrz decyzja Andrzeja niżej)
- Typ: feature
- Zgłosił: Andrzej Nowak (projekt)
- Opis: dodać do Timeline (`RenderTimelineBottom()`, App.cpp) wykres wartości Q
  — jasnoszara, cienka (1px) linia — narysowany w wierszu UTC ruler (`rulerY`
  .. `rulerY + kRulerH`), razem z tickami/etykietami czasu UTC. Cel: pozwolić
  operatorowi oszacować "na oko" jasność sceny w dowolnym momencie zaćmienia,
  patrząc na sam Timeline, bez przełączania się do osobnego okna.

  **Decyzja co do modelu (2026-07-19)**: podczas doprecyzowania okazało się,
  że tabela Q z NASA Table 40 (patrz `docs/solar_eclipse_exposure_model.md`
  i `external/Q/eclipse_photo_optimization_context.md`) NIE jest funkcją
  czasu — dla totality Q zależy od promienia korony (widocznego jednocześnie
  w całej totality), a dla fazy częściowej NASA podaje tylko dwie stałe
  wartości z filtrem ND (Q=8 przy ND 5.0, Q=11 przy ND 4.0), nie krzywą
  zależną od stopnia zasłonięcia tarczy. Andrzej zdecydował: **wykres ma być
  ciągłą krzywą policzoną dla całego zaćmienia od C1 do C4** (nie schodkową
  wersją opartą na istniejących strefach paska faz) — czyli trzeba
  wyprowadzić/dobrać model Q(t) ciągły w obu fazach częściowych (na bazie
  magnitude zaćmienia / ułamka zasłoniętej tarczy z elementów Besselian,
  już liczonego przez `BesselCalc`/kontakty C1-C4) i sensowne przejście w
  wartość(i) Q dla totality. **W procesie implementacji porównać wyliczone
  wartości z dostępnymi badaniami empirycznymi NASA** (nie tylko z tabelą
  Q, ale z realnymi pomiarami jasności nieba/korony podczas wcześniejszych
  zaćmień) — nie przyjmować własnego wzoru bez takiej weryfikacji.

  **Nie zaczynać implementacji teraz** — Andrzej wstrzymał to do osobnej sesji
  (zbyt duża niepewność modelu, żeby robić to przy okazji). Powiązane z
  "Moduł optymalizacji ekspozycji/bracketingu" wyżej — ten wykres to
  prawdopodobnie naturalny punkt wejścia/wizualizacja dla tamtego modułu,
  ale nie wymaga czekania na całą jego implementację.
