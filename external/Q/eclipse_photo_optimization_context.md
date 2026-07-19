# Eclipse Photography Optimization — Kontekst dla Claude Code
## TSE 2026, Burgos/Lerma, 12.08.2026 | Sony A7R IVA

> Dokument wygenerowany z sesji analitycznej. Zawiera wszystkie wypracowane
> modele, formuły, decyzje projektowe i kod gotowy do integracji z TotalControl.

---

## 1. Setup kamery i optyki

### Sensor — Sony A7R IVA
```
Sensor:          Full Frame BSI-CMOS, 35.9 × 24.0 mm
Rozdzielczość:   61 MP (9504 × 6336 px)
Piksel:          3.76 µm
Migawka mech.:   1/8000 s  (min)
Migawka elektr.: 1/32000 s (min, tryb Silent)
Bracketing:      3 / 5 / 9 klatek, krok 0.3/0.5/0.7/1.0/1.5/2.0 EV
```

### Setup 1 — Bresser AR90 @ 900mm f/10
```
Ogniskowa:       900 mm
Przysłona:       f/10 (stała — refraktor achromat)
Skala piksela:   0.86 arcsec/px
FOV:             137' × 92'  (2.28° × 1.53°)
Słońce w kadrze: 23% szerokości
Korona widoczna: ~3.3 R☉ od centrum
Cel:             chromosfera, protuberancje, korona wewnętrzna
```

### Setup 2 — Sony SEL24-240 @ 240mm f/6.3
```
Ogniskowa:       240 mm (max zoom)
Przysłona:       f/6.3 (maksymalna przy 240mm)
Skala piksela:   3.23 arcsec/px
FOV:             514' × 344'  (8.57° × 5.73°)
Słońce w kadrze: 6% szerokości
Korona widoczna: ~15 R☉
Cel:             pełna korona, earthshine, kontekst
```

### Odrzucony Setup 3 — SEL200600G + SEL14TC
```
Byłby: 840mm, f/9 (= 6.3 × 1.4)
Powód odrzucenia: +0.7 dB SSNR vs AR90 → nie warte ~9000 PLN
```

---

## 2. Parametry zaćmienia TSE 2026

```
Lokalizacja:     Burgos/Lerma, Hiszpania (42.34°N, 3.70°W, 860m n.p.m.)
Data/czas C2:    12.08.2026, 20:29 CEST (18:29 UTC)
Totality C2→C3:  104.1 s
Wysokość Słońca: h ≈ 8.0° (bardzo nisko!)
Masa powietrzna: X = 1/sin(8°) ≈ 7.2
Ekstynkcja atm.: ΔEV = k × X = 0.13 × 7.2 ≈ 0.94 EV (czyste niebo)
```

### Podział czasu totality
```
C2 do C2+5s:         5s   → faza kontaktu (Bailey, chromosfera, pierścień)
C2+5s do C3-5s:     94.1s → faza korony
Earthshine:          ~4s   → 1 klatka, ISO 1600, odjęte od korony
C3-5s do C3:         5s   → faza kontaktu C3
Dostępny czas korony: ≈ 90s
```

---

## 3. Model ekspozycji NASA (Espenak & Anderson 2004)

### Wzór bazowy
```
t = f² / (ISO × 2^Q_eff)

gdzie:
  t       = czas ekspozycji [s]
  f       = przysłona (f-number)
  ISO     = czułość
  Q_eff   = Q - ΔEV_extinction  (Q skorygowany o ekstynkcję)
  ΔEV     = k × (1/sin h°)      (ekstynkcja atmosferyczna)
```

### Tabela Q-values (NASA Table 40)
```
Zjawisko                    Q    Filtr ND    Faza
──────────────────────────────────────────────────
Koraliki Bailey'ego        12    —           kontakt
Faza częściowa (ND 5.0)     8    ✓           częściowa
Faza częściowa (ND 4.0)    11    ✓           częściowa
Chromosfera                11    —           totality
Protuberancje               9    —           totality
Pierścień diamentowy        8    —           kontakt
Korona < 0.1 R☉             7    —           totality
Korona < 0.2 R☉             5    —           totality
Korona < 0.5 R☉             3    —           totality
Korona < 1 R☉               1    —           totality
Korona < 2 R☉               0    —           totality
Korona < 4 R☉              -1    —           totality
Korona < 8 R☉              -3    —           totality
Earthshine                 -5    —           totality
```

