#pragma once
#include <functional>
#include <string_view>

namespace TotalControl {

struct ElevationResult {
    bool   ok    = false;
    double elevM = 0.0;
};

// Synchronous HTTPS GET to Open-Elevation.com (free, no API key required).
// Call from a background thread — blocks until the response arrives.
ElevationResult FetchElevationM(double lat, double lon,
                                 std::function<void(std::string_view)> logger = {});

} // namespace TotalControl
