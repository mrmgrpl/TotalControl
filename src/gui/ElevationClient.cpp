#include "ElevationClient.h"
#include <windows.h>
#include <winhttp.h>
#include <cassert>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

namespace TotalControl {

// ─── WinHTTP GET (mirrors IqpClient/EphClient HttpsGet) ────────────────────────
static std::string HttpsGet(const wchar_t* host, const std::string& path,
                             const std::function<void(std::string_view)>& logger) {
    assert(host && host[0] != L'\0');
    std::wstring wpath(path.begin(), path.end());

    HINTERNET hSes = WinHttpOpen(L"TotalControlElevation/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) {
        if (logger) logger("ELEV: WinHttpOpen failed");
        return {};
    }

    HINTERNET hCon = WinHttpConnect(hSes, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hCon) {
        if (logger) logger("ELEV: WinHttpConnect failed");
        WinHttpCloseHandle(hSes);
        return {};
    }

    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", wpath.c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!hReq) {
        if (logger) logger("ELEV: WinHttpOpenRequest failed");
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return {};
    }

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
           && WinHttpReceiveResponse(hReq, nullptr);

    std::string body;
    if (ok) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (WinHttpReadData(hReq, buf.data(), avail, &read))
                body.append(buf.data(), read);
        }
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    WinHttpCloseHandle(hSes);

    if (logger)
        logger(std::format("ELEV: GET {} -> {} ({} bytes)",
                            path, ok ? "OK" : "FAIL", body.size()));
    return body;
}

// Extract the first numeric value following "key": — flat response, no nesting.
static bool JsonNum(const std::string& json, const char* key, double& out) {
    std::string needle = std::string("\"") + key + "\":";
    auto p = json.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    while (p < json.size() && json[p] == ' ') ++p;
    if (p >= json.size()) return false;

    char* end = nullptr;
    double v = std::strtod(json.c_str() + p, &end);
    if (end == json.c_str() + p) return false;
    out = v;
    return true;
}

// ─── Public API ───────────────────────────────────────────────────────────────

ElevationResult FetchElevationM(double lat, double lon,
                                 std::function<void(std::string_view)> logger) {
    assert(lat >= -90.0  && lat <= 90.0);
    assert(lon >= -180.0 && lon <= 180.0);

    ElevationResult result;
    std::string path = std::format("/api/v1/lookup?locations={:.6f},{:.6f}", lat, lon);

    std::string body = HttpsGet(L"api.open-elevation.com", path, logger);
    if (body.empty()) return result;

    double elev = 0.0;
    if (!JsonNum(body, "elevation", elev)) {
        if (logger) logger("ELEV: no \"elevation\" field in response");
        return result;
    }

    result.ok    = true;
    result.elevM = elev;
    return result;
}

} // namespace TotalControl