### Python — core functions
```python
import math

EXT = 0.94          # ekstynkcja przy h=8°, k=0.13
OV  = 0.15          # overhead per frame [s] (CrSDK estimate)

def calc_t(Q: float, f: float, iso: int) -> float:
    """Czas ekspozycji wg wzoru NASA."""
    return (f * f) / (iso * math.pow(2, Q - EXT))

def seq_time(Q: float, f: float, iso: int, N: int, step_ev: float) -> float:
    """Całkowity czas serii bracketingu N klatek, krok step_ev EV."""
    t = calc_t(Q, f, iso)
    h = (N - 1) / 2
    total = sum(t * math.pow(2, k * step_ev) for k in range(-int(h), int(h)+1))
    return total + N * OV

def airmass(altitude_deg: float) -> float:
    return 1.0 / math.sin(math.radians(max(altitude_deg, 1.0)))

def extinction(altitude_deg: float, k: float = 0.13) -> float:
    """Ekstynkcja atmosferyczna [EV]."""
    return k * airmass(altitude_deg)
```

---

## 4. Model SNR i SSNR

### SNR sensora (DxOMark, Sony A7R IV)
```
ISO   SNR₁ [dB]   DR [EV]   Uwaga
100   44.0        14.8      baza / max DR
200   43.2        14.2      plateau DR
400   41.5        13.2      plateau DR ✓ OPTIMUM
800   39.2        12.2      dual-gain nearby
1600  36.3        11.1      dual-gain
3200  33.1        10.0      wysoki szum
6400  30.0         9.0      za wysoki
```

### SSNR — Sumaryczny SNR po stackowaniu
```
SSNR = SNR₁ × √N_total                    [liniowy]
SSNR [dB] = SNR₁ [dB] + 10·log₁₀(N_total)

gdzie:
  N_total = N_BRK × M
  N_BRK   = klatek na serię bracketingu (3/5/9)
  M       = liczba pełnych pętli sekwencji w czasie budżetu
```

```python
def ssnr_db(snr1_db: float, N_BRK: int, M: int) -> float:
    N_total = N_BRK * max(1, M)
    return snr1_db + 10 * math.log10(N_total)

def phase_M(phen_list: list, f: float, iso: int,
            N: int, step: float, budget: float) -> int:
    """Ile razy sekwencja mieści się w budżecie czasowym."""
    total_loop = sum(seq_time(p['Q'], f, iso, N, step)
                     for p in phen_list)
    return int(budget / total_loop) if total_loop > 0 else 0
```

### Model Hasinoffa — szum kamery (MIT CSAIL 2010)
```
Paper: "Noise-Optimal Capture for High Dynamic Range Photography"
       Hasinoff, Durand, Freeman — CVPR 2010

Var(n) = Φt + σ²_read + (σ_ADC × g)²
         |       |           |
      shot    readout    ADC noise
      noise   noise     (maleje z ISO!)

Sony A7R IV (szacunek):
  σ_read = 3.0 e⁻
  σ_ADC  = 0.5 DN
  g₁₀₀   = 2.75 e⁻/DN
  FWC₁₀₀ = 45 000 e⁻

High-ISO potential A7R IV ≈ 1.3 dB  (małe — BSI-CMOS jest dobry)
vs Canon 5D Mark II: 19.7 dB

Główny wniosek dla zaćmienia:
  Ciemne zjawiska (Q < 0) → wyższe ISO → krótsze ekspozycje → więcej M → lepszy SSNR
  Jasne zjawiska (Q > 8)   → overhead dominuje, ISO nie zmienia M → użyj niskiego ISO
```

### Optymalne ISO per zjawisko
```
Zjawisko/Q      Reżim szumu         Optimal ISO
Q ≥ 11         overhead dominuje    ISO 100
Q = 8–10       overhead dominuje    ISO 100–200
Q = 5–7        shot noise           ISO 200–400
Q = 1–4        mixed                ISO 400
Q = -1 to 0    ADC growing          ISO 800
Q = -3 to -2   ADC dominant        ISO 1600
Q ≤ -4         ADC dominant        ISO 1600–3200
Earthshine     bardzo ciemne        ISO 1600 (stałe)
```

