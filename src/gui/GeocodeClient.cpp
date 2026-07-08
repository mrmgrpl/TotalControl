#include "GeocodeClient.h"
#include <windows.h>
#include <winhttp.h>
#include <cassert>
#include <format>
#include <string>
#include <vector>

namespace TotalControl {

// ─── WinHTTP GET (mirrors IqpClient/EphClient/ElevationClient HttpsGet) ────────
static std::string HttpsGet(const wchar_t* host, const std::string& path,
                             const std::function<void(std::string_view)>& logger) {
    assert(host && host[0] != L'\0');
    std::wstring wpath(path.begin(), path.end());

    HINTERNET hSes = WinHttpOpen(L"TotalControlGeocode/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) {
        if (logger) logger("GEOCODE: WinHttpOpen failed");
        return {};
    }

    HINTERNET hCon = WinHttpConnect(hSes, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hCon) {
        if (logger) logger("GEOCODE: WinHttpConnect failed");
        WinHttpCloseHandle(hSes);
        return {};
    }

    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", wpath.c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!hReq) {
        if (logger) logger("GEOCODE: WinHttpOpenRequest failed");
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
        logger(std::format("GEOCODE: GET {} -> {} ({} bytes)",
                            path, ok ? "OK" : "FAIL", body.size()));
    return body;
}

// Extract a JSON string value, unescaping \", \\, \n, \uXXXX (mirrors
// IqpClient's JsonStr — each network client keeps its own tiny parser).
static std::string JsonStr(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\":";
    auto p = json.find(needle);
    if (p == std::string::npos) return {};
    p += needle.size();
    while (p < json.size() && json[p] == ' ') ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;

    std::string out;
    out.reserve(128);
    bool esc = false;
    while (p < json.size()) {
        char c = json[p++];
        if (esc) {
            switch (c) {
            case '"': case '\\': out += c; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': p += 4; break;
            default:  out += c; break;
            }
            esc = false;
        } else if (c == '\\') {
            esc = true;
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

GeocodeResult ReverseGeocode(double lat, double lon,
                              std::function<void(std::string_view)> logger) {
    assert(lat >= -90.0  && lat <= 90.0);
    assert(lon >= -180.0 && lon <= 180.0);

    // zoom=10 (Nominatim's own scale) only resolves to city level, so
    // nearby-but-distinct points within the same town all return the same
    // display_name — not a rate-limit issue, just too coarse a granularity.
    // zoom=16 resolves to street level, distinguishing small position
    // corrections (tens to low hundreds of metres).
    GeocodeResult result;
    std::string path = std::format(
        "/reverse?format=jsonv2&lat={:.6f}&lon={:.6f}&zoom=16&accept-language=en",
        lat, lon);

    std::string body = HttpsGet(L"nominatim.openstreetmap.org", path, logger);
    if (body.empty()) return result;

    std::string name = JsonStr(body, "display_name");
    if (name.empty()) {
        if (logger) logger("GEOCODE: no \"display_name\" field in response");
        return result;
    }

    result.ok   = true;
    result.name = name;
    return result;
}

} // namespace TotalControl
