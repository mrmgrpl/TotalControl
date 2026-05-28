# Solar Eclipse Exposure Calculator — Model Reference
## TSE 2026, Burgos/Lerma, August 12 2026

> **Sources:** NASA RP-1457 (Espenak & Anderson 2004); NASA Table 40 — eclipse.gsfc.nasa.gov;
> *Bulletin of the American Astronomical Society* 36, 1557 (2004).
> Jubier calculator: http://xjubier.free.fr/en/site_pages/SolarEclipseExposure.html (v1.0.2, last updated 2017).

---

## 1. Base formula (NASA/Espenak)

```
t = f² / (ISO × 2^Q_eff)
```

| Symbol    | Meaning                                              |
|-----------|------------------------------------------------------|
| `t`       | Exposure time [s]                                    |
| `f`       | Aperture (f-number)                                  |
| `ISO`     | Sensor sensitivity                                   |
| `Q`       | Brightness exponent for the phenomenon (NASA empirical constant) |
| `Q_eff`   | Q corrected for atmospheric extinction               |

Rearranged in EV:

```
EV = Q_eff + log2(ISO) - 2·log2(f)
t  = 2^(-EV)
```

---

## 2. Atmospheric correction

```
X      = 1 / sin(h°)          # airmass
ΔEV    = k × X                # brightness loss [EV]
Q_eff  = Q - ΔEV              # (totality phase; ND filter dominates partial phase)
```

### Extinction coefficients `k`

| Sky condition      | k [mag/airmass] |
|--------------------|-----------------|
| Clear              | 0.13            |
| Light haze         | 0.25            |
| Hazy               | 0.50            |

### TSE 2026 Burgos — atmospheric parameters

```
Location    : 42.34°N, 3.70°W  (Burgos/Lerma)
Date/time   : 2026-08-12, 20:29 CEST (18:29 UTC)
h_sun       ≈ 8.0°
X           ≈ 7.2
ΔEV (clear) ≈ 0.94 EV
ΔEV (haze)  ≈ 1.80 EV
Totality    ≈ 104 s
```

> ⚠️ Low Sun altitude (h ≈ 8°) is the main challenge of TSE 2026.
> Outer corona at Q = −3 requires multi-second exposures —
> an equatorial mount with tracking is required.

---

## 3. Q-values table (NASA Table 40)

| Phenomenon / object             |  Q  | ND filter | Phase    |
|---------------------------------|:---:|:---------:|----------|
| Partial phase (ND 5.0 = 1/100k) |  8  | ✓         | partial  |
| Partial phase (ND 4.0 = 1/10k)  | 11  | ✓         | partial  |
| Baily's Beads                   | 12  | —         | contact  |
| Chromosphere                    | 11  | —         | totality |
| Prominences                     |  9  | —         | totality |
| Diamond ring (±6 s)             |  8  | —         | contact  |
| Inner corona (< 0.1 R☉)        |  7  | —         | totality |
| Inner corona (< 0.2 R☉)        |  5  | —         | totality |
| Mid corona   (< 0.5 R☉)        |  3  | —         | totality |
| Corona       (< 1.0 R☉)        |  1  | —         | totality |
| Outer corona (< 2.0 R☉)        |  0  | —         | totality |
| Outer corona (< 4.0 R☉)        | −1  | —         | totality |
| Outer corona (< 8.0 R☉)        | −3  | —         | totality |
| Earthshine                      | −5  | —         | totality |

---

## 4. Camera parameters — Sony A7R IVA

```
Sensor          : Full Frame 35.9 × 24.0 mm
Resolution      : 61 MP (9504 × 6336 px)
Pixel size      : 3.76 µm
Mechanical shutter : 1/8000 s  (min)
Electronic shutter : 1/32000 s (min, Silent mode)
Maximum ISO     : 51200 (native: 100–32000)
```

### Pixel scale and field of view

```python
pixel_scale_arcsec = (206265 * pixel_um / 1000) / focal_mm
fov_w_arcmin       = (35.9 / focal_mm) * (180/pi) * 60
fov_h_arcmin       = (24.0 / focal_mm) * (180/pi) * 60
sun_fill_pct       = (32.0 / fov_w_arcmin) * 100   # Sun diameter ≈ 32'
```

