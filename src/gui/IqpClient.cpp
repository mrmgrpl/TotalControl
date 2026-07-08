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

static std::string   s_beApiKey;   // dedicated BE REST key (40 hex chars, user-supplied)
static std::mutex    s_keyMutex;
static std::function<void(std::string_view)> s_logger;

static void IqpLog(std::string msg) {
    if (s_logger) s_logger(msg);
}

static std::string Truncate(const std::string& s, size_t n = 8) {
    if (s.size() <= n * 2) return s;
    return s.substr(0, n) + "..." + s.substr(s.size() - n);
}

// ─── JSON helper ──────────────────────────────────────────────────────────────

// Extract a JSON string value, properly unescaping \", \\, \n, \uXXXX.
static std::string JsonStr(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\":";
    auto p = json.find(needle);
    if (p == std::string::npos) return {};
    p += needle.size();
    while (p < json.size() && json[p] == ' ') ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;

    std::string out;
    out.reserve(256);
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

// ─── WinHTTP GET ──────────────────────────────────────────────────────────────

// extraHeaders: "Name: value\r\n" string, or nullptr.
static std::string HttpsGet(const wchar_t* host, const std::string& path,
                             const wchar_t* extraHeaders = nullptr) {
    assert(host && host[0] != L'\0');
    std::wstring wpath(path.begin(), path.end());

    HINTERNET hSes = WinHttpOpen(L"TotalControl/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) {
        IqpLog(std::format("HTTP GET {} -> WinHttpOpen failed ({})", path, GetLastError()));
        return {};
    }

    HINTERNET hCon = WinHttpConnect(hSes, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hCon) {
        IqpLog(std::format("HTTP GET {} -> WinHttpConnect failed ({})", path, GetLastError()));
        WinHttpCloseHandle(hSes); return {};
    }

    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", wpath.c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!hReq) {
        IqpLog(std::format("HTTP GET {} -> WinHttpOpenRequest failed ({})", path, GetLastError()));
        WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes); return {};
    }

    if (extraHeaders)
        WinHttpAddRequestHeaders(hReq, extraHeaders, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

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
        IqpLog(std::format("HTTP GET {} -> failed ({})", path, GetLastError()));
    } else {
        IqpLog(std::format("HTTP GET {} -> {} ({} bytes)", path, httpStatus, body.size()));
        if (!body.empty()) {
            std::string snippet = body.substr(0, std::min(body.size(), size_t{600}));
            IqpLog(std::format("body[0:600]: {}", snippet));
        }
    }

    return body;
}

// ─── IQP REST API ─────────────────────────────────────────────────────────────

// Actual response format (discovered from log):
// {
//   "eclipse_type": "TOTAL ECLIPSE",
//   "duration": "1m 44.2s",
//   "eclipse_events": [
//     {"event_type":"c1",  "utc_date":"2026-08-12", "utc_time":"17:33:54"},
//     {"event_type":"c2",  "utc_date":"2026-08-12", "utc_time":"18:28:53.700000"},
//     {"event_type":"max", "utc_date":"2026-08-12", "utc_time":"18:29:46"},
//     {"event_type":"c3",  "utc_date":"2026-08-12", "utc_time":"18:30:37.900000"},
//     {"event_type":"c4",  "utc_date":"2026-08-12", "utc_time":"19:22:12"}
//   ]
// }

// Convert separate date "YYYY-MM-DD" + time "HH:MM:SS[.S*]" → UTC ms since epoch.
static int64_t DateTimeToUtcMs(const std::string& date, const std::string& time) {
    int yr = 0, mo = 0, dy = 0, hh = 0, mm = 0;
    float ss = 0.f;
    if (sscanf_s(date.c_str(), "%d-%d-%d", &yr, &mo, &dy) < 3) return -1;
    if (sscanf_s(time.c_str(), "%d:%d:%f",  &hh, &mm, &ss) < 2) return -1;
    using namespace std::chrono;
    auto ymd = year_month_day{ year(yr), month(static_cast<unsigned>(mo)),
                               day(static_cast<unsigned>(dy)) };
    int64_t dateMs = duration_cast<milliseconds>(sys_days{ymd}.time_since_epoch()).count();
    int64_t timeMs = (int64_t(hh) * 3600 + int64_t(mm) * 60) * 1000
                   + static_cast<int64_t>(ss * 1000.f + 0.5f);
    return dateMs + timeMs;
}

