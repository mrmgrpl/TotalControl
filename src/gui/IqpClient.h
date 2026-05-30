#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace TotalControl {

enum class ContactSource { None, IQP, Besselian };

struct ContactTimes {
    int64_t       c1Ms  = -1;   // UTC ms since Unix epoch; -1 = unavailable
    int64_t       c2Ms  = -1;   // -1 when outside totality/annularity belt
    int64_t       c3Ms  = -1;
    int64_t       c4Ms  = -1;
    int64_t       maxMs = -1;   // time of maximum eclipse
    std::string   eclType;      // "TOTAL ECLIPSE", "PARTIAL ECLIPSE", "NO ECLIPSE", …
    std::string   duration;     // "2m 03.4s"
    bool          valid  = false; // true → at least C1 present
    bool          apiOk  = false; // true → source produced a result (IQP OK or BE computed)
    ContactSource source = ContactSource::None;
};

// Derives the IQP eclipse identifier from type char + date.
// T→TSE, A→ASE, H→HSE, P→PSE
std::string BuildEclipseId(char typeChar, int year, int month, int day);

// Synchronous HTTPS GET to maps.besselianelements.com.
// Call from a background thread — blocks until response arrives (typically <3 s).
// Auto-refreshes the API key when the server returns "Wrong Key".
ContactTimes FetchContactTimes(const std::string& eclipseId,
                               double lat, double lon,
                               int year, int month, int day);

// Key management — call SetApiKey() on startup with value from config DB.
// GetCurrentApiKey() returns the key currently in use (may have been auto-refreshed).
void        SetApiKey(const std::string& key);
std::string GetCurrentApiKey();

// Optional logger — set once on startup. Called from background thread; must be thread-safe.
void SetIqpLogger(std::function<void(std::string_view)> fn);

} // namespace TotalControl