---

## 5. Zakres tonalny sceny

```
Scena                               EV      Uwagi
──────────────────────────────────────────────────
Sensor DR (ISO 400)                13.2    1 klatka
Korona 0.1–8 R☉ (w kadrze 240mm)  10      Q=7 do Q=-3
Chromosfera → korona 8 R☉          14      Q=11 do Q=-3
Pełna scena z earthshine            16      Q=11 do Q=-5

WAŻNE: ekstynkcja atmosferyczna NIE zmienia zakresu sceny
       — przesuwa wszystkie Q o tę samą wartość.

Zakres bracketingu = DR_sensor + (N_BRK - 1) × krok_EV
Np. ISO400, 5×1EV: 13.2 + 4.0 = 17.2 EV  → pełna scena pokryta ✓
```

---

## 6. Optymalna strategia bracketingu

### Wynikające reguły
```
1. Przysłona: MAKSYMALNA (f/10 dla AR90, f/6.3 dla SEL240)
   → więcej fotonów/s → krótsze ekspozycje → więcej M → wyższy SSNR

2. Faza kontaktu (5s):
   → overhead dominuje (t_exp << 150ms)
   → M jest stałe dla każdego ISO
   → ISO 100 daje najwyższe SNR₁ przy tym samym M
   → OPTIMAL: ISO 100, N_BRK=3, krok=1.0 EV

3. Faza korony (90s):
   → mix krótkich (chromosfera) i długich (korona zewn.) ekspozycji
   → optimum SNR(ISO) × √M leży przy ISO 400–800
   → OPTIMAL per zjawisko wg Hasinoffa (patrz tabela wyżej)

4. Earthshine (1 klatka, 30s budżet):
   → ISO 1600, f/10 lub f/6.3
   → t ≈ 2.5–4s (zależy od f)
```

### Konfiguracja BRK — rekomendacja dla każdego setupu
```
Setup 1 (900mm f/10, korona do 3.3 R☉, scena = 12 EV):
  ISO 400, N_BRK=3, krok=0.3 EV
  Zakres: 13.2 + 0.6 = 13.8 EV ✓
  M ≈ 11 pętli w 90s
  SSNR korona: ~51.9 dB

Setup 2 (240mm f/6.3, korona do 15 R☉, scena = 14 EV):
  ISO 400, N_BRK=3, krok=0.5 EV
  Zakres: 13.2 + 1.0 = 14.2 EV ✓
  M ≈ 8 pętli w 90s
  SSNR korona: ~50.5 dB
```

---

## 7. Przykładowe czasy ekspozycji — Setup 1 (900mm f/10, ISO 400, h=8°)

```
Zjawisko              Q    Q_eff   t_baza      Migawka
──────────────────────────────────────────────────────
Chromosfera          11   10.06   0.000240s   1/4000
Protuberancje         9    8.06   0.000947s   1/1000
Pierścień diamt.      8    7.06   0.001871s   1/500
Korona < 0.1 R☉       7    6.06   0.003740s   1/250
Korona < 0.2 R☉       5    4.06   0.014830s   1/60
Korona < 0.5 R☉       3    2.06   0.059990s   1/15
Korona < 1 R☉         1    0.06   0.239900s   1/4
Korona < 2 R☉         0   -0.94   0.479100s   1/2
Korona < 4 R☉        -1   -1.94   0.961500s   1"
Earthshine*          -5   -5.94   3.840000s   4"  (*ISO1600)
```

---

## 8. Alerty — kiedy sekwencja jest nierealna

```python
MECH_MIN   = 1/8000   # mechaniczna migawka min
ELEC_MIN   = 1/32000  # elektroniczna migawka min
TOTALITY   = 104.1    # C2→C3 [s]
EARTH_DUR  = 30       # budżet earthshine [s]
C2_MARGIN  = 5        # budżet C2 kontakt [s]
C3_MARGIN  = 5        # budżet C3 kontakt [s]

def check_alert(t_series: float, phenomenon_duration: float) -> str:
    if t_series > phenomenon_duration:
        return f"ALERT: {t_series:.2f}s > {phenomenon_duration}s — PRZEKROCZENIE!"
    if t_series < ELEC_MIN:
        return "ALERT: zbyt krótki nawet dla e-shutter"
    return "OK"
```

