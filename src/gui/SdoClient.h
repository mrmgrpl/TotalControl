#pragma once
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace TotalControl {

// Download the latest SDO HMI Intensitygram (HMIIC) JPEG from NASA.
// URL: https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_HMIIC.jpg
// Returns raw JPEG bytes on success, empty vector on failure.
std::vector<uint8_t> FetchSdoJpeg(
    const std::function<void(std::string_view)>& logger = nullptr);

} // namespace TotalControl