// Parse eclipse_events array; set c1/c2/c3/c4/max from matching event_type entries.
// Objects have no nesting so simple {…} scan works.
static void ParseEclipseEvents(const std::string& body, ContactTimes& out) {
    auto arrPos = body.find("\"eclipse_events\"");
    if (arrPos == std::string::npos) return;
    auto arrStart = body.find('[', arrPos);
    if (arrStart == std::string::npos) return;

    size_t pos = arrStart + 1;
    static constexpr int kMaxEvents = 16;
    for (int ev = 0; ev < kMaxEvents; ++ev) {
        size_t objStart = body.find('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd   = body.find('}', objStart);
        if (objEnd   == std::string::npos) break;
        pos = objEnd + 1;

        std::string obj = body.substr(objStart, objEnd - objStart + 1);
        std::string evType = JsonStr(obj, "event_type");
        std::string utcDate = JsonStr(obj, "utc_date");
        std::string utcTime = JsonStr(obj, "utc_time");
        if (evType.empty() || utcDate.empty() || utcTime.empty()) continue;

        int64_t ms = DateTimeToUtcMs(utcDate, utcTime);
        if      (evType == "c1")  out.c1Ms  = ms;
        else if (evType == "c2")  out.c2Ms  = ms;
        else if (evType == "c3")  out.c3Ms  = ms;
        else if (evType == "c4")  out.c4Ms  = ms;
        else if (evType == "max") out.maxMs = ms;

        if (pos >= body.size() || body[pos] == ']') break;
    }
}

static ContactTimes FetchBesselApiTimes(const std::string& eclipseId,
                                        double lat, double lon, int altM,
                                        const std::string& apiKey) {
    assert(!eclipseId.empty());
    assert(!apiKey.empty());
    ContactTimes result;

    std::string path = std::format(
        "/v1/eclipse?eclipse={}&latitude={:.6f}&longitude={:.6f}&altitude={}",
        eclipseId, lat, lon, altM);

    // x-api-key header — key is ASCII hex, safe narrow→wide
    std::wstring hdr = L"x-api-key: " + std::wstring(apiKey.begin(), apiKey.end()) + L"\r\n";

    IqpLog(std::format("IQP-API fetch: {} lat={:.4f} lon={:.4f} alt={}m  key={}",
                       eclipseId, lat, lon, altM, Truncate(apiKey)));

    static constexpr wchar_t kHost[] =
        L"tryjhlq5f5.execute-api.eu-west-1.amazonaws.com";

    std::string body = HttpsGet(kHost, path, hdr.c_str());
    if (body.empty()) { IqpLog("IQP-API: empty response"); return result; }

    IqpLog(std::format("IQP-API raw: {}", body));

    // Check for API-level error
    std::string errMsg = JsonStr(body, "message");
    if (errMsg.empty()) errMsg = JsonStr(body, "error");
    if (!errMsg.empty() && errMsg != "OK") {
        IqpLog(std::format("IQP-API error: \"{}\"", errMsg));
        return result;
    }

    // Top-level fields
    result.duration = JsonStr(body, "duration");
    result.eclType  = JsonStr(body, "eclipse_type");

    // Contact times from eclipse_events array
    ParseEclipseEvents(body, result);

    result.apiOk  = true;
    result.valid  = result.c1Ms > 0;
    result.source = ContactSource::BesselApi;

    IqpLog(std::format("IQP-API: done — valid={} C1={}ms C2={}ms duration=\"{}\"",
                       result.valid, result.c1Ms, result.c2Ms, result.duration));
    return result;
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

void SetBeApiKey(const std::string& key) {
    std::lock_guard lk(s_keyMutex);
    s_beApiKey = key;
}

std::string GetBeApiKey() {
    std::lock_guard lk(s_keyMutex);
    return s_beApiKey;
}

ContactTimes FetchContactTimes(const std::string& eclipseId,
                               double lat, double lon, int altM,
                               [[maybe_unused]] int year,
                               [[maybe_unused]] int month,
                               [[maybe_unused]] int day) {
    assert(!eclipseId.empty());
    assert(lat   >= -90.0  && lat  <= 90.0);
    assert(lon   >= -180.0 && lon  <= 180.0);
    assert(year  >= 1900   && year <= 2200);
    assert(month >= 1      && month <= 12);
    assert(day   >= 1      && day  <= 31);

    std::string beKey;
    { std::lock_guard lk(s_keyMutex); beKey = s_beApiKey; }

    if (beKey.empty()) {
        // No key → caller (App) uses local BE model (CalcBesselian), per agreement.
        IqpLog("IQP-API: no key set — local BE model used");
        return {};
    }

    auto ct = FetchBesselApiTimes(eclipseId, lat, lon, altM, beKey);
    if (!ct.apiOk)
        IqpLog("IQP-API: fetch failed — local BE model used as fallback");
    return ct;
}

} // namespace TotalControl
