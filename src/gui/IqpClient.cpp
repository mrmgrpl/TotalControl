#include "IqpClient.h"
#include <windows.h>
#include <winhttp.h>
#include <chrono>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace TotalControl {

// Compile-time fallback key (last known good value).
static constexpr const char* kApiKeyDefault =
    "be98861b670a78e46d64f54b4e1d11f37fcf6f5f65788604caf9cf275d0e89d9"
    "8c49c0fcc366e96c71b46b15051e9f4ea1f649180da4f4880bf11b460a4e82a1";

static std::string s_apiKey   = kApiKeyDefault;
static std::mutex  s_keyMutex;

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

// Parse UTC offset in seconds from the footer of message1.
// Handles two formats emitted by the IQP API:
//   "Times in UTC-1."              → -3600
//   "Times in CEST [Europe/Warsaw]." → +7200 (via locate_zone for the eclipse date)
static int64_t UtcOffsetSeconds(const std::string& html, int year, int month, int day) {
    auto timesPos = html.find("Times in ");
    if (timesPos == std::string::npos) return 0;

    // Format 1: "Times in UTC±N"
    auto utcPos = html.find("Times in UTC", timesPos);
    if (utcPos != std::string::npos) {
        size_t p = utcPos + 12;
        if (p >= html.size()) return 0;
        int64_t sign = 0;
        if      (html[p] == '+') { sign =  1; ++p; }
        else if (html[p] == '-') { sign = -1; ++p; }
        else return 0;
        int64_t n = 0;
        while (p < html.size() && html[p] >= '0' && html[p] <= '9')
            n = n * 10 + (html[p++] - '0');
        return sign * n * 3600LL;
    }

    // Format 2: "Times in ABBR [IANA/Zone]." — extract IANA name and use chrono
    auto lb = html.find('[', timesPos);
    auto rb = (lb != std::string::npos) ? html.find(']', lb) : std::string::npos;
    if (lb == std::string::npos || rb == std::string::npos) return 0;

    std::string iana = html.substr(lb + 1, rb - lb - 1);
    using namespace std::chrono;
    try {
        const time_zone* tz = locate_zone(iana);
        auto ymd = year_month_day{ std::chrono::year(year),
                                   std::chrono::month(static_cast<unsigned>(month)),
                                   std::chrono::day(static_cast<unsigned>(day)) };
        sys_info info = tz->get_info(sys_days{ymd} + hours{12});
        return info.offset.count();   // seconds east of UTC
    } catch (...) { return 0; }
}

