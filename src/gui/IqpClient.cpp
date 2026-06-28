#include "IqpClient.h"
#include <windows.h>
#include <cassert>
#include <winhttp.h>
#include <chrono>
#include <cstdio>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace TotalControl {

// Compile-time fallback key (last known good value).
static constexpr const char* kApiKeyDefault =
    "be98861b670a78e46d64f54b4e1d11f37fcf6f5f65788604caf9cf275d0e89d9"
    "8c49c0fcc366e96c71b46b15051e9f4ea1f649180da4f4880bf11b460a4e82a1";

static std::string   s_apiKey   = kApiKeyDefault;  // classic IQP key (128 hex chars)
static std::string   s_beApiKey;                    // dedicated BE REST key (40 hex chars)
static std::mutex    s_keyMutex;
static std::function<void(std::string_view)> s_logger;

static void IqpLog(std::string msg) {
    if (s_logger) s_logger(msg);
}

// Returns first N + "..." + last N chars of a long string (for key display).
static std::string Truncate(const std::string& s, size_t n = 8) {
    if (s.size() <= n * 2) return s;
    return s.substr(0, n) + "..." + s.substr(s.size() - n);
}

// ─── String helpers ───────────────────────────────────────────────────────────

// Extract a JSON string value, properly unescaping \", \\, \n, \uXXXX.
static std::string JsonStr(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\":";
    auto p = json.find(needle);
    if (p == std::string::npos) return {};
    p += needle.size();
    while (p < json.size() && json[p] == ' ') ++p;  // skip optional whitespace after ":"
    if (p >= json.size() || json[p] != '"') return {};
    ++p;

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

// extraHeaders: additional headers in "Name: value\r\n" format, or nullptr.
// iqpReferer: when true, adds the classic IQP X-Requested-With/Referer headers.
static std::string HttpsGet(const wchar_t* host, const std::string& path,
                             const wchar_t* extraHeaders = nullptr,
                             bool iqpReferer = false) {
    assert(host && host[0] != L'\0');
    // URL paths are always ASCII — safe narrow→wide conversion
    std::wstring wpath(path.begin(), path.end());

    HINTERNET hSes = WinHttpOpen(L"TotalControl/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) {
        IqpLog(std::format("IQP GET {} -> WinHttpOpen failed ({})", path, GetLastError()));
        return {};
    }

    HINTERNET hCon = WinHttpConnect(hSes, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hCon) {
        IqpLog(std::format("IQP GET {} -> WinHttpConnect failed ({})", path, GetLastError()));
        WinHttpCloseHandle(hSes); return {};
    }

    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", wpath.c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!hReq) {
        IqpLog(std::format("IQP GET {} -> WinHttpOpenRequest failed ({})", path, GetLastError()));
        WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes); return {};
    }

    if (iqpReferer) {
        WinHttpAddRequestHeaders(hReq,
            L"X-Requested-With: XMLHttpRequest\r\n"
            L"Referer: https://maps.besselianelements.com/\r\n",
            (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (extraHeaders) {
        WinHttpAddRequestHeaders(hReq, extraHeaders, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
           && WinHttpReceiveResponse(hReq, nullptr);

    DWORD httpStatus = 0;
    if (ok) {
        DWORD len = sizeof(httpStatus);
        WinHttpQueryHeaders(hReq,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &httpStatus, &len, WINHTTP_NO_HEADER_INDEX);
    }

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

    if (!ok) {
        IqpLog(std::format("IQP GET {} -> request failed ({})", path, GetLastError()));
    } else {
        IqpLog(std::format("IQP GET {} -> HTTP {} ({} bytes)", path, httpStatus, body.size()));
        if (!body.empty()) {
            std::string snippet = body.substr(0, std::min(body.size(), size_t{600}));
            IqpLog(std::format("IQP body[0:600]: {}", snippet));
        }
    }

    return body;
}

// ─── Dedicated BE REST API ─────────────────────────────────────────────────────

// Parse ISO 8601 UTC string "YYYY-MM-DDTHH:MM:SS[.S*]Z" to UTC ms since epoch.
// Returns -1 on failure.
static int64_t ParseIso8601Ms(const std::string& json, const char* key) {
    std::string s = JsonStr(json, key);
    if (s.empty()) return -1;
    int yr = 0, mo = 0, dy = 0, hh = 0, mm = 0;
    float ss = 0.f;
    if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%f", &yr, &mo, &dy, &hh, &mm, &ss) < 5) return -1;
    using namespace std::chrono;
    auto ymd = year_month_day{ year(yr), month(static_cast<unsigned>(mo)),
                               day(static_cast<unsigned>(dy)) };
    int64_t dateMs = duration_cast<milliseconds>(sys_days{ymd}.time_since_epoch()).count();
    int64_t timeMs = (int64_t(hh) * 3600 + int64_t(mm) * 60) * 1000
                   + static_cast<int64_t>(ss * 1000.f + 0.5f);
    return dateMs + timeMs;
}

static ContactTimes FetchBesselApiTimes(const std::string& eclipseId,
                                        double lat, double lon, int altM,
                                        const std::string& apiKey) {
    assert(!eclipseId.empty());
    assert(!apiKey.empty());
    ContactTimes result;

    // Build path — ASCII only, safe narrow→wide in HttpsGet
    std::string path = std::format(
        "/v1/eclipse?eclipse={}&latitude={:.6f}&longitude={:.6f}&altitude={}",
        eclipseId, lat, lon, altM);

    // Build x-api-key header (key is ASCII hex — safe narrow→wide)
    std::wstring hdr = L"x-api-key: " + std::wstring(apiKey.begin(), apiKey.end()) + L"\r\n";

    IqpLog(std::format("BE-API fetch: {} lat={:.4f} lon={:.4f} alt={}m  key={}",
                       eclipseId, lat, lon, altM, Truncate(apiKey)));

    static constexpr wchar_t kBeHost[] =
        L"tryjhlq5f5.execute-api.eu-west-1.amazonaws.com";

    std::string body = HttpsGet(kBeHost, path, hdr.c_str(), false);
    if (body.empty()) {
        IqpLog("BE-API: empty response");
        return result;
    }

    // Log full body (small JSON, usually < 500 bytes)
    IqpLog(std::format("BE-API raw: {}", body));

    // Check for API-level error fields
    std::string errMsg = JsonStr(body, "message");
    if (errMsg.empty()) errMsg = JsonStr(body, "error");
    if (!errMsg.empty() && errMsg != "OK") {
        IqpLog(std::format("BE-API error: \"{}\"", errMsg));
        return result;
    }

    // Parse contact times as ISO 8601 UTC strings
    result.c1Ms  = ParseIso8601Ms(body, "c1");
    result.c2Ms  = ParseIso8601Ms(body, "c2");
    result.c3Ms  = ParseIso8601Ms(body, "c3");
    result.c4Ms  = ParseIso8601Ms(body, "c4");
    result.maxMs = ParseIso8601Ms(body, "max");

    // Duration and eclipse type (best-effort; field names may vary)
    result.duration = JsonStr(body, "duration");
    std::string typeStr = JsonStr(body, "type");
    if (typeStr.empty()) typeStr = JsonStr(body, "eclipse_type");
    if (!typeStr.empty()) {
        // Normalise to uppercase to match existing display code
        for (char& c : typeStr) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        result.eclType = typeStr;
    }

    result.apiOk  = true;
    result.valid  = result.c1Ms > 0;
    result.source = ContactSource::BesselApi;

    IqpLog(std::format("BE-API: done — valid={} C1={}ms C2={}ms duration=\"{}\"",
                       result.valid, result.c1Ms, result.c2Ms, result.duration));
    return result;
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

// Fetch one map page and its JS files; extract the API key.
// Returns true and updates s_apiKey on success.
static bool TryRefreshFromPage(const std::string& eclipseId) {
    IqpLog(std::format("IQP key refresh: fetching map page for {}", eclipseId));
    std::string mapPath = "/map/" + eclipseId + "/";
    std::string html = HttpsGet(L"maps.besselianelements.com", mapPath);
    if (html.empty()) {
        IqpLog("IQP key refresh: map page empty");
        return false;
    }

    // Check inline scripts first
    {
        auto k = ExtractKeyFromContent(html);
        if (!k.empty()) {
            IqpLog(std::format("IQP key refresh: found key in inline script: {}", Truncate(k)));
            std::lock_guard lk(s_keyMutex);
            s_apiKey = k;
            return true;
        }
    }

    // Collect <script src="..."> paths
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
                    if (!url.empty() && url[0] == '/')
                        jsPaths.push_back(url);
                }
            }
        }
        p = (endTag != std::string::npos) ? endTag + 1 : p + 1;
    }

    // Fetch each JS and search for the key
    for (auto& path : jsPaths) {
        std::string js = HttpsGet(L"maps.besselianelements.com", path);
        if (js.empty()) continue;
        auto k = ExtractKeyFromContent(js);
        if (!k.empty()) {
            IqpLog(std::format("IQP key refresh: found key in JS {}: {}", path, Truncate(k)));
            std::lock_guard lk(s_keyMutex);
            s_apiKey = k;
            return true;
        }
    }
    IqpLog(std::format("IQP key refresh: key not found in {} JS files for {}", jsPaths.size(), eclipseId));
    return false;
}

// Try the queried eclipse first, then fall back to TSE20260812 (known-good map page).
static bool RefreshApiKey(const std::string& eclipseId) {
    if (TryRefreshFromPage(eclipseId)) return true;
    static const std::string kFallback = "TSE20260812";
    if (eclipseId != kFallback) {
        IqpLog(std::format("IQP key refresh: trying fallback page {}", kFallback));
        if (TryRefreshFromPage(kFallback)) return true;
    }
    IqpLog("IQP key refresh: FAILED — key not found on any page");
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

void SetIqpLogger(std::function<void(std::string_view)> fn) {
    s_logger = std::move(fn);
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

void SetBeApiKey(const std::string& key) {
    std::lock_guard lk(s_keyMutex);
    s_beApiKey = key;
}

std::string GetBeApiKey() {
    std::lock_guard lk(s_keyMutex);
    return s_beApiKey;
}

static std::string BuildPath(const std::string& eclipseId, double lat, double lon) {
    std::string key;
    { std::lock_guard lk(s_keyMutex); key = s_apiKey; }
    return std::format(
        "/api/local_circumstances_map/{0}/?latitude={1:.6f}&longitude={2:.6f}&key={3}",
        eclipseId, lat, lon, key);
}

ContactTimes FetchContactTimes(const std::string& eclipseId,
                               double lat, double lon, int altM,
                               int year, int month, int day) {
    assert(!eclipseId.empty());
    assert(lat   >= -90.0  && lat  <= 90.0);
    assert(lon   >= -180.0 && lon  <= 180.0);
    assert(year  >= 1900   && year <= 2200);
    assert(month >= 1      && month <= 12);
    assert(day   >= 1      && day  <= 31);

    // Dedicated BE REST API takes priority when key is set
    {
        std::string beKey;
        { std::lock_guard lk(s_keyMutex); beKey = s_beApiKey; }
        if (!beKey.empty()) {
            auto ct = FetchBesselApiTimes(eclipseId, lat, lon, altM, beKey);
            if (ct.apiOk) return ct;
            IqpLog("BE-API failed — falling back to classic IQP");
        }
    }

    ContactTimes result;

    {
        std::lock_guard lk(s_keyMutex);
        IqpLog(std::format("IQP fetch: {} lat={:.4f} lon={:.4f}  key={}",
                           eclipseId, lat, lon, Truncate(s_apiKey)));
    }

    std::string body = HttpsGet(L"maps.besselianelements.com",
                                 BuildPath(eclipseId, lat, lon),
                                 nullptr, true);
    if (body.empty()) { IqpLog("IQP: empty response, aborting"); return result; }

    auto msg = JsonStr(body, "message");
    IqpLog(std::format("IQP: message=\"{}\"", msg));

    // Success: msg absent ("") or explicit "OK".
    // Key error: msg non-empty and contains "Key"/"key" — refresh and retry once.
    // Other errors (Limit Error, etc.): abort without retry.
    bool isOk = msg.empty() || msg == "OK";
    if (!isOk) {
        bool isKeyError = msg.find("Key") != std::string::npos
                       || msg.find("key") != std::string::npos;
        if (isKeyError) {
            IqpLog(std::format("IQP: key error \"{}\" — refreshing key", msg));
            if (RefreshApiKey(eclipseId)) {
                {
                    std::lock_guard lk(s_keyMutex);
                    IqpLog(std::format("IQP retry with new key={}", Truncate(s_apiKey)));
                }
                body = HttpsGet(L"maps.besselianelements.com",
                                BuildPath(eclipseId, lat, lon));
                msg = JsonStr(body, "message");
                IqpLog(std::format("IQP: message after retry=\"{}\"", msg));
                isOk = msg.empty() || msg == "OK";
            } else {
                IqpLog("IQP: key refresh failed, aborting");
                return result;
            }
        }
        if (!isOk) {
            IqpLog(std::format("IQP: API error \"{}\", aborting", msg));
            return result;
        }
    }

    std::string html = JsonStr(body, "message1");
    if (html.empty()) { IqpLog("IQP: message1 empty"); return result; }

    result.apiOk   = true;
    result.eclType = EclipseType(html);
    IqpLog(std::format("IQP: eclType=\"{}\"", result.eclType));

    // "NO ECLIPSE" — API OK but nothing to see at this location
    if (result.eclType == "NO ECLIPSE") return result;

    int64_t offSec = UtcOffsetSeconds(html, year, month, day);

    auto toMs = [&](const char* ev) {
        return ToUtcMs(EventTime(html, ev), year, month, day, offSec);
    };

    result.c1Ms     = toMs("C1");
    result.c2Ms     = toMs("C2");
    result.c3Ms     = toMs("C3");
    result.c4Ms     = toMs("C4");
    result.maxMs    = toMs("Max");
    result.duration = ParseDuration(html);
    result.valid    = result.c1Ms > 0;
    result.source   = ContactSource::IQP;

    IqpLog(std::format("IQP: done — valid={} C1={} duration=\"{}\"",
                       result.valid, result.c1Ms, result.duration));
    return result;
}

} // namespace TotalControl
