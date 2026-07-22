#include "IersDeltaTClient.h"
#include <windows.h>
#include <winhttp.h>
#include <cassert>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

namespace TotalControl {

// ΔT = TT − UT1 = (TT − TAI) + (TAI − UTC) − (UT1 − UTC)
//                = kTtMinusTai + kLeapSeconds − UT1UTC
// kTtMinusTai is fixed by definition (32.184s, never changes). kLeapSeconds
// is TAI−UTC, currently 37 (last leap second inserted 2016-12-31; IERS
// announces any future one via Bulletin C with ~6 months' notice — update
// this constant if that happens before TSE2026).
static constexpr double kTtMinusTai  = 32.184;
static constexpr double kLeapSeconds = 37.0;

// ─── WinHTTP GET (mirrors ElevationClient/IqpClient/EphClient HttpsGet) ────────
static std::string HttpsGet(const wchar_t* host, const std::string& path,
                             const std::function<void(std::string_view)>& logger) {
    assert(host && host[0] != L'\0');
    std::wstring wpath(path.begin(), path.end());

    HINTERNET hSes = WinHttpOpen(L"TotalControlIersDeltaT/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) {
        if (logger) logger("IERS: WinHttpOpen failed");
        return {};
    }

    HINTERNET hCon = WinHttpConnect(hSes, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hCon) {
        if (logger) logger("IERS: WinHttpConnect failed");
        WinHttpCloseHandle(hSes);
        return {};
    }

    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", wpath.c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!hReq) {
        if (logger) logger("IERS: WinHttpOpenRequest failed");
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
        logger(std::format("IERS: GET {} -> {} ({} bytes)",
                            path, ok ? "OK" : "FAIL", body.size()));
    return ok ? body : std::string();
}

// Fixed-width column layout of finals.all.iau2000.txt, verified byte-for-byte
// against a live download on 2026-07-21 (row for 2026-08-12):
//   [0:2)   year, 2-digit ("26" -> 2026; IERS data never covers pre-1970,
//           so no 19xx/20xx ambiguity in practice for this app's date range)
//   [2:4)   month, right-justified ("26 812 ..." -> " 8")
//   [4:6)   day,   right-justified ("26 812 ..." -> "12")
//   [7:15)  MJD (unused here — matching is by calendar date, not MJD)
//   [16]    polar-motion I/P flag (unused)
//   [57]    UT1-UTC I/P flag: 'I' = IERS/Bulletin B measured, 'P' = predicted
//   [58:68) UT1-UTC value (seconds), 10-char field, may have leading blank(s)
static bool ParseUt1UtcForDate(std::string_view text, int year, int month, int day,
                                double& outUt1Utc, bool& outPredicted) {
    assert(month >= 1 && month <= 12);
    assert(day   >= 1 && day   <= 31);
    int yy = year % 100;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string_view line = text.substr(pos, eol == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : eol - pos);
        pos = (eol == std::string_view::npos) ? text.size() : eol + 1;

        if (line.size() < 68) continue;  // short/blank trailing line

        int lyy = std::atoi(std::string(line.substr(0, 2)).c_str());
        int lmo = std::atoi(std::string(line.substr(2, 2)).c_str());
        int lda = std::atoi(std::string(line.substr(4, 2)).c_str());
        if (lyy != yy || lmo != month || lda != day) continue;

        char flag = line[57];
        std::string valStr(line.substr(58, 10));
        char* end = nullptr;
        double v = std::strtod(valStr.c_str(), &end);
        if (end == valStr.c_str()) return false;  // field present but unparseable

        outUt1Utc    = v;
        outPredicted = (flag == 'P');
        return true;
    }
    return false;  // date not covered by this file (too far past/future)
}

// ─── Public API ───────────────────────────────────────────────────────────────

DeltaTResult FetchDeltaT(int year, int month, int day,
                         std::function<void(std::string_view)> logger) {
    assert(month >= 1 && month <= 12);
    assert(day   >= 1 && day   <= 31);

    DeltaTResult result;
    std::string body = HttpsGet(L"datacenter.iers.org",
                                 "/data/latestVersion/finals.all.iau2000.txt",
                                 logger);
    if (body.empty()) return result;

    double ut1utc = 0.0;
    bool   predicted = false;
    if (!ParseUt1UtcForDate(body, year, month, day, ut1utc, predicted)) {
        if (logger) logger(std::format(
            "IERS: no row for {:04d}-{:02d}-{:02d} in bulletin", year, month, day));
        return result;
    }

    result.ok             = true;
    result.deltaTSeconds  = kTtMinusTai + kLeapSeconds - ut1utc;
    result.predicted      = predicted;
    if (logger) logger(std::format(
        "IERS: dT for {:04d}-{:02d}-{:02d} = {:.3f}s (UT1UTC={:.7f}, {})",
        year, month, day, result.deltaTSeconds, ut1utc,
        predicted ? "predicted" : "measured"));
    return result;
}

} // namespace TotalControl