// Parse eclipse type string from first info row.
static std::string EclipseType(const std::string& html) {
    static const char* kTypes[] = {
        "TOTAL ECLIPSE", "ANNULAR ECLIPSE", "HYBRID ECLIPSE",
        "PARTIAL ECLIPSE", "NO ECLIPSE", nullptr
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
// utcOffsetSeconds: seconds east of UTC — displayed = UTC + offset.
static int64_t ToUtcMs(const std::string& timeStr, int year, int month, int day,
                        int64_t utcOffsetSeconds) {
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
    return dateMs + locMs - utcOffsetSeconds * 1000LL;
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

// ─── Key management ───────────────────────────────────────────────────────────

// Search content for a standalone 128-char hex string, preferably near "key".
static std::string ExtractKeyFromContent(const std::string& content) {
    static constexpr size_t kLen = 128;

    auto isHex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    auto isStandalone = [&](size_t pos) {
        if (pos > 0 && isHex(content[pos - 1])) return false;
        if (pos + kLen < content.size() && isHex(content[pos + kLen])) return false;
        return true;
    };
    auto allHex128 = [&](size_t pos) {
        if (pos + kLen > content.size()) return false;
        for (size_t k = pos; k < pos + kLen; ++k)
            if (!isHex(content[k])) return false;
        return true;
    };

    // Priority: search within 64 chars after any occurrence of "key"
    size_t p = 0;
    while ((p = content.find("key", p)) != std::string::npos) {
        size_t q = p + 3;
        size_t end = std::min(q + 64, content.size());
        while (q < end) {
            if (allHex128(q) && isStandalone(q))
                return content.substr(q, kLen);
            ++q;
        }
        ++p;
    }

    // Fallback: any standalone 128-char hex run in the content
    for (size_t i = 0; i + kLen <= content.size(); ++i) {
        if (isHex(content[i]) && allHex128(i) && isStandalone(i))
            return content.substr(i, kLen);
        // Skip over non-hex runs quickly
        if (!isHex(content[i])) continue;
    }
    return {};
}

// Fetch the map page, find JS files, extract the API key.
// Returns true and updates s_apiKey on success.
static bool RefreshApiKey() {
    // Step 1: fetch HTML of the map page
    std::string html = HttpsGet(L"maps.besselianelements.com",
                                 std::wstring(L"/map/TSE20260812/"));
    if (html.empty()) return false;

    // Check inline scripts first
    {
        auto k = ExtractKeyFromContent(html);
        if (!k.empty()) {
            std::lock_guard lk(s_keyMutex);
            s_apiKey = k;
            return true;
        }
    }

    // Step 2: collect <script src="..."> paths
    std::vector<std::string> jsPaths;
    size_t p = 0;
    while ((p = html.find("<script", p)) != std::string::npos) {
        size_t endTag = html.find('>', p);
        size_t srcPos = html.find("src=", p);
        if (srcPos != std::string::npos && srcPos < endTag) {
            size_t q = srcPos + 4;
            char d = html[q];
            if (d == '"' || d == '\'') {
                ++q;
                size_t e = html.find(d, q);
                if (e != std::string::npos) {
                    std::string url = html.substr(q, e - q);
                    // Keep only root-relative paths on the same host
                    if (!url.empty() && url[0] == '/')
                        jsPaths.push_back(url);
                }
            }
        }
        p = (endTag != std::string::npos) ? endTag + 1 : p + 1;
    }

    // Step 3: fetch each JS and search for the key
    for (auto& path : jsPaths) {
        std::wstring wpath(path.begin(), path.end());
        std::string js = HttpsGet(L"maps.besselianelements.com", wpath);
        if (js.empty()) continue;
        auto k = ExtractKeyFromContent(js);
        if (!k.empty()) {
            std::lock_guard lk(s_keyMutex);
            s_apiKey = k;
            return true;
        }
    }
    return false;
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

void SetApiKey(const std::string& key) {
    if (key.empty()) return;
    std::lock_guard lk(s_keyMutex);
    s_apiKey = key;
}

std::string GetCurrentApiKey() {
    std::lock_guard lk(s_keyMutex);
    return s_apiKey;
}

static std::wstring BuildPath(const std::string& eclipseId, double lat, double lon) {
    std::string key;
    { std::lock_guard lk(s_keyMutex); key = s_apiKey; }
    std::string p = std::format(
        "/api/local_circumstances_map/{0}/?latitude={1:.6f}&longitude={2:.6f}&key={3}",
        eclipseId, lat, lon, key);
    return std::wstring(p.begin(), p.end());
}

ContactTimes FetchContactTimes(const std::string& eclipseId,
                               double lat, double lon,
                               int year, int month, int day) {
    ContactTimes result;

    std::string body = HttpsGet(L"maps.besselianelements.com",
                                 BuildPath(eclipseId, lat, lon));

    // Auto-refresh key once on "Wrong Key" response
    if (body.find("\"Wrong Key\"") != std::string::npos) {
        if (RefreshApiKey()) {
            body = HttpsGet(L"maps.besselianelements.com",
                            BuildPath(eclipseId, lat, lon));
        }
    }
    if (body.empty()) return result;

    // Verify "message":"OK"
    auto msg = JsonStr(body, "message");
    if (msg != "OK") return result;

    std::string html = JsonStr(body, "message1");
    if (html.empty()) return result;

    result.apiOk   = true;
    result.eclType = EclipseType(html);

    // "NO ECLIPSE" — API OK but nothing to see at this location
    if (result.eclType == "NO ECLIPSE") return result;

    int64_t offSec = UtcOffsetSeconds(html, year, month, day);

    auto toMs = [&](const char* ev) {
        return ToUtcMs(EventTime(html, ev), year, month, day, offSec);
    };

    result.c1Ms     = toMs("C1");
    result.c2Ms     = toMs("C2");   // -1 for partial eclipse
    result.c3Ms     = toMs("C3");   // -1 for partial eclipse
    result.c4Ms     = toMs("C4");
    result.maxMs    = toMs("Max");
    result.duration = ParseDuration(html);
    result.valid    = result.c1Ms > 0;
    result.source   = ContactSource::IQP;

    return result;
}

} // namespace TotalControl
