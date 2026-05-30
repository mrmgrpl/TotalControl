#include "IqpClient.h"
#include <windows.h>
#include <winhttp.h>
#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

namespace TotalControl {

// Public JS key embedded in the site's frontend — may need updating if site redeploys.
static constexpr const char* kApiKey =
    "1b2a1f32490874fb53d006c7dadef27603a24d4c573e018f1ffe1ae78b9ca768f"
    "38d01567461389badae9b8e9d0c8ed63ace08bd7912f3becead5d240c997c5f";

// ─── String helpers ───────────────────────────────────────────────────────────

// Extract a JSON string value, properly unescaping \", \\, \n, \uXXXX.
static std::string JsonStr(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\":\"";
    auto p = json.find(needle);
    if (p == std::string::npos) return {};
    p += needle.size();

    std::string out;
    out.reserve(4096);
    bool esc = false;
    while (p < json.size()) {
        char c = json[p++];
        if (esc) {
            switch (c) {
            case '"': case '\\': out += c; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': p += 4; break;   // skip \uXXXX — not needed for time parsing
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

// Extract time string "HH:MM:SS[.S]" for a named event (C1/C2/C3/C4/Max) from
// the HTML table in message1.
// Row pattern: <th>EVENT</th><th>DATE</th><th>TIME</th><th>ALT</th><th>AZM</th>
static std::string EventTime(const std::string& html, const char* event) {
    std::string pat = std::string("<th>") + event + "</th>";
    auto p = html.find(pat);
    if (p == std::string::npos) return {};
    p += pat.size();

    // skip date field
    auto t2 = html.find("<th>", p);     if (t2 == std::string::npos) return {};
    auto e2 = html.find("</th>", t2+4); if (e2 == std::string::npos) return {};

    // time field
    auto t3 = html.find("<th>", e2+5);  if (t3 == std::string::npos) return {};
    t3 += 4;
    auto e3 = html.find("</th>", t3);   if (e3 == std::string::npos) return {};

    return html.substr(t3, e3 - t3);
}

// Parse "Times in UTC±N" → returns the integer offset N (e.g. -1, +2).
// UTC = displayed_time - offset
static int UtcOffset(const std::string& html) {
    auto p = html.find("Times in UTC");
    if (p == std::string::npos) return 0;
    p += 12;
    if (p >= html.size()) return 0;
    int sign = 0;
    if      (html[p] == '+') { sign =  1; ++p; }
    else if (html[p] == '-') { sign = -1; ++p; }
    else return 0;
    int n = 0;
    while (p < html.size() && html[p] >= '0' && html[p] <= '9')
        n = n * 10 + (html[p++] - '0');
    return sign * n;
}

// Parse eclipse type string from first info row.
// "<th>TOTAL ECLIPSE</th>" → "TOTAL ECLIPSE"
static std::string EclipseType(const std::string& html) {
    static const char* kTypes[] = {
        "TOTAL ECLIPSE", "ANNULAR ECLIPSE", "HYBRID ECLIPSE", "PARTIAL ECLIPSE", nullptr
    };
    for (int i = 0; kTypes[i]; ++i)
        if (html.find(kTypes[i]) != std::string::npos) return kTypes[i];
    return {};
}

// Parse duration string "Duration: Xm YY.Ys" → "Xm YY.Ys"
static std::string ParseDuration(const std::string& html) {
    const char* needle = "Duration: ";
    auto p = html.find(needle);
    if (p == std::string::npos) return {};
    p += strlen(needle);
    auto e = html.find("</th>", p);
    if (e == std::string::npos) e = html.find('<', p);
    if (e == std::string::npos) return {};
    return html.substr(p, e - p);
}

// Convert a displayed time string "HH:MM:SS[.S]" on a given UTC date to UTC ms.
// utcOffsetHours: value from "Times in UTC±N" — displayed = UTC + offset.
static int64_t ToUtcMs(const std::string& timeStr, int year, int month, int day,
                        int utcOffsetHours) {
    int hh = 0, mm = 0;
    float fs = 0.f;
    if (sscanf_s(timeStr.c_str(), "%d:%d:%f", &hh, &mm, &fs) < 2) return -1;

    using namespace std::chrono;
    auto ymd = year_month_day{ std::chrono::year(year),
                               std::chrono::month(static_cast<unsigned>(month)),
                               std::chrono::day(static_cast<unsigned>(day)) };
    int64_t dateMs = duration_cast<milliseconds>(
                         sys_days{ymd}.time_since_epoch()).count();
    int64_t locMs  = (int64_t(hh) * 3600 + int64_t(mm) * 60) * 1000
                   + static_cast<int64_t>(fs * 1000.f);
    return dateMs + locMs - int64_t(utcOffsetHours) * 3600000LL;
}

// ─── WinHTTP GET ──────────────────────────────────────────────────────────────

static std::string HttpsGet(const wchar_t* host, const std::wstring& path) {
    HINTERNET hSes = WinHttpOpen(L"TotalControl/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) return {};

    HINTERNET hCon = WinHttpConnect(hSes, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hCon) { WinHttpCloseHandle(hSes); return {}; }

    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path.c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes); return {}; }

    // Add headers matching the browser request observed
    WinHttpAddRequestHeaders(hReq,
        L"X-Requested-With: XMLHttpRequest\r\n"
        L"Referer: https://maps.besselianelements.com/\r\n",
        (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

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
    return body;
}

// ─── Public API ───────────────────────────────────────────────────────────────

std::string BuildEclipseId(char typeChar, int year, int month, int day) {
    const char* prefix = "TSE";
    if (typeChar == 'A') prefix = "ASE";
    else if (typeChar == 'H') prefix = "HSE";
    else if (typeChar == 'P') prefix = "PSE";
    char buf[16];
    snprintf(buf, sizeof(buf), "%s%04d%02d%02d", prefix, year, month, day);
    return buf;
}

ContactTimes FetchContactTimes(const std::string& eclipseId,
                               double lat, double lon,
                               int year, int month, int day) {
    ContactTimes result;

    // Build query path (ASCII only — safe wstring cast)
    std::string pathA = std::format(
        "/api/local_circumstances_map/{0}/?latitude={1:.6f}&longitude={2:.6f}&key={3}",
        eclipseId, lat, lon, kApiKey);
    std::wstring path(pathA.begin(), pathA.end());

    std::string body = HttpsGet(L"maps.besselianelements.com", path);
    if (body.empty()) return result;

    // Verify "message":"OK"
    auto msg = JsonStr(body, "message");
    if (msg != "OK") return result;

    std::string html = JsonStr(body, "message1");
    if (html.empty()) return result;

    int off = UtcOffset(html);

    auto toMs = [&](const char* ev) {
        return ToUtcMs(EventTime(html, ev), year, month, day, off);
    };

    result.c1Ms     = toMs("C1");
    result.c2Ms     = toMs("C2");
    result.c3Ms     = toMs("C3");
    result.c4Ms     = toMs("C4");
    result.eclType  = EclipseType(html);
    result.duration = ParseDuration(html);
    result.valid    = result.c1Ms > 0;

    return result;
}

} // namespace TotalControl
