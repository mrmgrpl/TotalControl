# TotalControl Alpha — raport naruszeń Clean Code

Dokument sporządzony 2026-06-06 na podstawie kodu wersji Alpha (commit `58f4c4d`).  
Służy jako lista zadań dla przepisania projektu do wersji v1.0.

Reguły Clean Code zastosowane do analizy:
- Nazewnictwo (naming)
- Długość funkcji (max ~20 linii)
- Liczba argumentów (max 3; więcej → struct)
- Komentarze (opisują DLACZEGO, nie CO)
- Obsługa błędów (HRESULT macro, std::optional zamiast nullptr)
- Prawo Demeter
- DRY
- Thread safety

---

## 1. Nazewnictwo

### 1a. Stałe — `k` prefix zamiast UPPER_SNAKE_CASE

Reguła: `UPPER_SNAKE_CASE` — np. `MAX_EXPOSURE_MS`, `CRSDK_MIN_DELAY_MS`.

```
src/main.cpp:99            kVersion, kEnumTimeoutSec, kConnectTimeoutMs, kExpectedPhaseMs
src/gui/App.cpp:253        kCamOverheadMs
src/gui/App.cpp:588–592    kLabelW, kMarkerH, kPhaseH, kRulerH, kTrackH
src/gui/IqpClient.cpp:256  kLen
src/daemon/CommandHandler.cpp:174  kPropsCount
```

Dotyczy dziesiątek stałych we wszystkich plikach.

### 1b. Skrócone nazwy helperów JSON

```
src/daemon/CommandHandler.cpp:53   JStr  → JsonString / ParseJsonString
src/daemon/CommandHandler.cpp:65   JInt  → ParseJsonInt
src/daemon/CommandHandler.cpp:75   JFlt  → ParseJsonFloat
src/daemon/CommandHandler.cpp:85   JHas  → HasJsonKey
src/gui/App.cpp:79                 JStr  (ten sam problem)
src/gui/App.cpp:95                 JInt
```

### 1c. Zmienne jednocyfrowe poza kontekstem matematycznym

```
src/gui/App.cpp:200        y, mo, d       → year, month, day
src/gui/IqpClient.cpp:82   t2, e2, t3, e3 → dateStart, dateEnd, timeStart, timeEnd
src/gui/IqpClient.cpp:274  p, q           → searchPos, scanPos
src/gui/Database.cpp:10    n              → charCount
src/main.cpp:27            n              → charCount (w WtoU8)
src/main.cpp:37,53         st             → localTime
```

> Wyjątek dozwolony: `o.u`, `o.v`, `o.n`, `tau`, `rhoSin` w `BesselCalc.cpp` —
> są to symbole astronomiczne z Meeus/Bessel. Należy je zachować z komentarzem
> wyjaśniającym odwołanie do wzoru.

---

## 2. Długość funkcji (reguła: ~20 linii)

| Funkcja | Plik:linia | Długość |
|---|---|---|
| `CommandHandler::Handle()` | CommandHandler.cpp:566–1093 | **527 linii** |
| `main()` (TotalControlSRV) | main.cpp:164–455 | **291 linii** |
| `RenderTimelineBottom()` | App.cpp:587–885 | **298 linii** |
| `RenderInspectorColumn()` | App.cpp:353–583 | **230 linii** |
| `DecodePropValue()` | CommandHandler.cpp:184–358 | **174 linii** |
| `RenderCameraSection()` | App.cpp:1196–1292 | **96 linii** |
| `RenderExtraClock()` | App.cpp:1065–1146 | **81 linii** |
| `App()` constructor | App.cpp:889–968 | **79 linii** |
| `FetchContactTimes()` | IqpClient.cpp:403–485 | **82 linii** |
| `HttpsGet()` | IqpClient.cpp:177–250 | **73 linii** |
| `ApplyStyleDark()` | App.cpp:979–1038 | **59 linii** |
| `TryRefreshFromPage()` | IqpClient.cpp:298–354 | **56 linii** |

### Sugerowany podział `CommandHandler::Handle()`:

```cpp
// zamiast jednej funkcji 527-liniowej:
bool HandleQuit(const Request& req, Response& resp);
bool HandleListCameras(const Request& req, Response& resp);
bool HandleStatus(CameraController* cam, const Request& req, Response& resp);
bool HandleShoot(CameraController* cam, const Request& req, Response& resp);
bool HandleBracket(CameraController* cam, const Request& req, Response& resp);
bool HandleMovie(CameraController* cam, const Request& req, Response& resp);
bool HandleAf(CameraController* cam, const Request& req, Response& resp);
bool HandleCmd(CameraController* cam, const Request& req, Response& resp);
bool HandleGet(CameraController* cam, const Request& req, Response& resp);
bool HandleSet(CameraController* cam, const Request& req, Response& resp);
bool HandleSeq(const Request& req, Response& resp);
```

---

## 3. Liczba argumentów (reguła: max 3; więcej → struct)

```
BesselCalc.cpp:28   evalFrame(e, tau, rhoSin, rhoCos, lonDeg)              — 5 arg
BesselCalc.cpp:94   refineContact(e, t0, rhoSin, rhoCos, lonDeg,
                                  sign, usePenumbra)                        — 7 arg
IqpClient.cpp:158   ToUtcMs(timeStr, year, month, day, utcOffsetSeconds)   — 5 arg
IqpClient.cpp:403   FetchContactTimes(eclipseId, lat, lon,
                                       year, month, day)                    — 6 arg
App.cpp:57          FormatLocalHms(utcMs, ianaName, buf, len)              — 4 arg
App.h:99            RenderExtraClock(clockId, popupId, show, tzIana)       — 4 arg
```