### Krytyczne ograniczenia
```
Bailey'ego (dur=3s):   przy N=9, krok>0.5 → T_serii > 3s → CZERWONY
Earthshine (dur=30s):  przy ISO 400, f/10, N=5 → T_serii ≈ 34s → CZERWONY
                       rozwiązanie: ISO 1600 → T_serii ≈ 8s → OK
```

---

## 9. Kod C++ dla TotalControl

```cpp
#include <cmath>
#include <vector>
#include <string>
#include <map>

// === Stałe kamery ===
constexpr double F_900  = 10.0;   // AR90 f/10
constexpr double F_240  = 6.3;    // SEL24-240 @ 240mm
constexpr double EXT    = 0.94;   // ekstynkcja h=8°
constexpr double OV     = 0.15;   // overhead per frame [s]
constexpr double MECH_MIN = 1.0/8000.0;
constexpr double ELEC_MIN = 1.0/32000.0;
constexpr double TOTALITY = 104.1;
constexpr int    EARTH_ISO = 1600;

// === Q-values (NASA Table 40) ===
const std::map<std::string, double> Q_VALUES = {
    {"bailys_beads",   12.0}, {"chromosphere",  11.0},
    {"prominences",     9.0}, {"diamond_ring",   8.0},
    {"corona_01rs",     7.0}, {"corona_02rs",    5.0},
    {"corona_05rs",     3.0}, {"corona_1rs",     1.0},
    {"corona_2rs",      0.0}, {"corona_4rs",    -1.0},
    {"corona_8rs",     -3.0}, {"earthshine",    -5.0},
};

// === Core functions ===
double calc_t(double Q, double f, int iso) {
    return (f * f) / (static_cast<double>(iso) * std::pow(2.0, Q - EXT));
}

double seq_time(double Q, double f, int iso, int N, double step_ev) {
    const double t = calc_t(Q, f, iso);
    const int half = (N - 1) / 2;
    double sum = 0.0;
    for (int k = -half; k <= half; ++k)
        sum += t * std::pow(2.0, k * step_ev);
    return sum + N * OV;
}

double ssnr_db(double snr1_db, int N_brk, int M) {
    int N_total = N_brk * std::max(1, M);
    return snr1_db + 10.0 * std::log10(static_cast<double>(N_total));
}

// === Exposure entry ===
struct ExposureEntry {
    std::string phenomenon;
    double Q;
    int    iso;
    double t_base;      // base exposure [s]
    double t_series;    // full bracket time [s]
    int    M;           // loops in budget
    int    N_total;     // N_brk × M
    double ssnr;        // final stacked SNR [dB]
    bool   elec_shutter;
    bool   over_budget;
};

// === SNR₁ lookup (DxOMark A7R IV) ===
double snr1_from_iso(int iso) {
    if (iso <= 100)  return 44.0;
    if (iso <= 200)  return 43.2;
    if (iso <= 400)  return 41.5;
    if (iso <= 800)  return 39.2;
    if (iso <= 1600) return 36.3;
    if (iso <= 3200) return 33.1;
    return 30.0;
}

// === Optimal ISO per phenomenon (Hasinoff-informed) ===
int optimal_iso(double Q, double f, int N, double step_ev, double budget) {
    const std::vector<int> isos = {100, 200, 400, 800, 1600, 3200, 6400};
    double best_ssnr = -999.0;
    int best_iso = 400;
    for (int iso : isos) {
        double ts = seq_time(Q, f, iso, N, step_ev);
        int M = (ts > 0) ? static_cast<int>(budget / ts) : 0;
        int Nt = N * std::max(1, M);
        double ssnr = snr1_from_iso(iso) + 10.0 * std::log10(static_cast<double>(Nt));
        if (ssnr > best_ssnr) { best_ssnr = ssnr; best_iso = iso; }
    }
    return best_iso;
}
```

---

## 10. JSON sekwencji dla TotalControl

