#include "EphClient.h"
#include <windows.h>
#include <winhttp.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <vector>

namespace TotalControl {

// ─── Body tables ─────────────────────────────────────────────────────────────

const char* EphBodyName(EphBody b) {
    static constexpr const char* kNames[] = {
        "SUN","MOON","MERCURY","VENUS","MARS","JUPITER","SATURN"
    };
    int i = static_cast<int>(b);
    assert(i >= 0 && i < static_cast<int>(EphBody::Count));
    return kNames[i];
}

const char* EphBodyHorizCmd(EphBody b) {
    // JPL Horizons target body codes
    static constexpr const char* kCmds[] = {
        "10","301","199","299","499","599","699"
    };
    int i = static_cast<int>(b);
    assert(i >= 0 && i < static_cast<int>(EphBody::Count));
    return kCmds[i];
}

// ─── Date formatting ─────────────────────────────────────────────────────────

std::string HorizonsDate(int year, int month, int day) {
    static constexpr const char* kMonths[] = {
        "","Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    assert(year  >= 1900 && year  <= 2200);
    assert(month >= 1    && month <= 12);
    assert(day   >= 1    && day   <= 31);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%s-%02d", year, kMonths[month], day);
    return buf;
}

// ─── Internal helpers ─────────────────────────────────────────────────────────

static constexpr const char* kMonNames[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

// Parse "YYYY-Mon-DD HH:MM" → UTC ms.  Returns -1 on parse failure.
static int64_t ParseHorizDate(const char* p) {
    assert(p != nullptr);
    int yr = 0, dy = 0, hr = 0, mn = 0;
    char monBuf[4] = {};
    if (sscanf_s(p, "%d-%3s-%d %d:%d", &yr, monBuf, 4u, &dy, &hr, &mn) < 5)
        return -1;
    int mo = 0;
    for (int i = 0; i < 12; ++i)
        if (strcmp(monBuf, kMonNames[i]) == 0) { mo = i + 1; break; }
    if (mo == 0) return -1;

    using namespace std::chrono;
    auto ymd = year_month_day{
        std::chrono::year(yr),
        std::chrono::month(static_cast<unsigned>(mo)),
        std::chrono::day(static_cast<unsigned>(dy))
    };
    int64_t dayMs = duration_cast<milliseconds>(
        sys_days{ymd}.time_since_epoch()).count();
    return dayMs + (int64_t(hr) * 3600 + int64_t(mn) * 60) * 1000LL;
}

// Extract the "result" field from Horizons JSON response (handles \n, \", \\ escapes).
static std::string ExtractResult(const std::string& json) {
    assert(!json.empty());
    const char* needle = "\"result\":";
    auto p = json.find(needle);
    if (p == std::string::npos) return {};
    p += strlen(needle);
    while (p < json.size() && json[p] == ' ') ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;

    std::string out;
    out.reserve(65536);
    bool esc = false;
    while (p < json.size()) {
        char c = json[p++];
        if (esc) {
            switch (c) {
            case '"': case '\\': out += c;    break;
            case 'n':            out += '\n'; break;
            case 'r':            out += '\r'; break;
            case 't':            out += '\t'; break;
            default:             out += c;    break;
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

// Parse the $$SOE..$$EOE block from a Horizons observer table response.
// Expected line format (QUANTITIES='1,4,13', ANG_FORMAT=DEG):
//   " YYYY-Mon-DD HH:MM  *   RA_deg  Dec_deg  Az_deg  El_deg  AngDiam_arcsec"
// date field = 18 chars, flag = 2+ chars, then 5 space-separated doubles.
// ang_diam from Horizons is in arcsec; stored as arcmin.
static std::vector<EphRow> ParseSoeBlock(
    const std::string& text,
    const std::function<void(std::string_view)>& logger)
{
    assert(!text.empty());
    std::vector<EphRow> rows;

    auto soe = text.find("$$SOE");
    auto eoe = text.find("$$EOE");
    if (soe == std::string::npos || eoe == std::string::npos || soe >= eoe) {
        if (logger) logger("EPH: $$SOE/$$EOE markers not found in Horizons output");
        return rows;
    }

    soe += 5;
    while (soe < eoe && (text[soe] == '\n' || text[soe] == '\r')) ++soe;

    // 288 samples/day × 7 bodies = 2016 max; 1500 is a safe per-body bound
    static constexpr size_t kMaxRows = 1500;
    size_t pos = soe;
    while (pos < eoe && rows.size() < kMaxRows) {
        size_t lineEnd = text.find('\n', pos);
        if (lineEnd == std::string::npos || lineEnd > eoe) lineEnd = eoe;
        size_t lineLen  = lineEnd - pos;
        if (lineLen < 22) { pos = lineEnd + 1; continue; }

        const char* lp    = text.c_str() + pos;
        int64_t     utcMs = ParseHorizDate(lp);
        if (utcMs < 0) { pos = lineEnd + 1; continue; }

        // Skip 18-char date + flag chars; advance to first digit / sign
        const char* vp   = lp + 18;
        const char* vEnd = lp + lineLen;
        while (vp < vEnd && *vp != '-' && *vp != '+'
               && !isdigit(static_cast<unsigned char>(*vp)))
            ++vp;

        double ra = 0, dec = 0, az = 0, el = 0, angDiam = 0;
        int n = sscanf_s(vp, "%lf %lf %lf %lf %lf",
                         &ra, &dec, &az, &el, &angDiam);
        if (n == 5) {
            EphRow r;
            r.utc_ms          = utcMs;
            r.ra_deg          = ra;
            r.dec_deg         = dec;
            r.az_deg          = az;
            r.alt_deg         = el;
            r.ang_diam_arcmin = angDiam / 60.0;   // arcsec → arcmin
            rows.push_back(r);
        }
        pos = lineEnd + 1;
    }

    if (logger)
        logger(std::format("EPH: parsed {} rows from $$SOE block", rows.size()));
    return rows;
}

// Percent-encode a string for use in a URL query parameter value.
static std::string UrlEncode(const std::string& s) {
    assert(!s.empty());
    std::string out;
    out.reserve(s.size() * 3);
    static constexpr const char* kHex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

// Build the Horizons API query path for one body, one UTC day, one observer.
// startDate / stopDate: "YYYY-Mon-DD" format (HorizonsDate output).
// SITE_COORD: lon (E+, deg), lat (N+, deg), alt (km above WGS84).
// QUANTITIES='1,4,13': RA/Dec (ICRF, deg), Az/El (apparent, deg), ang. diameter (arcsec).
static std::string BuildPath(EphBody body,
                              const std::string& startDate,
                              const std::string& stopDate,
                              double latDeg, double lonDeg, double altM,
                              int stepMin)
{
    assert(stepMin >= 1 && stepMin <= 60);
    double altKm = altM / 1000.0;
    // SITE_COORD format: lon_E, lat_N, alt_km
    std::string siteCoord = std::format("{:.4f},{:.4f},{:.4f}", lonDeg, latDeg, altKm);

    std::string path = "/api/horizons.api?format=json";
    path += "&COMMAND="    + UrlEncode(std::string("'") + EphBodyHorizCmd(body) + "'");
    path += "&CENTER="     + UrlEncode("coord@399");
    path += "&SITE_COORD=" + UrlEncode("'" + siteCoord + "'");
    path += "&START_TIME=" + UrlEncode("'" + startDate + " 00:00'");
    path += "&STOP_TIME="  + UrlEncode("'" + stopDate  + " 00:01'");
    path += "&STEP_SIZE="  + UrlEncode(std::format("'{:d}m'", stepMin));
    path += "&QUANTITIES=" + UrlEncode("'1,4,13'");
    path += "&ANG_FORMAT=DEG";
    path += "&OBJ_DATA=NO";
    path += "&MAKE_EPHEM=YES";
    return path;
}

// HTTPS GET to ssd.jpl.nasa.gov.  Returns raw response body (JSON).
static std::string EphHttpsGet(
    const std::string& path,
    const std::function<void(std::string_view)>& logger)
{
    assert(!path.empty());
    std::wstring wpath(path.begin(), path.end());

    HINTERNET hSes = WinHttpOpen(L"TotalControlEph/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) {
        if (logger) logger(std::format("EPH: WinHttpOpen failed ({})", GetLastError()));
        return {};
    }

    HINTERNET hCon = WinHttpConnect(hSes, L"ssd.jpl.nasa.gov",
                                     INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hCon) {
        if (logger) logger(std::format("EPH: WinHttpConnect failed ({})", GetLastError()));
        WinHttpCloseHandle(hSes);
        return {};
    }

    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", wpath.c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!hReq) {
        if (logger) logger(std::format("EPH: WinHttpOpenRequest failed ({})", GetLastError()));
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return {};
    }

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
           && WinHttpReceiveResponse(hReq, nullptr);

    std::string body;
    if (ok) {
        // Dynamic allocation here is unavoidable for HTTP streaming;
        // eph fetch runs in a background thread before any camera operation.
        static constexpr DWORD kMaxBytes = 2u * 1024u * 1024u;
        body.reserve(256 * 1024);
        DWORD avail = 0;
        while (static_cast<DWORD>(body.size()) < kMaxBytes &&
               WinHttpQueryDataAvailable(hReq, &avail) && avail > 0)
        {
            DWORD toRead = std::min(avail,
                           kMaxBytes - static_cast<DWORD>(body.size()));
            std::vector<char> buf(toRead);
            DWORD rd = 0;
            if (WinHttpReadData(hReq, buf.data(), toRead, &rd))
                body.append(buf.data(), rd);
        }
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    WinHttpCloseHandle(hSes);

    if (!ok) {
        if (logger) logger(std::format("EPH: request failed ({})", GetLastError()));
    } else if (logger) {
        logger(std::format("EPH: {} bytes received", body.size()));
    }

    return body;
}

// ─── Public API ───────────────────────────────────────────────────────────────

std::vector<EphRow> FetchEphemeris(
    EphBody body,
    const std::string& eclipse_date,
    double latDeg, double lonDeg, double altM,
    int step_min,
    const std::function<void(std::string_view)>& logger)
{
    assert(step_min >= 1  && step_min <= 60);
    assert(latDeg >= -90.0  && latDeg <=  90.0);
    assert(lonDeg >= -180.0 && lonDeg <= 180.0);
    assert(!eclipse_date.empty());

    if (logger)
        logger(std::format("EPH: fetching {} for {} lat={:.3f} lon={:.3f} step={}m",
                           EphBodyName(body), eclipse_date, latDeg, lonDeg, step_min));

    // Compute stop date = eclipse_date + 1 calendar day
    int yr = 0, dy = 0;
    char mon[4] = {};
    sscanf_s(eclipse_date.c_str(), "%d-%3s-%d", &yr, mon, 4u, &dy);
    int mo = 0;
    for (int i = 0; i < 12; ++i)
        if (strcmp(mon, kMonNames[i]) == 0) { mo = i + 1; break; }

    std::string stopDate = eclipse_date;
    if (mo > 0) {
        using namespace std::chrono;
        auto ymd  = year_month_day{
            std::chrono::year(yr),
            std::chrono::month(static_cast<unsigned>(mo)),
            std::chrono::day(static_cast<unsigned>(dy))
        };
        auto next = sys_days{ymd} + days{1};
        auto nYmd = year_month_day{next};
        stopDate  = HorizonsDate(
            static_cast<int>(nYmd.year()),
            static_cast<unsigned>(nYmd.month()),
            static_cast<unsigned>(nYmd.day()));
    }

    std::string path = BuildPath(body, eclipse_date, stopDate,
                                  latDeg, lonDeg, altM, step_min);
    std::string json = EphHttpsGet(path, logger);
    if (json.empty()) {
        if (logger) logger("EPH: empty response from Horizons");
        return {};
    }

    std::string result = ExtractResult(json);
    if (result.empty()) {
        if (logger) logger("EPH: could not extract result field from JSON response");
        return {};
    }

    if (result.find("ERROR") != std::string::npos) {
        auto ep = result.find("ERROR");
        auto el = result.find('\n', ep);
        if (logger)
            logger(std::format("EPH: Horizons error: {}",
                   result.substr(ep, el == std::string::npos ? 200 : el - ep)));
        return {};
    }

    return ParseSoeBlock(result, logger);
}

} // namespace TotalControl
