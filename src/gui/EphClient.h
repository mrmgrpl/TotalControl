#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace TotalControl {

// Bodies fetched from JPL Horizons
enum class EphBody : int {
    Sun=0, Moon, Mercury, Venus, Mars, Jupiter, Saturn,
    Count
};

const char* EphBodyName    (EphBody b);  // "SUN", "MOON", …
const char* EphBodyHorizCmd(EphBody b);  // "10", "301", …

// One ephemeris sample
struct EphRow {
    int64_t utc_ms          = 0;
    double  az_deg          = 0;   // azimuth  (0=N, 90=E)
    double  alt_deg         = 0;   // altitude above horizon
    double  ang_diam_arcmin = 0;   // angular diameter (arcminutes)
    double  ra_deg          = 0;   // ICRF right ascension (degrees)
    double  dec_deg         = 0;   // ICRF declination (degrees)
};

// Fetch one body's observer ephemeris from JPL Horizons for a full UTC day.
// eclipse_date: Horizons-format date string, e.g. "2026-Aug-12"
// latDeg: geodetic latitude (N+), lonDeg: geodetic longitude (E+), altM: elevation (m)
// step_min: interval between records (5 → 288 records/day)
// Blocks the calling thread; call from a background thread.
// Returns empty vector on network error or parse failure.
std::vector<EphRow> FetchEphemeris(
    EphBody body,
    const std::string& eclipse_date,   // e.g. "2026-Aug-12"
    double latDeg, double lonDeg, double altM,
    int    step_min = 5,
    const std::function<void(std::string_view)>& logger = nullptr);

// Format a calendar date as Horizons date string: 2026-08-12 → "2026-Aug-12"
std::string HorizonsDate(int year, int month, int day);

} // namespace TotalControl
