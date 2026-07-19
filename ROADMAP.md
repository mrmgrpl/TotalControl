# Roadmap

Pomysły i feature requesty zgłoszone przez beta testerów (korespondencja e-mail),
jeszcze niezaimplementowane. Po wdrożeniu wpis przenosi się do `WHATS_NEW.md`
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
- Status: proponowane
- Typ: feature
- Zgłosił: John Melson
- Opis: podgląd/narzędzie do oceny ostrości (focus) na żywo — do doprecyzowania
  zakresu z Johnem (np. czy to powiększony wycinek z Live View z wyostrzonymi
  krawędziami, czy osobny peaking overlay).

## Weryfikacja przypisania kamer (sortowanie po GUID, dynamiczne tory) na 2+ kamerach
- Status: zaplanowane na 2026-07-21 (wtorek) — Andrzej wejdzie w posiadanie
  dodatkowych kamer testowych
- Typ: bug (weryfikacja fixa)
- Zgłosił: John Melson (pierwotne zgłoszenie — Timeline pokazywał kamerę pod
  cudzą, zaszytą na sztywno nazwą modelu; **naprawione i przeniesione do
  `WHATS_NEW.md` → 2026-07-19**)
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
- Status: proponowane
- Typ: feature
- Zgłosił: John Melson
- Opis: John nie pamiętał, jak ustawić aparat przed połączeniem z aplikacją.
  U niego zadziałało ustawienie w menu aparatu "Remote Shoot/Trn" + Set —
  ale to jego domysł, że ustawienie różni się między modelami, nie
  potwierdzony fakt. Realny problem to brak jakiejkolwiek instrukcji dla
  operatora w aplikacji — kwestia edukacyjna, nie logiki zależnej od modelu.
  Potrzebna krótka instrukcja krok-po-kroku (np. tooltip/"?" przy przycisku
  "Connect cameras" albo osobna sekcja w About/Help) opisująca ogólne
  przygotowanie aparatu do podłączenia, bez rozgałęziania per model dopóki nie
  potwierdzimy, że to faktycznie konieczne. Ustawienia aparatu do wymienienia
  w instrukcji:
  - Tryb ekspozycji: **M** (Manual)
  - Jakość zapisu: **RAW**
  - Ostrość: **MF** (Manual Focus)
  - Połączenie USB: **Camera Control by PC (USB)**

## Reset stanu połączenia do "świeżo uruchomiona aplikacja" bez restartu
- Status: proponowane
- Typ: feature
- Zgłosił: John Melson
- Opis: John próbował kilku typów połączenia zanim trafił na właściwy i chciał
  zresetować bieżący stan/próby połączenia do stanu początkowego bez Quit +
  ponownego uruchamiania aplikacji. Prośba: pozycja w menu, która czyści
  bieżący stan połączenia (disconnect + reset) bez zamykania SRV/GUI.