### Sugerowane struktury dla v1.0:

```cpp
struct ObserverLocation { double latDeg, lonDeg, altM; };
struct EclipseDate      { int year, month, day; };
struct BesselObserver   { double rhoSin, rhoCos, lonDeg; };

// zamiast 7 argumentów:
double RefineContact(const BesselianElements& e,
                     BesselObserver obs,
                     double t0, double sign, bool usePenumbra);

// zamiast 6 argumentów:
ContactTimes FetchContactTimes(const std::string& eclipseId,
                                ObserverLocation obs,
                                EclipseDate date);
```

---

## 4. Komentarze opisujące CO (nie DLACZEGO)

```
App.cpp:33    // Returns UTC milliseconds since Unix epoch using high-resolution Windows clock.
              // Nazwa UtcNowMs() to już mówi — komentarz zbędny

App.cpp:44    // Formats HH:MM:SS.mmm for the UTC clock (millisecond precision).
              // Nazwa FormatUtcHms() to już mówi

App.cpp:78    // Extract first string or numeric value for key from flat/shallow JSON.
              // Opisuje CO robi JStr() — sygnał że nazwa jest zła

App.cpp:101   // Extract all values of a repeated string key (e.g. "guid" inside cameras array).
              // Opisuje CO robi JStrAll()
```

> Separatory sekcji `// ─── Section ───` oraz komentarze z timing constants CrSDK
> są **dozwolone** — wyjaśniają WHY lub dokumentują ograniczenia sprzętowe.

---

## 5. Obsługa błędów — ignorowane wartości zwracane

```
PipeClient.cpp:48      SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
                       // zwraca BOOL — nie sprawdzane

IqpClient.cpp:205      WinHttpAddRequestHeaders(...);
                       // zwraca BOOL — nie sprawdzane

App.cpp:1046           io.Fonts->Build();
                       // zwraca bool — nie sprawdzane

main.cpp:451           SetEvent(g_shutdownDone);
                       // zwraca BOOL — nie sprawdzane

Database.cpp:64        sqlite3_prepare_v2(m_db, ..., &st, nullptr);
                       // w GetSetting() wynik rc nie jest sprawdzany →
                       // jeśli prepare failuje, st == null →
                       // sqlite3_bind_text(null,...) = undefined behavior
```

Brak makra `TC_CHECK_HR` — reguła nakazuje sprawdzać każdy HRESULT przez makro,
zwłaszcza w `main_gui.cpp` (D3D11, DXGI). Plik `main_gui.cpp` nie był analizowany
— wymaga osobnego przeglądu.

---

## 6. DRY — zduplikowana logika

### 6a. `ExeDir()` — dwie różne implementacje

```
src/main.cpp:89–96       ExeDir()  — wcsrchr + zerowanie znaku
src/gui/App.cpp:25–31    ExeDir()  — rfind + substr
```

Różne implementacje tej samej funkcji. W v1.0: jedna funkcja w `include/Utils.h`.

### 6b. `WideToUtf8` / `WtoU8` — identyczna konwersja, dwa pliki

```
src/gui/Database.cpp:10–17   WideToUtf8()
src/main.cpp:27–33           WtoU8()
```

### 6c. Formatowanie czasu kamery zduplikowane

```
src/main.cpp:358–373              formatowanie camTime → ISO 8601 (startup status table)
src/daemon/CommandHandler.cpp:631–668  to samo formatowanie (HandleStatus response)
```

---

## 7. Prawo Demeter

Wzorce ImGui API (`ImGui::GetIO().MouseWheel`, `ImGui::GetStyle().Colors`) są
nieuchronne ze względu na API biblioteki — **nie wymagają naprawy**.

W kodzie projektu brak istotnych naruszeń Demeter poza ImGui.

---

## 8. Thread safety — dangling pointer

```cpp
// App.cpp:213
m_iqpState.store(1);
if (m_iqpThread.joinable()) m_iqpThread.detach();  // ← PROBLEM
```

Lambda wątku IQP przechwytuje surowe wskaźniki do składowych `App`:
`statePtr`, `mutexPtr`, `contactPtr`, `cfgDb`.

Po `detach()` destruktor `~App()` (App.cpp:971) **nie dołącza wątku**
(`joinable()` == false po detach). Wątek kontynuuje działanie z wiszącymi
wskaźnikami po zniszczeniu obiektu `App`.

**Rozwiązanie dla v1.0:** `std::jthread` + `std::stop_token`,
lub shared state przez `std::shared_ptr<IqpState>`.

---

## Priorytety dla v1.0

| Priorytet | Naruszenie | Ryzyko |
|---|---|---|
| KRYTYCZNE | Thread safety — dangling ptr w wątku IQP (App.cpp:213) | crash w runtime |
| KRYTYCZNE | `GetSetting()` — brak sprawdzenia `sqlite3_prepare_v2` (Database.cpp:64) | UB / null-deref |
| WYSOKIE | `CommandHandler::Handle()` — 527 linii (CommandHandler.cpp:566) | nieczytelność, maintenance |
| WYSOKIE | Brak makra `TC_CHECK_HR` dla HRESULT | silent failures |
| ŚREDNIE | Argumenty >3: `refineContact`, `FetchContactTimes` → struct | czytelność |
| ŚREDNIE | DRY: `ExeDir()`, `WideToUtf8` duplikaty → `Utils.h` | rozsynchronizowanie |
| NISKIE | Nazewnictwo stałych (`k` prefix → UPPER_SNAKE_CASE) | konwencja |
| NISKIE | Komentarze opisujące CO zamiast DLACZEGO | konwencja |

---

*Raport: Claude Sonnet 4.6 / 2026-06-06*