| Focal length | Scale ["/px] | FOV width | Sun in frame |
|-------------:|:------------:|----------:|:------------:|
|    200 mm    |   3.88"      |  9.8°     |  ~5%         |
|    300 mm    |   2.59"      |  6.5°     |  ~8%         |
|    500 mm    |   1.55"      |  3.9°     | ~14%         |
|    600 mm    |   1.29"      |  3.3°     | ~16%         |
|    800 mm    |   0.97"      |  2.5°     | ~21%         |
|   1200 mm    |   0.65"      |  1.6°     | ~33%         |
|   2000 mm    |   0.39"      |  1.0°     | ~54%         |

---

## 5. Sample exposure times — reference setup

**Setup:** Sony A7R IVA + 500 mm focal length, f/8, ISO 400, h = 8°, clear sky (ΔEV = 0.94)

```
Q_eff = Q - 0.94

Phenomenon                  Q    Q_eff   t [s]         Shutter
────────────────────────────────────────────────────────────────
Chromosphere               11   10.06   1/1000        1/1000 s
Prominences                 9    8.06   1/250         1/250 s
Inner corona (< 0.1 R☉)    7    6.06   1/64         ~1/60 s
Inner corona (< 0.2 R☉)    5    4.06   1/16         ~1/15 s
Mid corona   (< 0.5 R☉)    3    2.06   1/4           1/4 s
Corona       (< 1.0 R☉)    1    0.06   1.0 s         1"
Outer corona (< 2.0 R☉)    0   -0.94   1.9 s         2"
Outer corona (< 4.0 R☉)   -1   -1.94   3.9 s         4"
Outer corona (< 8.0 R☉)   -3   -3.94  15.6 s        16"
Earthshine                 -5   -5.94  62.5 s        ⚠ >60"
────────────────────────────────────────────────────────────────
```

> Legend: ⚡ = electronic shutter required | ⛔ = out of range | ⚠ = critical

---

## 6. Implementation — Python/C++ functions

### Python (reference / planning scripts)

```python
import math

PIXEL_UM   = 3.76          # Sony A7R IVA
SENSOR_W   = 35.9          # mm
SENSOR_H   = 24.0          # mm
MECH_MIN   = 1 / 8000      # s
ELEC_MIN   = 1 / 32000     # s
TOTALITY_S = 104           # TSE 2026 Burgos

Q_VALUES = {
    "partial_nd5":      8,
    "partial_nd4":     11,
    "bailys_beads":    12,
    "chromosphere":    11,
    "prominences":      9,
    "diamond_ring":     8,
    "corona_01rs":      7,
    "corona_02rs":      5,
    "corona_05rs":      3,
    "corona_1rs":       1,
    "corona_2rs":       0,
    "corona_4rs":      -1,
    "corona_8rs":      -3,
    "earthshine":      -5,
}

def airmass(altitude_deg: float) -> float:
    """Flat-Earth airmass approximation (valid for h > 3°)."""
    return 1.0 / math.sin(math.radians(max(altitude_deg, 1.0)))

def atmospheric_extinction(altitude_deg: float, k: float = 0.13) -> float:
    """Atmospheric extinction in EV (stops)."""
    return k * airmass(altitude_deg)

def exposure_time(Q: float, f_number: float, iso: int) -> float:
    """
    NASA/Espenak formula: t = f² / (ISO × 2^Q)
    Q is already Q_eff (corrected for atmospheric extinction).
    Returns exposure time in seconds.
    """
    return (f_number ** 2) / (iso * (2 ** Q))

def pixel_scale(focal_mm: float) -> float:
    """Pixel scale in arcsec/px for Sony A7R IVA."""
    return (206265 * PIXEL_UM / 1000) / focal_mm

def field_of_view(focal_mm: float) -> tuple[float, float]:
    """FOV in arcminutes (width, height)."""
    w = (SENSOR_W / focal_mm) * (180 / math.pi) * 60
    h = (SENSOR_H / focal_mm) * (180 / math.pi) * 60
    return w, h

def build_sequence(
    focal_mm: float,
    f_number: float,
    iso: int,
    altitude_deg: float,
    k: float = 0.13,
    bracket_stops: int = 2,
) -> list[dict]:
    """
    Build complete eclipse exposure sequence.
    Returns list of dicts with phenomenon, Q_eff, time, shutter_mode.
    """
    ext = atmospheric_extinction(altitude_deg, k)
    sequence = []

    for name, Q in Q_VALUES.items():
        is_filtered = name.startswith("partial")
        Q_eff = Q if is_filtered else Q - ext
        t = exposure_time(Q_eff, f_number, iso)

        # Determine shutter mode
        if t < ELEC_MIN:
            mode = "OUT_OF_RANGE"
        elif t < MECH_MIN:
            mode = "ELECTRONIC"
        else:
            mode = "MECHANICAL"

        # Generate bracket exposures (±N stops)
        brackets = []
        for ev_offset in range(-bracket_stops, bracket_stops + 1):
            t_b = t * (2 ** ev_offset)
            brackets.append(round(t_b, 6))

        sequence.append({
            "phenomenon":  name,
            "Q":           Q,
            "Q_eff":       round(Q_eff, 3),
            "t_s":         round(t, 6),
            "shutter_mode": mode,
            "brackets_s":  brackets,
            "filter_required": is_filtered,
        })

    return sequence


# --- Usage ---
if __name__ == "__main__":
    seq = build_sequence(
        focal_mm=500,
        f_number=8,
        iso=400,
        altitude_deg=8.0,
        k=0.13,
    )
    for s in seq:
        print(f"{s['phenomenon']:25s}  Q_eff={s['Q_eff']:+.2f}  t={s['t_s']:.4f}s  [{s['shutter_mode']}]")
```

### C++ — skeleton for TotalControl

```cpp
#include <cmath>
#include <string>
#include <vector>
#include <map>

// Sony A7R IVA constants
constexpr double PIXEL_UM   = 3.76;
constexpr double SENSOR_W   = 35.9;
constexpr double SENSOR_H   = 24.0;
constexpr double MECH_MIN   = 1.0 / 8000.0;
constexpr double ELEC_MIN   = 1.0 / 32000.0;
constexpr int    TOTALITY_S = 104;

// NASA Q-values
const std::map<std::string, double> Q_VALUES = {
    {"partial_nd5",    8.0},
    {"partial_nd4",   11.0},
    {"bailys_beads",  12.0},
    {"chromosphere",  11.0},
    {"prominences",    9.0},
    {"diamond_ring",   8.0},
    {"corona_01rs",    7.0},
    {"corona_02rs",    5.0},
    {"corona_05rs",    3.0},
    {"corona_1rs",     1.0},
    {"corona_2rs",     0.0},
    {"corona_4rs",    -1.0},
    {"corona_8rs",    -3.0},
    {"earthshine",    -5.0},
};

struct ExposureEntry {
    std::string phenomenon;
    double Q;
    double Q_eff;
    double exposure_s;     // computed exposure time [s]
    bool   filter_required;
    enum class Mode { MECHANICAL, ELECTRONIC, OUT_OF_RANGE } shutter_mode;
};

double airmass(double altitude_deg) {
    return 1.0 / std::sin(altitude_deg * M_PI / 180.0);
}

double atmospheric_extinction(double altitude_deg, double k = 0.13) {
    return k * airmass(altitude_deg);
}

double exposure_time(double Q_eff, double f_number, int iso) {
    return (f_number * f_number) / (static_cast<double>(iso) * std::pow(2.0, Q_eff));
}

std::vector<ExposureEntry> build_sequence(
    double focal_mm,
    double f_number,
    int    iso,
    double altitude_deg,
    double k = 0.13)
{
    const double ext = atmospheric_extinction(altitude_deg, k);
    std::vector<ExposureEntry> seq;

    for (const auto& [name, Q] : Q_VALUES) {
        bool is_filtered = (name.rfind("partial", 0) == 0);
        double Q_eff = is_filtered ? Q : Q - ext;
        double t = exposure_time(Q_eff, f_number, iso);

        ExposureEntry e;
        e.phenomenon      = name;
        e.Q               = Q;
        e.Q_eff           = Q_eff;
        e.exposure_s      = t;
        e.filter_required = is_filtered;

        if (t < ELEC_MIN)
            e.shutter_mode = ExposureEntry::Mode::OUT_OF_RANGE;
        else if (t < MECH_MIN)
            e.shutter_mode = ExposureEntry::Mode::ELECTRONIC;
        else
            e.shutter_mode = ExposureEntry::Mode::MECHANICAL;

        seq.push_back(e);
    }
    return seq;
}
```

---

## 7. JSON — sequence format for TotalControl

```json
{
  "sequence_meta": {
    "event":         "TSE_2026_Burgos",
    "date":          "2026-08-12",
    "totality_utc":  "18:29:00",
    "totality_s":    104,
    "location": {
      "name":        "Burgos/Lerma, Spain",
      "lat":         42.34,
      "lon":         -3.70,
      "altitude_m":  860
    },
    "camera":        "Sony A7R IVA",
    "focal_mm":      500,
    "f_number":      8.0,
    "iso":           400,
    "sun_altitude_deg": 8.0,
    "sky_k":         0.13,
    "extinction_ev": 0.94
  },
  "shots": [
    { "id": 1,  "phenomenon": "diamond_ring",  "t_s": 0.004,   "mode": "MECHANICAL", "filter": false, "bracket_ev": 0  },
    { "id": 2,  "phenomenon": "chromosphere",  "t_s": 0.001,   "mode": "MECHANICAL", "filter": false, "bracket_ev": 0  },
    { "id": 3,  "phenomenon": "prominences",   "t_s": 0.004,   "mode": "MECHANICAL", "filter": false, "bracket_ev": 0  },
    { "id": 4,  "phenomenon": "corona_01rs",   "t_s": 0.008,   "mode": "MECHANICAL", "filter": false, "bracket_ev": 0  },
    { "id": 5,  "phenomenon": "corona_02rs",   "t_s": 0.0625,  "mode": "MECHANICAL", "filter": false, "bracket_ev": 0  },
    { "id": 6,  "phenomenon": "corona_05rs",   "t_s": 0.25,    "mode": "MECHANICAL", "filter": false, "bracket_ev": 0  },
    { "id": 7,  "phenomenon": "corona_1rs",    "t_s": 1.0,     "mode": "MECHANICAL", "filter": false, "bracket_ev": 0  },
    { "id": 8,  "phenomenon": "corona_2rs",    "t_s": 2.0,     "mode": "MECHANICAL", "filter": false, "bracket_ev": 0  },
    { "id": 9,  "phenomenon": "corona_4rs",    "t_s": 4.0,     "mode": "MECHANICAL", "filter": false, "bracket_ev": 0  },
    { "id": 10, "phenomenon": "corona_8rs",    "t_s": 16.0,    "mode": "MECHANICAL", "filter": false, "bracket_ev": 0  },
    { "id": 11, "phenomenon": "diamond_ring",  "t_s": 0.004,   "mode": "MECHANICAL", "filter": false, "bracket_ev": 0  }
  ]
}
```

---

## 8. Sources and links

| Source | URL |
|--------|-----|
| NASA Eclipse Photography (Espenak) | https://eclipse.gsfc.nasa.gov/SEhelp/eclipsePhoto.html |
| NASA Table 40 — Exposure Guide | https://eclipse.gsfc.nasa.gov/SEpubs/19990811/tables/table_40.html |
| Xavier Jubier Exposure Calculator | http://xjubier.free.fr/en/site_pages/SolarEclipseExposure.html |
| Shaun Tarpley Exposure Spreadsheet | https://www.shaunctarpley.com/exposure-calculator |
| TSE 2026 Interactive Map (Jubier) | http://xjubier.free.fr/en/site_pages/solar_eclipses/xSE_GoogleMap3.php?Ecl=+20260812&Acc=1&Umb=1&Lmt=1&Mag=1 |

---

*Generated: 2026-05-25 | Model: Claude Sonnet 4.6 | TotalControl project*
