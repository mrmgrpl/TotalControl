#pragma once
#include <functional>
#include <string>
#include <string_view>

namespace TotalControl {

struct GeocodeResult {
    bool        ok = false;
    std::string name;   // OSM Nominatim "display_name"
};

// Synchronous reverse-geocode via OpenStreetMap Nominatim (free, no API key).
// Call from a background thread — blocks until the response arrives.
GeocodeResult ReverseGeocode(double lat, double lon,
                              std::function<void(std::string_view)> logger = {});

} // namespace TotalControl
