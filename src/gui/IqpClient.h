#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace TotalControl {

enum class ContactSource { None, IQP, BesselApi, Besselian };

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

// Synchronous HTTPS GET for contact times.
// Tries the dedicated BE REST API first (when be_api_key is set), then falls
// back to the classic maps.besselianelements.com scraping API.
// Call from a background thread — blocks until response arrives (typically <3 s).
ContactTimes FetchContactTimes(const std::string& eclipseId,
                               double lat, double lon, int altM,
                               int year, int month, int day);

// Classic IQP key (maps.besselianelements.com, 128-char hex, auto-refreshed).
// Call SetApiKey() on startup with the value from config DB.
void        SetApiKey(const std::string& key);
std::string GetCurrentApiKey();

// Dedicated BE REST API key (40-char hex, user-supplied, never auto-refreshed).
// When non-empty, FetchContactTimes uses the AWS endpoint first.
void        SetBeApiKey(const std::string& key);
std::string GetBeApiKey();

// Optional logger — set once on startup. Called from background thread; must be thread-safe.
void SetIqpLogger(std::function<void(std::string_view)> fn);

} // namespace TotalControl