```json
{
  "meta": {
    "event": "TSE_2026_Burgos",
    "date": "2026-08-12",
    "totality_s": 104.1,
    "h_sun_deg": 8.0,
    "extinction_ev": 0.94,
    "overhead_s": 0.15,
    "note": "Parametry wg modelu NASA + Hasinoff ISO optimization"
  },
  "setups": [
    {
      "id": "ar90_900mm",
      "focal_mm": 900, "f_number": 10.0,
      "base_iso": 400, "brk_n": 3, "brk_step_ev": 0.3,
      "target": "chromosphere, prominences, inner corona"
    },
    {
      "id": "sel240_240mm",
      "focal_mm": 240, "f_number": 6.3,
      "base_iso": 400, "brk_n": 3, "brk_step_ev": 0.5,
      "target": "full corona, earthshine"
    }
  ],
  "phases": [
    {
      "id": "c2_contact", "budget_s": 5,
      "iso_override": 100,
      "phenomena": ["bailys_beads", "diamond_ring", "chromosphere"]
    },
    {
      "id": "corona", "budget_s": 90.1,
      "iso_hasinoff": true,
      "phenomena": ["prominences","corona_01rs","corona_02rs","corona_05rs",
                    "corona_1rs","corona_2rs","corona_4rs","corona_8rs"]
    },
    {
      "id": "earthshine", "budget_s": 30,
      "iso_override": 1600, "n_frames": 1,
      "phenomena": ["earthshine"]
    },
    {
      "id": "c3_contact", "budget_s": 5,
      "iso_override": 100,
      "phenomena": ["diamond_ring", "bailys_beads"]
    }
  ]
}
```

---

## 11. Kluczowe wzory — ściągawka

```
# Ekspozycja (NASA)
t = f² / (ISO × 2^(Q - ΔEV))

# Ekstynkcja
ΔEV = k × (1 / sin h°)        k=0.13 (czyste), k=0.25 (mgiełka)

# Zakres tonalny bracketingu
TonalRange = DR_sensor + (N_BRK - 1) × krok_EV

# Stacking SNR
SSNR_dB = SNR₁_dB + 10·log₁₀(N_BRK × M)

# Skala piksela
px_scale ["/px] = 206265 × 3.76 [µm] / focal [mm]

# FOV
FOV_w ['] = (35.9 / focal) × (180/π) × 60
FOV_h ['] = (24.0 / focal) × (180/π) × 60

# Masa powietrzna
X = 1 / sin(h°)
```

---

## 12. Zależności projektu

```
TotalControl (C++, GPL v3)
├── GitHub: maciejszupiluk/TotalControl
├── SDK: Sony CrSDK (shutter latency ~80ms)
├── Deadline: 01.08.2026 (community feedback)
├── Target event: TSE 2026, Burgos, 12.08.2026
└── Powiązane
    ├── Besselian Elements API (IQP model, kontakty)
    ├── EclipseWise (Michael Zeiler)
    └── Chronological audio countdown (ElevenLabs, 85 × MP3)

Referencje naukowe
├── Espenak & Anderson (2004) NASA RP-1457 — Q-values, wzór ekspozycji
├── NASA Table 40 — eclipse.gsfc.nasa.gov
├── Hasinoff, Durand, Freeman (2010) CVPR — Noise-Optimal HDR Capture
│   URL: https://people.csail.mit.edu/billf/publications/Noise-Optimal_Capture.pdf
└── DxOMark — Sony A7R IV full sensor measurements
```

---

## 13. Otwarte pytania / TODO

```
[ ] Zaimplementować findOptISO() w TotalControl dla per-phenomenon ISO
[ ] Zweryfikować overhead CrSDK (założono 150ms — zmierzyć empirycznie)
[ ] Test AR90 @ pełna klatka — sprawdzić winietowanie i chromatyzm
[ ] Dodać bracketing dla fazy częściowej (ND 5.0 i 4.0)
[ ] Sekwencja JSON → loader w TotalControl
[ ] Test h=8° ekstynkcja vs rzeczywiste niebo Burgos — sprawdzić k w dniu zaćmienia
[ ] Kalkulator pixel scale dla nowych teleskopów / obiektywów
```

---

*Wygenerowano z sesji Claude Sonnet 4.6 · Maciej Szupiluk / SysIT*
*Do użytku w projekcie TotalControl — GPL v3*
