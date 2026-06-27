#pragma once
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace TotalControl {

// Generic single-shot WinHTTP HTTPS GET.
// Returns raw response bytes (up to maxBytes), or empty on error / non-200 status.
std::vector<uint8_t> FetchHttpsBytes(
    const wchar_t* host,
    const wchar_t* path,
    int maxBytes = 4 * 1024 * 1024,
    const std::function<void(std::string_view)>& logger = nullptr);

} // namespace TotalControl
