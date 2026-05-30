#pragma once
#include <cstdint>
#include <string>

namespace TotalControl {

struct ContactTimes {
    int64_t     c1Ms  = -1;   // UTC ms since Unix epoch; -1 = unavailable
    int64_t     c2Ms  = -1;
    int64_t     c3Ms  = -1;
    int64_t     c4Ms  = -1;
    std::string eclType;       // "TOTAL ECLIPSE", "ANNULAR ECLIPSE", …
    std::string duration;      // "2m 03.4s"
    bool        valid = false;
};

// Derives the IQP eclipse identifier from type char + date.
// T→TSE, A→ASE, H→HSE, P→PSE
std::string BuildEclipseId(char typeChar, int year, int month, int day);

// Synchronous HTTPS GET to maps.besselianelements.com.
// Call from a background thread — blocks until response arrives (typically <3 s).
ContactTimes FetchContactTimes(const std::string& eclipseId,
                               double lat, double lon,
                               int year, int month, int day);

} // namespace TotalControl