## Network error przy geokodowaniu linku Google Maps (lokalizacja)
- Status: proponowane
- Typ: bug
- Zgłosił: John Melson
- Opis: John wkleił link Google Maps do Mallorki — aplikacja poprawnie
  rozpoznała kraj (Hiszpania), ale wystąpił "network error" (zrzut ekranu
  załączony w mailu, nieprzeniesiony do repo). Do zbadania: `GeocodeClient`/
  `ElevationClient` (WinHTTP) — możliwe że to sieć na wycieczce (AstroTrails,
  Luksor) blokowała żądanie, ale wymaga potwierdzenia logiem/screenshotem od
  Johna zanim uznamy to za defekt aplikacji. Potwierdzone z kodu: wysokość po
  wklejeniu linku Google Maps pochodzi z Open-Elevation API (`ElevationClient.cpp`,
  WinHTTP GET) — to prawdopodobne miejsce awarii; brak logowania błędu do pliku
  w obu logach Johna (patrz wpis "GeocodeClient/ElevationClient nie logują błędów
  do pliku" niżej) uniemożliwił potwierdzenie. Patrz też pytanie Alessandro Pessi
  o źródło wysokości — ten wpis jest odpowiedzią.

## "shoot failed" — sporadyczne błędy strzału niezdiagnozowane (log SRV nie objął zdarzenia)
- Status: proponowane (brak wystarczających danych do diagnozy)
- Typ: bug
- Zgłosił: John Melson
- Opis: w logu GUI Johna 4× wystąpił "ERROR: shoot failed" (test picture) mimo
  że kamera była połączona i większość strzałów w tej samej sesji się udawała.
  Nie udało się ustalić przyczyny z przesłanych logów — SRV.log obcina się przy
  każdym starcie (patrz CLAUDE.md: "next to exe, truncated on start"), a plik
  dostarczony przez Johna nie obejmuje momentu żadnego z 4 błędów (widoczne w
  nim tylko ruch `list_cameras`/`status` z wątku statusu, zero `shoot` w tych
  oknach czasowych) — log z faktycznej awarii został nadpisany kolejnym
  restartem SRV. Do zrobienia dopiero po naprawieniu wpisu o obcinaniu logu
  poniżej i zebraniu nowego materiału od Johna.

## SRV.log obcina się przy każdym restarcie — niszczy dowody diagnostyczne
- Status: proponowane
- Typ: bug
- Zgłosił: John Melson (namierzone podczas analizy jego logów, nie zgłoszone
  wprost przez niego)
- Opis: `TotalControlSRV.log` jest truncated on start (CLAUDE.md, sekcja
  Logging). Beta testerzy w praktyce restartują SRV wielokrotnie podczas
  rozwiązywania problemów z połączeniem (patrz wpis "Reset stanu połączenia"
  wyżej) — każdy restart kasuje log z poprzedniej, potencjalnie właśnie
  nieudanej, próby. To wprost uniemożliwiło zdiagnozowanie zgłoszenia
  "shoot failed" powyżej. Propozycja: przed obcięciem przy starcie skopiować
  poprzedni `TotalControlSRV.log` do `TotalControlSRV.log.1` (rotacja jednego
  poziomu wstecz), albo przejść na append + wyraźny separator sesji
  (`=== SRV start <timestamp> ===`, analogicznie do GUI.log).

## Precyzja C1/C2 do dziesiątych sekundy w tabeli kontaktów
- Status: proponowane
- Typ: feature
- Zgłosił: Alessandro Pessi
- Opis: Alessandro (po obejrzeniu prezentacji na YouTube) zasugerował pokazywanie
  dziesiątych sekundy dla C1/C2 — istotne przy planowaniu fotograficznym, gdzie
  liczy się precyzja timingu. Dane już mają precyzję milisekundową
  (`ContactTimes.c1Ms` itd., int64_t ms), tylko formatowanie w tabeli kontaktów
  (`App::RenderContactTimesSection()`, lambdy `fmtUtc`/`fmtLoc`, App.cpp ~5390)
  obcina do pełnych sekund (`%02d:%02d:%02d`). Do zrobienia: rozszerzyć format
  o `.d` (dziesiąte) przynajmniej dla C1/C2, ewentualnie dla wszystkich zdarzeń
  w tabeli.

## Wyjaśnić w UI, że lokalizację można wpisać też bezpośrednio jako współrzędne
- Status: proponowane
- Typ: feature
- Zgłosił: Alessandro Pessi
- Opis: Alessandro nie był pewien, jak działa pole na link Google Maps i pytał,
  czy da się wpisać współrzędne bezpośrednio. Pole DMS Latitude/Longitude do
  ręcznego wpisania już istnieje (sekcja LOCATION, lewa kolumna) — to nie jest
  brakująca funkcja, tylko brak jasności w UI/prezentacji, że obie ścieżki
  (wklej link Google Maps vs wpisz współrzędne ręcznie) są dostępne równolegle.
  Do rozważenia: krótki podpis/tooltip przy polu na link ("albo wpisz
  współrzędne bezpośrednio poniżej ↓").

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

## Nazwy kolumn w tabeli kontaktów niejasne + brak precyzji sekund
- Status: proponowane
- Typ: feature (UI clarity)
- Zgłosił: Alexandru Barbovschi
- Opis: Alexandru nie rozumiał znaczenia nagłówków kolumn w tabeli kontaktów
  (IQP/BE/Loc/GE — zrzut ekranu załączony) i pytał, dlaczego czasy nie pokazują
  sekund. Częściowo pokrywa się z istniejącym wpisem Alessandra "Precyzja C1/C2
  do dziesiątych sekundy" (sekundy już są w danych, tylko obcięte w formacie),
  ale to nowy, odrębny problem: same nazwy kolumn (IQP/BE/Loc/GE) nie są
  wyjaśnione w UI (brak tooltipa/legendy) — użytkownik nie wie, co dana kolumna
  reprezentuje bez czytania CLAUDE.md.

## Rozwijana lista (dropdown) się otwiera, ale nie da się nic wybrać
- Status: proponowane (brak wystarczających danych do diagnozy)
- Typ: bug
- Zgłosił: Alexandru Barbovschi
- Opis: "Drop down opens, but I can't choose anything" — nie sprecyzował, który
  dropdown (kandydaci: combo 11 898 zaćmień, combo trybu bracket w Inspectorze,
  combo Track Mode Sun/Moon/Horizon). Do wyjaśnienia z Alexandru zanim da się
  to zdiagnozować/naprawić.

## Stopka obrazu GOES-19 SUVI rozprasza uwagę
- Status: proponowane (decyzja podjęta, do wdrożenia)
- Typ: feature
- Zgłosił: Alexandru Barbovschi
- Opis: obraz Fe171 pobierany z NOAA CDN ma widoczną stopkę (data/źródło
  wypalone w obrazie) nakładającą się na symulator. Decyzja: przyciąć stopkę
  z pobieranego obrazu (crop) i przenieść podziękowanie/źródło do sekcji
  Acknowledgements w About modal (już istnieje NOAA GOES-19 SUVI w tej sekcji —
  patrz CLAUDE.md "About modal").

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
- Status: proponowane
- Typ: feature
- Zgłosił: Alexandru Barbovschi (kontekst: powyższa rozbieżność SEM/SEW)
- Opis: zamiast (albo obok) biernego pokazywania obu kolumn IQP i BE w tabeli
  kontaktów, dać użytkownikowi jawny wybór/przełącznik, który model ma być
  traktowany jako "główny" (np. do generowania bloków Timeline / countdown
  T-). Powiązane z wpisem Alessandra o stress-teście BE vs IQP przy krawędzi
  pasa — obie sugestie wskazują na potrzebę jaśniejszej komunikacji w UI,
  że to dwa niezależne silniki, nie jeden "prawdziwy" wynik.

## Zabezpieczenie przed przypadkowym zamknięciem GUI/SRV
- Status: proponowane (WYSOKI priorytet — ryzyko utraty połączenia z kamerą
  w dniu zaćmienia)
- Typ: feature
- Zgłosił: Alexandru Barbovschi
- Opis: przypadkowe zamknięcie TotalControlGUI lub TotalControlSRV podczas
  zaćmienia byłoby katastrofą (utrata sekwencji w trakcie totality). Potrzebne
  okno potwierdzenia przy próbie zamknięcia obu procesów, z konkretną
  informacją o koszcie czasowym:
  - GUI: "restart zajmie ok. 5 sekund"
  - SRV: "ponowne połączenie z kamerami może zająć do 60 sekund"
  Komunikaty mają być konkretne (czas), nie ogólnikowe ostrzeżenie "czy na
  pewno chcesz zamknąć?".

## "TotalControlCLI quit" nie zamyka faktycznie procesu SRV
- Status: proponowane
- Typ: bug
- Zgłosił: Andrzej Nowak (znalezione przy okazji sesji 2026-07-19, podczas
  próby zbudowania projektu przy uruchomionym SRV)
- Opis: `tc quit` wysyła `{"cmd":"quit"}`, SRV odpowiada `{"ok":true}`, ale
  sam proces `TotalControlSRV.exe` nie kończy działania — build blokował się
  na `LNK1104: nie można otworzyć pliku TotalControlSRV.exe` mimo
  wcześniejszego `tc quit`. Przyczyna: `CommandHandler::Handle()` zwraca
  `false` dla `quit`, ale w `PipeServer::ServeClient()` (`PipeServer.cpp`)
  `false` z handlera kończy tylko pętlę obsługi TEGO JEDNEGO klienta
  (rozłącza ten jeden named-pipe) — nie wywołuje `server.Stop()` ani nie
  ustawia żadnej globalnej flagi zamknięcia procesu. Dopóki inny klient (np.
  GUI, które trzyma trwałe połączenie i odpytuje `list_cameras`/`status` co
  ~3s) jest podłączony, `Run()` (główna pętla accept) działa dalej i proces
  żyje. To najpewniej regresja z wprowadzenia multi-client pipe (Phase 3d,
  `PIPE_UNLIMITED_INSTANCES` — patrz CLAUDE.md) — zachowanie "quit kończy
  cały proces" musiało działać poprawnie w erze jednego klienta. Do
  naprawy: `quit` powinien wywołać `server.Stop()` (jak `CtrlHandler` przy
  Ctrl+C) i/lub ustawić globalną flagę sprawdzaną po `server.Run()` w
  `main()`, nie tylko zamykać bieżące połączenie.
## Tooltips (on-hover) dla wszystkich pól interfejsu
- Status: proponowane (tabela przeglądowa gotowa do akceptacji)
- Typ: feature
- Zgłosił: Andrzej Nowak (projekt)
- Opis: dodać `ImGui::SetItemTooltip`/`IsItemHovered()+SetTooltip` na każdym
  eksponowanym polu GUI z krótkim opisem, co dana wartość znaczy i skąd się
  bierze. Zakres duży (dziesiątki pól w kilku kolumnach + Timeline + okna
  Camera Config/Options + Solar Simulator) — etapami:
  1. **Zrobione**: tabela przeglądowa — `docs/tooltips_review.md` — lista pól
     wyeksponowanych w GUI wg sekcji (ECLIPSE/LOCATION/TIME/SERVER/Block
     Inspector/EXECUTE/Solar Simulator/SUVI Alignment/Live View/Camera
     Config/Options), zmienna w kodzie + proponowana treść tooltipa + uwaga
     techniczna (widget zwykły vs draw-list wymagający ręcznego hit-testu vs
     pole tylko-do-odczytu). Zawiera 3 otwarte pytania do rozstrzygnięcia
     (zakres: readonly czy tylko edytowalne; canvas-drawn labels w pierwszej
     iteracji czy później; czy komunikaty błędów też dostają tooltip).
  2. Andrzej przegląda `docs/tooltips_review.md` i decyduje, co i jak
     dopisać/poprawić.
  3. Dopiero po akceptacji — implementacja tooltipów w App.cpp.

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
