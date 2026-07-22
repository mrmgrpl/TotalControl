#pragma once
#include <functional>
#include <string_view>

namespace TotalControl {

struct DeltaTResult {
    bool   ok            = false;
    double deltaTSeconds = 0.0;
    bool   predicted      = false;  // true = IERS Bulletin A forecast, false = Bulletin B measured
};

// Downloads the IERS Earth-orientation bulletin (finals.all.iau2000.txt) and
// extracts UT1-UTC for the given calendar date (UTC), converting to
// Delta T = TT - UT1 = 32.184 + leap_seconds - UT1UTC.
//
// This is the same quantity besselianelements.com documents needing
// "periodic updates" as the eclipse date approaches — Earth's actual
// rotation isn't known years in advance, so IERS revises its UT1-UTC
// forecast weekly/monthly (see Change log, Alessandro/besselianelements.com
// report 2026-07-21: Espenak's static catalog Delta T for TSE2026 was found
// to be 6s stale).
//
// Blocks until the ~3.6 MB file is downloaded and parsed — call from a
// background thread. Returns ok=false if the date isn't covered by the file
// (too far in the past/future of IERS's published range) or the HTTP fetch
// fails outright.
DeltaTResult FetchDeltaT(int year, int month, int day,
                         std::function<void(std::string_view)> logger = {});

} // namespace TotalControl
