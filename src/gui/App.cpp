#include "App.h"
#include "TzEntry.h"
#include "SdoClient.h"
#include <winhttp.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <cassert>
#include <commdlg.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <algorithm>
#include <chrono>
#include <set>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>

namespace TotalControl {

// ─── Live View shared memory layout ──────────────────────────────────────────
// Mirrored verbatim in src/CameraController.cpp (keep in sync).
// GUI reads (App::LvThreadProc), SRV writes (CameraController).
static constexpr uint32_t kLvDataMax  = 2u * 1024u * 1024u;
static constexpr uint32_t kLvShmTotal = 8u + kLvDataMax;
struct LvShmLayout {
    volatile LONG frameNo;   // incremented by SRV after each write
    uint32_t      jpegSize;
    uint8_t       data[kLvDataMax];
};
static_assert(sizeof(LvShmLayout) == 8 + 2u * 1024u * 1024u, "LvShmLayout size mismatch");

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    auto slash = s.rfind(L'\\');
    return (slash != std::wstring::npos) ? s.substr(0, slash) : s;
}

// UTF-8 string → wstring
static std::wstring Utf8ToW(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// wstring → UTF-8 string
static std::string WToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// Open an MP3 via MCI, read its length in milliseconds, then close. Returns 0 on failure.
static int32_t MciProbeDurMs(const std::wstring& fullPath) {
    assert(!fullPath.empty());              // rule 5: path must be non-empty
    assert(fullPath.size() < MAX_PATH);    // rule 5: path must fit Win32 API limit
    static std::atomic<int> s_id{0};
    std::wstring alias = L"tc_probe" + std::to_wstring(++s_id);
    std::wstring open  = L"open \"" + fullPath + L"\" type mpegvideo alias " + alias;
    if (mciSendStringW(open.c_str(), nullptr, 0, nullptr) != 0) return 0;
    mciSendStringW((L"set " + alias + L" time format milliseconds").c_str(), nullptr, 0, nullptr);
    wchar_t buf[32]{};
    mciSendStringW((L"status " + alias + L" length").c_str(), buf, 32, nullptr);
    mciSendStringW((L"close " + alias).c_str(), nullptr, 0, nullptr);
    int32_t ms = static_cast<int32_t>(_wtoi(buf));
    return (ms > 0) ? ms : 0;
}

// Fire-and-forget MP3 playback. Detaches a thread that opens, plays (blocking wait),
// then closes the MCI device. Exception to NASA rule 3: one allocation per announcement,
// no sequencer interaction, audio never blocks camera operations.
static void MciPlayAsync(const std::wstring& fullPath) {
    assert(!fullPath.empty());             // rule 5: path must be non-empty
    assert(fullPath.size() < MAX_PATH);   // rule 5: path must fit Win32 API limit
    static std::atomic<int> s_id{0};
    std::wstring alias = L"tc_play" + std::to_wstring(++s_id);
    std::thread([fullPath, alias]() {
        std::wstring open = L"open \"" + fullPath + L"\" type mpegvideo alias " + alias;
        if (mciSendStringW(open.c_str(), nullptr, 0, nullptr) != 0) return;
        mciSendStringW((L"set " + alias + L" time format milliseconds").c_str(), nullptr, 0, nullptr);
        mciSendStringW((L"play " + alias + L" wait").c_str(), nullptr, 0, nullptr);
        mciSendStringW((L"close " + alias).c_str(), nullptr, 0, nullptr);
    }).detach();
}

// Returns UTC milliseconds since Unix epoch using high-resolution Windows clock.
static int64_t UtcNowMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ui;
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(ui.QuadPart / 10000) - 11644473600000LL;
}

// Formats HH:MM:SS.mmm for the UTC clock (millisecond precision).
static void FormatUtcHms(int64_t ms, char* buf, int len) {
    int64_t s  = ms / 1000;
    int     ms3 = static_cast<int>(ms % 1000);
    int     hh  = static_cast<int>((s / 3600) % 24);
    int     mm  = static_cast<int>((s / 60)   % 60);
    int     sc  = static_cast<int>(s % 60);
    snprintf(buf, len, "%02d:%02d:%02d.%03d", hh, mm, sc, ms3);
}

// Formats HH:MM:SS for a local clock identified by an IANA timezone name.
// Uses std::chrono::locate_zone (C++20) — DST is handled by the MSVC runtime
// which maps IANA names to Windows registry zones (updated by Windows Update).
// Falls back to "--:--:--" for unknown or invalid timezone names.
static void FormatLocalHms(int64_t utcMs, const std::string& ianaName,
                            char* buf, int len) {
    using namespace std::chrono;
    try {
        const time_zone* tz = locate_zone(ianaName);
        auto sys_tp = system_clock::time_point{milliseconds{utcMs}};
        auto zoned  = zoned_time{tz, sys_tp};
        auto local  = zoned.get_local_time();
        auto day_start = floor<days>(local);
        hh_mm_ss tod{local - day_start};
        snprintf(buf, len, "%02d:%02d:%02d",
            static_cast<int>(tod.hours().count()),
            static_cast<int>(tod.minutes().count()),
            static_cast<int>(tod.seconds().count()));
    } catch (...) {
        snprintf(buf, len, "--:--:--");
    }
}

// ─── JSON field helpers (no external library) ────────────────────────────────

// Extract first string or numeric value for key from flat/shallow JSON.
static std::string JStr(std::string_view j, std::string_view key) {
    std::string needle = std::string("\"") + std::string(key) + "\":";
    auto p = j.find(needle);
    if (p == std::string_view::npos) return {};
    p += needle.size();
    while (p < j.size() && j[p] == ' ') ++p;
    if (p >= j.size()) return {};
    if (j[p] == '"') {
        ++p;
        auto e = j.find('"', p);
        return e == std::string_view::npos ? std::string{} : std::string(j.substr(p, e - p));
    }
    auto e = j.find_first_of(",}", p);
    return std::string(j.substr(p, e == std::string_view::npos ? j.size() - p : e - p));
}

static int JInt(std::string_view j, std::string_view key, int def = 0) {
    auto s = JStr(j, key);
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

// Extract all values of a repeated string key (e.g. "guid" inside cameras array).
static std::vector<std::string> JStrAll(std::string_view j, std::string_view key) {
    std::vector<std::string> out;
    std::string needle = std::string("\"") + std::string(key) + "\":\"";
    size_t p = 0;
    while ((p = j.find(needle, p)) != std::string_view::npos) {
        p += needle.size();
        auto e = j.find('"', p);
        if (e != std::string_view::npos) { out.emplace_back(j.substr(p, e - p)); p = e; }
    }
    return out;
}

// ─── Logging ─────────────────────────────────────────────────────────────────

void App::LogLine(std::string_view msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::lock_guard lk(m_logMutex);
    if (m_logFile.is_open()) {
        m_logFile << std::format("{:02d}:{:02d}:{:02d}.{:03d}  {}\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        m_logFile.flush();
    }
}

// ─── Default config ───────────────────────────────────────────────────────────

void App::EnsureDefaultConfig(const std::wstring& path) {
    if (std::filesystem::exists(path)) return;

    Database db;
    if (!db.Open(path)) return;

    db.Exec(R"SQL(
        CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);
        INSERT OR IGNORE INTO schema_version VALUES (1);

        CREATE TABLE IF NOT EXISTS settings (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        INSERT OR IGNORE INTO settings VALUES ('show_home_clock', '1');
        INSERT OR IGNORE INTO settings VALUES ('show_ecl_clock',  '1');
        INSERT OR IGNORE INTO settings VALUES ('home_tz_iana', 'Europe/Warsaw');
        INSERT OR IGNORE INTO settings VALUES ('ecl_tz_iana',  'Europe/Madrid');
        INSERT OR IGNORE INTO settings VALUES ('obs_lat',   '0.0');
        INSERT OR IGNORE INTO settings VALUES ('obs_lon',   '0.0');
        INSERT OR IGNORE INTO settings VALUES ('obs_alt_m', '0');
    )SQL");
}

// ─── Persistent settings ──────────────────────────────────────────────────────

void App::SaveClockSettings() {
    m_configDb.SetSettingInt("show_home_clock", m_showHomeClock ? 1 : 0);
    m_configDb.SetSettingInt("show_ecl_clock",  m_showEclClock  ? 1 : 0);
    m_configDb.SetSetting("home_tz_iana", m_homeTzIana.c_str());
    m_configDb.SetSetting("ecl_tz_iana",  m_eclTzIana.c_str());
}

void App::SaveObserverSettings() {
    m_configDb.SetSetting("obs_lat",   std::format("{:.6f}", m_obsLat).c_str());
    m_configDb.SetSetting("obs_lon",   std::format("{:.6f}", m_obsLon).c_str());
    m_configDb.SetSettingInt("obs_alt_m", m_obsAltM);
    m_configDb.SetSetting("obs_name",  m_obsName.c_str());
}

static float DmsToDecimal(const DmsCoord& d) {
    float v = d.deg + d.min / 60.f + d.sec / 3600.f;
    return d.pos ? v : -v;
}

static void DecimalToDms(float decimal, DmsCoord& d) {
    d.pos = decimal >= 0.f;
    float a = fabsf(decimal);
    d.deg = static_cast<int>(a);
    float r = (a - d.deg) * 60.f;
    d.min = static_cast<int>(r);
    d.sec = roundf((r - d.min) * 60.f * 100.f) / 100.f;
    if (d.sec >= 60.f) { d.sec = 0.f; if (++d.min >= 60) { d.min = 0; ++d.deg; } }
}

void App::SyncDecimalToDms() {
    DecimalToDms(m_obsLat, m_latDms);
    DecimalToDms(m_obsLon, m_lonDms);
}

void App::SyncDmsToDecimal() {
    m_obsLat = DmsToDecimal(m_latDms);
    m_obsLon = DmsToDecimal(m_lonDms);
}

// ─── Google Maps URL → lat/lon/name ────────────────────────────────────────────

// '+' → space, "%XX" → raw byte (percent-encoded UTF-8 passes through unchanged
// and renders fine — ImGui text is UTF-8, default font covers Latin-1 Supplement).
static std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    auto hexVal = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            int hi = hexVal(in[i + 1]), lo = hexVal(in[i + 2]);
            if (hi >= 0 && lo >= 0) { out += static_cast<char>((hi << 4) | lo); i += 2; }
            else out += in[i];
        } else {
            out += in[i];
        }
    }
    return out;
}

// Extracts the clicked-point coordinates and (optionally) the place name from a
// Google Maps URL. Prefers the precise pin "!3d<lat>!4d<lon>" pair over the map
// camera centre "@<lat>,<lon>,<zoom>" — the former is the actual marked point.
static bool ParseGoogleMapsUrl(const std::string& url, double& lat, double& lon,
                                std::string& name) {
    bool found = false;

    if (auto p3 = url.find("!3d"); p3 != std::string::npos) {
        const char* s = url.c_str() + p3 + 3;
        char* endLat  = nullptr;
        double la = std::strtod(s, &endLat);
        if (endLat != s && endLat[0] == '!' && endLat[1] == '4' && endLat[2] == 'd') {
            char* endLon = nullptr;
            double lo = std::strtod(endLat + 3, &endLon);
            if (endLon != endLat + 3) { lat = la; lon = lo; found = true; }
        }
    }

    if (!found) {
        if (auto pAt = url.find('@'); pAt != std::string::npos) {
            const char* s = url.c_str() + pAt + 1;
            char* endLat = nullptr;
            double la = std::strtod(s, &endLat);
            if (endLat != s && *endLat == ',') {
                char* endLon = nullptr;
                double lo = std::strtod(endLat + 1, &endLon);
                if (endLon != endLat + 1) { lat = la; lon = lo; found = true; }
            }
        }
    }

    if (!found || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
        return false;

    name.clear();
    static constexpr char kPlaceMarker[] = "/place/";
    if (auto pPlace = url.find(kPlaceMarker); pPlace != std::string::npos) {
        size_t s = pPlace + sizeof(kPlaceMarker) - 1;
        size_t e = url.find('/', s);
        if (e == std::string::npos) e = url.size();
        name = UrlDecode(url.substr(s, e - s));
    }
    return true;
}

void App::ApplyGoogleMapsUrl() {
    std::string url(m_gmapsUrlBuf);
    double lat = 0.0, lon = 0.0;
    std::string name;

    if (!ParseGoogleMapsUrl(url, lat, lon, name)) {
        m_gmapsStatusMsg   = "Nie rozpoznano URL Google Maps";
        m_gmapsStatusIsErr = true;
        return;
    }

    m_obsLat = static_cast<float>(lat);
    m_obsLon = static_cast<float>(lon);
    // Place name is no longer taken from the Google Maps URL (often absent,
    // e.g. bare @lat,lon links) — resolved via OpenStreetMap reverse geocoding
    // instead, together with elevation, in GeoElevationThreadProc.
    SyncDecimalToDms();
    SaveObserverSettings();

    m_gmapsStatusMsg   = std::format("Lat={:.6f} Lon={:.6f} — pobieranie wysokosci i nazwy...", lat, lon);
    m_gmapsStatusIsErr = false;

    if (m_geoThread.joinable()) m_geoThread.join();
    m_geoState.store(1);
    m_geoThread = std::thread(&App::GeoElevationThreadProc, this, lat, lon);

    LogLine(std::format("SET LOCATION: parsed lat={:.6f} lon={:.6f} (url place=\"{}\")",
                        lat, lon, name));
}

void App::ApplyLocationAndCalculate() {
    bool haveMapCoords = false;
    if (m_gmapsUrlBuf[0] != '\0') {
        ApplyGoogleMapsUrl();
        haveMapCoords = !m_gmapsStatusIsErr;
    }

    // No coordinates from the map (empty field or unparseable URL) — fall
    // back to the selected eclipse's GE (greatest eclipse) location.
    if (!haveMapCoords &&
        m_eclipseIdx >= 0 && m_eclipseIdx < static_cast<int>(m_eclipses.size())) {
        const auto& e = m_eclipses[m_eclipseIdx];
        m_obsLat = e.latGe;
        m_obsLon = e.lonGe;
        SyncDecimalToDms();
        SaveObserverSettings();
    }

    TriggerIqpFetch();
}

void App::GeoElevationThreadProc(double lat, double lon) {
    auto logger = [this](std::string_view msg) { LogLine(msg); };

    // Reverse-geocode the place name via OpenStreetMap Nominatim. Cleared
    // first so a failed lookup here never reapplies a stale name from a
    // previous successful run.
    GeocodeResult geo = ReverseGeocode(lat, lon, logger);
    { std::lock_guard lk(m_geoMutex); m_geoNamePending = geo.ok ? geo.name : std::string(); }

    ElevationResult res = FetchElevationM(lat, lon, logger);
    if (res.ok) {
        { std::lock_guard lk(m_geoMutex); m_geoElevM = res.elevM; }
        m_geoState.store(2);
    } else {
        m_geoState.store(3);
    }
}

void App::TriggerIqpFetch() {
    if (m_eclipseIdx < 0 || m_eclipseIdx >= static_cast<int>(m_eclipses.size())) return;

    const auto& e = m_eclipses[m_eclipseIdx];
    float lat = m_obsLat, lon = m_obsLon;
    int   altM = m_obsAltM;
    int   y = e.year, mo = e.month, d = e.day;
    std::string id = BuildEclipseId(e.type.empty() ? 'T' : e.type[0], y, mo, d);

    m_iqpFetchedLat = lat;
    m_iqpFetchedLon = lon;
    m_iqpFetchedIdx = m_eclipseIdx;

    // Besselian: synchronous (fast, always available immediately)
    BesselianElements bel = m_dataDb.LoadBesselianElements(y, mo, d);
    m_beResult = CalcBesselian(bel, lat, lon, static_cast<double>(altM));
    // GE column: same elements but at eclipse geographic maximum lat/lon, alt=0
    m_geResult = CalcBesselian(bel, static_cast<double>(e.latGe),
                                    static_cast<double>(e.lonGe), 0.0);

    // IQP: asynchronous, no BE fallback (results shown side-by-side)
    m_iqpState.store(1);
    if (m_iqpThread.joinable()) m_iqpThread.detach();

    auto* statePtr   = &m_iqpState;
    auto* mutexPtr   = &m_iqpMutex;
    auto* contactPtr = &m_contacts;

    m_iqpThread = std::thread([id, lat, lon, altM, y, mo, d,
                                statePtr, mutexPtr, contactPtr]() {
        auto ct = FetchContactTimes(id, lat, lon, altM, y, mo, d);
        { std::lock_guard lk(*mutexPtr); *contactPtr = ct; }
        statePtr->store(ct.apiOk ? 2 : 3);
    });
    LogLine(std::format("IQP+BE triggered: {} lat={:.4f} lon={:.4f}  BE valid={}",
                        id, lat, lon, m_beResult.valid));

    TriggerEphFetch();
}

// ─── JPL Horizons ephemeris ───────────────────────────────────────────────────

// P₀: position angle of solar N pole from north celestial pole.
// Uses IAU/WGCCRE solar pole direction: RA₀=286.13°, Dec₀=63.87° (ICRF J2000).
double App::ComputeP0(double raSun_deg, double decSun_deg) {
    static constexpr double kPi   = 3.14159265358979323846;
    static constexpr double kRa0  = 286.13 * kPi / 180.0;
    static constexpr double kDec0 =  63.87 * kPi / 180.0;
    double ra  = raSun_deg  * kPi / 180.0;
    double dec = decSun_deg * kPi / 180.0;
    double dRA = kRa0 - ra;
    return std::atan2(std::cos(kDec0) * std::sin(dRA),
                      std::sin(kDec0) * std::cos(dec)
                    - std::cos(kDec0) * std::sin(dec) * std::cos(dRA))
           * 180.0 / kPi;
}

// V: position angle of the Moon's north pole from celestial north.
// IAU Moon pole: RA₀=269.9949°, Dec₀=66.5392° (J2000, constant approx).
double App::ComputeMoonV(double raMoon_deg, double decMoon_deg) {
    assert(decMoon_deg >= -90.0 && decMoon_deg <= 90.0);
    static constexpr double kPi   = 3.14159265358979323846;
    static constexpr double kRa0  = 269.9949 * kPi / 180.0;
    static constexpr double kDec0 =  66.5392 * kPi / 180.0;
    double ra  = raMoon_deg  * kPi / 180.0;
    double dec = decMoon_deg * kPi / 180.0;
    double dRA = kRa0 - ra;
    return std::atan2(std::cos(kDec0) * std::sin(dRA),
                      std::sin(kDec0) * std::cos(dec)
                    - std::cos(kDec0) * std::sin(dec) * std::cos(dRA))
           * 180.0 / kPi;
}

// q: parallactic angle of the Sun (angle from celestial N to zenith, measured E).
// H (hour angle) = GMST + lon_deg − RA_sun_deg.
double App::ComputeQ(double raSun_deg, double decSun_deg,
                     double lat_deg, double lon_deg, int64_t utcMs)
{
    static constexpr double kPi = 3.14159265358979323846;
    // Julian Day Number from Unix ms
    double jd = static_cast<double>(utcMs) / 86400000.0 + 2440587.5;
    double T  = (jd - 2451545.0) / 36525.0;
    // GMST in degrees (IAU 1982)
    double gmst = std::fmod(280.46061837
                 + 360.98564736629 * (jd - 2451545.0)
                 + 0.000387933 * T * T, 360.0);
    double H   = (gmst + lon_deg - raSun_deg) * kPi / 180.0;
    double dec = decSun_deg * kPi / 180.0;
    double lat = lat_deg    * kPi / 180.0;
    return std::atan2(std::sin(H),
                      std::tan(lat) * std::cos(dec) - std::sin(dec) * std::cos(H))
           * 180.0 / kPi;
}

// Linear interpolation between two EphRow samples.
EphRow App::InterpEphAt(EphBody body, int64_t utcMs) const {
    assert(static_cast<int>(body) >= 0 &&
           static_cast<int>(body) < static_cast<int>(EphBody::Count));
    std::lock_guard lk(m_ephMutex);
    const auto& rows = m_ephSamples[static_cast<size_t>(body)];
    if (rows.empty()) return {};
    assert(rows.size() >= 1);

    if (utcMs <= rows.front().utc_ms) return rows.front();
    if (utcMs >= rows.back().utc_ms)  return rows.back();

    // Binary search for lo/hi bounding pair
    size_t lo = 0, hi = rows.size() - 1;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (rows[mid].utc_ms <= utcMs) lo = mid;
        else                            hi = mid;
    }
    double t = double(utcMs          - rows[lo].utc_ms)
             / double(rows[hi].utc_ms - rows[lo].utc_ms);
    EphRow r;
    r.utc_ms          = utcMs;
    r.ra_deg          = rows[lo].ra_deg          + t * (rows[hi].ra_deg          - rows[lo].ra_deg);
    r.dec_deg         = rows[lo].dec_deg         + t * (rows[hi].dec_deg         - rows[lo].dec_deg);
    r.az_deg          = rows[lo].az_deg          + t * (rows[hi].az_deg          - rows[lo].az_deg);
    r.alt_deg         = rows[lo].alt_deg         + t * (rows[hi].alt_deg         - rows[lo].alt_deg);
    r.ang_diam_arcmin = rows[lo].ang_diam_arcmin + t * (rows[hi].ang_diam_arcmin - rows[lo].ang_diam_arcmin);
    return r;
}

// Background worker: fetches 7 bodies sequentially, writes to DB, updates m_ephSamples.
void App::EphThreadProc(std::string eclDate, std::string locKey,
                         std::wstring configPath,
                         double lat, double lon, double altM)
{
    assert(!eclDate.empty() && !locKey.empty() && !configPath.empty());
    auto logger = [this](std::string_view msg) { LogLine(msg); };

    Database db;
    if (!db.Open(configPath)) {
        logger("EPH thread: cannot open config DB");
        m_ephFetching.store(false);
        return;
    }
    db.CreateEphTables();

    static constexpr int kBodyCount = static_cast<int>(EphBody::Count);
    for (int i = 0; i < kBodyCount && m_ephFetching.load(); ++i) {
        EphBody body = static_cast<EphBody>(i);
        auto rows = FetchEphemeris(body, eclDate, lat, lon, altM,
                                    5, logger);
        if (rows.empty()) {
            logger(std::format("EPH: no data for {}, skipping remaining bodies",
                               EphBodyName(body)));
            m_ephFetching.store(false);
            return;
        }
        db.SaveEphRows(body, rows);
        // Publish to render thread under mutex
        {
            std::lock_guard lk(m_ephMutex);
            m_ephSamples[static_cast<size_t>(body)] = std::move(rows);
        }
        logger(std::format("EPH: {} cached", EphBodyName(body)));
    }

    db.SetEphMeta(eclDate, locKey);
    logger(std::format("EPH: all bodies cached for {} @ {}", eclDate, locKey));
    m_ephFetching.store(false);
}

void App::TriggerEphFetch() {
    if (m_eclipseIdx < 0 || m_eclipseIdx >= static_cast<int>(m_eclipses.size())) return;
    if (m_ephFetching.load()) return;   // already in progress

    const auto& e = m_eclipses[m_eclipseIdx];
    std::string eclDate = HorizonsDate(e.year, e.month, e.day);
    std::string locKey  = std::format("{:.3f},{:.3f}", m_obsLat, m_obsLon);
    double lat = m_obsLat, lon = m_obsLon, altM = static_cast<double>(m_obsAltM);

    // Check if we already have data for this eclipse+location combination
    if (m_configDb.EphemerisExists(eclDate, locKey)) {
        // Load from DB into memory (if not already loaded)
        bool alreadyLoaded;
        {
            std::lock_guard lk(m_ephMutex);
            alreadyLoaded = !m_ephSamples[0].empty();
        }
        if (!alreadyLoaded) {
            static constexpr int kBodyCount = static_cast<int>(EphBody::Count);
            for (int i = 0; i < kBodyCount; ++i) {
                auto rows = m_configDb.LoadEphRows(static_cast<EphBody>(i));
                std::lock_guard lk(m_ephMutex);
                m_ephSamples[static_cast<size_t>(i)] = std::move(rows);
            }
            LogLine(std::format("EPH: loaded from cache for {} @ {}", eclDate, locKey));
        }
        return;
    }

    // Clear stale data before starting new fetch
    {
        std::lock_guard lk(m_ephMutex);
        for (auto& v : m_ephSamples) v.clear();
    }

    m_ephFetching.store(true);
    LogLine(std::format("EPH: starting fetch for {} @ {}", eclDate, locKey));

    if (m_ephThread.joinable()) m_ephThread.detach();
    m_ephThread = std::thread(&App::EphThreadProc, this,
                               eclDate, locKey, m_configPath,
                               lat, lon, altM);
}

// ─── GOES-19 SUVI Fe171 animation ────────────────────────────────────────────
// URL: cdn.star.nesdis.noaa.gov/GOES19/SUVI/FD/Fe171/YYYYDDDHHMMSS1_GOES19-SUVI-Fe171-600x600.jpg
// Cadence: 4 min; SS=37 fixed; alpha = max(R,G,B) so black space is transparent.

void App::TriggerSuviFetch() {
    assert(m_d3dDev != nullptr);
    if (m_suviFetching.load()) return;
    // Release previous batch textures so the refreshed batch replaces them cleanly.
    // Called only from the render thread — D3D resource release is safe here.
    LogLine(std::format(
        "SUVI: TriggerFetch srvs={} halfQ={:.4f} foot={:.1f} corrR={:.1f} corrU={:.1f} rot={:.2f}deg msSinceLastDone={}",
        (int)m_suviSrvs.size(), m_suviHalfQ, m_suviFooterPx, m_suviCorrRightPx, m_suviCorrUpPx,
        m_solarP - m_solarQ,
        m_suviFetchedAtMs.load() > 0 ? UtcNowMs() - m_suviFetchedAtMs.load() : -1LL));
    for (auto* srv : m_suviSrvs) if (srv) srv->Release();
    m_suviSrvs.clear();
    m_suviCurFrame  = 0;
    m_suviAnimTimer = 0.f;
    { std::lock_guard lk(m_suviMutex); m_suviPending.clear(); }
    m_suviJustCleared = true;
    m_suviFetching.store(true);
    // m_suviFetchedAtMs is set at completion (SuviThreadProc end) so the 1-min
    // interval is measured from when the previous fetch FINISHED, not started.
    if (m_suviThread.joinable()) m_suviThread.detach();
    m_suviThread = std::thread(&App::SuviThreadProc, this);
}

void App::SuviThreadProc() {
    assert(m_suviFetching.load());
    assert(m_d3dDev != nullptr);   // rule 5: D3D device must be set before SUVI fetch is triggered
    CoInitialize(nullptr);

    // Capture channel at thread start — m_suviChannel only changes on main thread when not fetching.
    const std::string channel = m_suviChannel;

    // SUVI filenames have variable SS and trailing digit — cannot guess.
    // Strategy: fetch CDN directory listing, parse actual 1200x1200 filenames,
    // take last 300 (alphabetical = chronological), download each.
    static constexpr int    kNumFrames    = 300;
    static constexpr size_t kMaxJpegBytes = 2u * 1024u * 1024u;   // 1200x1200 ~1.35 MB each
    static constexpr size_t kMaxDirBytes  = 20u * 1024u * 1024u;  // directory listing ~10 MB

    // Per-channel cache directory: suvi_cache/<channel>/
    std::wstring cacheBase = ExeDir() + L"\\suvi_cache";
    CreateDirectoryW(cacheBase.c_str(), nullptr);
    std::wstring channelW(channel.begin(), channel.end());
    std::wstring cacheDir = cacheBase + L"\\" + channelW;
    CreateDirectoryW(cacheDir.c_str(), nullptr);

    // ── WIC factory ──────────────────────────────────────────────────────────
    IWICImagingFactory* wicFac = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFac));
    if (FAILED(hr)) {
        LogLine(std::format("SUVI: WIC factory {:08x}", static_cast<unsigned>(hr)));
        m_suviFetching.store(false); CoUninitialize(); return;
    }

    // JPEG -> RGBA; alpha = max(R,G,B) so black space becomes transparent
    auto decodeJpeg = [&](const std::vector<uint8_t>& jpg) -> SuviFrame {
        SuviFrame result;
        if (jpg.empty()) return result;
        IWICStream* ws = nullptr;
        if (FAILED(wicFac->CreateStream(&ws))) return result;
        HRESULT hrc = ws->InitializeFromMemory(const_cast<BYTE*>(jpg.data()),
                                                static_cast<DWORD>(jpg.size()));
        if (FAILED(hrc)) { ws->Release(); return result; }
        IWICBitmapDecoder* dec = nullptr;
        hrc = wicFac->CreateDecoderFromStream(ws, nullptr,
                                               WICDecodeMetadataCacheOnLoad, &dec);
        ws->Release();
        if (FAILED(hrc)) return result;
        IWICBitmapFrameDecode* frm = nullptr;
        hrc = dec->GetFrame(0, &frm); dec->Release();
        if (FAILED(hrc)) return result;
        IWICFormatConverter* conv = nullptr;
        hrc = wicFac->CreateFormatConverter(&conv);
        if (SUCCEEDED(hrc))
            hrc = conv->Initialize(frm, GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.f,
                                   WICBitmapPaletteTypeCustom);
        frm->Release();
        if (FAILED(hrc)) { if (conv) conv->Release(); return result; }
        UINT imgW = 0, imgH = 0;
        conv->GetSize(&imgW, &imgH);
        std::vector<uint8_t> px(static_cast<size_t>(imgW) * imgH * 4);
        hrc = conv->CopyPixels(nullptr, imgW * 4, static_cast<UINT>(px.size()), px.data());
        conv->Release();
        if (FAILED(hrc)) return result;
        for (size_t p = 0; p + 3 < px.size(); p += 4)
            px[p + 3] = std::max({ px[p], px[p+1], px[p+2] });
        result.rgba = std::move(px);
        result.w = static_cast<int>(imgW);
        result.h = static_cast<int>(imgH);
        return result;
    };

    int loaded = 0;
    auto pushDecoded = [&](SuviFrame frame) {
        if (frame.rgba.empty()) return;
        { std::lock_guard lk(m_suviMutex); m_suviPending.push_back(std::move(frame)); }
        if ((++loaded % 5) == 0) m_suviNewFrames.store(true);
    };

    // ── WinHTTP session + connection (reused for dir listing + all frames) ────
    HINTERNET hSes = WinHttpOpen(L"TotalControlSUVI/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hCon = nullptr;
    if (hSes) {
        WinHttpSetTimeouts(hSes, 30000, 30000, 30000, 30000);
        hCon = WinHttpConnect(hSes, L"cdn.star.nesdis.noaa.gov",
                              INTERNET_DEFAULT_HTTPS_PORT, 0);
    }

    // Generic GET; returns body bytes up to maxBytes, empty on error/non-200.
    auto httpGet = [&](const wchar_t* path, size_t maxBytes) -> std::vector<uint8_t> {
        if (!hCon) return {};
        HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path, nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
        if (!hReq) return {};
        bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
               && WinHttpReceiveResponse(hReq, nullptr);
        if (!ok) { WinHttpCloseHandle(hReq); return {}; }
        DWORD status = 0, statusSz = sizeof(status);
        WinHttpQueryHeaders(hReq,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSz, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) { WinHttpCloseHandle(hReq); return {}; }
        std::vector<uint8_t> body; body.reserve(256 * 1024);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
            size_t off = body.size(); body.resize(off + avail);
            DWORD got = 0;
            if (!WinHttpReadData(hReq, body.data() + off, avail, &got)) break;
            body.resize(off + got);
            if (body.size() >= maxBytes) break;
        }
        WinHttpCloseHandle(hReq);
        return body;
    };

    // ── Step 1: fetch CDN directory listing and parse 1200x1200 filenames ────
    const std::string suffix   = "_GOES19-SUVI-" + channel + "-1200x1200.jpg";
    const std::wstring dirPath = L"/GOES19/SUVI/FD/" + channelW + L"/";
    const std::wstring cdnBase = L"/GOES19/SUVI/FD/" + channelW + L"/";

    LogLine(std::format("SUVI: fetching directory listing for channel {}…", channel));
    auto dirHtml = httpGet(dirPath.c_str(), kMaxDirBytes);
    if (dirHtml.empty()) {
        LogLine("SUVI: directory fetch failed");
        wicFac->Release();
        if (hCon) WinHttpCloseHandle(hCon);
        if (hSes) WinHttpCloseHandle(hSes);
        m_suviFetching.store(false); CoUninitialize(); return;
    }

    // Scan for "NNNNNNNNNNNNNN_GOES19-SUVI-<channel>-1200x1200.jpg" (14-digit timestamp)
    std::vector<std::string> names;
    const char* p = reinterpret_cast<const char*>(dirHtml.data());
    const char* end = p + dirHtml.size();
    // Rule 2: each iteration advances p by at least suffix.size() chars,
    // so the loop executes at most dirHtml.size() / suffix.size() times.
    [[maybe_unused]] const size_t kMaxIter = kMaxDirBytes / suffix.size() + 1;
    [[maybe_unused]] size_t iterCount = 0;
    while (p < end) {
        assert(++iterCount <= kMaxIter);  // rule 2: explicit upper bound
        // Find suffix
        const char* hit = std::search(p, end, suffix.begin(), suffix.end());
        if (hit == end) break;
        // Walk back 14 digits
        if (hit < p + 14) { p = hit + 1; continue; }
        const char* nameStart = hit - 14;
        bool allDigits = true;
        for (int i = 0; i < 14; ++i)
            if (nameStart[i] < '0' || nameStart[i] > '9') { allDigits = false; break; }
        if (allDigits) {
            std::string name(nameStart, 14 + suffix.size());
            if (names.empty() || names.back() != name)
                names.push_back(std::move(name));
        }
        p = hit + suffix.size();
    }
    dirHtml.clear();   // free ~10 MB

    if (names.empty()) {
        LogLine("SUVI: no 1200x1200 files found in directory");
        wicFac->Release();
        if (hCon) WinHttpCloseHandle(hCon);
        if (hSes) WinHttpCloseHandle(hSes);
        m_suviFetching.store(false); CoUninitialize(); return;
    }

    // Sort chronologically (lexicographic = chronological for YYYYDDDHHMMSS? names)
    std::sort(names.begin(), names.end());
    LogLine(std::format("SUVI: {} files in directory; taking last {}",
                        names.size(), kNumFrames));

    // Take last kNumFrames
    if ((int)names.size() > kNumFrames)
        names.erase(names.begin(), names.begin() + ((int)names.size() - kNumFrames));

    // ── Step 2: scan cache for existing files ────────────────────────────────
    std::set<std::wstring> wantedSet;
    for (const auto& n : names)
        wantedSet.insert(std::wstring(n.begin(), n.end()));

    std::set<std::wstring> cachedSet;
    {
        WIN32_FIND_DATAW fdw;
        HANDLE hFind = FindFirstFileW((cacheDir + L"\\*.jpg").c_str(), &fdw);
        if (hFind != INVALID_HANDLE_VALUE) {
            do { cachedSet.insert(fdw.cFileName); }
            while (FindNextFileW(hFind, &fdw));
            FindClose(hFind);
        }
    }

    auto readCache = [&](const std::wstring& fname) -> std::vector<uint8_t> {
        HANDLE hf = CreateFileW((cacheDir + L"\\" + fname).c_str(), GENERIC_READ,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hf == INVALID_HANDLE_VALUE) return {};
        DWORD sz = GetFileSize(hf, nullptr);
        std::vector<uint8_t> buf;
        if (sz > 0 && sz < kMaxJpegBytes) {
            buf.resize(sz);
            DWORD got = 0;
            if (!ReadFile(hf, buf.data(), sz, &got, nullptr) || got != sz) buf.clear();
        }
        CloseHandle(hf);
        return buf;
    };

    auto writeCache = [&](const std::wstring& fname, const std::vector<uint8_t>& data) {
        if (data.empty()) return;
        HANDLE hf = CreateFileW((cacheDir + L"\\" + fname).c_str(), GENERIC_WRITE, 0,
                                nullptr, CREATE_ALWAYS, 0, nullptr);
        if (hf == INVALID_HANDLE_VALUE) return;
        DWORD wr = 0;
        WriteFile(hf, data.data(), static_cast<DWORD>(data.size()), &wr, nullptr);
        CloseHandle(hf);
    };

    // ── Step 3: load from cache + download missing, in chronological order ───
    int fromCache = 0, downloaded = 0;
    for (const auto& name : names) {
        std::wstring wname(name.begin(), name.end());
        std::vector<uint8_t> jpg;

        if (cachedSet.count(wname)) {
            jpg = readCache(wname);
            if (!jpg.empty()) ++fromCache;
        }
        if (jpg.empty()) {
            std::wstring cdnPath = cdnBase + wname;
            jpg = httpGet(cdnPath.c_str(), kMaxJpegBytes);
            if (!jpg.empty()) { writeCache(wname, jpg); ++downloaded; }
        }
        pushDecoded(decodeJpeg(jpg));
    }

    if (hCon) WinHttpCloseHandle(hCon);
    if (hSes) WinHttpCloseHandle(hSes);
    wicFac->Release();

    // ── Step 4: delete cache files not in wanted set ─────────────────────────
    int deleted = 0;
    for (const auto& fname : cachedSet) {
        if (!wantedSet.count(fname)) {
            DeleteFileW((cacheDir + L"\\" + fname).c_str());
            ++deleted;
        }
    }

    m_suviNewFrames.store(true);
    LogLine(std::format("SUVI: done — {} cache + {} CDN + {} deleted  pending={}",
                        fromCache, downloaded, deleted, (int)m_suviPending.size()));
    m_suviFetchedAtMs.store(UtcNowMs());   // interval measured from completion, not start
    m_suviFetching.store(false);
    CoUninitialize();
}

void App::CreateSuviTextures() {
    assert(m_d3dDev != nullptr);
    assert(m_suviNewFrames.load());
    m_suviNewFrames.store(false);

    std::vector<SuviFrame> newFrames;
    {
        std::lock_guard lk(m_suviMutex);
        newFrames = std::move(m_suviPending);
        m_suviPending.clear();
    }
    if (newFrames.empty()) return;

    for (auto& f : newFrames) {
        if (f.rgba.empty() || f.w <= 0 || f.h <= 0) continue;

        D3D11_TEXTURE2D_DESC td{};
        td.Width            = static_cast<UINT>(f.w);
        td.Height           = static_cast<UINT>(f.h);
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem     = f.rgba.data();
        init.SysMemPitch = static_cast<UINT>(f.w * 4);

        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = m_d3dDev->CreateTexture2D(&td, &init, &tex);
        if (FAILED(hr)) {
            LogLine(std::format("SUVI: CreateTexture2D {:08x}", static_cast<unsigned>(hr)));
            continue;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvd.ViewDimension           = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels     = 1;

        ID3D11ShaderResourceView* srv = nullptr;
        hr = m_d3dDev->CreateShaderResourceView(tex, &srvd, &srv);
        tex->Release();
        if (FAILED(hr)) {
            LogLine(std::format("SUVI: CreateSRV {:08x}", static_cast<unsigned>(hr)));
            continue;
        }
        m_suviSrvs.push_back(srv);
        f.rgba.clear();   // free CPU memory after GPU upload
    }
}

// ─── Moon texture (static NASA archive photo, fetched once) ───────────────────
// images-assets.nasa.gov/image/GSFC_20171208_Archive_e001982 — full lunar disc
// on pure black background; alpha = max(R,G,B) so the background is
// transparent. Unlike SUVI this never changes, so it's cached locally and
// only fetched from the network on first run.

void App::TriggerMoonFetch() {
    assert(m_d3dDev != nullptr);
    if (m_moonFetching.load()) return;
    m_moonFetching.store(true);
    if (m_moonThread.joinable()) m_moonThread.detach();
    m_moonThread = std::thread(&App::MoonThreadProc, this);
}

void App::MoonThreadProc() {
    assert(m_moonFetching.load());
    CoInitialize(nullptr);

    static constexpr size_t kMaxJpegBytes = 4u * 1024u * 1024u;
    std::wstring cachePath = ExeDir() + L"\\TotalControlMoon.jpg";

    // Local cache first — this is a static archive photo that never changes,
    // so it's fetched from NASA exactly once, ever (per this exe location).
    std::vector<uint8_t> jpg;
    HANDLE hf = CreateFileW(cachePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(hf, nullptr);
        if (sz > 0 && sz < kMaxJpegBytes) {
            jpg.resize(sz);
            DWORD got = 0;
            if (!ReadFile(hf, jpg.data(), sz, &got, nullptr) || got != sz) jpg.clear();
        }
        CloseHandle(hf);
        if (!jpg.empty()) LogLine("MOON: loaded from local cache (TotalControlMoon.jpg)");
    }

    if (jpg.empty()) {
        LogLine("MOON: not cached — fetching from images-assets.nasa.gov (one-time)");
        jpg = FetchHttpsBytes(L"images-assets.nasa.gov",
            L"/image/GSFC_20171208_Archive_e001982/GSFC_20171208_Archive_e001982~orig.jpg",
            static_cast<int>(kMaxJpegBytes),
            [this](std::string_view m) { LogLine(m); });
        if (!jpg.empty()) {
            HANDLE hw = CreateFileW(cachePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, 0, nullptr);
            if (hw != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(hw, jpg.data(), static_cast<DWORD>(jpg.size()), &wr, nullptr);
                CloseHandle(hw);
                LogLine("MOON: cached to TotalControlMoon.jpg for future runs");
            }
        }
    }

    if (jpg.empty()) {
        LogLine("MOON: fetch failed — no image available, using flat disc fallback");
        m_moonFetching.store(false); CoUninitialize(); return;
    }

    // ── WIC decode: JPEG -> RGBA; alpha = max(R,G,B) ──────────────────────────
    IWICImagingFactory* wicFac = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFac));
    if (FAILED(hr)) {
        LogLine(std::format("MOON: WIC factory {:08x}", static_cast<unsigned>(hr)));
        m_moonFetching.store(false); CoUninitialize(); return;
    }

    std::vector<uint8_t> px;
    UINT imgW = 0, imgH = 0;
    IWICStream* ws = nullptr;
    if (SUCCEEDED(wicFac->CreateStream(&ws))) {
        HRESULT hrc = ws->InitializeFromMemory(jpg.data(), static_cast<DWORD>(jpg.size()));
        IWICBitmapDecoder* dec = nullptr;
        if (SUCCEEDED(hrc))
            hrc = wicFac->CreateDecoderFromStream(ws, nullptr, WICDecodeMetadataCacheOnLoad, &dec);
        ws->Release();
        if (SUCCEEDED(hrc)) {
            IWICBitmapFrameDecode* frm = nullptr;
            hrc = dec->GetFrame(0, &frm);
            dec->Release();
            if (SUCCEEDED(hrc)) {
                IWICFormatConverter* conv = nullptr;
                hrc = wicFac->CreateFormatConverter(&conv);
                if (SUCCEEDED(hrc))
                    hrc = conv->Initialize(frm, GUID_WICPixelFormat32bppRGBA,
                                           WICBitmapDitherTypeNone, nullptr, 0.f,
                                           WICBitmapPaletteTypeCustom);
                frm->Release();
                if (SUCCEEDED(hrc)) {
                    conv->GetSize(&imgW, &imgH);
                    px.resize(static_cast<size_t>(imgW) * imgH * 4);
                    hrc = conv->CopyPixels(nullptr, imgW * 4,
                                          static_cast<UINT>(px.size()), px.data());
                    if (FAILED(hrc)) px.clear();
                    conv->Release();
                }
            }
        }
    }
    wicFac->Release();

    if (px.empty() || imgW == 0 || imgH == 0) {
        LogLine("MOON: JPEG decode failed");
        m_moonFetching.store(false); CoUninitialize(); return;
    }

    // Pass 1: rough luminance scan just to find the disc's bounding box.
    // (Not used for the final alpha — dark maria/shadowed craters are dim
    // enough to trip a luminance threshold too, which made the Moon itself
    // partly transparent even at 100% opacity.)
    static constexpr uint8_t kAlphaThresh = 12;
    int minX = static_cast<int>(imgW), maxX = -1;
    int minY = static_cast<int>(imgH), maxY = -1;
    for (UINT y = 0; y < imgH; ++y) {
        for (UINT x = 0; x < imgW; ++x) {
            size_t p = (static_cast<size_t>(y) * imgW + x) * 4;
            uint8_t lum = static_cast<uint8_t>(std::max({ px[p], px[p+1], px[p+2] }));
            if (lum > kAlphaThresh) {
                minX = std::min(minX, static_cast<int>(x));
                maxX = std::max(maxX, static_cast<int>(x));
                minY = std::min(minY, static_cast<int>(y));
                maxY = std::max(maxY, static_cast<int>(y));
            }
        }
    }
    if (maxX < minX || maxY < minY) {
        // Degenerate (all-black) decode — fall back to the full frame.
        minX = 0; maxX = static_cast<int>(imgW) - 1;
        minY = 0; maxY = static_cast<int>(imgH) - 1;
    }

    // Pass 2: geometric circular mask from the detected centre/radius —
    // fully opaque anywhere inside the disc (however dark that terrain is),
    // fully transparent outside, with a small feather at the edge to avoid
    // a jagged silhouette.
    {
        float discCx = (minX + maxX) * 0.5f;
        float discCy = (minY + maxY) * 0.5f;
        float discR  = std::max(1.f, ((maxX - minX) + (maxY - minY)) * 0.25f);
        constexpr float kFeatherPx = 1.5f;
        for (UINT y = 0; y < imgH; ++y) {
            float dy = static_cast<float>(y) - discCy;
            for (UINT x = 0; x < imgW; ++x) {
                float dx = static_cast<float>(x) - discCx;
                float d  = sqrtf(dx * dx + dy * dy);
                uint8_t a;
                if (d <= discR - kFeatherPx) a = 255;
                else if (d >= discR + kFeatherPx) a = 0;
                else a = static_cast<uint8_t>(
                    (discR + kFeatherPx - d) / (2.f * kFeatherPx) * 255.f + 0.5f);
                px[(static_cast<size_t>(y) * imgW + x) * 4 + 3] = a;
            }
        }
    }

    {
        std::lock_guard lk(m_moonMutex);
        m_moonPending   = std::move(px);
        m_moonPendingW  = static_cast<int>(imgW);
        m_moonPendingH  = static_cast<int>(imgH);
        m_moonPendingCx = (minX + maxX) * 0.5f;
        m_moonPendingCy = (minY + maxY) * 0.5f;
        m_moonPendingR  = std::max(1.f, ((maxX - minX) + (maxY - minY)) * 0.25f);
    }
    m_moonNewData.store(true);
    LogLine(std::format("MOON: decoded {}x{}  disc centre=({:.0f},{:.0f}) r={:.0f}px",
                        imgW, imgH, (minX + maxX) * 0.5f, (minY + maxY) * 0.5f,
                        ((maxX - minX) + (maxY - minY)) * 0.25f));
    m_moonFetching.store(false);
    CoUninitialize();
}

void App::CreateMoonTexture() {
    assert(m_d3dDev != nullptr);
    assert(m_moonNewData.load());
    m_moonNewData.store(false);

    std::vector<uint8_t> px;
    int w = 0, h = 0;
    {
        std::lock_guard lk(m_moonMutex);
        px = std::move(m_moonPending);
        m_moonPending.clear();
        w = m_moonPendingW;
        h = m_moonPendingH;
        m_moonDiscCx = m_moonPendingCx;
        m_moonDiscCy = m_moonPendingCy;
        m_moonDiscR  = m_moonPendingR;
    }
    if (px.empty() || w <= 0 || h <= 0) return;

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = static_cast<UINT>(w);
    td.Height           = static_cast<UINT>(h);
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem     = px.data();
    init.SysMemPitch = static_cast<UINT>(w * 4);

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = m_d3dDev->CreateTexture2D(&td, &init, &tex);
    if (FAILED(hr)) {
        LogLine(std::format("MOON: CreateTexture2D {:08x}", static_cast<unsigned>(hr)));
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format              = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;

    if (m_moonSrv) { m_moonSrv->Release(); m_moonSrv = nullptr; }
    hr = m_d3dDev->CreateShaderResourceView(tex, &srvd, &m_moonSrv);
    tex->Release();
    if (FAILED(hr)) {
        LogLine(std::format("MOON: CreateSRV {:08x}", static_cast<unsigned>(hr)));
        return;
    }
    m_moonImgW = w;
    m_moonImgH = h;
    LogLine("MOON: texture uploaded to GPU");
}

// ─── Live View thread ────────────────────────────────────────────────────────

void App::StartLvThread() {
    assert(!m_lvThread.joinable());
    m_lvThreadRun.store(true);
    m_lvThread = std::thread(&App::LvThreadProc, this);
}

void App::StopLvThread() {
    m_lvThreadRun.store(false);
    if (m_lvThread.joinable()) m_lvThread.join();
}

void App::LvThreadProc() {
    assert(m_lvThreadRun.load());
    CoInitialize(nullptr);

    IWICImagingFactory* wicFac = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFac));
    if (FAILED(hr)) {
        LogLine(std::format("LV: WIC factory {:08x}", static_cast<unsigned>(hr)));
        m_lvThreadRun.store(false);
        CoUninitialize(); return;
    }

    // JPEG → RGBA (straight alpha = 255, no luminance mapping like SUVI)
    auto decodeJpeg = [&](const uint8_t* src, uint32_t sz) -> LvFrame {
        assert(src != nullptr && sz > 0);
        LvFrame result;
        IWICStream* ws = nullptr;
        if (FAILED(wicFac->CreateStream(&ws))) return result;
        if (FAILED(ws->InitializeFromMemory(const_cast<BYTE*>(src), sz))) {
            ws->Release(); return result;
        }
        IWICBitmapDecoder* dec = nullptr;
        HRESULT hrc = wicFac->CreateDecoderFromStream(ws, nullptr,
                                                        WICDecodeMetadataCacheOnLoad, &dec);
        ws->Release();
        if (FAILED(hrc)) return result;
        IWICBitmapFrameDecode* frm = nullptr;
        hrc = dec->GetFrame(0, &frm); dec->Release();
        if (FAILED(hrc)) return result;
        IWICFormatConverter* conv = nullptr;
        hrc = wicFac->CreateFormatConverter(&conv);
        if (SUCCEEDED(hrc))
            hrc = conv->Initialize(frm, GUID_WICPixelFormat32bppRGBA,
                                    WICBitmapDitherTypeNone, nullptr, 0.f,
                                    WICBitmapPaletteTypeCustom);
        frm->Release();
        if (FAILED(hrc)) { if (conv) conv->Release(); return result; }
        UINT imgW = 0, imgH = 0;
        conv->GetSize(&imgW, &imgH);
        result.rgba.resize(static_cast<size_t>(imgW) * imgH * 4, 255);
        hrc = conv->CopyPixels(nullptr, imgW * 4,
                                static_cast<UINT>(result.rgba.size()), result.rgba.data());
        conv->Release();
        if (FAILED(hrc)) { result.rgba.clear(); return result; }
        result.w = static_cast<int>(imgW);
        result.h = static_cast<int>(imgH);
        return result;
    };

    uint32_t lastFrameNo[kMaxCamTracks] = {};

    // Bounded loop: 1e6 × 200 ms ≈ 55 hours — stopped in practice via m_lvThreadRun flag.
    static constexpr int kMaxLvIter = 1'000'000;
    for (int iter = 0; iter < kMaxLvIter && m_lvThreadRun.load(); ++iter) {
        ::Sleep(200);

        for (int ci = 0; ci < kMaxCamTracks; ++ci) {
            if (!m_lvEnabled[ci]) continue;

            wchar_t shmName[64];
            swprintf_s(shmName, _countof(shmName), L"TotalControl_LV_%d", ci);
            HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, shmName);
            if (!hMap) continue;   // SRV hasn't called lv_start yet — silent skip

            const void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, kLvShmTotal);
            if (!view) { CloseHandle(hMap); continue; }

            const auto* layout = static_cast<const LvShmLayout*>(view);
            // volatile read: on x86-64 an aligned 4-byte load is naturally atomic
            uint32_t frameNo = static_cast<uint32_t>(layout->frameNo);

            if (frameNo != lastFrameNo[ci] && frameNo > 0) {
                uint32_t jpegSz = layout->jpegSize;
                LvFrame frame;
                if (jpegSz > 0 && jpegSz <= kLvDataMax)
                    frame = decodeJpeg(layout->data, jpegSz);
                if (!frame.rgba.empty()) {
                    std::lock_guard lk(m_lvMutex);
                    m_lvPending[ci] = std::move(frame);
                    m_lvNewData[ci] = true;
                    m_lvNewFrames.store(true);
                }
                lastFrameNo[ci] = frameNo;
            }

            UnmapViewOfFile(view);
            CloseHandle(hMap);
        }
    }

    wicFac->Release();
    CoUninitialize();
}

void App::CreateLvTextures() {
    assert(m_d3dDev != nullptr);
    m_lvNewFrames.store(false);

    LvFrame frames[kMaxCamTracks];
    bool    hasNew[kMaxCamTracks] = {};
    {
        std::lock_guard lk(m_lvMutex);
        for (int ci = 0; ci < kMaxCamTracks; ++ci) {
            if (!m_lvNewData[ci]) continue;
            frames[ci]      = std::move(m_lvPending[ci]);
            hasNew[ci]      = true;
            m_lvNewData[ci] = false;
        }
    }

    for (int ci = 0; ci < kMaxCamTracks; ++ci) {
        if (!hasNew[ci] || frames[ci].rgba.empty()) continue;

        if (m_lvSrv[ci]) { m_lvSrv[ci]->Release(); m_lvSrv[ci] = nullptr; }

        D3D11_TEXTURE2D_DESC td{};
        td.Width            = static_cast<UINT>(frames[ci].w);
        td.Height           = static_cast<UINT>(frames[ci].h);
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem     = frames[ci].rgba.data();
        init.SysMemPitch = static_cast<UINT>(frames[ci].w * 4);

        ID3D11Texture2D* tex = nullptr;
        if (FAILED(m_d3dDev->CreateTexture2D(&td, &init, &tex))) {
            LogLine(std::format("LV[{}]: CreateTexture2D failed", ci)); continue;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format              = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* srv = nullptr;
        HRESULT hr = m_d3dDev->CreateShaderResourceView(tex, &srvd, &srv);
        tex->Release();
        if (SUCCEEDED(hr)) {
            m_lvSrv[ci] = srv;
            m_lvW[ci]   = frames[ci].w;
            m_lvH[ci]   = frames[ci].h;
        } else {
            LogLine(std::format("LV[{}]: CreateSRV failed {:08x}", ci, static_cast<unsigned>(hr)));
        }
    }
}

// ─── Camera overhead model (ILCE-7RM4A) ──────────────────────────────────────

// Parse shutter speed string → milliseconds.
// "1/1000" → 1, "2s" or "2" → 2000, "0.6" → 600
static int64_t ParseSsMs(const std::string& ss) {
    if (ss.empty()) return 0;
    auto slash = ss.find('/');
    if (slash != std::string::npos) {
        int num = 1, den = 1000;
        sscanf_s(ss.c_str(), "%d/%d", &num, &den);
        return den > 0 ? int64_t(num) * 1000LL / den : 0;
    }
    double v = 0.0;
    sscanf_s(ss.c_str(), "%lf", &v);
    return int64_t(v * 1000.0);
}

// Conservative overhead per shot (USB latency + buffer + SDK confirm).
// Measured: 303 ms; conservative: 350 ms.
static constexpr int64_t kCamOverheadMs = 350;

// Shutter speed the bracket_calib table's lat_avg_ms/lat_max_ms values were
// measured at (ss=1/125 => 8ms). BlockDurMs's ssExtra correction scales from
// this baseline, so it must always match whatever ss the stored calibration
// rows actually used (see BktCalibEntry::ss / SeedBuiltinCalib).
static constexpr int64_t kCalibBaselineSsMs = 8;

// Returns true if blocks a and b require an ARM command between them
// (different drive mode, shutter speed, ISO, or f-stop).
static bool BlockParamsDiffer(const TLBlock& a, const TLBlock& b) {
    assert(a.type != BlockType::Audio && b.type != BlockType::Audio);
    if (a.type != b.type)                                      return true;
    if (a.ss != b.ss || a.fstop != b.fstop || a.iso != b.iso) return true;
    if (a.type == BlockType::Bracket) return a.ev != b.ev || a.count != b.count;
    if (a.type == BlockType::Burst)   return a.burstDrive != b.burstDrive;
    return false;
}

// Conservative estimate of ARM overhead after block b fires — the LONGER of:
//  (a) pure settings-change latency (SDK/USB round-trip when the camera isn't
//      busy), and
//  (b) camera-side buffer-clear time (DriveMode changes are rejected while the
//      camera is still flushing the previous capture(s) to card).
// Measured on ILCE-7RM4A 2026-07-12 via a gap sweep from 5000ms down to 250ms
// per bracket count (calibration/arm_sweep_final.csv): for every count tested, (b) fully
// dominates (a) — gap+confirm_ms converges to a near-constant total once the
// gap drops below the true buffer-clear threshold, which is what these
// per-count values are. Only 3/5/9 are covered: the Inspector's bracket-mode
// dropdown never writes any other count, and the ILCE-7RM4A has no 7-shot
// bracket mode on real hardware.
static int64_t ArmEstMs(const TLBlock& b) {
    assert(b.type != BlockType::Audio);
    if (b.type == BlockType::Bracket) {
        static constexpr int64_t kSettingsChangeMs = 300; // factor (a)
        int64_t bufferClearMs;                            // factor (b)
        if      (b.count <= 3) bufferClearMs = 4100;
        else if (b.count <= 5) bufferClearMs = 4350;
        else                   bufferClearMs = 5500; // 9-count and above
        return std::max(kSettingsChangeMs, bufferClearMs);
    }
    return 2000LL;
}

int64_t App::SnapMsToRelativeSecond(int64_t ms) {
    ContactTimes ct;
    { std::lock_guard lk(m_iqpMutex); ct = m_contacts; }
    if (!ct.valid) ct = m_beResult;

    const int64_t cMs[4] = { ct.c1Ms, ct.c2Ms, ct.c3Ms, ct.c4Ms };
    int64_t best = -1, bestD = INT64_MAX;
    for (int64_t c : cMs) {
        if (c <= 0) continue;
        int64_t d = llabs(ms - c);
        if (d < bestD) { bestD = d; best = c; }
    }

    // No contacts yet — fall back to rounding to the nearest whole UTC second.
    if (best < 0) return ((ms + 500) / 1000) * 1000;

    int64_t offset = ms - best;
    int64_t roundedOffset = ((offset >= 0 ? offset + 500 : offset - 500) / 1000) * 1000;
    return best + roundedOffset;
}

// Sum of per-shot exposure-time multipliers for a symmetric N-stop bracket
// (order-independent: minus-to-plus vs zero-to-minus-to-plus sum to the same
// total). An N-shot bracket at `evStep` EV spans stops -(N-1)/2 .. +(N-1)/2
// around the base ss, e.g. a 9-shot 1EV bracket at ss=0.2s fires 3.2s, 1.6s,
// 0.8s, 0.4s, 0.2s, 0.1s, 1/20, 1/40, 1/80 — sum multiplier 31.9375, not 9.
static double BracketSumMultiplier(int count, double evStep) {
    assert(count >= 1 && count % 2 == 1);        // kBrackets only defines odd counts
    assert(evStep > 0.0 && evStep <= 5.0);        // widest supported step is 3.0ev
    const int half = (count - 1) / 2;
    double sum = 0.0;
    for (int k = -half; k <= half; ++k)
        sum += std::pow(2.0, k * evStep);
    return sum;
}

int64_t App::BlockDurMs(const TLBlock& b, std::string_view camModel) const {
    assert(b.type == BlockType::Audio || !b.ss.empty());  // camera blocks must have a shutter speed
    assert(b.type != BlockType::Bracket || b.count >= 1); // bracket must have at least 1 shot
    assert(b.type != BlockType::Burst   || b.burstDurMs > 0);
    assert(b.type != BlockType::Audio   || b.audioDurMs > 0);
    int64_t ssMs = ParseSsMs(b.ss);
    switch (b.type) {
    case BlockType::Single:
        return ssMs + kCamOverheadMs;
    case BlockType::Bracket: {
        double evStep = 1.0;
        try { evStep = std::stod(b.ev); } catch (...) {}
        const double sumMul = BracketSumMultiplier(b.count, evStep);

        // Calibration lookup: prefer named model, fall back to first available.
        const std::map<std::pair<int,std::string>, int>* table = nullptr;
        if (!camModel.empty()) {
            auto it = m_calibCache.find(std::string(camModel));
            if (it != m_calibCache.end()) table = &it->second;
        }
        if (!table && !m_calibCache.empty())
            table = &m_calibCache.begin()->second;
        if (table) {
            auto jt = table->find({b.count, b.ev});
            if (jt != table->end()) {
                // jt->second = lat_avg_ms + 50, calibrated at ss=kCalibBaselineSsMs.
                // Exposure time scales with the bracket's stop-multiplier sum, not with
                // count alone — an N-shot 1EV bracket spans N stops of exposure time,
                // not N x base-ss. Correction applies both above and below baseline.
                int64_t ssExtra = static_cast<int64_t>(
                    std::llround(sumMul * double(ssMs - kCalibBaselineSsMs)));
                return jt->second + ssExtra;
            }
        }
        // No calibration — fall back to formula: per-shot overhead + summed exposure time.
        return int64_t(b.count) * kCamOverheadMs
             + static_cast<int64_t>(std::llround(sumMul * double(ssMs)));
    }
    case BlockType::Burst:
        return b.burstDurMs;
    case BlockType::Audio:
        return b.audioDurMs;
    default:
        return 0;
    }
}

static ImU32 BlockColor(BlockType t) {
    switch (t) {
    case BlockType::Single:  return IM_COL32( 40, 160,  70, 220);
    case BlockType::Burst:   return IM_COL32( 50, 120, 200, 220);
    case BlockType::Bracket: return IM_COL32(200, 130,  30, 220);
    case BlockType::Audio:   return IM_COL32(140,  80, 200, 220);
    default:                 return IM_COL32(100, 100, 100, 220);
    }
}

static const char* BlockTypeName(BlockType t) {
    switch (t) {
    case BlockType::Single:  return "Single";
    case BlockType::Burst:   return "Burst";
    case BlockType::Bracket: return "Bracket";
    case BlockType::Audio:   return "Audio";
    default:                 return "?";
    }
}

// ─── Rotated text helper ──────────────────────────────────────────────────────

// Renders text at `anchor` rotated 90° CCW: first char at anchor.y, text flows
// downward; glyphs readable by tilting head to the left.
static void AddTextRotated90CCW(ImDrawList* dl, ImFont* font, float sz,
                                 ImVec2 anchor, ImU32 col, const char* text) {
    assert(font != nullptr && sz > 0.f);
    assert(text != nullptr);
    float s   = sz / font->FontSize;
    float cur = 0.f;
    for (const char* p = text; *p; ++p) {
        const ImFontGlyph* g = font->FindGlyph(
            static_cast<ImWchar>(static_cast<unsigned char>(*p)));
        if (!g) continue;
        if (g->Visible) {
            // 90° CCW visual (screen Y-down): glyph (x,y) → screen (y, -x) from anchor;
            // text flows upward from anchor, reads bottom-to-top.
            ImVec2 p1{anchor.x + g->Y0 * s, anchor.y - cur - g->X0 * s};
            ImVec2 p2{anchor.x + g->Y0 * s, anchor.y - cur - g->X1 * s};
            ImVec2 p3{anchor.x + g->Y1 * s, anchor.y - cur - g->X1 * s};
            ImVec2 p4{anchor.x + g->Y1 * s, anchor.y - cur - g->X0 * s};
            dl->AddImageQuad(font->ContainerAtlas->TexID,
                             p1, p2, p3, p4,
                             {g->U0, g->V0}, {g->U1, g->V0},
                             {g->U1, g->V1}, {g->U0, g->V1},
                             col);
        }
        cur += g->AdvanceX * s;
    }
}

// ─── Background status thread ────────────────────────────────────────────────
//
// Polls camera list + per-camera status every ~2 s.
// Writes m_cameras under m_camerasMutex; never touches the render thread.
// PipeClient::SendRequest is internally mutex-protected — safe to call from here.

void App::StartStatusThread() {
    assert(!m_statusRun.load());
    m_statusRun.store(true);
    m_statusThread = std::thread([this]() { StatusThreadProc(); });
}

void App::StopStatusThread() {
    assert(m_statusThread.joinable() || !m_statusRun.load());
    m_statusRun.store(false);
    if (m_statusThread.joinable()) m_statusThread.join();
}

void App::StatusThreadProc() {
    assert(m_statusRun.load());
    while (m_statusRun.load()) {
        if (m_pipe.GetState() == PipeClient::State::Connected) {
            auto listRes = m_pipe.SendRequest(R"({"cmd":"list_cameras"})");
            if (listRes) {
                auto guids = JStrAll(*listRes, "guid");
                std::vector<CamStatus> next;
                next.reserve(guids.size());
                for (auto& guid : guids) {
                    std::string req =
                        std::format(R"({{"cmd":"status","cam":"{}"}})", guid);
                    auto res = m_pipe.SendRequest(req);
                    if (!res) continue;
                    CamStatus s;
                    s.valid          = JStr(*res, "connected") == "true";
                    s.guid           = guid;
                    s.model          = JStr(*res, "model");
                    s.batteryPct     = JInt(*res, "battery");
                    s.batteryLevel   = JStr(*res, "battery_level");
                    s.mode           = JStr(*res, "mode");
                    s.ss             = JStr(*res, "ss");
                    s.iso            = JInt(*res, "iso");
                    s.fnum           = JStr(*res, "f");
                    s.focus          = JStr(*res, "focus");
                    s.drive          = JStr(*res, "drive");
                    s.slot1Status    = JStr(*res, "slot1_status");
                    s.slot2Status    = JStr(*res, "slot2_status");
                    s.slot1Remaining = JInt(*res, "remaining",       -1);
                    s.slot2Remaining = JInt(*res, "slot2_remaining", -1);
                    s.store          = JStr(*res, "store");
                    // Carry forward lastShotMs and slot max-remaining across polls
                    {
                        std::lock_guard lk(m_camerasMutex);
                        for (auto& prev : m_cameras) {
                            if (prev.guid != guid) continue;
                            s.lastShotMs  = prev.lastShotMs;
                            s.slot1MaxRem = prev.slot1MaxRem;
                            s.slot2MaxRem = prev.slot2MaxRem;
                            break;
                        }
                    }
                    if (s.slot1Remaining > 0 && s.slot1Remaining > s.slot1MaxRem)
                        s.slot1MaxRem = s.slot1Remaining;
                    if (s.slot2Remaining > 0 && s.slot2Remaining > s.slot2MaxRem)
                        s.slot2MaxRem = s.slot2Remaining;
                    next.push_back(std::move(s));
                }
                std::lock_guard lk(m_camerasMutex);
                m_cameras = std::move(next);
            }
        } else {
            std::lock_guard lk(m_camerasMutex);
            m_cameras.clear();
        }
        // Interruptible 2 s sleep — wakes every 10 ms to check m_statusRun
        static constexpr int kPollIntervals = 200;  // 200 × 10 ms = 2 s
        for (int i = 0; i < kPollIntervals && m_statusRun.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ─── GUI sequencer ────────────────────────────────────────────────────────────
//
// BuildBlockCmd: builds a pipe JSON command string for one timeline block.
// camIdx is the 0-based camera-track index (used as "cam":<index> in the pipe protocol).

/*static*/ std::string App::BuildBlockCmd(const TLBlock& blk, int camIdx) {
    assert(camIdx >= 0 && camIdx < kMaxCamTracks);
    assert(!blk.ss.empty() || blk.type == BlockType::Audio);

    if (blk.type == BlockType::Audio) return {};  // audio blocks have no pipe command

    std::string cam = std::to_string(camIdx);

    if (blk.type == BlockType::Single) {
        // Timeout must scale with shutter speed — the daemon's flat 5000ms
        // default (CommandHandler.cpp, count==1 case) times out mid-exposure
        // for long single shots (e.g. Earthshine 10-40s), which then leaves
        // the camera physically still exposing when the next block's ARM
        // fires, cascading into CrError_Api_Insufficient on every following
        // property set until the real exposure finally ends.
        int64_t ssMs = ParseSsMs(blk.ss);
        int64_t timeoutMs = ssMs + 5000;
        return std::format(
            "{{\"cmd\":\"shoot\",\"drive\":\"single\","
            "\"ss\":\"{}\",\"iso\":{},\"f\":{},"
            "\"timeout_ms\":{},\"cam\":\"{}\"}}",
            blk.ss, blk.iso, blk.fstop, timeoutMs, cam);
    }
    if (blk.type == BlockType::Bracket) {
        // Same scaling issue as Single: the daemon's flat 15000ms default
        // (CommandHandler.cpp bracket branch) doesn't account for count x SS
        // — a slow-corona bracket (e.g. 5 shots at several seconds each) can
        // exceed it well before the real capture sequence finishes.
        int64_t ssMs = ParseSsMs(blk.ss);
        int64_t timeoutMs = int64_t(blk.count) * (ssMs + kCamOverheadMs) + 5000;
        return std::format(
            "{{\"cmd\":\"bracket\",\"mode\":\"cont\","
            "\"ss\":\"{}\",\"iso\":{},\"f\":{},"
            "\"ev\":\"{}\",\"count\":{},"
            "\"timeout_ms\":{},\"cam\":\"{}\"}}",
            blk.ss, blk.iso, blk.fstop, blk.ev, blk.count, timeoutMs, cam);
    }
    if (blk.type == BlockType::Burst) {
        int timeoutMs = blk.burstDurMs + 2000;
        return std::format(
            "{{\"cmd\":\"shoot\",\"drive\":\"{}\","
            "\"ss\":\"{}\",\"iso\":{},\"f\":{},"
            "\"timeout_ms\":{},\"cam\":\"{}\"}}",
            blk.burstDrive, blk.ss, blk.iso, blk.fstop, timeoutMs, cam);
    }
    return {};
}

// SeqThreadProc: core sequencer loop.
// mode       — TestRunning or Running
// playheadStartMs — UTC ms of playhead when this run started/resumed
// realStartMs     — real UTC ms at start/resume (for simulated time calculation)
void App::SeqThreadProc(GuiSeqMode mode,
                        int64_t   playheadStartMs,
                        int64_t   realStartMs) {
    assert(m_seqRun.load());
    assert(playheadStartMs > 0);
    assert(realStartMs > 0);

    // Local copy of per-track next-block indices for this run segment
    int nextBlock[kMaxCamTracks];
    for (int i = 0; i < kMaxCamTracks; ++i)
        nextBlock[i] = m_seqNextBlock[i];

    // Audio is handled by m_audioSeqThread — not here.

    // ── Pre-arm deduplication ─────────────────────────────────────────────────
    // ARM is sent after a block fires only when the NEXT block has different
    // params (BlockParamsDiffer). Identical consecutive blocks skip the ARM —
    // the camera is already configured from the initial arm or the previous ARM.

    auto sendArm = [&](const TLBlock& blk, int camIdx) {
        assert(camIdx >= 0 && camIdx < kMaxCamTracks);
        if (blk.type == BlockType::Audio) return;

        std::string armCmd = std::format(
            "{{\"cmd\":\"arm\",\"cam\":\"{}\",\"ss\":\"{}\",\"iso\":{},\"f\":\"{}\"",
            camIdx, blk.ss, blk.iso, blk.fstop);

        switch (blk.type) {
        case BlockType::Single:
            armCmd += ",\"drive\":\"single\"";
            break;
        case BlockType::Burst:
            armCmd += std::format(",\"drive\":\"{}\"", blk.burstDrive);
            break;
        case BlockType::Bracket:
            armCmd += std::format(",\"ev\":\"{}\",\"count\":{},\"mode\":\"cont\"",
                                  blk.ev, blk.count);
            break;
        default: return;
        }
        armCmd += "}";

        auto res = m_pipe.SendRequest(armCmd);
        bool ok  = res && JStr(*res, "ok") == "true";
        int  lat = res ? JInt(*res, "latency_ms", -1) : -1;
        LogLine(std::format("ARM cam={} ok={} lat={} type={} ss={} ev={} count={}",
            camIdx, ok ? 1 : 0, lat,
            BlockTypeName(blk.type), blk.ss, blk.ev, blk.count));
    };

    // ── Initial arm: arm the first upcoming block for each track ─────────────
    // Happens before the first tick; arm duration is excluded from simulated time
    // by shifting realStartMs forward so the playhead doesn't jump.
    {
        int camIdx = 0;
        for (int ti = 0;
             ti < static_cast<int>(m_tracks.size()) && camIdx < kMaxCamTracks;
             ++ti) {
            if (!m_tracks[ti].IsCamera()) continue;
            int ni = nextBlock[camIdx];
            const auto& blks = m_tracks[ti].blocks;
            while (ni < static_cast<int>(blks.size()) && blks[ni].atMs < 0) ++ni;
            if (ni < static_cast<int>(blks.size()))
                sendArm(blks[ni], camIdx);
            ++camIdx;
        }
    }
    // Absorb arm latency: start the simulated clock from now so that arm time
    // is not counted as elapsed playhead time.
    realStartMs = UtcNowMs();

    while (m_seqRun.load()) {
        int64_t nowReal = UtcNowMs();
        int64_t simMs   = (mode == GuiSeqMode::TestRunning)
                        ? (playheadStartMs + (nowReal - realStartMs))
                        : nowReal;

        // Update displayed playhead (atomic — render thread reads every frame)
        m_tlPlayheadMs.store(simMs);

        // Iterate camera tracks in timeline order (up to kMaxCamTracks)
        int camIdx = 0;
        for (int ti = 0;
             ti < static_cast<int>(m_tracks.size()) && camIdx < kMaxCamTracks;
             ++ti) {
            if (!m_tracks[ti].IsCamera()) continue;
            auto& tr = m_tracks[ti];
            int&  ni = nextBlock[camIdx];

            // Fire all blocks whose scheduled time has passed.
            // simMs is refreshed at the top of every iteration: SendRequest blocks for
            // 1–3s per bracket/burst, and the stale pre-call simMs would cause all
            // subsequent blocks that became due during that wait to fire instantly.
            while (ni < static_cast<int>(tr.blocks.size())) {
                int64_t nowInner = UtcNowMs();
                simMs = (mode == GuiSeqMode::TestRunning)
                      ? (playheadStartMs + (nowInner - realStartMs))
                      : nowInner;
                m_tlPlayheadMs.store(simMs);

                const TLBlock& blk = tr.blocks[ni];
                if (blk.atMs < 0)    { ++ni; continue; }  // invalid block
                if (blk.atMs > simMs) break;               // not yet

                std::string cmd = BuildBlockCmd(blk, camIdx);

                int     seqNum      = ++m_execSeqNum;
                int64_t firedRealMs = UtcNowMs();
                int64_t driftMs     = firedRealMs - blk.atMs;

                if (!cmd.empty()) {
                    if (auto res = m_pipe.SendRequest(cmd)) {
                        int64_t latMs = UtcNowMs() - firedRealMs;
                        bool ok  = JStr(*res, "ok") == "true";
                        int  lat = JInt(*res, "latency_ms", -1);
                        // lat from SRV = camera execution time; latMs = pipe round-trip
                        // Use whichever is available; they differ by ~10ms pipe overhead.
                        int64_t logLat = (lat >= 0) ? lat : latMs;
                        if (ok && lat >= 0) {
                            std::lock_guard lk(m_camerasMutex);
                            if (camIdx < static_cast<int>(m_cameras.size()))
                                m_cameras[camIdx].lastShotMs = lat;
                        }
                        std::string lbl = blk.label.empty() ? cmd.substr(0, 40) : blk.label;
                        LogLine(std::format(
                            "SEQ seq={} cam={} ok={} lat={} drift={}"
                            " type={} count={} ev={} ss={} lbl={}",
                            seqNum, camIdx, ok ? 1 : 0, logLat, driftMs,
                            BlockTypeName(blk.type), blk.count, blk.ev, blk.ss, lbl));
                        // Collect bracket samples for post-run calibration save
                        if (ok && blk.type == BlockType::Bracket && logLat > 0)
                            m_seqCalibBuf.push_back({blk.count, blk.ev,
                                                     static_cast<int>(logLat)});
                        m_lastResult = std::format("SEQ cam{} {} {}",
                            camIdx, ok ? "OK" : "FAIL",
                            blk.label.empty() ? blk.ss : blk.label);
                    } else {
                        std::string errMsg(PipeErrorMessage(res.error()));
                        LogLine(std::format("SEQ seq={} cam={} ok=-1 err={}",
                            seqNum, camIdx, errMsg));
                        m_lastResult = std::format("SEQ ERROR cam{}: {}", camIdx, errMsg);
                    }
                } else {
                    // Audio / empty block — no pipe call, log for timeline record
                    LogLine(std::format(
                        "SEQ seq={} cam={} ok=0 lat=0 drift={} type={} lbl={}",
                        seqNum, camIdx, driftMs, BlockTypeName(blk.type),
                        blk.label.empty() ? "-" : blk.label));
                }
                // Advance playhead to post-block time before ARM starts.
                // Without this, the playhead freezes for shoot+ARM duration and
                // then jumps; with it, the freeze only covers the ARM phase.
                {
                    int64_t nowPost  = UtcNowMs();
                    int64_t simPost  = (mode == GuiSeqMode::TestRunning)
                                     ? (playheadStartMs + (nowPost - realStartMs))
                                     : nowPost;
                    m_tlPlayheadMs.store(simPost);
                }

                ++ni;
                m_seqNextBlock[camIdx] = ni;  // persist resume index

                // Pre-arm the next block so the camera is ready before it fires.
                // ARM is skipped when the next block has identical params (BlockParamsDiffer).
                {
                    int niNext = ni;
                    const auto& blks = tr.blocks;
                    while (niNext < static_cast<int>(blks.size()) &&
                           blks[niNext].atMs < 0)
                        ++niNext;
                    if (niNext < static_cast<int>(blks.size()) &&
                        blk.type != BlockType::Audio &&
                        BlockParamsDiffer(blk, blks[niNext]))
                        sendArm(blks[niNext], camIdx);
                }
            }
            ++camIdx;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void App::StartSeqThread(GuiSeqMode mode) {
    assert(!m_seqRun.load());
    assert(mode == GuiSeqMode::TestRunning || mode == GuiSeqMode::Running);

    int64_t nowReal       = UtcNowMs();
    int64_t playheadStart = m_tlPlayheadMs.load();

    if (playheadStart < 0) {
        // Fallback: if playhead not set, default to now (shouldn't happen normally)
        playheadStart = nowReal;
        m_tlPlayheadMs.store(playheadStart);
    }

    m_testPlayheadAtStart = playheadStart;
    m_testStartRealMs     = nowReal;

    m_execSeqNum = 0;
    m_seqCalibBuf.clear();
    LogLine(std::format("SEQ_START mode={}",
        (mode == GuiSeqMode::TestRunning) ? "test" : "run"));

    m_seqRun.store(true);
    m_guiSeqMode.store(mode);
    m_seqThread = std::thread([this, mode, playheadStart, nowReal]() {
        SeqThreadProc(mode, playheadStart, nowReal);
    });
    // Audio runs in its own thread — unblocked by camera pipe latency.
    m_audioSeqThread = std::thread([this, mode, playheadStart, nowReal]() {
        AudioSeqThreadProc(mode, playheadStart, nowReal);
    });
}

void App::StopSeqThread() {
    m_seqRun.store(false);
    if (m_seqThread.joinable())      m_seqThread.join();
    if (m_audioSeqThread.joinable()) m_audioSeqThread.join();
    // m_seqNextBlock[] / m_audioNextBlock[] preserved for resume
}

void App::AudioSeqThreadProc(GuiSeqMode mode, int64_t playheadStartMs, int64_t realStartMs) {
    assert(playheadStartMs >= 0 && realStartMs >= 0);
    assert(mode == GuiSeqMode::TestRunning || mode == GuiSeqMode::Running);  // rule 5: only valid run modes

    // Snapshot audio tracks at start so we never race with the render thread.
    std::vector<TLTrack> audioTracks;
    for (const auto& tr : m_tracks)
        if (tr.IsAudio()) audioTracks.push_back(tr);

    // Local next-block indices — written back to m_audioNextBlock on exit.
    int next[kMaxAudioTracks];
    for (int i = 0; i < kMaxAudioTracks; ++i)
        next[i] = m_audioNextBlock[i].load(std::memory_order_relaxed);

    std::wstring exeDir = ExeDir();

    // 5 ms tick loop — completely independent of camera pipe-calls.
    static constexpr int kMaxAudioTracks_ = kMaxAudioTracks; // capture for lambda
    while (m_seqRun.load()) {
        int64_t nowMs  = UtcNowMs();
        int64_t simMs  = (mode == GuiSeqMode::TestRunning)
                       ? (playheadStartMs + (nowMs - realStartMs))
                       : nowMs;

        int ai = 0;
        for (const auto& tr : audioTracks) {
            if (ai >= kMaxAudioTracks_) break;
            int& ni = next[ai];
            while (ni < static_cast<int>(tr.blocks.size())) {
                const TLBlock& blk = tr.blocks[ni];
                if (blk.atMs < 0)      { ++ni; continue; }
                if (blk.atMs > simMs)    break;
                if (!blk.audioFile.empty()) {
                    std::wstring rel = Utf8ToW(blk.audioFile);
                    for (auto& c : rel) if (c == L'/') c = L'\\';
                    MciPlayAsync(exeDir + L"\\" + rel);
                }
                LogLine(std::format("SEQ_AUDIO ai={} idx={} drift={}ms file={}",
                    ai, ni, nowMs - blk.atMs,
                    blk.audioFile.empty() ? "-" : blk.audioFile));
                ++ni;
                m_audioNextBlock[ai].store(ni, std::memory_order_relaxed);  // live: render may read
            }
            ++ai;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for (int i = 0; i < kMaxAudioTracks; ++i) m_audioNextBlock[i].store(next[i], std::memory_order_relaxed);
}

// ─── Audio file scanner ───────────────────────────────────────────────────────

void App::ScanAudioFilesAsync() {
    if (m_audioScanThread.joinable()) m_audioScanThread.join();
    m_audioScanProgress.store(0);
    m_audioScanTotal.store(0);
    m_audioScanThread = std::thread([this]() { AudioScanThreadProc(); });
}

void App::AudioScanThreadProc() {
    assert(m_audioScanProgress.load() == 0);
    assert(m_audioScanTotal.load() == 0);   // rule 5: totals must be reset before scan starts

    // Open separate DB connection (scan thread must not share m_configDb with render thread).
    std::wstring exeDir = ExeDir();
    Database db;
    if (!db.Open(exeDir + L"\\TotalControlConfig.db")) return;
    db.CreateAudioFilesTable();

    // Load set of already-cached language tags — skip those dirs entirely.
    auto cachedLangs = db.LoadAudioCachedLangs();
    auto isCached = [&](const std::string& lang) {
        for (const auto& l : cachedLangs) if (l == lang) return true;
        return false;
    };

    // Collect (lang, filename, fullPath) only for uncached dirs.
    // Dir must match eclipse_audio_XX exactly (2-character language suffix).
    struct FileEntry { std::string lang; std::string filename; std::wstring fullPath; };
    std::vector<FileEntry> files;

    WIN32_FIND_DATAW dirFd{};
    HANDLE dh = FindFirstFileW((exeDir + L"\\eclipse_audio_*").c_str(), &dirFd);
    if (dh != INVALID_HANDLE_VALUE) {
        do {
            if (!(dirFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring dn  = dirFd.cFileName;
            std::string  dns = WToUtf8(dn);
            // Require exactly 2-character suffix: "eclipse_audio_PL" has length 16.
            if (dns.size() != 16) continue;
            std::string lang = dns.substr(14);
            for (char& c : lang) c = static_cast<char>(
                std::toupper(static_cast<unsigned char>(c)));
            // Skip if this lang is already fully cached.
            if (isCached(lang)) continue;

            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((exeDir + L"\\" + dn + L"\\*.mp3").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    files.push_back({lang, WToUtf8(fd.cFileName),
                                     exeDir + L"\\" + dn + L"\\" + fd.cFileName});
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        } while (FindNextFileW(dh, &dirFd));
        FindClose(dh);
    }

    m_audioScanTotal.store(static_cast<int>(files.size()));
    if (files.empty()) {
        LogLine("AudioScan: all eclipse_audio_XX dirs already cached — skipping probe");
        return;
    }

    LogLine(std::format("AudioScan: probing {} new MP3 files", files.size()));

    // Probe each file and save to DB immediately.
    std::map<std::string, int32_t> newEntries;
    for (const auto& f : files) {
        int32_t dur = MciProbeDurMs(f.fullPath);
        if (dur > 0) {
            db.SaveAudioFileDur(f.lang, f.filename, dur);
            newEntries[f.lang + "/" + f.filename] = dur;
        }
        m_audioScanProgress.fetch_add(1);
    }

    // Merge new entries into the in-memory cache under mutex.
    { std::lock_guard lk(m_audioDurMutex);
      for (auto& [k, v] : newEntries) m_audioDurCache[k] = v; }

    LogLine(std::format("AudioScan: {} files probed and cached (audio_files table)",
                        m_audioScanProgress.load()));

    // Signal main thread that scan is done (checked in OnFrame).
    m_audioScanComplete.store(true);
}

// ─── Audio preset ─────────────────────────────────────────────────────────────

void App::LoadAudioPreset(std::string_view lang) {
    assert(!lang.empty());
    assert(lang.size() <= 10);   // rule 5: lang is a short country code ("PL", "EN", …)

    ContactTimes ct;
    { std::lock_guard lk(m_iqpMutex); ct = m_contacts; }
    if (!ct.valid) ct = m_beResult;
    if (!ct.valid || ct.c2Ms <= 0) {
        m_lastResult = "Audio Preset: calculate contacts first (C2 required)";
        return;
    }

    // Reference contacts used for timing offsets.
    // Max = midpoint of totality when not provided by source.
    int64_t c1  = ct.c1Ms;
    int64_t c2  = ct.c2Ms;
    int64_t c3  = ct.c3Ms;
    int64_t c4  = ct.c4Ms;
    int64_t mx  = (ct.maxMs > 0) ? ct.maxMs : (c2 + c3) / 2;

    // Table: {filename, reference_ms, offset_ms, estimated_dur_ms}
    // Timing is derived from the filename convention used in generate_eclipse_audio.py.
    // Short countdown numbers (1–9) are ~1.2 s; mid-speech clips ~4–8 s.
    struct E { const char* file; int64_t ref; int64_t off; int32_t dur; };
    static constexpr E kT[] = {
        // PRE-C1
        {"01_pre_c1_10min.mp3", 0, -600000, 8000},
        {"02_pre_c1_5min.mp3",  0, -300000, 3000},
        {"03_pre_c1_1min.mp3",  0,  -60000, 3000},
        {"04_pre_c1_30s.mp3",   0,  -30000, 2000},
        {"05_pre_c1_15s.mp3",   0,  -15000, 2000},
        {"06_c1.mp3",           0,       0, 4000},
        // PARTIAL PHASE — every 5 min countdown to C2
        {"07_to_c2_60min.mp3",  1, -3600000, 7000},
        {"08_to_c2_55min.mp3",  1, -3300000, 3000},
        {"09_to_c2_50min.mp3",  1, -3000000, 3000},
        {"10_to_c2_45min.mp3",  1, -2700000, 3000},
        {"11_to_c2_40min.mp3",  1, -2400000, 3000},
        {"12_to_c2_35min.mp3",  1, -2100000, 3000},
        {"13_to_c2_30min.mp3",  1, -1800000, 8000},
        {"14_to_c2_25min.mp3",  1, -1500000, 6000},
        {"15_to_c2_20min.mp3",  1, -1200000, 8000},
        {"16_to_c2_15min.mp3",  1,  -900000, 8000},
        {"17_to_c2_10min.mp3",  1,  -600000, 5000},
        // Per-minute
        {"18_to_c2_9min.mp3",   1,  -540000, 8000},
        {"19_to_c2_8min.mp3",   1,  -480000, 8000},
        {"20_to_c2_7min.mp3",   1,  -420000, 5000},
        {"21_to_c2_6min.mp3",   1,  -360000, 6000},
        {"22_to_c2_5min.mp3",   1,  -300000, 3000},
        {"23_to_c2_4min.mp3",   1,  -240000, 2000},
        {"24_to_c2_3min.mp3",   1,  -180000, 5000},
        {"25_to_c2_2min.mp3",   1,  -120000, 6000},
        {"26_to_c2_1min.mp3",   1,   -60000, 6000},
        // Final 45 s
        {"27_to_c2_45s.mp3",    1,   -45000, 6000},
        {"28_to_c2_30s.mp3",    1,   -30000, 6000},
        {"29_to_c2_20s.mp3",    1,   -20000, 8000},
        {"30_to_c2_15s.mp3",    1,   -15000, 2500},
        // 14-1 countdown
        {"31_to_c2_14s.mp3",    1,   -14000, 1200},
        {"32_to_c2_13s.mp3",    1,   -13000, 1200},
        {"33_to_c2_12s.mp3",    1,   -12000, 1200},
        {"34_to_c2_11s.mp3",    1,   -11000, 1200},
        {"35_to_c2_10s.mp3",    1,   -10000, 1200},
        {"36_to_c2_9s.mp3",     1,    -9000, 1200},
        {"37_to_c2_8s.mp3",     1,    -8000, 1200},
        {"38_to_c2_7s.mp3",     1,    -7000, 1200},
        {"39_to_c2_6s.mp3",     1,    -6000, 1200},
        {"40_to_c2_5s.mp3",     1,    -5000, 1200},
        {"41_to_c2_4s.mp3",     1,    -4000, 1200},
        {"42_to_c2_3s.mp3",     1,    -3000, 1200},
        {"43_to_c2_2s.mp3",     1,    -2000, 1200},
        {"44_to_c2_1s.mp3",     1,    -1000, 1200},
        // C2 — TOTALITY BEGIN
        {"45_c2_totality.mp3",  1,        0, 3000},
        // TOTALITY — countdown to max eclipse (Max-relative)
        {"46_to_max_50s.mp3",   4,   -50000, 6000},
        {"47_to_max_40s.mp3",   4,   -40000, 3000},
        {"48_to_max_30s.mp3",   4,   -30000, 3000},
        {"49_to_max_20s.mp3",   4,   -20000, 3000},
        {"50_to_max_10s.mp3",   4,   -10000, 3000},
        {"51_to_max_now.mp3",   4,        0, 2000},
        {"52_max_eclipse.mp3",  4,     2000, 4000},
        // TOTALITY — countdown to C3
        {"53_to_c3_50s.mp3",    2,   -50000, 3000},
        {"54_to_c3_40s.mp3",    2,   -40000, 3000},
        {"55_to_c3_30s.mp3",    2,   -30000, 3000},
        {"56_to_c3_20s.mp3",    2,   -20000, 3000},
        {"57_to_c3_10s.mp3",    2,   -10000, 3000},
        // 9-1 countdown to C3
        {"58_to_c3_9s.mp3",     2,    -9000, 1200},
        {"59_to_c3_8s.mp3",     2,    -8000, 1200},
        {"60_to_c3_7s.mp3",     2,    -7000, 1200},
        {"61_to_c3_6s.mp3",     2,    -6000, 1200},
        {"62_to_c3_5s.mp3",     2,    -5000, 1200},
        {"63_to_c3_4s.mp3",     2,    -4000, 1200},
        {"64_to_c3_3s.mp3",     2,    -3000, 1200},
        {"65_to_c3_2s.mp3",     2,    -2000, 1200},
        {"66_to_c3_1s.mp3",     2,    -1000, 1200},
        // C3 — TOTALITY END
        {"67_c3.mp3",           2,        0, 4000},
        {"68_post_c3_filters.mp3", 2,   3000, 5000},
        // PARTIAL PHASE — countdown to C4
        {"69_to_c4_60min.mp3",  3, -3600000, 4000},
        {"70_to_c4_50min.mp3",  3, -3000000, 3000},
        {"71_to_c4_40min.mp3",  3, -2400000, 3000},
        {"72_to_c4_30min.mp3",  3, -1800000, 3000},
        {"73_to_c4_20min.mp3",  3, -1200000, 3000},
        {"74_to_c4_10min.mp3",  3,  -600000, 3000},
        // Final 10 s to C4
        {"75_to_c4_10s.mp3",    3,   -10000, 3000},
        {"76_to_c4_9s.mp3",     3,    -9000, 1200},
        {"77_to_c4_8s.mp3",     3,    -8000, 1200},
        {"78_to_c4_7s.mp3",     3,    -7000, 1200},
        {"79_to_c4_6s.mp3",     3,    -6000, 1200},
        {"80_to_c4_5s.mp3",     3,    -5000, 1200},
        {"81_to_c4_4s.mp3",     3,    -4000, 1200},
        {"82_to_c4_3s.mp3",     3,    -3000, 1200},
        {"83_to_c4_2s.mp3",     3,    -2000, 1200},
        {"84_to_c4_1s.mp3",     3,    -1000, 1200},
        // C4 — ECLIPSE END
        {"85_c4_end.mp3",       3,        0, 8000},
    };

    // ref codes: 0=C1, 1=C2, 2=C3, 3=C4, 4=Max
    auto refMs = [&](int r) -> int64_t {
        switch (r) { case 0: return c1; case 1: return c2;
                     case 2: return c3; case 3: return c4; default: return mx; }
    };

    // Prefix e.g. "eclipse_audio_PL\" (upper-case lang)
    std::string prefix = "eclipse_audio_";
    for (char ch : lang) prefix += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    prefix += '\\';

    // Find the first audio track and replace its blocks
    TLTrack* audioTrk = nullptr;
    for (auto& tr : m_tracks)
        if (tr.IsAudio()) { audioTrk = &tr; break; }
    if (!audioTrk) { m_lastResult = "Audio Preset: no audio track"; return; }

    // Snapshot cache under mutex — read-only from this point
    std::map<std::string, int32_t> durCache;
    { std::lock_guard lk(m_audioDurMutex); durCache = m_audioDurCache; }

    audioTrk->blocks.clear();
    int loaded = 0;
    for (const auto& e : kT) {
        int64_t base = refMs(static_cast<int>(e.ref));
        if (base <= 0) continue;
        // Prefer DB-cached duration; fall back to compile-time estimate.
        std::string cacheKey = std::string(lang) + "/" + e.file;
        auto it = durCache.find(cacheKey);
        TLBlock b;
        b.type       = BlockType::Audio;
        b.atMs       = base + e.off;
        b.audioFile  = prefix + e.file;
        b.audioDurMs = (it != durCache.end()) ? it->second : e.dur;
        b.label      = e.file;
        audioTrk->blocks.push_back(std::move(b));
        ++loaded;
    }

    // Sort by time (entries are already ordered but contacts could be non-monotonic)
    std::sort(audioTrk->blocks.begin(), audioTrk->blocks.end(),
              [](const TLBlock& a, const TLBlock& b) { return a.atMs < b.atMs; });

    m_tlDirty    = true;
    m_lastResult = std::format("Audio Preset {} loaded — {} blocks", lang, loaded);
    LogLine(m_lastResult);
}

// ─── Photo preset ─────────────────────────────────────────────────────────────

void App::AddPhotoPreset() {
    assert(m_presetTargetTrack >= 0);
    assert(m_presetTargetTrack < static_cast<int>(m_tracks.size()));  // rule 5: target track must exist

    ContactTimes ct;
    { std::lock_guard lk(m_iqpMutex); ct = m_contacts; }
    if (!ct.valid) ct = m_beResult;
    if (!ct.valid || ct.c1Ms <= 0 || ct.c4Ms <= 0) {
        m_lastResult = "One Picture Per Minute: calculate contacts first (C1 and C4 required)";
        return;
    }

    // Validate/fix target track — must be a camera track.
    int ti = m_presetTargetTrack;
    if (ti < 0 || ti >= static_cast<int>(m_tracks.size()) || m_tracks[ti].IsAudio()) {
        ti = -1;
        for (int i = 0; i < static_cast<int>(m_tracks.size()); ++i)
            if (!m_tracks[i].IsAudio()) { ti = i; break; }
        if (ti < 0) { m_lastResult = "One Picture Per Minute: no camera track available"; return; }
        m_presetTargetTrack = ti;
    }

    int64_t startMs = ct.c1Ms - 5LL * 60 * 1000;
    int64_t endMs   = ct.c4Ms + 5LL * 60 * 1000;
    static constexpr int64_t kStepMs = 60LL * 1000;

    TLTrack& trk = m_tracks[ti];
    trk.blocks.clear();

    // Skip C2..C3 (totality) — that window gets its own bracket sequence via
    // AddTotalityBracketPreset(); one single shot per minute isn't meant for
    // the fast-changing corona.
    bool hasTotality = ct.c2Ms > 0 && ct.c3Ms > 0;

    int count = 0;
    for (int64_t t = startMs; t <= endMs; t += kStepMs) {
        if (hasTotality && t >= ct.c2Ms && t <= ct.c3Ms) continue;
        TLBlock b;
        b.type   = BlockType::Single;
        b.atMs   = t;
        b.ss     = "1/8000";
        b.iso    = 100;
        b.fstop  = "6.3";
        b.count  = 1;
        trk.blocks.push_back(std::move(b));
        ++count;
    }

    m_tlDirty    = true;
    m_lastResult = std::format("One Picture Per Minute: {} blocks on track \"{}\"", count, trk.label);
    LogLine(m_lastResult);
}

void App::AddTotalityBracketPreset() {
    assert(m_presetTargetTrack >= 0);
    assert(m_presetTargetTrack < static_cast<int>(m_tracks.size()));  // rule 5: target track must exist

    ContactTimes ct;
    { std::lock_guard lk(m_iqpMutex); ct = m_contacts; }
    if (!ct.valid) ct = m_beResult;
    if (!ct.valid || ct.c2Ms <= 0 || ct.c3Ms <= 0) {
        m_lastResult = "Totality Brackets: calculate contacts first (C2 and C3 required)";
        return;
    }

    // Validate/fix target track — must be a camera track.
    int ti = m_presetTargetTrack;
    if (ti < 0 || ti >= static_cast<int>(m_tracks.size()) || m_tracks[ti].IsAudio()) {
        ti = -1;
        for (int i = 0; i < static_cast<int>(m_tracks.size()); ++i)
            if (!m_tracks[i].IsAudio()) { ti = i; break; }
        if (ti < 0) { m_lastResult = "Totality Brackets: no camera track available"; return; }
        m_presetTargetTrack = ti;
    }

    TLTrack& trk = m_tracks[ti];

    // Replace only the C2..C3 window — leaves any partial-phase blocks
    // outside totality untouched, so this preset can be (re)run independently
    // of "One Picture Per Minute".
    trk.blocks.erase(
        std::remove_if(trk.blocks.begin(), trk.blocks.end(),
            [&](const TLBlock& b) { return b.atMs >= ct.c2Ms && b.atMs <= ct.c3Ms; }),
        trk.blocks.end());

    static constexpr int64_t kStepMs = 3LL * 1000;
    int count = 0;
    for (int64_t t = ct.c2Ms; t <= ct.c3Ms; t += kStepMs) {
        TLBlock b;
        b.type  = BlockType::Bracket;
        b.atMs  = t;
        b.ss    = "1/125";
        b.iso   = 100;
        b.fstop = "8.0";
        b.count = 5;
        b.ev    = "1.0ev";
        trk.blocks.push_back(std::move(b));
        ++count;
    }
    std::sort(trk.blocks.begin(), trk.blocks.end(),
              [](const TLBlock& a, const TLBlock& b) { return a.atMs < b.atMs; });

    m_tlDirty    = true;
    m_lastResult = std::format("Totality Brackets: {} blocks on track \"{}\"", count, trk.label);
    LogLine(m_lastResult);
}

// Adds one Bracket block per entry in the Inspector's bracket-mode dropdown
// (the same 16 ev/count combinations as `kBrk` in RenderInspectorColumn) —
// a calibration/verification run to exercise every supported bracket variant.
// Blocks are placed back-to-back: each starts 2s after the end of the
// previous block's ARM (every consecutive pair has different ev/count, so
// ARM always applies between them).
void App::AddAllBracketVariantsPreset() {
    assert(m_presetTargetTrack >= 0);
    assert(m_presetTargetTrack < static_cast<int>(m_tracks.size()));  // rule 5: target track must exist

    // Validate/fix target track — must be a camera track.
    int ti = m_presetTargetTrack;
    if (ti < 0 || ti >= static_cast<int>(m_tracks.size()) || m_tracks[ti].IsAudio()) {
        ti = -1;
        for (int i = 0; i < static_cast<int>(m_tracks.size()); ++i)
            if (!m_tracks[i].IsAudio()) { ti = i; break; }
        if (ti < 0) { m_lastResult = "All Bracket Variants: no camera track available"; return; }
        m_presetTargetTrack = ti;
    }

    // Mirrors kBrk in RenderInspectorColumn — 16 valid modes matching CrSDK.
    // 2.0ev / 3.0ev support x3 and x5 only (SDK limitation — no 9-pic).
    struct BrkVariant { const char* ev; int count; };
    static constexpr BrkVariant kVariants[] = {
        {"0.3ev", 3}, {"0.3ev", 5}, {"0.3ev", 9},
        {"0.5ev", 3}, {"0.5ev", 5}, {"0.5ev", 9},
        {"0.7ev", 3}, {"0.7ev", 5}, {"0.7ev", 9},
        {"1.0ev", 3}, {"1.0ev", 5}, {"1.0ev", 9},
        {"2.0ev", 3}, {"2.0ev", 5},
        {"3.0ev", 3}, {"3.0ev", 5},
    };
    static constexpr int kNumVariants = static_cast<int>(std::size(kVariants));
    static_assert(kNumVariants == 16, "must mirror the Inspector's bracket-mode dropdown");
    static constexpr int64_t kGapMs = 2000; // ARM-end -> next block start

    TLTrack& trk = m_tracks[ti];
    trk.blocks.clear();

    int64_t startMs = m_tlPlayheadMs.load();
    if (startMs < 0) startMs = UtcNowMs();

    TLBlock prev;
    int64_t atMs = startMs;
    for (int i = 0; i < kNumVariants; ++i) {
        TLBlock b;
        b.type  = BlockType::Bracket;
        b.ss    = "1/125";
        b.iso   = 100;
        b.fstop = "8.0";
        b.ev    = kVariants[i].ev;
        b.count = kVariants[i].count;

        if (i > 0)
            atMs = atMs + BlockDurMs(prev) + ArmEstMs(prev) + kGapMs;
        b.atMs = atMs;

        trk.blocks.push_back(b);
        prev = b;
    }

    m_tlDirty    = true;
    m_lastResult = std::format("All Bracket Variants: {} blocks on track \"{}\"",
                                kNumVariants, trk.label);
    LogLine(m_lastResult);
}

// ─── Track initialization ─────────────────────────────────────────────────────

void App::InitTracks() {
    if (!m_tracks.empty()) return;

    TLTrack cam1;
    cam1.type     = "camera";
    cam1.cameraId = "ILCE-7RM4A";
    cam1.label    = "Cam1 ILCE-7RM4A";
    m_tracks.push_back(std::move(cam1));

    TLTrack audio;
    audio.type  = "audio";
    audio.label = "Audio";
    m_tracks.push_back(std::move(audio));
}

// ─── Sequencer buttons (Col1 — TEST RUN / RUN) ───────────────────────────────
//
// TEST buttons (yellow/orange):  TEST RUN starts/resumes from playhead; STOP pauses.
// RUN  buttons (red):            RUN fires on real UTC time; STOP RUN stops.
// During any active run: timeline editing and interactive pipe buttons are locked.

void App::RenderSequencerButtons() {
    assert(m_fontMono != nullptr);
    assert(m_fontLarge != nullptr);

    static const ImVec4 kGray   {0.40f, 0.40f, 0.45f, 1.0f};
    static const ImVec4 kDim    {0.28f, 0.28f, 0.32f, 1.0f};
    static const ImVec4 kGreen  {0.20f, 0.85f, 0.30f, 1.0f};
    static const ImVec4 kWhiteTxt {1.00f, 1.00f, 1.00f, 1.0f};
    // TEST palette — yellow/amber
    static const ImVec4 kTestBg    {0.35f, 0.28f, 0.04f, 1.0f};
    static const ImVec4 kTestHover {0.60f, 0.48f, 0.06f, 1.0f};
    // RUN palette — red
    static const ImVec4 kRunBg     {0.38f, 0.06f, 0.06f, 1.0f};
    static const ImVec4 kRunHover  {0.65f, 0.10f, 0.10f, 1.0f};
    // STOP palette — neutral dark
    static const ImVec4 kStopBg    {0.22f, 0.22f, 0.26f, 1.0f};
    static const ImVec4 kStopHover {0.35f, 0.35f, 0.40f, 1.0f};

    GuiSeqMode sm   = m_guiSeqMode.load();
    bool connected  = (m_pipe.GetState() == PipeClient::State::Connected);
    bool hasBlocks  = false;
    for (auto& tr : m_tracks)
        if (tr.IsCamera() && !tr.blocks.empty()) { hasBlocks = true; break; }

    const float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const ImVec2 kBtnSz(halfW, 30);

    ImGui::Spacing();
    ImGui::SeparatorText("EXECUTE");

    ImGui::PushFont(m_fontMono);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

    // Playhead time display moved to the Sky View Simulator status bar
    // (next to "Obs=") — this column was too narrow to show it legibly.

    // ── TEST RUN / STOP ───────────────────────────────────────────────────
    bool testRunning = (sm == GuiSeqMode::TestRunning);
    bool testPaused  = (sm == GuiSeqMode::TestPaused);
    bool anyRun      = (sm == GuiSeqMode::Running);

    // TEST RUN  (disabled: no blocks, no connection, RUN is active)
    bool canTestStart = connected && hasBlocks && !anyRun && !testRunning;
    if (!canTestStart) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        kTestBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kTestHover);
    ImGui::PushStyleColor(ImGuiCol_Text,          kWhiteTxt);
    const char* testBtnLabel = testPaused
        ? "RESUME TEST"
        : "TEST RUN";
    if (ImGui::Button(testBtnLabel, kBtnSz)) {
        if (!testPaused) {
            // Fresh start: reset per-track indices to blocks at/after playhead
            int64_t ph     = m_tlPlayheadMs.load();
            int     camIdx = 0;
            for (int ti = 0;
                 ti < static_cast<int>(m_tracks.size()) && camIdx < kMaxCamTracks;
                 ++ti) {
                if (!m_tracks[ti].IsCamera()) continue;
                int ni = 0;
                for (; ni < static_cast<int>(m_tracks[ti].blocks.size()); ++ni)
                    if (m_tracks[ti].blocks[ni].atMs >= ph) break;
                m_seqNextBlock[camIdx++] = ni;
            }
            int audioIdx = 0;
            for (int ti = 0;
                 ti < static_cast<int>(m_tracks.size()) && audioIdx < kMaxAudioTracks;
                 ++ti) {
                if (!m_tracks[ti].IsAudio()) continue;
                int ni = 0;
                for (; ni < static_cast<int>(m_tracks[ti].blocks.size()); ++ni)
                    if (m_tracks[ti].blocks[ni].atMs >= ph) break;
                m_audioNextBlock[audioIdx++].store(ni, std::memory_order_relaxed);
            }
        }
        StartSeqThread(GuiSeqMode::TestRunning);
        LogLine("Sequencer: TEST RUN started");
    }
    ImGui::PopStyleColor(3);
    if (!canTestStart) ImGui::EndDisabled();

    ImGui::SameLine();

    // STOP TEST  (enabled only when TestRunning or TestPaused)
    bool canTestStop = testRunning || testPaused;
    if (!canTestStop) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        kStopBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kStopHover);
    ImGui::PushStyleColor(ImGuiCol_Text,          kWhiteTxt);
    if (ImGui::Button("STOP TEST", kBtnSz)) {
        if (testRunning) {
            StopSeqThread();
            m_guiSeqMode.store(GuiSeqMode::TestPaused);
            LogLine("Sequencer: TEST paused");
        } else if (testPaused) {
            // Full reset to idle
            m_guiSeqMode.store(GuiSeqMode::Idle);
            m_tlPlayheadMs.store(-1);    // resets to C2-45s on next frame
            for (int i = 0; i < kMaxCamTracks;  ++i) m_seqNextBlock[i]   = 0;
            for (int i = 0; i < kMaxAudioTracks; ++i) m_audioNextBlock[i].store(0, std::memory_order_relaxed);
            LogLine("Sequencer: TEST reset to idle");
        }
    }
    ImGui::PopStyleColor(3);
    if (!canTestStop) ImGui::EndDisabled();

    // ── RUN (production — real UTC time) ─────────────────────────────────
    bool canRun = connected && hasBlocks && !anyRun && !testRunning;
    if (!canRun) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        kRunBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRunHover);
    ImGui::PushStyleColor(ImGuiCol_Text,          kWhiteTxt);
    if (ImGui::Button("RUN", kBtnSz)) {
        // RUN always starts from beginning of all tracks
        for (int i = 0; i < kMaxCamTracks;  ++i) m_seqNextBlock[i]   = 0;
        for (int i = 0; i < kMaxAudioTracks; ++i) m_audioNextBlock[i].store(0, std::memory_order_relaxed);
        StartSeqThread(GuiSeqMode::Running);
        LogLine("Sequencer: RUN started (production)");
        m_lastResult = "RUN started — real UTC timing";
    }
    ImGui::PopStyleColor(3);
    if (!canRun) ImGui::EndDisabled();

    ImGui::SameLine();

    bool canStopRun = anyRun;
    if (!canStopRun) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        kStopBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kStopHover);
    ImGui::PushStyleColor(ImGuiCol_Text,          kWhiteTxt);
    if (ImGui::Button("STOP RUN", kBtnSz)) {
        StopSeqThread();
        m_guiSeqMode.store(GuiSeqMode::Idle);
        LogLine("Sequencer: RUN stopped");
        m_lastResult = "RUN stopped";
    }
    ImGui::PopStyleColor(3);
    if (!canStopRun) ImGui::EndDisabled();

    // ── Save calibration (visible after any run that produced bracket samples) ─
    bool idle = (sm == GuiSeqMode::Idle || sm == GuiSeqMode::TestPaused);
    if (idle && !m_seqCalibBuf.empty()) {
        ImGui::Spacing();
        static const ImVec4 kCalibBg    {0.06f, 0.24f, 0.38f, 1.0f};
        static const ImVec4 kCalibHover {0.10f, 0.40f, 0.65f, 1.0f};

        // Determine camera model from first connected camera, or "Unknown"
        std::string camModel;
        {
            std::lock_guard lk(m_camerasMutex);
            if (!m_cameras.empty() && !m_cameras[0].model.empty())
                camModel = m_cameras[0].model;
        }
        if (camModel.empty()) camModel = "Unknown";

        std::string btnLbl = std::format(
            "SAVE CALIB: {} ({} smp)", camModel, m_seqCalibBuf.size());
        ImGui::PushStyleColor(ImGuiCol_Button,        kCalibBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kCalibHover);
        ImGui::PushStyleColor(ImGuiCol_Text,          kWhiteTxt);
        if (ImGui::Button(btnLbl.c_str(), ImVec2(-1, 30)))
            SaveCalibFromBuf(camModel);
        ImGui::PopStyleColor(3);
    }

    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::Spacing();
}

static void FormatCountdown(int64_t diffMs, char* buf, int len);  // defined below

// ─── Status column (middle-right of top area) ─────────────────────────────────

void App::RenderStatusColumn() {
    RenderSolarView();
}

// ─── Solar system wireframe view ──────────────────────────────────────────────

void App::RenderSolarView() {
    assert(m_fontMono != nullptr);
    ImGui::SeparatorText("SKY VIEW SIMULATOR");
    ImGui::Spacing();
    // Upload any newly decoded SUVI frames to D3D11 (render-thread only)
    if (m_suviNewFrames.load()) CreateSuviTextures();
    // Periodic re-fetch: refresh every 30 min so the animation stays current
    // during long sessions (e.g. hours of eclipse processing on-site).
    static constexpr int64_t kSuviRefetchMs = 1LL * 60 * 1000;
    if (const int64_t fetchedAt = m_suviFetchedAtMs.load();
        !m_suviFetching.load() && fetchedAt > 0 &&
        UtcNowMs() - fetchedAt > kSuviRefetchMs) {
        TriggerSuviFetch();
    }
    static constexpr float kPi = 3.14159265358979323846f;
    static const ImVec4    kGray{0.40f, 0.40f, 0.45f, 1.0f};

    ImVec2      avail = ImGui::GetContentRegionAvail();
    ImVec2      p0    = ImGui::GetCursorScreenPos();
    ImDrawList* dl    = ImGui::GetWindowDrawList();

    // Full-width rectangle — no gray bars on the sides
    float szW = avail.x;
    float szH = avail.y - 40.f;
    if (szH < 80.f || szW < 80.f) return;

    float ox = p0.x;
    float oy = p0.y;
    float cx = ox + szW * 0.5f;
    float cy = oy + szH * 0.5f;

    // Interaction zone — captures mouse wheel for zoom
    ImGui::InvisibleButton("##sv", {szW, szH});
    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f)
            m_solarZoom = std::clamp(m_solarZoom * (1.f + wheel * 0.15f), 0.2f, 20.f);
    }

    // Scale: ±5° FoV at zoom=1; zoom=2 shows ±2.5°
    static constexpr float kViewHalf = 5.0f;
    float szMin = std::min(szW, szH);
    float scale = szMin * m_solarZoom / (2.f * kViewHalf);

    // ── Simulator time: playhead when available, else real UTC ────────────────
    int64_t simMs = m_tlPlayheadMs.load();
    if (simMs < 0) simMs = UtcNowMs();

    // ── Snapshot ephemeris once per frame (single mutex lock) ────────────────
    std::vector<EphRow> sunSnap, moonSnap;
    {
        std::lock_guard lk(m_ephMutex);
        sunSnap  = m_ephSamples[static_cast<size_t>(EphBody::Sun)];
        moonSnap = m_ephSamples[static_cast<size_t>(EphBody::Moon)];
    }
    bool hasEph = !sunSnap.empty() && !moonSnap.empty();

    // Inline interpolation from local snapshot (no mutex per call)
    auto interpSnap = [&](const std::vector<EphRow>& rows, int64_t ms) -> EphRow {
        assert(!rows.empty());
        if (ms <= rows.front().utc_ms) return rows.front();
        if (ms >= rows.back().utc_ms)  return rows.back();
        size_t lo = 0, hi = rows.size() - 1;
        while (lo + 1 < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (rows[mid].utc_ms <= ms) lo = mid; else hi = mid;
        }
        double t = double(ms - rows[lo].utc_ms)
                 / double(rows[hi].utc_ms - rows[lo].utc_ms);
        EphRow r; r.utc_ms = ms;
        r.ra_deg          = rows[lo].ra_deg          + t*(rows[hi].ra_deg          - rows[lo].ra_deg);
        r.dec_deg         = rows[lo].dec_deg         + t*(rows[hi].dec_deg         - rows[lo].dec_deg);
        r.az_deg          = rows[lo].az_deg          + t*(rows[hi].az_deg          - rows[lo].az_deg);
        r.alt_deg         = rows[lo].alt_deg         + t*(rows[hi].alt_deg         - rows[lo].alt_deg);
        r.ang_diam_arcmin = rows[lo].ang_diam_arcmin + t*(rows[hi].ang_diam_arcmin - rows[lo].ang_diam_arcmin);
        return r;
    };

    EphRow sunEph, moonEph;
    if (hasEph) {
        sunEph  = interpSnap(sunSnap,  simMs);
        moonEph = interpSnap(moonSnap, simMs);
    }

    float sunR  = 0.267f * scale;
    float moonR = 0.278f * scale;
    float moonX = cx, moonY = cy;
    float moonVrad  = 0.f;   // Moon north pole PA from zenith (for drawing)
    bool  hasMoonV  = false;

    if (hasEph && sunEph.utc_ms > 0 && moonEph.utc_ms > 0) {
        sunR  = static_cast<float>(sunEph.ang_diam_arcmin  / 2.0 / 60.0) * scale;
        moonR = static_cast<float>(moonEph.ang_diam_arcmin / 2.0 / 60.0) * scale;
        float cosAlt = cosf(static_cast<float>(sunEph.alt_deg) * kPi / 180.f);
        float dAz    = static_cast<float>(moonEph.az_deg  - sunEph.az_deg);
        float dAlt   = static_cast<float>(moonEph.alt_deg - sunEph.alt_deg);
        moonX = cx + dAz  * cosAlt * scale;
        moonY = cy - dAlt * scale;
        double solarP0 = ComputeP0(sunEph.ra_deg, sunEph.dec_deg);
        double q       = ComputeQ (sunEph.ra_deg, sunEph.dec_deg,
                                   m_obsLat, m_obsLon, simMs);
        m_solarP    = static_cast<float>(solarP0);
        m_solarQ    = static_cast<float>(q);
        m_sunAltDeg = sunEph.alt_deg;
        m_sunAzDeg  = sunEph.az_deg;
        double moonV0 = ComputeMoonV(moonEph.ra_deg, moonEph.dec_deg);
        moonVrad      = static_cast<float>((moonV0 - q) * kPi / 180.0);
        hasMoonV      = true;
    }

    // ── Obscuration ─────────────────────────────────────────────────────────────
    // Fraction of the Sun's disc area covered by the Moon (lens formula).
    float moonDist = sqrtf((moonX - cx) * (moonX - cx) + (moonY - cy) * (moonY - cy));
    float obscuration = 0.f;
    {
        float R1 = sunR, R2 = moonR, d = moonDist;
        if (d < R1 + R2) {
            if (d + R1 <= R2)       obscuration = 1.f;           // total: Moon covers Sun
            else if (d + R2 <= R1)  obscuration = (R2*R2)/(R1*R1); // annular: Moon inside Sun
            else {
                float alpha = acosf(std::clamp((d*d + R1*R1 - R2*R2) / (2.f*d*R1), -1.f, 1.f));
                float beta  = acosf(std::clamp((d*d + R2*R2 - R1*R1) / (2.f*d*R2), -1.f, 1.f));
                float s     = (-d+R1+R2) * (d+R1-R2) * (d-R1+R2) * (d+R1+R2);
                float area  = R1*R1 * alpha + R2*R2 * beta - 0.5f * sqrtf(std::max(0.f, s));
                obscuration = std::clamp(area / (kPi * R1 * R1), 0.f, 1.f);
            }
        }
    }

    // cosAltSun: projection factor for az→screen (constant per frame)
    float cosAltSun = cosf((hasEph && sunEph.utc_ms > 0
                           ? (float)sunEph.alt_deg : 8.f) * kPi / 180.f);

    // N☉ drawing angle from screen vertical (zenith) = P₀ − q
    float P_rad = (m_solarP - m_solarQ) * kPi / 180.f;

    // ── Moon-Sun relative motion → C2/C3 contact side directions ────────────
    // Direction of Moon's angular velocity relative to Sun (in screen coords).
    // C2 = approaching side (opposite to motion), C3 = departing side.
    float kC2ux_dyn = 0.f, kC2uy_dyn = 0.f;
    float kC3ux_dyn = 0.f, kC3uy_dyn = 0.f;
    bool  hasContactDir = false;
    if (hasEph && sunSnap.size() > 1 && moonSnap.size() > 1) {
        size_t idx0 = 0;
        for (size_t i = 0; i + 1 < moonSnap.size(); ++i) {
            if (moonSnap[i].utc_ms <= simMs) idx0 = i; else break;
        }
        size_t idx1 = std::min(idx0 + 1, moonSnap.size() - 1);
        if (idx0 != idx1) {
            size_t si0 = std::min(idx0, sunSnap.size() - 1);
            size_t si1 = std::min(idx1, sunSnap.size() - 1);
            double relAz0  = moonSnap[idx0].az_deg  - sunSnap[si0].az_deg;
            double relAz1  = moonSnap[idx1].az_deg  - sunSnap[si1].az_deg;
            double relAlt0 = moonSnap[idx0].alt_deg - sunSnap[si0].alt_deg;
            double relAlt1 = moonSnap[idx1].alt_deg - sunSnap[si1].alt_deg;
            float  vx      = static_cast<float>((relAz1 - relAz0) * cosAltSun);
            float  vy      = -static_cast<float>(relAlt1 - relAlt0);  // y-down
            float  vlen    = sqrtf(vx * vx + vy * vy);
            if (vlen > 1e-6f) {
                kC3ux_dyn =  vx / vlen;  kC3uy_dyn =  vy / vlen;
                kC2ux_dyn = -vx / vlen;  kC2uy_dyn = -vy / vlen;
                hasContactDir = true;
            }
        }
    }

    // ── Clip rect (prevents drawing outside view area) ────────────────────────
    dl->PushClipRect({ox, oy}, {ox+szW, oy+szH}, true);

    // ── Background ────────────────────────────────────────────────────────────
    dl->AddRectFilled({ox, oy}, {ox+szW, oy+szH}, IM_COL32(6, 8, 16, 255));

    // View center in sky coordinates (current Sun position or fallback)
    double centerAz  = (hasEph && sunEph.utc_ms > 0) ? sunEph.az_deg  : 285.0;
    double centerAlt = (hasEph && sunEph.utc_ms > 0) ? sunEph.alt_deg :   8.0;
    // cosAltSun already computed before PushClipRect — reused here

    // Projection: (az, alt) → screen. Center of view = current Sun.
    auto projectAbs = [&](double az, double alt) -> ImVec2 {
        float dAz  = static_cast<float>(az  - centerAz);
        float dAlt = static_cast<float>(alt - centerAlt);
        while (dAz >  180.f) dAz -= 360.f;
        while (dAz < -180.f) dAz += 360.f;
        return { cx + dAz * cosAltSun * scale, cy - dAlt * scale };
    };

    // ── Alt/Az grid ───────────────────────────────────────────────────────────
    {
        float viewHalfDeg = kViewHalf / m_solarZoom;
        float gd = (viewHalfDeg > 10.f) ? 5.f
                 : (viewHalfDeg >  3.f) ? 1.f : 0.5f;
        ImU32 gCol = IM_COL32(28, 36, 55, 100);
        ImU32 lCol = IM_COL32(48, 60, 85, 170);

        // Altitude lines (horizontal in this view)
        float altBase = std::floorf(static_cast<float>(centerAlt) / gd) * gd;
        for (int k = -12; k <= 12; ++k) {
            float a  = altBase + k * gd;
            if (a < -15.f || a > 90.f) continue;
            float sy = cy - (a - static_cast<float>(centerAlt)) * scale;
            if (sy < oy - 1.f || sy > oy + szH + 1.f) continue;
            dl->AddLine({ox, sy}, {ox + szW, sy}, gCol, 1.f);
            char lbl[8]; snprintf(lbl, sizeof(lbl), "%.0f\xc2\xb0", a);
            dl->AddText({ox + 4.f, sy - 13.f}, lCol, lbl);
        }

        // Azimuth lines (vertical in this view)
        float azBase = std::floorf(static_cast<float>(centerAz) / gd) * gd;
        for (int k = -12; k <= 12; ++k) {
            float az  = azBase + k * gd;
            float dAz = az - static_cast<float>(centerAz);
            float sx  = cx + dAz * cosAltSun * scale;
            if (sx < ox - 1.f || sx > ox + szW + 1.f) continue;
            dl->AddLine({sx, oy}, {sx, oy + szH}, gCol, 1.f);
            char lbl[8]; snprintf(lbl, sizeof(lbl), "%.0f\xc2\xb0", az);
            ImVec2 ts = ImGui::CalcTextSize(lbl);
            dl->AddText({sx - ts.x * 0.5f, oy + szH - ts.y - 2.f}, lCol, lbl);
        }
    }

    // ── Horizon (alt = 0) ─────────────────────────────────────────────────────
    {
        float hY = cy + static_cast<float>(centerAlt) * scale;
        if (hY > oy && hY < oy + szH) {
            dl->AddLine({ox, hY}, {ox + szW, hY}, IM_COL32(50, 65, 90, 220), 2.f);
            dl->AddText({ox + 4.f, hY + 2.f}, IM_COL32(50, 65, 90, 200), "horizon");
        }
    }

    // ── Zenith direction ──────────────────────────────────────────────────────
    for (float y = cy - 4.f; y > oy + 20.f; y -= 9.f)
        dl->AddLine({cx, y}, {cx, std::max(y - 5.f, oy + 20.f)},
                    IM_COL32(50, 65, 90, 100), 1.f);
    dl->AddTriangleFilled({cx - 4.f, oy + 22.f}, {cx + 4.f, oy + 22.f},
                           {cx,       oy + 14.f}, IM_COL32(50, 65, 90, 120));
    dl->AddText({cx + 5.f, oy + 10.f}, IM_COL32(50, 65, 90, 160), "Z");

    // ── Sun trajectory — absolute 24h arc ────────────────────────────────────
    if (hasEph && sunSnap.size() > 1) {
        for (size_t i = 1; i < sunSnap.size(); ++i) {
            float dAz0 = static_cast<float>(sunSnap[i-1].az_deg - sunEph.az_deg);
            float dAz1 = static_cast<float>(sunSnap[i  ].az_deg - sunEph.az_deg);
            if (std::fabs(dAz1 - dAz0) > 180.f) continue;  // az wrap gap
            ImVec2 a = projectAbs(sunSnap[i-1].az_deg, sunSnap[i-1].alt_deg);
            ImVec2 b = projectAbs(sunSnap[i  ].az_deg, sunSnap[i  ].alt_deg);
            dl->AddLine(a, b, IM_COL32(200, 140, 30, 110), 1.2f);
        }
        // Hour marks with UTC hour labels
        for (size_t i = 0; i < sunSnap.size(); ++i) {
            if (sunSnap[i].utc_ms % 3600000 != 0) continue;
            ImVec2 pt = projectAbs(sunSnap[i].az_deg, sunSnap[i].alt_deg);
            dl->AddCircleFilled(pt, 2.5f, IM_COL32(200, 140, 30, 200));
            int hh = static_cast<int>(sunSnap[i].utc_ms / 3600000 % 24);
            char tl[4]; snprintf(tl, sizeof(tl), "%02d", hh);
            dl->AddText({pt.x + 3.f, pt.y - 12.f}, IM_COL32(200, 140, 30, 160), tl);
        }
    }

    // ── Moon trajectory — absolute 24h arc ───────────────────────────────────
    if (hasEph && moonSnap.size() > 1) {
        for (size_t i = 1; i < moonSnap.size(); ++i) {
            float dAz0 = static_cast<float>(moonSnap[i-1].az_deg - sunEph.az_deg);
            float dAz1 = static_cast<float>(moonSnap[i  ].az_deg - sunEph.az_deg);
            if (std::fabs(dAz1 - dAz0) > 180.f) continue;
            ImVec2 a = projectAbs(moonSnap[i-1].az_deg, moonSnap[i-1].alt_deg);
            ImVec2 b = projectAbs(moonSnap[i  ].az_deg, moonSnap[i  ].alt_deg);
            dl->AddLine(a, b, IM_COL32(55, 130, 200, 100), 1.2f);
        }
        // Hour marks (no label — Moon labels would overlap Sun labels)
        for (size_t i = 0; i < moonSnap.size(); ++i) {
            if (moonSnap[i].utc_ms % 3600000 != 0) continue;
            ImVec2 pt = projectAbs(moonSnap[i].az_deg, moonSnap[i].alt_deg);
            dl->AddCircleFilled(pt, 2.5f, IM_COL32(55, 130, 200, 180));
        }
    }

    // ── P angle arc (from zenith to N☉ axis direction) ────────────────────────
    float arcR = sunR * 2.1f;
    if (fabsf(m_solarP) > 0.3f) {
        static constexpr int kArcSeg = 24;
        float a0 = -kPi * 0.5f;           // up = -y in screen coords
        float a1 = a0 + P_rad;
        for (int i = 0; i < kArcSeg; ++i) {
            float t0 = a0 + (a1-a0) * float(i)   / float(kArcSeg);
            float t1 = a0 + (a1-a0) * float(i+1) / float(kArcSeg);
            dl->AddLine({cx + cosf(t0)*arcR, cy + sinf(t0)*arcR},
                        {cx + cosf(t1)*arcR, cy + sinf(t1)*arcR},
                        IM_COL32(59,139,212,190), 1.2f);
        }
        float amid = a0 + (a1-a0) * 0.5f;
        char plbl[12];
        snprintf(plbl, sizeof(plbl), "P=%.0f\xc2\xb0", m_solarP);
        dl->AddText({cx + cosf(amid)*(arcR+8.f) - 14.f,
                     cy + sinf(amid)*(arcR+8.f) - 6.f},
                    IM_COL32(59,139,212,200), plbl);
    }

    // ── Camera frame helper (rotated rectangle around arbitrary centre) ──────────
    // fcx, fcy: screen-space centre of frame; rot_rad: 0 = horizontal
    auto DrawFrame = [&](float fcx, float fcy, float fovW_deg, float fovH_deg,
                          float rot_rad, ImU32 col, bool dashed) {
        float hw = fovW_deg * scale * 0.5f;
        float hh = fovH_deg * scale * 0.5f;
        float cr = cosf(rot_rad), sr = sinf(rot_rad);
        const float lx[4] = {-hw, hw, hw,-hw};
        const float ly[4] = {-hh,-hh, hh, hh};
        ImVec2 pts[4];
        for (int i = 0; i < 4; ++i)
            pts[i] = {fcx + lx[i]*cr - ly[i]*sr,
                      fcy + lx[i]*sr + ly[i]*cr};
        if (dashed) {
            for (int i = 0; i < 4; ++i) {
                ImVec2 a = pts[i], b = pts[(i+1)%4];
                float ex = b.x-a.x, ey = b.y-a.y;
                float seg = sqrtf(ex*ex + ey*ey);
                float dx = ex/seg, dy = ey/seg;
                for (float t = 0.f; t < seg; t += 9.f) {
                    float t2 = std::min(t+6.f, seg);
                    dl->AddLine({a.x+dx*t,  a.y+dy*t},
                                {a.x+dx*t2, a.y+dy*t2}, col, 1.3f);
                }
            }
        } else {
            dl->AddQuad(pts[0], pts[1], pts[2], pts[3], col, 1.5f);
        }
    };

    // Inverse projection: screen pos → (az, alt) degrees
    auto unprojectAbs = [&](float sx, float sy) -> std::pair<double,double> {
        assert(cosAltSun > 1e-4f);
        double az  = centerAz  + (sx - cx) / (cosAltSun * scale);
        double alt = centerAlt - (sy - cy) / scale;
        return {az, alt};
    };

    // Camera frame centre from track mode
    auto FrameCenter = [&](const CamConfig& cc) -> ImVec2 {
        assert(cc.focalMm > 0);
        switch (cc.trackMode) {
        case CamTrackMode::Moon:
            return {moonX, moonY};
        case CamTrackMode::Horizon:
            return projectAbs(cc.horizonAzDeg, cc.horizonAltDeg);
        case CamTrackMode::Sun:
        default:
            return {cx, cy};
        }
    };

    // ── Horizon-mode drag: detect mouse-down inside frame, update Alt/Az ────────
    {
        ImVec2 mpos = ImGui::GetMousePos();
        bool  mbDown    = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        bool  mbClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool  mbRel     = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        static constexpr float kSensorW = 35.9f, kSensorH = 24.0f;

        // Start drag: check if click lands inside a Horizon-mode frame
        if (mbClicked && m_dragHorizonCamIdx < 0) {
            assert(m_camConfigs.size() <= 64);
            for (int ci = 0; ci < (int)m_camConfigs.size(); ++ci) {
                CamConfig& cc = m_camConfigs[ci];
                if (cc.focalMm <= 0 || cc.trackMode != CamTrackMode::Horizon) continue;
                float f    = static_cast<float>(cc.focalMm);
                float fovW = 2.f * std::atan2f(kSensorW * 0.5f, f) * 180.f / kPi;
                float fovH = 2.f * std::atan2f(kSensorH * 0.5f, f) * 180.f / kPi;
                float hw   = fovW * scale * 0.5f;
                float hh   = fovH * scale * 0.5f;
                ImVec2 fc  = FrameCenter(cc);
                // Axis-aligned hit test (conservative — ignores rotation for simplicity)
                if (mpos.x >= fc.x - hw && mpos.x <= fc.x + hw
                &&  mpos.y >= fc.y - hh && mpos.y <= fc.y + hh) {
                    m_dragHorizonCamIdx = ci;
                    break;
                }
            }
        }

        // Continue drag
        if (mbDown && m_dragHorizonCamIdx >= 0
            && m_dragHorizonCamIdx < (int)m_camConfigs.size()) {
            CamConfig& cc = m_camConfigs[m_dragHorizonCamIdx];
            auto [az, alt] = unprojectAbs(mpos.x, mpos.y);
            cc.horizonAzDeg  = az;
            cc.horizonAltDeg = std::clamp(alt, -10.0, 90.0);
        }

        // End drag: save to DB
        if (mbRel && m_dragHorizonCamIdx >= 0) {
            if (m_dragHorizonCamIdx < (int)m_camConfigs.size()) {
                CamConfig& cc = m_camConfigs[m_dragHorizonCamIdx];
                m_configDb.SaveCamConfig(cc.guid, cc.model, cc.focalMm, cc.applyP,
                                         cc.trackMode, cc.horizonAltDeg, cc.horizonAzDeg);
            }
            m_dragHorizonCamIdx = -1;
        }
    }

    // Camera frames — driven by m_camConfigs (focal length + applyP + trackMode)
    // Sony full-frame sensor: 35.9 × 24.0 mm
    {
        static constexpr float kSensorW = 35.9f, kSensorH = 24.0f;
        static constexpr ImU32 kFrameColors[4] = {
            IM_COL32( 24,  95, 165, 220),   // niebieski
            IM_COL32( 55, 160,  70, 220),   // zielony
            IM_COL32(180,  60,  60, 220),   // czerwony
            IM_COL32(200, 140,  30, 220),   // bursztyn
        };
        assert(m_camConfigs.size() <= 64);
        for (int ci = 0; ci < (int)m_camConfigs.size(); ++ci) {
            const CamConfig& cc = m_camConfigs[ci];
            if (cc.focalMm <= 0) continue;
            float  f    = static_cast<float>(cc.focalMm);
            float  fovW = 2.f * std::atan2f(kSensorW * 0.5f, f) * 180.f / kPi;
            float  fovH = 2.f * std::atan2f(kSensorH * 0.5f, f) * 180.f / kPi;
            float  rot  = cc.applyP ? P_rad : 0.f;
            ImVec2 fc   = FrameCenter(cc);
            DrawFrame(fc.x, fc.y, fovW, fovH, rot, kFrameColors[ci % 4], ci == 0 /*dashed*/);
        }
    }

    // ── Visible planets (clip rect handles bounds) ────────────────────────────
    if (hasEph && sunEph.utc_ms > 0) {
        struct PlanetDef { EphBody body; ImU32 col; const char* name; };
        static constexpr PlanetDef kPlanets[] = {
            { EphBody::Mercury, IM_COL32(180,150,100,220), "Mer" },
            { EphBody::Venus,   IM_COL32(220,210,150,220), "Ven" },
            { EphBody::Mars,    IM_COL32(200, 90, 70,220), "Mar" },
            { EphBody::Jupiter, IM_COL32(175,150,110,200), "Jup" },
            { EphBody::Saturn,  IM_COL32(155,145,115,200), "Sat" },
        };
        for (const auto& pl : kPlanets) {
            EphRow pr = InterpEphAt(pl.body, simMs);
            if (pr.utc_ms == 0) continue;
            float dAz  = static_cast<float>(pr.az_deg  - sunEph.az_deg);
            float dAlt = static_cast<float>(pr.alt_deg - sunEph.alt_deg);
            float px   = cx + dAz  * cosAltSun * scale;
            float py   = cy - dAlt * scale;
            dl->AddCircleFilled({px, py}, 3.f, pl.col);
            dl->AddText({px+4.f, py-6.f}, pl.col, pl.name);
        }
    }

    // ── Corona glow — color gradient from pearl-white (limb) to amber (outer) ───
    // Circles drawn outside-in; color mirrors real corona photometry:
    //   outer streamers (> 3R): amber/orange  →  inner corona (< 1.2R): pearl white.
    // All circles are behind SUVI — when SUVI loads the inner ones are hidden;
    // outer circles (> 1.5×sunR) peek out past the SUVI disc edge as far-corona glow.
    dl->AddCircleFilled({cx,cy}, sunR*9.0f, IM_COL32(220,140, 50,  2));  // far outer, deep amber
    dl->AddCircleFilled({cx,cy}, sunR*6.5f, IM_COL32(235,160, 60,  3));
    dl->AddCircleFilled({cx,cy}, sunR*4.5f, IM_COL32(245,175, 75,  6));  // outer corona
    dl->AddCircleFilled({cx,cy}, sunR*3.0f, IM_COL32(250,190, 90, 11));
    dl->AddCircleFilled({cx,cy}, sunR*2.1f, IM_COL32(252,208,115, 18));  // mid corona, warm amber
    dl->AddCircleFilled({cx,cy}, sunR*1.5f, IM_COL32(254,225,155, 26));  // inner, pale yellow
    dl->AddCircleFilled({cx,cy}, sunR*1.2f, IM_COL32(255,245,210, 36));  // limb, near white
    dl->AddCircleFilled({cx,cy}, sunR*1.06f,IM_COL32(255,255,245, 50));  // immediate limb, pearl

    // ── Live View overlays (shortest focal = bottom, longest = top) ──────────────
    // Upload any newly decoded LV frames to D3D11 (render-thread only).
    if (m_lvNewFrames.load()) CreateLvTextures();
    {
        static constexpr float kSensorW = 35.9f, kSensorH = 24.0f;
        // Build render list sorted by focal length ascending (short = first = bottom)
        struct LvRenderEntry { int focalMm; int ci; };
        LvRenderEntry lvOrder[kMaxCamTracks];
        int lvCount = 0;
        for (int ci = 0; ci < kMaxCamTracks && ci < (int)m_camConfigs.size(); ++ci) {
            if (!m_lvEnabled[ci] || !m_lvSrv[ci]) continue;
            if (m_camConfigs[ci].focalMm <= 0) continue;
            lvOrder[lvCount++] = { m_camConfigs[ci].focalMm, ci };
        }
        // insertion sort (kMaxCamTracks ≤ 4, trivial)
        for (int i = 1; i < lvCount; ++i) {
            LvRenderEntry key = lvOrder[i];
            int j = i - 1;
            while (j >= 0 && lvOrder[j].focalMm > key.focalMm) {
                lvOrder[j + 1] = lvOrder[j]; --j;
            }
            lvOrder[j + 1] = key;
        }
        for (int li = 0; li < lvCount; ++li) {
            int ci = lvOrder[li].ci;
            const CamConfig& cc = m_camConfigs[ci];
            const ImU32 lvCol = IM_COL32(255, 255, 255,
                static_cast<ImU32>(m_lvOpacity[ci] * 255.f + 0.5f));
            float  f    = static_cast<float>(cc.focalMm);
            float  fovW = 2.f * std::atan2f(kSensorW * 0.5f, f) * 180.f / kPi;
            float  fovH = 2.f * std::atan2f(kSensorH * 0.5f, f) * 180.f / kPi;
            float  rot  = cc.applyP ? P_rad : 0.f;
            float  hw   = fovW * scale * 0.5f;
            float  hh   = fovH * scale * 0.5f;
            float  cr   = cosf(rot), sr = sinf(rot);
            ImVec2 fc   = FrameCenter(cc);
            ImVec2 tl{ fc.x + (-hw)*cr - (-hh)*sr, fc.y + (-hw)*sr + (-hh)*cr };
            ImVec2 tr{ fc.x + ( hw)*cr - (-hh)*sr, fc.y + ( hw)*sr + (-hh)*cr };
            ImVec2 br{ fc.x + ( hw)*cr - ( hh)*sr, fc.y + ( hw)*sr + ( hh)*cr };
            ImVec2 bl{ fc.x + (-hw)*cr - ( hh)*sr, fc.y + (-hw)*sr + ( hh)*cr };
            dl->AddImageQuad(
                static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(m_lvSrv[ci])),
                tl, tr, br, bl,
                {0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}, lvCol);
            DrawFrame(fc.x, fc.y, fovW, fovH, rot, IM_COL32(255, 255, 255, 180), false);
        }
    }

    // ── SUVI Fe171 animation — always on top of LV overlays ───────────────────
    // SUVI FD 1200×1200: solar disc radius ≈ 384 px → half-quad = sunR × m_suviHalfQ.
    // Amber disc shown when opacity < 5% or no frames available.
    {
        bool suviVisible = m_suviOpacity >= 0.05f;
        // Advance animation frame (render-thread only, no mutex needed)
        if (suviVisible) {
            m_suviAnimTimer += ImGui::GetIO().DeltaTime;
            float frameSec = 1.0f / m_suviAnimFps;
            if (m_suviAnimTimer >= frameSec) {
                m_suviAnimTimer -= frameSec;
                int n = (int)m_suviSrvs.size();
                if (n > 0) m_suviCurFrame = (m_suviCurFrame + 1) % n;
            }
        }
        int n = (int)m_suviSrvs.size();
        if (suviVisible && n > 0 && m_suviCurFrame < n && m_suviSrvs[m_suviCurFrame]) {
            static int s_prevSrvN = 0;
            if (m_suviJustCleared) { s_prevSrvN = 0; m_suviJustCleared = false; }
            if (s_prevSrvN == 0 && n > 0)
                LogLine(std::format(
                    "SUVI: first frame visible  halfQ={:.4f} foot={:.1f} corrR={:.1f} corrU={:.1f} rot={:.2f}deg",
                    m_suviHalfQ, m_suviFooterPx, m_suviCorrRightPx, m_suviCorrUpPx, m_solarP - m_solarQ));
            s_prevSrvN = n;
            float halfQ = sunR * m_suviHalfQ;
            float sc    = sunR / 384.f;
            float cosP  = cosf(P_rad), sinP = sinf(P_rad);
            float adjCx = cx + m_suviFooterPx * sc * (-sinP)
                             + m_suviCorrRightPx * sc * ( cosP)
                             + m_suviCorrUpPx    * sc * ( sinP);
            float adjCy = cy + m_suviFooterPx * sc * ( cosP)
                             + m_suviCorrRightPx * sc * ( sinP)
                             - m_suviCorrUpPx    * sc * ( cosP);
            float rx = halfQ * cosP,  ry = halfQ * sinP;
            float dxx = -halfQ * sinP, dyy = halfQ * cosP;
            ImVec2 tl{ adjCx - rx - dxx, adjCy - ry - dyy };
            ImVec2 tr{ adjCx + rx - dxx, adjCy + ry - dyy };
            ImVec2 br{ adjCx + rx + dxx, adjCy + ry + dyy };
            ImVec2 bl{ adjCx - rx + dxx, adjCy - ry + dyy };
            ImU32 suviCol = IM_COL32(255, 255, 255,
                static_cast<ImU32>(m_suviOpacity * 255.f + 0.5f));
            dl->AddImageQuad(
                static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(m_suviSrvs[m_suviCurFrame])),
                tl, tr, br, bl,
                {0.f,0.f},{1.f,0.f},{1.f,1.f},{0.f,1.f}, suviCol);
            dl->AddCircle({cx, cy}, sunR, IM_COL32(255, 0, 0, 200), 128, 2.f);
        } else {
            // Amber disc when SUVI off or loading
            dl->AddCircleFilled({cx, cy}, sunR, IM_COL32(250, 199, 117, 255));
            dl->AddCircle      ({cx, cy}, sunR, IM_COL32(186, 117,  23, 255), 0, 1.f);
        }
    }

    // ── Moon disc — on top of SUVI and LV (Księżyc zakrywa Słońce) ───────────
    // Textured NASA photo when available/enabled; flat disc as fallback.
    // During totality always solid black, regardless of the texture.
    {
        if (m_moonNewData.load()) CreateMoonTexture();

        float dxM = moonX - cx, dyM = moonY - cy;
        float dist = sqrtf(dxM * dxM + dyM * dyM);
        bool  isTotality     = (dist + sunR < moonR);
        bool  moonTexVisible = !isTotality && m_moonOpacity >= 0.05f
                            && m_moonSrv && m_moonImgW > 0 && m_moonImgH > 0;

        if (isTotality) {
            dl->AddCircleFilled({moonX, moonY}, moonR, IM_COL32(0, 0, 0, 255));
            dl->AddCircle      ({moonX, moonY}, moonR, IM_COL32(110, 108, 104, 255), 0, 0.8f);
        } else if (moonTexVisible) {
            // Scale so the auto-detected disc (not the letterboxed frame)
            // matches moonR; rotate with the Moon's N-pole PA (moonVrad).
            float moonScale = moonR / m_moonDiscR;
            float halfW = (m_moonImgW * 0.5f) * moonScale;
            float halfH = (m_moonImgH * 0.5f) * moonScale;
            float offXpx = m_moonDiscCx - m_moonImgW * 0.5f;
            float offYpx = m_moonDiscCy - m_moonImgH * 0.5f;
            float cosV = cosf(moonVrad), sinV = sinf(moonVrad);
            float offX = (offXpx * cosV - offYpx * sinV) * moonScale;
            float offY = (offXpx * sinV + offYpx * cosV) * moonScale;
            float ccx  = moonX - offX, ccy = moonY - offY;

            float rx  = halfW * cosV,  ry  = halfW * sinV;
            float dxx = -halfH * sinV, dyy = halfH * cosV;
            ImVec2 tl{ ccx - rx - dxx, ccy - ry - dyy };
            ImVec2 tr{ ccx + rx - dxx, ccy + ry - dyy };
            ImVec2 br{ ccx + rx + dxx, ccy + ry + dyy };
            ImVec2 bl{ ccx - rx + dxx, ccy - ry + dyy };
            ImU32 moonCol = IM_COL32(255, 255, 255,
                static_cast<ImU32>(m_moonOpacity * 255.f + 0.5f));
            dl->AddImageQuad(
                static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(m_moonSrv)),
                tl, tr, br, bl,
                {0.f,0.f},{1.f,0.f},{1.f,1.f},{0.f,1.f}, moonCol);
        } else {
            dl->AddCircleFilled({moonX, moonY}, moonR, IM_COL32(55, 55, 52, 255));
            dl->AddCircle      ({moonX, moonY}, moonR, IM_COL32(110, 108, 104, 255), 0, 0.8f);
        }
    }

    // ── Solar rotation axis N☉ ────────────────────────────────────────────────
    // Direction from centre: N pole = (sin P, -cos P) in screen (y-down)
    float axNx = sinf(P_rad);
    float axNy = -cosf(P_rad);
    float axN  = sunR * 3.6f;
    float axS  = sunR * 1.8f;
    dl->AddLine({cx, cy}, {cx+axNx*axN, cy+axNy*axN},
                IM_COL32(239,159,39,230), 2.f);
    dl->AddLine({cx, cy}, {cx-axNx*axS, cy-axNy*axS},
                IM_COL32(239,159,39, 90), 1.2f);
    // Arrowhead at N end
    {
        float tip  = axN;
        float ptx  = cx + axNx*tip,        pty  = cy + axNy*tip;
        float perx = -axNy,                pery =  axNx;
        float base = tip - 6.f;
        dl->AddTriangleFilled(
            {cx+axNx*base - perx*3.f, cy+axNy*base - pery*3.f},
            {cx+axNx*base + perx*3.f, cy+axNy*base + pery*3.f},
            {ptx, pty}, IM_COL32(239,159,39,230));
    }
    // N label + subscript drawn sun symbol (☉ not in Consolas)
    {
        float lx = cx + axNx*(axN+4.f) - 7.f;
        float ly = cy + axNy*(axN+4.f) - 6.f;
        dl->AddText({lx, ly}, IM_COL32(186,117,23,220), "N");
        // subscript ⊙: circle + center dot
        float sx = lx + 9.f, sy = ly + 7.f;
        dl->AddCircle     ({sx, sy}, 3.f, IM_COL32(186,117,23,190), 0, 1.f);
        dl->AddCircleFilled({sx, sy}, 1.1f, IM_COL32(186,117,23,210));
    }

    // ── Moon rotation axis + north pole marker ────────────────────────────────
    // V-q: position angle of Moon's north pole from zenith (screen up = zenith)
    if (hasMoonV && moonR > 4.f) {
        float mnx = sinf(moonVrad), mny = -cosf(moonVrad);
        float mR  = moonR;
        // Full axis line through Moon disc (dim south side)
        dl->AddLine({moonX - mnx*mR, moonY - mny*mR},
                    {moonX,           moonY},
                    IM_COL32(60, 140, 200, 110), 1.2f);
        dl->AddLine({moonX, moonY},
                    {moonX + mnx*mR, moonY + mny*mR},
                    IM_COL32(80, 170, 230, 200), 1.5f);
        // Arrowhead at N end
        float npx = moonX + mnx*mR, npy = moonY + mny*mR;
        float prx = -mny * 2.5f, pry = mnx * 2.5f;
        float base = mR - 4.f;
        dl->AddTriangleFilled(
            {moonX + mnx*base - prx, moonY + mny*base - pry},
            {moonX + mnx*base + prx, moonY + mny*base + pry},
            {npx, npy}, IM_COL32(80, 170, 230, 200));
        // "N" label + small subscript moon-disc circle
        float lx = npx + mnx*4.f - 4.f, ly = npy + mny*4.f - 6.f;
        dl->AddText({lx, ly}, IM_COL32(80, 170, 230, 200), "N");
        dl->AddCircle({lx + 9.f, ly + 7.f}, 3.f, IM_COL32(80, 170, 230, 160), 0, 1.f);
    }

    // ── C2 / C3 contact points on Moon limb — computed from relative motion ────
    // C2: Moon approaching from this side; C3: Moon departing
    if (hasContactDir) {
        float c2x = moonX + kC2ux_dyn * moonR, c2y = moonY + kC2uy_dyn * moonR;
        float c3x = moonX + kC3ux_dyn * moonR, c3y = moonY + kC3uy_dyn * moonR;
        dl->AddCircle({c2x, c2y}, 2.5f, IM_COL32(15, 110, 86, 230), 0, 1.5f);
        dl->AddText({c2x - 14.f, c2y - 12.f}, IM_COL32(15, 110, 86, 220), "C2");
        dl->AddCircle({c3x, c3y}, 2.5f, IM_COL32(153, 60, 29, 230), 0, 1.5f);
        dl->AddText({c3x + 4.f,  c3y + 4.f},  IM_COL32(153, 60, 29, 220), "C3");
    }

    // ── Frame labels (stacked top-left, one per camera) ──────────────────────
    {
        static constexpr float kSensorW = 35.9f, kSensorH = 24.0f;
        static constexpr ImU32 kFrameColors[4] = {
            IM_COL32( 24,  95, 165, 200),
            IM_COL32( 55, 160,  70, 200),
            IM_COL32(180,  60,  60, 200),
            IM_COL32(200, 140,  30, 200),
        };
        float lineY = oy + 4.f;
        float rot   = m_solarP - m_solarQ;
        assert(m_camConfigs.size() <= 64);
        for (int ci = 0; ci < (int)m_camConfigs.size(); ++ci) {
            const CamConfig& cc = m_camConfigs[ci];
            if (cc.focalMm <= 0) continue;
            float f    = static_cast<float>(cc.focalMm);
            float fovW = 2.f * std::atan2f(kSensorW * 0.5f, f) * 180.f / kPi;
            float fovH = 2.f * std::atan2f(kSensorH * 0.5f, f) * 180.f / kPi;
            const char* trackTag = (cc.trackMode == CamTrackMode::Moon)    ? " Moon"
                                 : (cc.trackMode == CamTrackMode::Horizon) ? " Horiz" : "";
            char lbl[80];
            if (cc.applyP)
                snprintf(lbl, sizeof(lbl),
                    "%s  %dmm  %.1f\xc2\xb0\xc3\x97%.1f\xc2\xb0  (P=%.0f\xc2\xb0)%s",
                    cc.model.c_str(), cc.focalMm, fovW, fovH, rot, trackTag);
            else
                snprintf(lbl, sizeof(lbl),
                    "%s  %dmm  %.1f\xc2\xb0\xc3\x97%.1f\xc2\xb0  (0\xc2\xb0)%s",
                    cc.model.c_str(), cc.focalMm, fovW, fovH, trackTag);
            dl->AddText({ox + 4.f, lineY}, kFrameColors[ci % 4], lbl);
            lineY += ImGui::GetTextLineHeight() + 2.f;
        }
    }

    // ── Zoom hint (bottom-right inside clip rect) ─────────────────────────────
    {
        char zhint[24];
        snprintf(zhint, sizeof(zhint), "zoom %.1fx", m_solarZoom);
        ImVec2 ts = ImGui::CalcTextSize(zhint);
        dl->AddText({ox+szW-ts.x-6.f, oy+szH-ts.y-4.f},
                    IM_COL32(60,65,80,160), zhint);
    }

    dl->PopClipRect();

    // ── P/q/rot/Alt/Obs + UTC/Home/Loc/PlayHead ───────────────────────────────
    // Unified style: gray label, white value, fixed 25px gap between fields.
    ImGui::PushFont(m_fontMono);
    {
        static const ImVec4 kValWhite{0.88f, 0.88f, 0.90f, 1.0f};

        auto Field = [&](const char* label, const char* fmt, auto&&... args) {
            ImGui::TextColored(kGray, "%s", label);
            ImGui::SameLine(0, 4);
            ImGui::TextColored(kValWhite, fmt, args...);
            ImGui::SameLine(0, 25);
        };

        if (hasEph && sunEph.utc_ms > 0) {
            float rot = m_solarP - m_solarQ;
            Field("P",   "%.1f\xc2\xb0", m_solarP);
            Field("q",   "%.1f\xc2\xb0", m_solarQ);
            Field("rot", "%.1f\xc2\xb0", rot);
            Field("Alt", "%.1f\xc2\xb0", static_cast<float>(sunEph.alt_deg));
            Field("Obs", "%.2f%%",       obscuration * 100.f);
        } else if (m_ephFetching.load()) {
            Field("EPH", "%s", "fetching...");
        } else {
            Field("P", "%.1f\xc2\xb0 (statyczny)", m_solarP);
        }

        int64_t nowMs = UtcNowMs();
        {
            char buf[20]; FormatUtcHms(nowMs, buf, sizeof(buf));
            Field("UTC", "%s", buf);
        }

        // One inline TZ clock: zone-name button (opens picker) + time.
        // idSuffix is a stable ImGui id ("##gh_sim") independent of the
        // visible zone code, which changes whenever the user picks a zone.
        auto TzInline = [&](std::string& tzIana, const char* idSuffix, const char* popupId) {
            char timeBuf[12]; FormatLocalHms(nowMs, tzIana, timeBuf, sizeof(timeBuf));
            int  ci = TzFindByIana(m_tzList, tzIana);
            assert(ci >= 0 && ci < static_cast<int>(m_tzList.size()));
            std::string btnLabel = m_tzList[ci].code + idSuffix;
            if (ImGui::SmallButton(btnLabel.c_str())) ImGui::OpenPopup(popupId);
            ImGui::SameLine(0, 4);
            ImGui::TextColored(kValWhite, "%s", timeBuf);
            ImGui::SameLine(0, 25);
            ImGui::SetNextWindowSizeConstraints(ImVec2(420, 0), ImVec2(420, 520));
            if (ImGui::BeginPopup(popupId)) {
                int ci2 = TzFindByIana(m_tzList, tzIana);
                assert(m_tzList.size() <= 1024); // bounded: DB has 598 IANA zones
                for (int i = 0; i < static_cast<int>(m_tzList.size()); ++i) {
                    bool sel = (i == ci2);
                    char entry[80];
                    snprintf(entry, sizeof(entry), "%-22s  %s",
                             m_tzList[i].code.c_str(), m_tzList[i].iana.c_str());
                    if (ImGui::Selectable(entry, sel)) {
                        tzIana = m_tzList[i].iana;
                        SaveClockSettings();
                        ImGui::CloseCurrentPopup();
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndPopup();
            }
        };

        TzInline(m_homeTzIana, "##gh_sim", "##tz_home_sim");
        TzInline(m_eclTzIana,  "##gl_sim", "##tz_loc_sim");

        // PlayHead — always shown regardless of ephemeris availability.
        int64_t ph = m_tlPlayheadMs.load();
        if (ph > 0) {
            int64_t sv = ph / 1000;
            Field("PlayHead", "%02d:%02d:%02d.%03d",
                (int)((sv/3600)%24), (int)((sv/60)%60), (int)(sv%60), (int)(ph%1000));
        } else {
            Field("PlayHead", "%s", "--:--:--");
        }
    }

    ImGui::PopFont();
}

// ─── Inspector + Palette column ───────────────────────────────────────────────

void App::RenderInspectorColumn() {
    static const ImVec4 kGray {0.40f, 0.40f, 0.45f, 1.0f};
    static const ImVec4 kDim  {0.28f, 0.28f, 0.32f, 1.0f};

    // ── SIMULATOR CONFIG ──────────────────────────────────────────────────
    ImGui::SeparatorText("SIMULATOR CONFIG");
    ImGui::Spacing();

    // ── Opacity sliders: SUVI + per-camera LV ─────────────────────────────
    // Helper: push 4 style colors (FrameBg, FrameBgHovered, SliderGrab, SliderGrabActive)
    // Green = on (opacity >= 5%), Red = off (opacity < 5%).
    auto PushSliderColor = [](bool on) {
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_FrameBg,          ImVec4(.08f,.24f,.08f,1.f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   ImVec4(.12f,.34f,.12f,1.f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ImVec4(.20f,.60f,.20f,1.f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(.28f,.78f,.28f,1.f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_FrameBg,          ImVec4(.20f,.08f,.08f,1.f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   ImVec4(.30f,.12f,.12f,1.f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ImVec4(.65f,.18f,.18f,1.f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(.80f,.22f,.22f,1.f));
        }
    };

    // Moon opacity slider — NASA archive photo, textured over the flat disc.
    ImGui::PushFont(m_fontMono);
    {
        int moonPct = static_cast<int>(m_moonOpacity * 100.f + 0.5f);
        PushSliderColor(moonPct >= 5);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##moon_op", &moonPct, 0, 100, "Moon  %d%%")) {
            m_moonOpacity = moonPct / 100.f;
            m_configDb.SetSettingInt("moon_opacity_pct", moonPct);
        }
        ImGui::PopStyleColor(4);
    }
    ImGui::PopFont();

    // SUVI opacity slider
    ImGui::PushFont(m_fontMono);
    {
        int suviPct = static_cast<int>(m_suviOpacity * 100.f + 0.5f);
        PushSliderColor(suviPct >= 5);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##suvi_op", &suviPct, 0, 100, "GOES-19 SUVI  %d%%")) {
            m_suviOpacity = suviPct / 100.f;
            m_configDb.SetSettingInt("suvi_opacity_pct", suviPct);
        }
        ImGui::PopStyleColor(4);
    }
    ImGui::PopFont();

    // SUVI channel selector
    static constexpr const char* kSuviChannels[] = {
        "Fe094", "Fe131", "Fe171", "Fe195", "Fe284", "He304", "R2G4B3"
    };
    static constexpr int kSuviChannelCount = 7;
    int curCh = 2; // default Fe171
    for (int i = 0; i < kSuviChannelCount; ++i)
        if (m_suviChannel == kSuviChannels[i]) { curCh = i; break; }
    ImGui::PushFont(m_fontMono);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##suvi_ch", &curCh, kSuviChannels, kSuviChannelCount)) {
        if (!m_suviFetching.load()) {
            m_suviChannel = kSuviChannels[curCh];
            m_configDb.SetSetting("suvi_channel", m_suviChannel.c_str());
            TriggerSuviFetch();
        }
    }
    ImGui::PopFont();
    ImGui::Spacing();

    // SUVI alignment: scale + offsets — value+buttons white, label gray
    bool suviChanged = false;
    ImGui::PushFont(m_fontMono);
    ImGui::SetNextItemWidth(135); if (ImGui::InputFloat("##suvi_disc", &m_suviHalfQ,       0.005f, 0.02f, "%8.4f")) suviChanged = true;
    ImGui::SameLine(); ImGui::TextColored(kGray, "Scale");
    ImGui::SetNextItemWidth(135); if (ImGui::InputFloat("##suvi_foot", &m_suviFooterPx,    1.0f, 5.0f, "%8.4f")) suviChanged = true;
    ImGui::SameLine(); ImGui::TextColored(kGray, "V-offset");
    ImGui::SetNextItemWidth(135); if (ImGui::InputFloat("##suvi_horz", &m_suviCorrRightPx, 1.0f, 5.0f, "%8.4f")) suviChanged = true;
    ImGui::SameLine(); ImGui::TextColored(kGray, "H-offset");
    // "Vertical offset" (m_suviCorrUpPx) control intentionally removed from the
    // UI — variable and its use in SUVI positioning/persistence stay untouched.
    ImGui::PopFont();
    if (suviChanged) {
        m_configDb.SetSetting("suvi_half_q",        std::to_string(m_suviHalfQ).c_str());
        m_configDb.SetSetting("suvi_footer_px",     std::to_string(m_suviFooterPx).c_str());
        m_configDb.SetSetting("suvi_corr_right_px", std::to_string(m_suviCorrRightPx).c_str());
        m_configDb.SetSetting("suvi_corr_up_px",    std::to_string(m_suviCorrUpPx).c_str());
        LogLine(std::format("SUVI align: halfQ={:.4f} foot={:.1f} corrR={:.1f} corrU={:.1f}",
                            m_suviHalfQ, m_suviFooterPx, m_suviCorrRightPx, m_suviCorrUpPx));
    }
    ImGui::Spacing();

    // Per-camera LV opacity sliders — sorted by focal length ascending
    ImGui::PushFont(m_fontMono);
    {
        struct LvSlEnt { int focalMm; int ci; };
        LvSlEnt ents[kMaxCamTracks]; int nEnts = 0;
        for (int ci = 0; ci < (int)m_camConfigs.size() && ci < kMaxCamTracks; ++ci)
            ents[nEnts++] = { m_camConfigs[ci].focalMm, ci };
        for (int i = 1; i < nEnts; ++i) {
            LvSlEnt key = ents[i]; int j = i - 1;
            while (j >= 0 && ents[j].focalMm > key.focalMm) { ents[j+1] = ents[j]; --j; }
            ents[j+1] = key;
        }
        for (int ei = 0; ei < nEnts; ++ei) {
            int ci = ents[ei].ci;
            const CamConfig& cc = m_camConfigs[ci];
            std::string guid4 = cc.guid.size() >= 4
                ? cc.guid.substr(cc.guid.size() - 4) : cc.guid;
            // Format string shown inside slider bar: "model  #guid4  XX%"
            std::string fmt = cc.model + "  #" + guid4 + "  %d%%";
            int opPct = static_cast<int>(m_lvOpacity[ci] * 100.f + 0.5f);
            bool nowOn = opPct >= 5;
            PushSliderColor(nowOn);
            ImGui::SetNextItemWidth(-1);
            std::string slId = std::format("##lvop_{}", ci);
            if (ImGui::SliderInt(slId.c_str(), &opPct, 0, 100, fmt.c_str())) {
                m_lvOpacity[ci] = opPct / 100.f;
                m_configDb.SetSettingInt(
                    std::format("lv_opacity_pct_{}", ci).c_str(), opPct);
                bool wantOn = opPct >= 5;
                if (wantOn != m_lvEnabled[ci]) {
                    m_lvEnabled[ci] = wantOn;
                    if (wantOn) {
                        auto res = m_pipe.SendRequest(
                            std::format(R"({{"cmd":"lv_start","cam":"{}"}})", ci));
                        (void)res;
                        if (!m_lvThread.joinable()) StartLvThread();
                    } else {
                        auto res = m_pipe.SendRequest(
                            std::format(R"({{"cmd":"lv_stop","cam":"{}"}})", ci));
                        (void)res;
                        if (m_lvSrv[ci]) { m_lvSrv[ci]->Release(); m_lvSrv[ci] = nullptr; }
                    }
                }
            }
            ImGui::PopStyleColor(4);
            ImGui::Spacing();
        }
        if (nEnts == 0)
            ImGui::TextColored(kDim, "(no cameras in Camera Config)");
    }
    ImGui::PopFont();
    ImGui::Spacing();

    // ── Action block palette ───────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("ACTION LIBRARY");
    ImGui::Spacing();

    struct PE { BlockType type; const char* name; ImU32 col; };
    PE pal[] = {
        {BlockType::Single,  "Single",  IM_COL32( 40,160, 70,255)},
        {BlockType::Burst,   "Burst",   IM_COL32( 50,120,200,255)},
        {BlockType::Bracket, "Bracket", IM_COL32(200,130, 30,255)},
        {BlockType::Audio,   "Audio",   IM_COL32(140, 80,200,255)},
    };

    // 2×2 grid — half-width cells, two rows
    const float palAvail = ImGui::GetContentRegionAvail().x;
    const float palGap   = ImGui::GetStyle().ItemSpacing.x;
    const float palHW    = (palAvail - palGap) / 2.f - 10.f;
    for (int pi = 0; pi < 4; ++pi) {
        auto& pe = pal[pi];
        if (pi == 1 || pi == 3) ImGui::SameLine(0, palGap);
        ImGui::PushID((int)pe.type);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##pb", {palHW, 34.0f});
        bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, {pos.x+palHW, pos.y+34.f},
                          hov ? IM_COL32(45,45,58,255) : IM_COL32(20,20,28,255), 4.f);
        dl->AddRectFilled({pos.x+5, pos.y+8}, {pos.x+17, pos.y+26}, pe.col, 2.f);
        dl->AddText({pos.x+22, pos.y+9}, IM_COL32(200,200,215,255), pe.name);
        if (hov) dl->AddRect(pos, {pos.x+palHW, pos.y+34.f}, pe.col, 4.f, 0, 1.f);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            const char* pid = (pe.type == BlockType::Audio) ? "TL_AUD_BLOCK" : "TL_CAM_BLOCK";
            ImGui::SetDragDropPayload(pid, &pe.type, sizeof(BlockType));
            ImGui::PushFont(m_fontMono);
            ImVec4 cv{float(pe.col&0xff)/255.f, float((pe.col>>8)&0xff)/255.f,
                      float((pe.col>>16)&0xff)/255.f, 1.f};
            ImGui::TextColored(cv, "%s", pe.name);
            ImGui::PopFont();
            ImGui::EndDragDropSource();
        }
        if (pi == 1 || pi == 3) ImGui::Spacing();
        ImGui::PopID();
    }

    // ── BLOCK INSPECTOR ────────────────────────────────────────────────────
    ImGui::SeparatorText("BLOCK INSPECTOR");

    bool hasTrack = m_selTrack >= 0 && m_selTrack < (int)m_tracks.size();
    bool hasSel   = hasTrack
               && m_selBlock >= 0 && m_selBlock < (int)m_tracks[m_selTrack].blocks.size();

    if (!hasSel) {
        ImGui::PushFont(m_fontMono);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 6));
        ImGui::TextColored(kDim, "Select block on");
        ImGui::TextColored(kDim, "TimeLine to adjust");
        ImGui::TextColored(kDim, "photo parameters");
        ImGui::PopStyleVar();
        ImGui::PopFont();
    } else {
        TLBlock& b = m_tracks[m_selTrack].blocks[m_selBlock];
        ImGui::PushFont(m_fontMono);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));

        // Field width: available minus widest label ("Burst duration") minus two
        // spacings, plus a fixed 10px so the fields read wider/less cramped.
        const float kFldW = ImGui::GetContentRegionAvail().x
                          - ImGui::CalcTextSize("Burst duration").x
                          - ImGui::GetStyle().ItemSpacing.x * 2.f
                          + 10.f;

        // Play button — audio blocks only, full width, before Type
        if (b.type == BlockType::Audio && !b.audioFile.empty()) {
            if (ImGui::Button("Play##aud_top", ImVec2(-1, 0))) {
                std::wstring wrel = Utf8ToW(b.audioFile);
                for (auto& c : wrel) if (c == L'/') c = L'\\';
                MciPlayAsync(ExeDir() + L"\\" + wrel);
            }
        }

        // Type — read-only InputText, same style as other fields
        {
            char typeBuf[16];
            snprintf(typeBuf, sizeof(typeBuf), "%s", BlockTypeName(b.type));
            ImGui::SetNextItemWidth(kFldW);
            ImGui::InputText("##type_ro", typeBuf, sizeof(typeBuf), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine(); ImGui::TextColored(kGray, "Type");
        }

        // UTC start time — read-only, same style as other fields
        {
            char atBuf[16] = "--:--:--.---";
            if (b.atMs > 0) {
                int64_t s = b.atMs / 1000;
                snprintf(atBuf, sizeof(atBuf), "%02d:%02d:%02d.%03d",
                         (int)((s/3600)%24),(int)((s/60)%60),(int)(s%60),(int)(b.atMs%1000));
            }
            ImGui::SetNextItemWidth(kFldW);
            ImGui::InputText("##at_ro", atBuf, sizeof(atBuf), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine(); ImGui::TextColored(kGray, "Start UTC");
        }

        if (b.type != BlockType::Audio) {
            // Shutter speed combo — Sony standard values
            static constexpr const char* kSS[] = {
                "30s","25s","20s","15s","13s","10s","8s","6s","5s","4s",
                "3.2s","2.5s","2s","1.6s","1.3s","1s","0.8s","0.6s","0.5s","0.4s",
                "1/3","1/4","1/5","1/6","1/8","1/10","1/13","1/15","1/20","1/25",
                "1/30","1/40","1/50","1/60","1/80","1/100","1/125","1/160","1/200",
                "1/250","1/320","1/400","1/500","1/640","1/800","1/1000","1/1250",
                "1/1600","1/2000","1/2500","1/3200","1/4000","1/5000","1/6400","1/8000"
            };
            static constexpr int kSSN = 55;
            const char* ssPrev = b.ss.empty() ? "1/100" : b.ss.c_str();
            ImGui::SetNextItemWidth(kFldW);
            if (ImGui::BeginCombo("##ss_c", ssPrev)) {
                for (int i = 0; i < kSSN; ++i) {
                    bool sel = (b.ss == kSS[i]);
                    if (ImGui::Selectable(kSS[i], sel)) { b.ss = kSS[i]; m_tlDirty = true; }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine(); ImGui::TextColored(kGray, "SS");

            // ISO combo — Sony standard values
            static constexpr int kISO[] = {
                50,64,80,100,125,160,200,250,320,400,500,640,800,
                1000,1250,1600,2000,2500,3200,4000,5000,6400,8000,
                10000,12800,16000,20000,25600,32000,40000,51200,102400
            };
            static constexpr int kISOn = 32;
            char isoPrev[16]; snprintf(isoPrev, sizeof(isoPrev), "%d", b.iso > 0 ? b.iso : 100);
            ImGui::SetNextItemWidth(kFldW);
            if (ImGui::BeginCombo("##iso_c", isoPrev)) {
                for (int i = 0; i < kISOn; ++i) {
                    char lb[16]; snprintf(lb, sizeof(lb), "%d", kISO[i]);
                    bool sel = (b.iso == kISO[i]);
                    if (ImGui::Selectable(lb, sel)) { b.iso = kISO[i]; m_tlDirty = true; }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine(); ImGui::TextColored(kGray, "ISO");

            // F-stop combo — Sony standard values
            static constexpr const char* kFS[] = {
                "1.4","1.6","1.8","2.0","2.2","2.5","2.8","3.2","3.5",
                "4.0","4.5","5.0","5.6","6.3","7.1","8.0","9.0","10",
                "11","13","14","16","18","20","22"
            };
            static constexpr int kFSN = 25;
            const char* fsPrev = b.fstop.empty() ? "8.0" : b.fstop.c_str();
            ImGui::SetNextItemWidth(kFldW);
            if (ImGui::BeginCombo("##fs_c", fsPrev)) {
                for (int i = 0; i < kFSN; ++i) {
                    bool sel = (b.fstop == kFS[i]);
                    if (ImGui::Selectable(kFS[i], sel)) { b.fstop = kFS[i]; m_tlDirty = true; }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine(); ImGui::TextColored(kGray, "f-number");
        }

        if (b.type == BlockType::Bracket) {
            // 16 valid modes matching CrSDK + camera menu.
            // 2.0ev / 3.0ev support x3 and x5 only (SDK limitation — no 9-pic).
            struct BrkMode { const char* label; const char* ev; int count; };
            static const BrkMode kBrk[] = {
                {"0.3ev \xc3\x97 3", "0.3ev",  3},
                {"0.3ev \xc3\x97 5", "0.3ev",  5},
                {"0.3ev \xc3\x97 9", "0.3ev",  9},
                {"0.5ev \xc3\x97 3", "0.5ev",  3},
                {"0.5ev \xc3\x97 5", "0.5ev",  5},
                {"0.5ev \xc3\x97 9", "0.5ev",  9},
                {"0.7ev \xc3\x97 3", "0.7ev",  3},
                {"0.7ev \xc3\x97 5", "0.7ev",  5},
                {"0.7ev \xc3\x97 9", "0.7ev",  9},
                {"1.0ev \xc3\x97 3", "1.0ev",  3},
                {"1.0ev \xc3\x97 5", "1.0ev",  5},
                {"1.0ev \xc3\x97 9", "1.0ev",  9},
                {"2.0ev \xc3\x97 3", "2.0ev",  3},
                {"2.0ev \xc3\x97 5", "2.0ev",  5},
                {"3.0ev \xc3\x97 3", "3.0ev",  3},
                {"3.0ev \xc3\x97 5", "3.0ev",  5},
            };
            static constexpr int kBrkN = 16;

            int idx = 10; // default: 1.0ev x5
            for (int i = 0; i < kBrkN; ++i)
                if (b.ev == kBrk[i].ev && b.count == kBrk[i].count) { idx = i; break; }

            ImGui::SetNextItemWidth(kFldW);
            if (ImGui::BeginCombo("##brk", kBrk[idx].label)) {
                for (int i = 0; i < kBrkN; ++i) {
                    if (i > 0 && strcmp(kBrk[i].ev, kBrk[i-1].ev) != 0)
                        ImGui::Separator();
                    if (ImGui::Selectable(kBrk[i].label, i == idx)) {
                        b.ev    = kBrk[i].ev;
                        b.count = kBrk[i].count;
                        m_tlDirty = true;
                    }
                    if (i == idx) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine(); ImGui::TextColored(kGray, "Bracket");
        }

        if (b.type == BlockType::Burst) {
            // Drive mode combo — maps to CrSDK drive mode strings
            struct BurstMode { const char* label; const char* drive; float fps; };
            static const BurstMode kModes[] = {
                {"HI+", "cont-hi-plus", 10.0f},
                {"HI",  "cont-hi",       8.0f},
                {"MID", "cont-mid",      5.0f},
                {"LO",  "cont-lo",       3.0f},
            };
            int modeIdx = 0;
            for (int i = 0; i < 4; ++i)
                if (b.burstDrive == kModes[i].drive) { modeIdx = i; break; }

            ImGui::SetNextItemWidth(kFldW);
            if (ImGui::BeginCombo("##bdrive", kModes[modeIdx].label)) {
                for (int i = 0; i < 4; ++i) {
                    char lbl[24];
                    snprintf(lbl, sizeof(lbl), "%-4s  (~%.0f fps)",
                             kModes[i].label, kModes[i].fps);
                    if (ImGui::Selectable(lbl, i == modeIdx)) {
                        b.burstDrive = kModes[i].drive;
                        m_tlDirty = true;
                    }
                    if (i == modeIdx) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine(); ImGui::TextColored(kGray, "Drive speed");

            ImGui::SetNextItemWidth(kFldW);
            float ds = b.burstDurMs / 1000.f;
            if (ImGui::InputFloat("##bdur", &ds, 0.f, 0.f, "%.1f")) {
                // Clamp to [0.1s, 3600s] — 0 or negative violates the
                // "burstDurMs > 0" invariant used elsewhere (BlockDurMs,
                // shoot timeout) and previously crashed the Debug build.
                ds = std::isfinite(ds) ? std::clamp(ds, 0.1f, 3600.f) : 0.1f;
                b.burstDurMs = int32_t(ds * 1000.f);
                m_tlDirty = true;
            }
            ImGui::SameLine(); ImGui::TextColored(kGray, "Burst duration");

            // Informational: estimated frame count
            float fps  = kModes[modeIdx].fps;
            float dsSafe = std::isfinite(ds) ? std::max(0.f, ds) : 0.f;
            int   nFrm = std::max(1, (int)(fps * dsSafe + 0.5f));
            ImGui::TextColored(kDim, "~%d frames @ %.0f fps", nFrm, fps);
        }

        if (b.type == BlockType::Audio) {
            // Combo: MP3 files from eclipse_audio_*/ dirs — scanned once per combo open.
            // audioFile stores relative path: "eclipse_audio_PL\01_pre_c1_10min.mp3"
            const char* preview = b.audioFile.empty() ? "(none)" : b.audioFile.c_str();
            ImGui::SetNextItemWidth(kFldW);
            static std::vector<std::string> s_audioFiles;
            static bool                     s_audScanned = false;
            if (ImGui::BeginCombo("##afc", preview)) {
                if (!s_audScanned) {
                    s_audioFiles.clear();
                    std::wstring exeDir = ExeDir();
                    WIN32_FIND_DATAW dirFd{};
                    HANDLE dh = FindFirstFileW((exeDir + L"\\eclipse_audio_*").c_str(), &dirFd);
                    if (dh != INVALID_HANDLE_VALUE) {
                        do {
                            if (!(dirFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                            std::wstring dn   = dirFd.cFileName;
                            std::string  dns  = WToUtf8(dn);
                            WIN32_FIND_DATAW fd{};
                            HANDLE h = FindFirstFileW((exeDir + L"\\" + dn + L"\\*.mp3").c_str(), &fd);
                            if (h != INVALID_HANDLE_VALUE) {
                                do { s_audioFiles.push_back(dns + "\\" + WToUtf8(fd.cFileName)); }
                                while (FindNextFileW(h, &fd));
                                FindClose(h);
                            }
                        } while (FindNextFileW(dh, &dirFd));
                        FindClose(dh);
                    }
                    s_audScanned = true;
                }
                if (s_audioFiles.empty()) {
                    ImGui::TextColored(kDim, "(no eclipse_audio_*/ dirs found)");
                } else {
                    for (const auto& rel : s_audioFiles) {
                        bool sel = (rel == b.audioFile);
                        if (ImGui::Selectable(rel.c_str(), sel)) {
                            b.audioFile  = rel;
                            // Build cache key: "LANG/filename.mp3" from "eclipse_audio_LANG\filename.mp3"
                            std::string ckey;
                            auto slash = rel.rfind('\\');
                            if (slash == std::string::npos) slash = rel.rfind('/');
                            if (slash != std::string::npos) {
                                std::string dir = rel.substr(0, slash);   // "eclipse_audio_PL"
                                std::string fn  = rel.substr(slash + 1);  // "01_...mp3"
                                std::string tag = (dir.size() > 14) ? dir.substr(14) : dir;
                                for (char& c : tag) c = static_cast<char>(
                                    std::toupper(static_cast<unsigned char>(c)));
                                ckey = tag + "/" + fn;
                            }
                            int32_t dur = 0;
                            { std::lock_guard lk(m_audioDurMutex);
                              auto it = m_audioDurCache.find(ckey);
                              if (it != m_audioDurCache.end()) dur = it->second; }
                            if (dur <= 0) {
                                // Fallback: probe live (only if not yet in cache)
                                std::wstring wrel = Utf8ToW(rel);
                                for (auto& c : wrel) if (c == L'/') c = L'\\';
                                dur = MciProbeDurMs(ExeDir() + L"\\" + wrel);
                            }
                            b.audioDurMs = (dur > 0) ? dur : 5000;
                            m_tlDirty    = true;
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            } else {
                s_audScanned = false;
            }
            ImGui::SameLine(); ImGui::TextColored(kGray, "File");

            // Duration (auto-detected from MP3 on file select)
            {
                char durBuf[16];
                snprintf(durBuf, sizeof(durBuf), "%.1f s", b.audioDurMs / 1000.f);
                ImGui::SetNextItemWidth(kFldW);
                ImGui::InputText("##aud_dur_ro", durBuf, sizeof(durBuf), ImGuiInputTextFlags_ReadOnly);
                ImGui::SameLine(); ImGui::TextColored(kGray, "Duration");
            }

            // Scan status — shown while AudioScanThreadProc is active or after
            {
                int prog  = m_audioScanProgress.load();
                int total = m_audioScanTotal.load();
                if (total > 0 && prog < total)
                    ImGui::TextColored(ImVec4(.8f,.6f,.1f,1.f),
                                       "Scanning: %d/%d MP3", prog, total);
                else if (total > 0)
                    ImGui::TextColored(ImVec4(.3f,.8f,.3f,1.f),
                                       "Cached: %d files", total);
            }

        }

        // Label
        {
            char lbuf[64];
            ImGui::SetNextItemWidth(kFldW);
            snprintf(lbuf, sizeof(lbuf), "%s", b.label.c_str());
            if (ImGui::InputText("##lbl", lbuf, sizeof(lbuf))) { b.label = lbuf; m_tlDirty = true; }
            ImGui::SameLine(); ImGui::TextColored(kGray, "Label");
        }

        bool prevSnap = b.snapToPrev;
        ImGui::PushStyleColor(ImGuiCol_Text, kGray);
        ImGui::Checkbox("Snap to previous##snp", &b.snapToPrev);
        ImGui::PopStyleColor();
        if (b.snapToPrev != prevSnap) {
            if (b.snapToPrev && m_selBlock > 0) {
                auto& prev = m_tracks[m_selTrack].blocks[m_selBlock - 1];
                b.atMs = prev.atMs + BlockDurMs(prev);
                if (prev.type != BlockType::Audio && b.type != BlockType::Audio &&
                    BlockParamsDiffer(prev, b))
                    b.atMs += ArmEstMs(prev);
            }
            m_tlDirty = true;
        }

        // Snap to Seconds — rounds so the OFFSET FROM THE NEAREST CONTACT is a
        // whole second (matches the Relative ruler row), not so atMs lands on
        // a whole UTC second. Also applied continuously while dragging the
        // block on the timeline (see drag-to-move in RenderTimelineBottom).
        bool secSnap = b.snapToSec;
        ImGui::PushStyleColor(ImGuiCol_Text, kGray);
        ImGui::Checkbox("Snap to Seconds##sns", &b.snapToSec);
        ImGui::PopStyleColor();
        if (b.snapToSec && !secSnap && b.atMs >= 0) {
            b.atMs = SnapMsToRelativeSecond(b.atMs);
            m_tlDirty = true;
        }
        ImGui::Spacing();

        ImGui::PopStyleVar();
        ImGui::PopFont();
    }

    // ── EXECUTE (sequencer run control) — pinned to the bottom of the ─────
    // column; BLOCK INSPECTOR above stretches to fill the gap. ImGui has no
    // native bottom-anchor, so this uses the standard trick: reserve space
    // based on EXECUTE's height measured on the *previous* frame (stable
    // frame-to-frame since content doesn't change shape), then remeasure.
    static float s_executeH = 160.f;   // seed guess; self-corrects each frame
    float spacerH = ImGui::GetContentRegionAvail().y - s_executeH;
    if (spacerH > 0.f) ImGui::Dummy(ImVec2(0.f, spacerH));

    float executeY0 = ImGui::GetCursorPosY();
    ImGui::Spacing();
    RenderSequencerButtons();
    s_executeH = ImGui::GetCursorPosY() - executeY0;
}

// ─── Timeline (bottom, full width) ────────────────────────────────────────────

void App::RenderTimelineBottom() {
    static const float kLabelW  = 140.0f;
    static const float kMarkerH =  14.0f;
    static const float kPhaseH  =  20.0f;
    static const float kRulerH  =  85.0f;
    static const float kTrackH  =  28.0f;

    ImVec2 winPos  = ImGui::GetWindowPos();
    float  winW    = ImGui::GetWindowWidth();
    float  cntW    = winW - kLabelW;   // content width (right of labels)
    m_tlScreenTopY = winPos.y;         // drag-above-here = delete; covers separator row

    // Snapshot connected cameras for dynamic track labels (avoids holding mutex in loop)
    std::vector<CamStatus> camSnap;
    { std::lock_guard lk(m_camerasMutex); camSnap = m_cameras; }

    // Helper: resolve track label from connection state
    auto TrackLabel = [&](const TLTrack& tr) -> std::string {
        if (tr.IsAudio()) return "Audio track";
        if (tr.cameraId.empty()) return "Photo track";
        for (const auto& cs : camSnap) {
            if (cs.model == tr.cameraId && !cs.guid.empty()) {
                std::string sid = cs.guid.size() >= 4
                    ? cs.guid.substr(cs.guid.size() - 4) : cs.guid;
                return cs.model + "  #" + sid;
            }
        }
        return tr.cameraId;   // configured but camera not connected yet
    };

    ImGui::SeparatorText("TIMELINE");
    float y = ImGui::GetCursorScreenPos().y;   // baseline BELOW the separator

    ContactTimes ct;
    { std::lock_guard lk(m_iqpMutex); ct = m_contacts; }
    if (!ct.valid) ct = m_beResult;

    if (!ct.valid) {
        ImGui::SetCursorScreenPos({winPos.x + kLabelW + 8.f, y + 8.f});
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored({0.28f,0.28f,0.32f,1.f}, "Calculate contacts to show timeline");
        ImGui::PopFont();
        return;
    }

    // Initialise view range once
    if (m_tlViewStart < 0) {
        m_tlViewStart = ct.c1Ms - 5LL*60000;
        m_tlViewEnd   = ct.c4Ms + 5LL*60000;
    }
    int64_t vDur = m_tlViewEnd - m_tlViewStart;
    if (vDur <= 0) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // y already set above (cursor pos after SeparatorText "TIMELINE")

    auto toPx = [&](int64_t ms) -> float {
        return winPos.x + kLabelW + float(ms - m_tlViewStart) / float(vDur) * cntW;
    };

    // ── Strip #1 label: Contacts ──────────────────────────────────────────
    dl->AddRectFilled({winPos.x, y}, {winPos.x + kLabelW - 2.f, y + kMarkerH},
                      IM_COL32(10, 10, 14, 255));
    dl->AddText({winPos.x + 4.f, y + 1.f}, IM_COL32(70, 75, 100, 200), "Contacts");

    // ── Strip #2 phase bar — label column holds "Fit timeline" button ─────
    float phaseY = y + kMarkerH;
    dl->AddRectFilled({winPos.x, phaseY}, {winPos.x + kLabelW, phaseY + kPhaseH},
                      IM_COL32(10, 10, 14, 255));
    {
        static constexpr int64_t kFitMarginMs = 5LL * 60 * 1000;
        ImGui::SetCursorScreenPos({winPos.x + 4.f, phaseY + 1.f});
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.26f, 0.59f, 0.98f, 0.40f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.59f, 0.98f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.06f, 0.53f, 0.98f, 1.00f));
        {   // centre label vertically: padV = (btnH - textH) / 2
            float btnH = kPhaseH - 2.f;
            float padV = std::max(0.f, (btnH - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, padV));
        if (ImGui::Button("Fit timeline##tz", ImVec2(kLabelW - 8.f, btnH))) {
            int64_t minMs = INT64_MAX, maxMs = INT64_MIN;
            for (const auto& tr : m_tracks)
                for (const auto& b : tr.blocks)
                    if (b.atMs >= 0) {
                        minMs = std::min(minMs, b.atMs);
                        maxMs = std::max(maxMs, b.atMs + BlockDurMs(b));
                    }
            if (minMs == INT64_MAX) { minMs = ct.c1Ms; maxMs = ct.c4Ms; }
            m_tlViewStart = minMs - kFitMarginMs;
            m_tlViewEnd   = maxMs + kFitMarginMs;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fit all blocks in view (+/- 5 min margin)");
        ImGui::PopStyleVar();   // FramePadding
        }
        ImGui::PopStyleColor(3);
    }

    int64_t c1=ct.c1Ms, c2=ct.c2Ms, mx=ct.maxMs, c3=ct.c3Ms, c4=ct.c4Ms;

    if (c2>0 && c3>0) {
        static constexpr ImU32 kTxtDark  = IM_COL32(180,168,138,155);
        static constexpr ImU32 kTxtBlack = IM_COL32(20, 15,  5, 220);
        struct PZ { int64_t s, e; ImU32 col; const char* lbl; ImU32 textCol; };
        PZ zones[] = {
            {m_tlViewStart, c2-30000, IM_COL32(28,28,36,255),    "Partial Solar Eclipse", kTxtDark },
            {c2-30000, c2-10000, IM_COL32(190,185,145,255),      "Diamond Ring",          kTxtBlack},
            {c2-10000, c2+10000, IM_COL32(110,90,15,255),        "Baily's Beads",         kTxtDark },
            {c2+10000, mx-5000,  IM_COL32(80,60,8,255),          "Total Solar Eclipse",   kTxtDark },
            {mx-5000,  mx+5000,  IM_COL32(150,118,18,255),       "Maximum",               kTxtDark },
            {mx+5000,  c3-10000, IM_COL32(80,60,8,255),          "Total Solar Eclipse",   kTxtDark },
            {c3-10000, c3+10000, IM_COL32(110,90,15,255),        "Baily's Beads",         kTxtDark },
            {c3+10000, c3+30000, IM_COL32(190,185,145,255),      "Diamond Ring",          kTxtBlack},
            {c3+30000, m_tlViewEnd, IM_COL32(28,28,36,255),      "Partial Solar Eclipse", kTxtDark },
        };
        for (auto& z : zones) {
            if (z.s >= z.e) continue;
            float px1 = std::max(toPx(z.s), winPos.x+kLabelW);
            float px2 = std::min(toPx(z.e), winPos.x+winW);
            if (px2 <= px1) continue;
            dl->AddRectFilled({px1,phaseY},{px2,phaseY+kPhaseH}, z.col);
            float zw = px2-px1;
            if (zw > 30.f) {
                float tw = ImGui::CalcTextSize(z.lbl).x;
                dl->AddText({px1+(zw-tw)*0.5f, phaseY+3.f}, z.textCol, z.lbl);
            }
        }
    } else {
        dl->AddRectFilled({winPos.x+kLabelW,phaseY},{winPos.x+winW,phaseY+kPhaseH},
                          IM_COL32(28,28,36,255));
    }
    dl->AddRect({winPos.x+kLabelW,phaseY},{winPos.x+winW,phaseY+kPhaseH},
                IM_COL32(40,40,52,255));

    // Contact markers
    struct CM { int64_t ms; const char* lbl; ImU32 col; };
    CM cms[] = {
        {c1,"C1",  IM_COL32(140,140,165,255)},
        {c2,"C2",  IM_COL32(255,210, 50,255)},
        {mx,"Max", IM_COL32(255,255,255,255)},
        {c3,"C3",  IM_COL32(255,210, 50,255)},
        {c4,"C4",  IM_COL32(140,140,165,255)},
    };
    for (auto& m : cms) {
        if (m.ms < 0) continue;
        float px = toPx(m.ms);
        if (px < winPos.x+kLabelW || px > winPos.x+winW) continue;
        dl->AddLine({px,phaseY},{px,phaseY+kPhaseH}, m.col, 1.5f);
        dl->AddText({px+2.f, y+1.f}, m.col, m.lbl);
    }

    // ── Relative time strip + UTC ruler ──────────────────────────────────
    static constexpr float kRelRulerH = 22.0f;  // strip above the UTC ruler
    float relRulerY = phaseY + kPhaseH;
    float rulerY    = relRulerY + kRelRulerH;

    // Relative ruler backgrounds
    dl->AddRectFilled({winPos.x,          relRulerY},
                      {winPos.x + kLabelW - 2.f, relRulerY + kRelRulerH},
                      IM_COL32(9, 9, 14, 255));
    dl->AddRectFilled({winPos.x + kLabelW, relRulerY},
                      {winPos.x + winW,    relRulerY + kRelRulerH},
                      IM_COL32(7, 7, 13, 255));
    // Bottom border separating relative strip from UTC ruler
    dl->AddLine({winPos.x + kLabelW, relRulerY + kRelRulerH - 1.f},
                {winPos.x + winW,    relRulerY + kRelRulerH - 1.f},
                IM_COL32(28, 28, 46, 255), 1.f);
    // Strip #3 label: Relative time
    dl->AddText({winPos.x + 4.f, relRulerY + 4.f}, IM_COL32(70, 75, 100, 200), "Relative");

    // UTC ruler background + strip #4 label
    dl->AddRectFilled({winPos.x, rulerY}, {winPos.x + winW, rulerY + kRulerH},
                      IM_COL32(10, 10, 14, 255));
    dl->AddText({winPos.x + 4.f, rulerY + 4.f}, IM_COL32(70, 75, 100, 200), "UTC");

    // Collect ticks: phase transitions + 60s / 10s / 1s interval marks.
    // Fixed-size stack array (no heap alloc): NASA rule 3.
    // hf = height fraction (1.0 phase, 0.55 min, 0.35 10s, 0.18 1s)
    struct RulerTick { int64_t ms; ImU32 col; bool isPhase; float hf; };
    static constexpr int kMaxRulerTicks = 512;
    RulerTick ticks[kMaxRulerTicks];
    int numTicks = 0;

    auto addTick = [&](int64_t ms, ImU32 col, bool phase, float hf = 1.0f) {
        assert(numTicks <= kMaxRulerTicks);
        if (numTicks < kMaxRulerTicks && ms >= m_tlViewStart && ms <= m_tlViewEnd)
            ticks[numTicks++] = {ms, col, phase, hf};
    };

    // Phase transition times — one tick per boundary of the phase-description
    // bar above (Partial/Diamond Ring/Baily's Beads/Total/Maximum zones), plus
    // the C1-C4/Max contact instants themselves. Boundaries match the `zones`
    // array above exactly so every visible colour transition has a UTC label.
    if (c2 > 0 && c3 > 0) {
        constexpr ImU32 kC   = IM_COL32(255, 210,  50, 200);
        constexpr ImU32 kMax = IM_COL32(255, 255, 255, 180);
        constexpr ImU32 kC14 = IM_COL32(140, 140, 165, 160);
        constexpr ImU32 kSub = IM_COL32(130, 110,  70, 140);
        if (c1 > 0)  addTick(c1,          kC14, true, 1.0f);
        addTick(c2 - 30000, kSub, true, 1.0f);   // Partial → Diamond Ring
        addTick(c2 - 10000, kSub, true, 1.0f);   // Diamond Ring → Baily's Beads
        addTick(c2,         kC,   true, 1.0f);   // C2 contact
        addTick(c2 + 10000, kSub, true, 1.0f);   // Baily's Beads → Total
        addTick(mx -  5000, kSub, true, 1.0f);   // Total → Maximum
        addTick(mx,         kMax, true, 1.0f);   // Max contact
        addTick(mx +  5000, kSub, true, 1.0f);   // Maximum → Total
        addTick(c3 - 10000, kSub, true, 1.0f);   // Total → Baily's Beads
        addTick(c3,         kC,   true, 1.0f);   // C3 contact
        addTick(c3 + 10000, kSub, true, 1.0f);   // Baily's Beads → Diamond Ring
        addTick(c3 + 30000, kSub, true, 1.0f);   // Diamond Ring → Partial
        if (c4 > 0)  addTick(c4,          kC14, true, 1.0f);
    }

    float px1s  = (vDur > 0) ? 1000.f  * cntW / static_cast<float>(vDur) : 0.f;
    float px10s = px1s * 10.f;
    float px60s = px1s * 60.f;

    // 60-second ticks — only when >= 20px apart
    if (px60s >= 20.f) {
        int64_t ft = ((m_tlViewStart / 60000LL) + 1) * 60000LL;
        static constexpr int k60Max = 300;
        int loop60 = 0;
        for (int64_t t = ft; t < m_tlViewEnd && loop60 < k60Max; t += 60000LL, ++loop60) {
            float tx = toPx(t);
            bool tooClose = false;
            for (int i = 0; i < numTicks; ++i)
                if (fabsf(toPx(ticks[i].ms) - tx) < 8.f) { tooClose = true; break; }
            if (!tooClose) addTick(t, IM_COL32(55, 55, 80, 180), false, 0.55f);
        }
    }

    // 10-second ticks — when >= 15px apart
    if (px10s >= 15.f) {
        int64_t ft = ((m_tlViewStart / 10000LL) + 1) * 10000LL;
        static constexpr int k10Max = 512;
        int loop10 = 0;
        for (int64_t t = ft; t < m_tlViewEnd && loop10 < k10Max; t += 10000LL, ++loop10) {
            float tx = toPx(t);
            bool tooClose = false;
            for (int i = 0; i < numTicks; ++i)
                if (fabsf(toPx(ticks[i].ms) - tx) < 5.f) { tooClose = true; break; }
            if (!tooClose) addTick(t, IM_COL32(42, 42, 68, 155), false, 0.35f);
        }
    }

    // 1-second ticks — when >= 8px apart
    if (px1s >= 8.f) {
        int64_t ft = ((m_tlViewStart / 1000LL) + 1) * 1000LL;
        // Bounded: at 8px/s max ~240 s visible — well within kMaxRulerTicks budget
        static constexpr int k1Max = 480;
        int loop1 = 0;
        for (int64_t t = ft; t < m_tlViewEnd && loop1 < k1Max; t += 1000LL, ++loop1) {
            float tx = toPx(t);
            bool tooClose = false;
            for (int i = 0; i < numTicks; ++i)
                if (fabsf(toPx(ticks[i].ms) - tx) < 3.f) { tooClose = true; break; }
            if (!tooClose) addTick(t, IM_COL32(32, 32, 55, 120), false, 0.18f);
        }
    }

    // Sort by time so greedy left-to-right label placement works
    std::sort(ticks, ticks + numTicks,
              [](const RulerTick& a, const RulerTick& b){ return a.ms < b.ms; });

    // Draw ticks + rotated UTC labels (UTC labels only for hf >= 0.5: phase + 60s ticks)
    ImFont* lblFont  = m_fontMono ? m_fontMono : ImGui::GetIO().Fonts->Fonts[0];
    constexpr float kLblSz   = 15.0f;  // native m_fontMono size
    constexpr float kTickH   =  7.0f;  // full tick height (phase)
    constexpr float kMinLblDx = 16.0f; // min px between UTC labels
    float lastLabelX = -9999.f;

    for (int i = 0; i < numTicks; ++i) {
        const RulerTick& tk = ticks[i];
        float tx = toPx(tk.ms);
        if (tx < winPos.x + kLabelW || tx > winPos.x + winW) continue;

        float th = tk.hf * kTickH;
        dl->AddLine({tx, rulerY}, {tx, rulerY + th}, tk.col, 1.f);

        // UTC label only for phase ticks and 60s ticks (hf >= 0.5)
        if (tk.hf >= 0.5f && tx - lastLabelX >= kMinLblDx) {
            int64_t sv = tk.ms / 1000;
            char lb[12];
            snprintf(lb, sizeof(lb), "%02d:%02d:%02d",
                     (int)((sv / 3600) % 24), (int)((sv / 60) % 60), (int)(sv % 60));
            AddTextRotated90CCW(dl, lblFont, kLblSz,
                                {tx, rulerY + kRulerH - 2.f}, tk.col, lb);
            lastLabelX = tx;
        }
    }

    // ── Relative time ruler (between phase bar and UTC ruler) ─────────────
    // Shows Cx±M:SS / Cx±Ss relative to nearest contact at each tick position.
    if (c1 > 0 || c2 > 0 || c3 > 0 || c4 > 0) {
        constexpr float kRelLblSz = 10.5f;
        constexpr float kRelMinDx = 48.f;   // min px between relative labels
        // Choose label interval to match visible tick density
        int64_t relInt = (px1s >= 8.f) ? 1000LL : (px10s >= 15.f) ? 10000LL : 60000LL;

        // Contact table
        const int64_t cMs[4]    = {c1, c2, c3, c4};
        const char*   cName[4]  = {"C1", "C2", "C3", "C4"};
        const ImU32   cCol[4]   = {IM_COL32(140,140,165,200), IM_COL32(255,200,40,220),
                                   IM_COL32(255,200,40,220),  IM_COL32(140,140,165,200)};

        float relLastX  = -9999.f;
        int64_t rtFirst = ((m_tlViewStart / relInt) + 1) * relInt;
        static constexpr int kRelMax = 512;
        int relLoop = 0;
        for (int64_t t = rtFirst; t < m_tlViewEnd && relLoop < kRelMax;
             t += relInt, ++relLoop) {
            float tx = toPx(t);
            if (tx < winPos.x + kLabelW || tx > winPos.x + winW) continue;
            if (tx - relLastX < kRelMinDx) continue;

            // Nearest contact
            int best = -1; int64_t bestD = INT64_MAX;
            for (int ci = 0; ci < 4; ++ci) {
                if (cMs[ci] <= 0) continue;
                int64_t d = llabs(t - cMs[ci]);
                if (d < bestD) { bestD = d; best = ci; }
            }
            if (best < 0) continue;

            int64_t diff    = t - cMs[best];
            int64_t absDiff = llabs(diff);
            int totalSecs   = static_cast<int>(absDiff / 1000);
            char lb[16];
            if (diff == 0)
                snprintf(lb, sizeof(lb), "%s", cName[best]);
            else
                snprintf(lb, sizeof(lb), "%s%s%ds",
                         cName[best], diff < 0 ? "-" : "+", totalSecs);

            dl->AddText(lblFont, kRelLblSz, {tx + 2.f, relRulerY + 5.f}, cCol[best], lb);
            // Short tick mark down to separate the two rulers
            dl->AddLine({tx, relRulerY + kRelRulerH - 5.f},
                        {tx, relRulerY + kRelRulerH - 1.f},
                        IM_COL32(50, 50, 75, 160), 1.f);
            relLastX = tx;
        }
    }


    // ── Tracks ────────────────────────────────────────────────────────────
    float tracksY = rulerY + kRulerH;
    int   nT      = (int)m_tracks.size();
    float tracksH = nT * kTrackH;

    // ── Drag-to-move update (runs every frame while LMB held) ────────────────
    if (m_tlDragTrack >= 0 && m_tlDragBlock >= 0
        && m_tlDragTrack < nT
        && m_tlDragBlock < (int)m_tracks[m_tlDragTrack].blocks.size()) {

        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
            m_tlDragging = true;
            float   dx  = ImGui::GetMousePos().x - m_tlDragMouseX0;
            int64_t dMs = int64_t(dx / cntW * vDur);
            int64_t newMs = std::max(m_tlDragStartMs + dMs, m_tlViewStart);
            TLBlock& dragged = m_tracks[m_tlDragTrack].blocks[m_tlDragBlock];
            if (dragged.snapToSec) newMs = SnapMsToRelativeSecond(newMs);
            dragged.atMs = newMs;
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (m_tlDragging) {
                ImVec2 mp  = ImGui::GetMousePos();
                auto&  trk = m_tracks[m_tlDragTrack];

                if (mp.y < m_tlScreenTopY) {
                    // Released above timeline → delete block
                    trk.blocks.erase(trk.blocks.begin() + m_tlDragBlock);
                    m_selTrack = -1; m_selBlock = -1;
                } else {
                    // Finalize: sort by time
                    int64_t   movedMs = trk.blocks[m_tlDragBlock].atMs;
                    BlockType movedTy = trk.blocks[m_tlDragBlock].type;
                    std::sort(trk.blocks.begin(), trk.blocks.end(),
                        [](const TLBlock& a, const TLBlock& b){ return a.atMs < b.atMs; });
                    // Update selection index after sort
                    for (int i = 0; i < (int)trk.blocks.size(); ++i) {
                        if (trk.blocks[i].atMs == movedMs && trk.blocks[i].type == movedTy) {
                            m_selBlock = i; break;
                        }
                    }
                    // Apply snap-to-prev if enabled
                    if (m_selBlock > 0 && trk.blocks[m_selBlock].snapToPrev) {
                        auto& prev = trk.blocks[m_selBlock - 1];
                        auto& cur  = trk.blocks[m_selBlock];
                        cur.atMs = prev.atMs + BlockDurMs(prev);
                        if (prev.type != BlockType::Audio && cur.type != BlockType::Audio &&
                            BlockParamsDiffer(prev, cur))
                            cur.atMs += ArmEstMs(prev);
                    }
                }
                m_tlDirty = true;
            }
            m_tlDragging    = false;
            m_tlDragTrack   = -1;
            m_tlDragBlock   = -1;
            m_tlDragStartMs = -1;
        }
    }

    // ── Click detection: select block + initiate drag state ──────────────────
    if (!m_tlDragging && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mp = ImGui::GetMousePos();
        bool hit = false;
        for (int ti = 0; ti < nT && !hit; ++ti) {
            float ty = tracksY + ti*kTrackH;
            if (mp.y < ty || mp.y >= ty+kTrackH) continue;
            for (int bi = 0; bi < (int)m_tracks[ti].blocks.size(); ++bi) {
                auto& blk = m_tracks[ti].blocks[bi];
                float bx  = toPx(blk.atMs);
                int64_t blkEnd = blk.atMs + BlockDurMs(blk);
                // Extend hit box to cover ARM extension zone.
                if (blk.type != BlockType::Audio &&
                    bi + 1 < (int)m_tracks[ti].blocks.size()) {
                    const TLBlock& nxt = m_tracks[ti].blocks[bi + 1];
                    if (nxt.atMs >= 0 && BlockParamsDiffer(blk, nxt))
                        blkEnd += ArmEstMs(blk);
                }
                float bx2 = toPx(blkEnd);
                if (mp.x >= bx && mp.x <= bx2) {
                    m_selTrack = ti; m_selBlock = bi;
                    // Arm drag state (actual drag starts after threshold)
                    m_tlDragTrack   = ti;
                    m_tlDragBlock   = bi;
                    m_tlDragStartMs = blk.atMs;
                    m_tlDragMouseX0 = mp.x;
                    hit = true; break;
                }
            }
        }
        if (!hit) {
            bool inArea = mp.x >= winPos.x+kLabelW && mp.x <= winPos.x+winW
                       && mp.y >= tracksY && mp.y < tracksY+tracksH;
            if (inArea) { m_selTrack = -1; m_selBlock = -1; }
        }
    }

    // Per-track DnD drop targets.
    // Camera tracks accept "TL_CAM_BLOCK", audio track accepts "TL_AUD_BLOCK".
    // dropTargetTi records which track is the active hover target so we can draw
    // the highlight rect in pass 2 (after track backgrounds, so it's not covered).
    int dropTargetTi = -1;
    for (int ti = 0; ti < nT; ++ti) {
        float      ty  = tracksY + ti * kTrackH;
        TLTrack&   tr  = m_tracks[ti];
        const char* pid = tr.IsCamera() ? "TL_CAM_BLOCK" : "TL_AUD_BLOCK";

        ImGui::SetCursorScreenPos({winPos.x + kLabelW, ty});
        char dropId[24]; snprintf(dropId, sizeof(dropId), "##tl_drop_%d", ti);
        ImGui::InvisibleButton(dropId, {cntW, kTrackH});

        // ImGui's RenderDragDropTargetRect writes to window->DrawList before track
        // backgrounds are drawn, so it gets covered. Push transparent to suppress it;
        // we draw our own rect in pass 2.
        ImGui::PushStyleColor(ImGuiCol_DragDropTarget, IM_COL32(0, 0, 0, 0));
        if (ImGui::BeginDragDropTarget()) {
            dropTargetTi = ti;
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(pid)) {
                BlockType bt   = *(const BlockType*)p->Data;
                float     relX = ImGui::GetMousePos().x - (winPos.x + kLabelW);
                int64_t   atMs = std::max(
                    m_tlViewStart + int64_t(relX / cntW * vDur), m_tlViewStart);
                TLBlock nb; nb.type = bt; nb.atMs = atMs;
                tr.blocks.push_back(nb);
                std::sort(tr.blocks.begin(), tr.blocks.end(),
                    [](const TLBlock& a, const TLBlock& b){ return a.atMs < b.atMs; });
                m_selTrack = ti;
                auto it = std::find_if(tr.blocks.begin(), tr.blocks.end(),
                    [&](const TLBlock& blk){ return blk.atMs==atMs && blk.type==bt; });
                m_selBlock = (int)(it - tr.blocks.begin());
                m_tlDirty = true;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopStyleColor();
    }

    // Draw tracks — pass 1: backgrounds, block fills, labels
    // 2-px gap at the bottom of each track (ty2-2 instead of ty2) separates rows visually.
    for (int ti = 0; ti < nT; ++ti) {
        TLTrack& tr  = m_tracks[ti];
        float ty     = tracksY + ti * kTrackH;
        float tyFill = ty + kTrackH - 2.f;   // bottom of visible content (2px gap below)

        // Label column — highlighted when track is selected
        bool isSelTrack = (ti == m_selTrack);
        ImU32 labelBg = isSelTrack ? IM_COL32(28, 40, 62, 255) : IM_COL32(14, 14, 22, 255);
        dl->AddRectFilled({winPos.x, ty}, {winPos.x + kLabelW - 2.f, tyFill}, labelBg);
        ImU32 lc = tr.IsAudio() ? IM_COL32(150,100,210,255) : IM_COL32(120,148,172,255);
        std::string dynLbl = TrackLabel(tr);

        dl->AddText({winPos.x + 8.f, ty + 7.f}, lc, dynLbl.c_str());
        // Left-click on track label → select track (highlights label background)
        {
            ImVec2 mp = ImGui::GetMousePos();
            bool hovered = (mp.x >= winPos.x && mp.x < winPos.x + kLabelW - 2.f
                            && mp.y >= ty && mp.y < tyFill);
            if (hovered && ImGui::IsMouseClicked(0)) {
                m_selTrack = ti;
                m_selBlock = -1;
                if (!tr.IsAudio()) m_presetTargetTrack = ti;
            }
        }

        // Track content bg
        ImU32 bg = (ti % 2 == 0) ? IM_COL32(14,14,20,255) : IM_COL32(12,12,18,255);
        dl->AddRectFilled({winPos.x+kLabelW, ty}, {winPos.x+winW, tyFill}, bg);

        // Block fills + text labels (no selection border here — drawn in pass 2)
        for (int bi = 0; bi < (int)tr.blocks.size(); ++bi) {
            TLBlock& blk = tr.blocks[bi];
            if (blk.atMs < 0) continue;
            float bx  = toPx(blk.atMs);
            float bx2 = toPx(blk.atMs + BlockDurMs(blk));
            bx2 = std::max(bx2, bx + 3.f);
            float cx1 = winPos.x + kLabelW, cx2 = winPos.x + winW;
            if (bx2 < cx1 || bx > cx2) continue;
            float dx  = std::max(bx, cx1);
            float dx2 = std::min(bx2, cx2);
            bool  dragged  = (m_tlDragging && m_tlDragTrack==ti && m_tlDragBlock==bi);
            bool  willDel  = dragged && (ImGui::GetMousePos().y < m_tlScreenTopY);
            // Audio overlap: red when this block's playback ends after the next block starts.
            bool  audOverlap = blk.type == BlockType::Audio
                            && bi + 1 < (int)tr.blocks.size()
                            && tr.blocks[bi+1].atMs > 0
                            && blk.atMs + blk.audioDurMs > tr.blocks[bi+1].atMs;
            ImU32 col = willDel   ? IM_COL32(200, 40,  40, 200)
                      : audOverlap ? IM_COL32(220, 40,  40, 220)
                      :              BlockColor(blk.type);
            dl->AddRectFilled({dx, ty+2}, {dx2, tyFill}, col, 2.f);
            if (dx2-dx > 20.f && !blk.label.empty())
                dl->AddText({dx+4.f, ty+6.f}, IM_COL32(240,240,240,215), blk.label.c_str());

            // ARM extension: dim bar showing camera write+arm time that follows a
            // block when the next block has different params.
            if (!tr.IsAudio() && bi + 1 < (int)tr.blocks.size()) {
                const TLBlock& nxt = tr.blocks[bi + 1];
                if (nxt.atMs >= 0 && blk.type != BlockType::Audio &&
                    BlockParamsDiffer(blk, nxt)) {
                    float bxA  = toPx(blk.atMs + BlockDurMs(blk));
                    float bxA2 = toPx(blk.atMs + BlockDurMs(blk) + ArmEstMs(blk));
                    bxA2 = std::max(bxA2, bxA + 2.f);
                    float dxA  = std::max(bxA,  cx1);
                    float dxA2 = std::min(bxA2, cx2);
                    if (dxA2 > dxA) {
                        dl->AddRectFilled({dxA, ty+4.f}, {dxA2, tyFill-2.f},
                                          IM_COL32(160, 90, 20, 90));
                        if (dxA2 - dxA > 28.f)
                            dl->AddText({dxA + 3.f, ty + 6.f},
                                        IM_COL32(200, 130, 60, 160), "ARM");
                    }
                }
            }
        }
    }

    // Draw tracks — pass 2: selection/drag borders on top of everything (incl. track gaps)
    for (int ti = 0; ti < nT; ++ti) {
        const TLTrack& tr = m_tracks[ti];
        float ty     = tracksY + ti * kTrackH;
        float tyFill = ty + kTrackH - 2.f;
        for (int bi = 0; bi < (int)tr.blocks.size(); ++bi) {
            bool sel     = (m_selTrack == ti && m_selBlock == bi);
            bool dragged = (m_tlDragging && m_tlDragTrack == ti && m_tlDragBlock == bi);
            if (!sel && !dragged) continue;
            const TLBlock& blk = tr.blocks[bi];
            if (blk.atMs < 0) continue;
            float bx  = toPx(blk.atMs);
            int64_t selEnd = blk.atMs + BlockDurMs(blk);
            if (blk.type != BlockType::Audio && bi + 1 < (int)tr.blocks.size()) {
                const TLBlock& nxt = tr.blocks[bi + 1];
                if (nxt.atMs >= 0 && BlockParamsDiffer(blk, nxt))
                    selEnd += ArmEstMs(blk);
            }
            float bx2 = toPx(selEnd);
            bx2 = std::max(bx2, bx + 3.f);
            float cx1 = winPos.x + kLabelW, cx2 = winPos.x + winW;
            if (bx2 < cx1 || bx > cx2) continue;
            float dx  = std::max(bx, cx1);
            float dx2 = std::min(bx2, cx2);
            bool  willDel = dragged && (ImGui::GetMousePos().y < m_tlScreenTopY);
            ImU32 rimCol  = willDel ? IM_COL32(255,60,60,255) : IM_COL32(255,220,60,255);
            dl->AddRect({dx-1, ty+1}, {dx2+1, tyFill+1}, rimCol, 2.f, 0, 2.f);
        }
    }

    // DnD drop target highlight — drawn after all track backgrounds so it's not covered.
    // Full-height border around the compatible target track row.
    if (dropTargetTi >= 0) {
        assert(dropTargetTi < nT);
        float ty     = tracksY + dropTargetTi * kTrackH;
        float tyFill = ty + kTrackH - 2.f;
        dl->AddRect({winPos.x + kLabelW + 1.f, ty + 1.f},
                    {winPos.x + winW - 1.f,    tyFill},
                    IM_COL32(255, 220, 60, 230), 0.f, 0, 2.f);
    }

    // "now" cursor
    int64_t nowMs = UtcNowMs();
    if (nowMs >= m_tlViewStart && nowMs <= m_tlViewEnd) {
        float nx = toPx(nowMs);
        dl->AddLine({nx,phaseY},{nx,tracksY+tracksH}, IM_COL32(210,70,70,175), 1.5f);
        dl->AddText({nx+2.f, y+1.f}, IM_COL32(210,70,70,255), "now");
    }

    // ── Playhead ──────────────────────────────────────────────────────────
    // Initialise playhead to C2 - 45 s when contacts first become available.
    {
        int64_t ph = m_tlPlayheadMs.load();
        if (ph < 0 && c2 > 0) {
            ph = c2 - 45000LL;
            m_tlPlayheadMs.store(ph);
        }
        if (ph >= m_tlViewStart && ph <= m_tlViewEnd) {
            float px = toPx(ph);
            // Cyan line from ruler top to bottom of tracks
            dl->AddLine({px, rulerY}, {px, tracksY + tracksH},
                        IM_COL32(80, 220, 255, 200), 1.5f);
            // Triangle marker at ruler top
            dl->AddTriangleFilled(
                {px,       rulerY + 6.f},
                {px - 6.f, rulerY},
                {px + 6.f, rulerY},
                IM_COL32(80, 220, 255, 230));
            // Time label removed — too little room here to read it; the same
            // playhead time is shown in the Sky View Simulator status bar.
        }
    }

    // ── Playhead drag (grab triangle ±8 px, hold, move) ──────────────────────
    {
        GuiSeqMode sm    = m_guiSeqMode.load();
        bool       canPh = (sm == GuiSeqMode::Idle || sm == GuiSeqMode::TestPaused);

        // Cancel drag if mode changed externally
        if (!canPh && m_tlPhDragging) m_tlPhDragging = false;

        if (canPh && ImGui::IsWindowHovered()) {
            ImVec2  mp    = ImGui::GetMousePos();
            int64_t ph    = m_tlPlayheadMs.load();
            float   phPx  = (ph >= m_tlViewStart && ph <= m_tlViewEnd)
                          ? toPx(ph) : -9999.f;

            // ── Start drag when clicking within 8 px of playhead triangle ────
            if (!m_tlPhDragging && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                bool inRuler = mp.y >= rulerY && mp.y < rulerY + kRulerH + 8.f
                            && mp.x > winPos.x + kLabelW
                            && mp.x < winPos.x + winW;
                if (inRuler && std::fabs(mp.x - phPx) <= 8.f)
                    m_tlPhDragging = true;
            }

            // ── Update playhead while dragging ───────────────────────────────
            if (m_tlPhDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                float   relX  = std::clamp(mp.x - (winPos.x + kLabelW), 0.f, cntW);
                int64_t tMs   = m_tlViewStart + int64_t(relX / cntW * vDur);
                int64_t newPh = std::clamp(tMs, m_tlViewStart, m_tlViewEnd);
                m_tlPlayheadMs.store(newPh);
                // Keep per-track next-block indices in sync for TestPaused resume
                if (sm == GuiSeqMode::TestPaused) {
                    int camIdx = 0;
                    for (int ti = 0;
                         ti < static_cast<int>(m_tracks.size()) && camIdx < kMaxCamTracks;
                         ++ti) {
                        if (!m_tracks[ti].IsCamera()) continue;
                        int ni = 0;
                        for (; ni < static_cast<int>(m_tracks[ti].blocks.size()); ++ni)
                            if (m_tracks[ti].blocks[ni].atMs >= newPh) break;
                        m_seqNextBlock[camIdx++] = ni;
                    }
                    int audioIdx = 0;
                    for (int ti = 0;
                         ti < static_cast<int>(m_tracks.size()) && audioIdx < kMaxAudioTracks;
                         ++ti) {
                        if (!m_tracks[ti].IsAudio()) continue;
                        int ni = 0;
                        for (; ni < static_cast<int>(m_tracks[ti].blocks.size()); ++ni)
                            if (m_tracks[ti].blocks[ni].atMs >= newPh) break;
                        m_audioNextBlock[audioIdx++].store(ni, std::memory_order_relaxed);
                    }
                }
            }

            // ── End drag on mouse release ────────────────────────────────────
            if (m_tlPhDragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                m_tlPhDragging = false;
        }
    }

    // ── Timeline pan (grab "Contacts" strip above phase bar, drag L/R) ──────
    // Deliberately restricted to the strip above the phase-description row
    // (y .. phaseY) so it never competes with playhead drag (ruler) or
    // block drag (track rows).
    {
        if (!m_tlPanDragging && ImGui::IsWindowHovered()
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ImVec2 mp = ImGui::GetMousePos();
            bool inContactsStrip = mp.y >= y && mp.y < phaseY
                                && mp.x > winPos.x + kLabelW
                                && mp.x < winPos.x + winW;
            if (inContactsStrip) {
                m_tlPanDragging     = true;
                m_tlPanMouseX0      = mp.x;
                m_tlPanStartViewMs  = m_tlViewStart;
                m_tlPanStartViewDur = vDur;
            }
        }

        if (m_tlPanDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float   dx  = ImGui::GetMousePos().x - m_tlPanMouseX0;
            int64_t dMs = int64_t(dx / cntW * float(m_tlPanStartViewDur));
            m_tlViewStart = m_tlPanStartViewMs - dMs;
            m_tlViewEnd   = m_tlViewStart + m_tlPanStartViewDur;
        }

        if (m_tlPanDragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            m_tlPanDragging = false;
    }

    // ── Scroll wheel zoom ─────────────────────────────────────────────────
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (std::fabs(wheel) > 0.01f) {
            ImVec2  mp   = ImGui::GetMousePos();
            float   relX = std::clamp((mp.x-(winPos.x+kLabelW))/cntW, 0.f, 1.f);
            int64_t fms  = m_tlViewStart + int64_t(relX * vDur);
            float   fac  = (wheel > 0) ? 0.75f : 1.333f;
            int64_t nd   = std::clamp(int64_t(float(vDur)*fac),
                                      10LL*1000LL, 4LL*3600000LL);
            m_tlViewStart = fms - int64_t(relX * nd);
            m_tlViewEnd   = m_tlViewStart + nd;
        }
    }
}

// ─── App lifecycle ────────────────────────────────────────────────────────────

App::App() {
    std::wstring dir = ExeDir();

    m_logFile.open(dir + L"\\TotalControlGUI.log", std::ios::app);
    LogLine("=== TotalControlGUI start ===");

    // Route IQP HTTP diagnostics through the same log file (called from bg thread).
    SetIqpLogger([this](std::string_view msg) { LogLine(msg); });

    // ── TotalControlDefaultConfig.db — factory defaults ───────────────────
    std::wstring defaultCfgPath = dir + L"\\TotalControlDefaultConfig.db";
    EnsureDefaultConfig(defaultCfgPath);

    // ── TotalControlConfig.db — active config ─────────────────────────────
    std::wstring configPath = dir + L"\\TotalControlConfig.db";
    m_configPath = configPath;
    if (!std::filesystem::exists(configPath)) {
        std::filesystem::copy_file(defaultCfgPath, configPath);
        LogLine("First run: TotalControlConfig.db created from defaults");
    }

    if (m_configDb.Open(configPath)) {
        m_showHomeClock = m_configDb.GetSettingInt("show_home_clock", 1) != 0;
        m_showEclClock  = m_configDb.GetSettingInt("show_ecl_clock",  1) != 0;
        std::string home = m_configDb.GetSetting("home_tz_iana", "");
        std::string ecl  = m_configDb.GetSetting("ecl_tz_iana",  "");
        if (!home.empty()) m_homeTzIana = home;
        if (!ecl.empty())  m_eclTzIana  = ecl;

        try { m_obsLat = std::stof(m_configDb.GetSetting("obs_lat", "0")); } catch (...) {}
        try { m_obsLon = std::stof(m_configDb.GetSetting("obs_lon", "0")); } catch (...) {}
        m_obsAltM  = m_configDb.GetSettingInt("obs_alt_m", 0);
        m_obsName  = m_configDb.GetSetting("obs_name", "");
        SyncDecimalToDms();

        // Load IQP API key (user-supplied, 40 hex chars, stored in Config.db, never in source)
        std::string beKey = m_configDb.GetSetting("be_api_key", "");
        if (!beKey.empty()) {
            SetBeApiKey(beKey);
            size_t copyLen = std::min(beKey.size(), sizeof(m_beApiKeyBuf) - 1);
            std::memcpy(m_beApiKeyBuf, beKey.c_str(), copyLen);
            m_beApiKeyBuf[copyLen] = '\0';
        }

        // SUVI alignment calibration + toggle (persisted; orbital drift + user pref)
        // suvi_calib_ver < 2 → one-time migration to v2 measured defaults (2026-06-28)
        constexpr int kSuviCalibVer = 2;
        if (m_configDb.GetSettingInt("suvi_calib_ver", 0) < kSuviCalibVer) {
            // Overwrite any stale values with the v2 calibration; members already hold them.
            m_configDb.SetSetting   ("suvi_half_q",        std::to_string(m_suviHalfQ).c_str());
            m_configDb.SetSetting   ("suvi_footer_px",     std::to_string(m_suviFooterPx).c_str());
            m_configDb.SetSetting   ("suvi_corr_right_px", std::to_string(m_suviCorrRightPx).c_str());
            m_configDb.SetSetting   ("suvi_corr_up_px",    std::to_string(m_suviCorrUpPx).c_str());
            m_configDb.SetSettingInt("suvi_calib_ver", kSuviCalibVer);
        } else {
            try { m_suviHalfQ       = std::stof(m_configDb.GetSetting("suvi_half_q",        std::to_string(m_suviHalfQ).c_str())); }       catch (...) {}
            try { m_suviFooterPx    = std::stof(m_configDb.GetSetting("suvi_footer_px",     std::to_string(m_suviFooterPx).c_str())); }    catch (...) {}
            try { m_suviCorrRightPx = std::stof(m_configDb.GetSetting("suvi_corr_right_px", std::to_string(m_suviCorrRightPx).c_str())); } catch (...) {}
            try { m_suviCorrUpPx    = std::stof(m_configDb.GetSetting("suvi_corr_up_px",    std::to_string(m_suviCorrUpPx).c_str())); }    catch (...) {}
        }
        m_suviOpacity = m_configDb.GetSettingInt("suvi_opacity_pct", 100) / 100.f;
        m_suviChannel = m_configDb.GetSetting("suvi_channel", "Fe171");
        m_moonOpacity = m_configDb.GetSettingInt("moon_opacity_pct", 100) / 100.f;

        LogLine(std::format("Config DB: home={}  ecl={}  obs={:.4f},{:.4f} {}m",
                            m_homeTzIana, m_eclTzIana, m_obsLat, m_obsLon, m_obsAltM));

        // Migration: create timeline tables if they don't exist yet
        m_configDb.Exec(R"SQL(
            CREATE TABLE IF NOT EXISTS tl_tracks (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                sort_order INTEGER NOT NULL DEFAULT 0,
                type       TEXT    NOT NULL DEFAULT 'camera',
                camera_id  TEXT    NOT NULL DEFAULT '',
                label      TEXT    NOT NULL DEFAULT ''
            );
            CREATE TABLE IF NOT EXISTS tl_blocks (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                track_id      INTEGER NOT NULL,
                at_ms         INTEGER NOT NULL DEFAULT -1,
                block_type    INTEGER NOT NULL DEFAULT 0,
                ss            TEXT    NOT NULL DEFAULT '1/100',
                iso           INTEGER NOT NULL DEFAULT 100,
                fstop         TEXT    NOT NULL DEFAULT '8.0',
                cnt           INTEGER NOT NULL DEFAULT 5,
                ev            TEXT    NOT NULL DEFAULT '1.0ev',
                burst_drive   TEXT    NOT NULL DEFAULT 'cont-hi-plus',
                burst_dur_ms  INTEGER NOT NULL DEFAULT 3000,
                audio_file    TEXT    NOT NULL DEFAULT '',
                audio_dur_ms  INTEGER NOT NULL DEFAULT 10000,
                label         TEXT    NOT NULL DEFAULT '',
                snap_to_prev  INTEGER NOT NULL DEFAULT 0
            );
        )SQL");

        // Migration: create named snapshot tables if they don't exist yet
        m_configDb.CreateSnapshotTables();

        // Bracket calibration — create tables, seed built-in data, warm cache
        m_configDb.CreateCalibTables();
        SeedBuiltinCalib();
        LoadCalibCache();

        // Ephemeris cache tables (created if absent; data loaded on first fetch)
        m_configDb.CreateEphTables();

        // Audio file duration cache — create table, pre-load existing durations, start scan
        m_configDb.CreateAudioFilesTable();
        { std::lock_guard lk(m_audioDurMutex);
          m_audioDurCache = m_configDb.LoadAudioFileDurs(); }
        ScanAudioFilesAsync();

        // Camera config — create table and load previously registered cameras
        m_configDb.CreateCamConfigTable();
        m_camConfigs = m_configDb.LoadCamConfigs();
        m_showCamCfgWnd.assign(m_camConfigs.size(), false);

        // Live view overlay — per-camera opacity; enabled when >= 5%
        for (int ci = 0; ci < kMaxCamTracks; ++ci) {
            m_lvOpacity[ci] = m_configDb.GetSettingInt(
                std::format("lv_opacity_pct_{}", ci).c_str(), 0) / 100.f;
            m_lvEnabled[ci] = m_lvOpacity[ci] >= 0.05f;
        }
        // Thread started lazily when user enables first camera in Inspector

        // Load saved timeline (overrides InitTracks defaults when data exists)
        auto saved = m_configDb.LoadTimeline();
        if (!saved.empty()) {
            m_tracks = std::move(saved);
            LogLine(std::format("Timeline loaded: {} tracks from DB", m_tracks.size()));
        }

        // Seed calibration snapshot once (idempotent)
        CreateCalibrationSnapshot();
    } else {
        LogLine("WARNING: cannot open TotalControlConfig.db — settings not persisted");
    }

    // ── TotalControlData.db — reference data (read-only, optional) ────────
    std::wstring dataPath = dir + L"\\TotalControlData.db";
    if (std::filesystem::exists(dataPath)) {
        if (m_dataDb.OpenReadOnly(dataPath)) {
            m_tzList   = m_dataDb.LoadTimezones();
            m_eclipses = m_dataDb.LoadEclipses();
            LogLine(std::format("Data DB: {} timezones, {} eclipses loaded",
                                m_tzList.size(), m_eclipses.size()));
        } else {
            LogLine("WARNING: TotalControlData.db found but could not be opened");
        }
    } else {
        LogLine("Data DB: TotalControlData.db not found — skipped");
    }

    // Fallback: if DB had no timezones, use hardcoded list
    if (m_tzList.empty()) {
        m_tzList = TzFallbackList();
        LogLine(std::format("Timezones: using fallback ({} entries)", m_tzList.size()));
    }

    // Default eclipse: nearest future (UTC date comparison)
    if (!m_eclipses.empty()) {
        SYSTEMTIME st{};
        GetSystemTime(&st);
        int today = st.wYear * 10000 + st.wMonth * 100 + st.wDay;
        m_eclipseIdx = static_cast<int>(m_eclipses.size()) - 1;
        for (int i = 0; i < static_cast<int>(m_eclipses.size()); ++i) {
            if (m_eclipses[i].DateInt() >= today) { m_eclipseIdx = i; break; }
        }
        const auto& e = m_eclipses[m_eclipseIdx];
        LogLine(std::format("Default eclipse: {}-{:02d}-{:02d} {}",
                            e.year, e.month, e.day, e.type));
    }

    InitTracks();
}

App::~App() {
    // Stop sequencer first so it releases the pipe before we quit
    if (m_seqRun.load()) StopSeqThread();
    m_guiSeqMode.store(GuiSeqMode::Idle);

    StopStatusThread();
    StopLvThread();
    for (int ci = 0; ci < kMaxCamTracks; ++ci)
        if (m_lvSrv[ci]) { m_lvSrv[ci]->Release(); m_lvSrv[ci] = nullptr; }

    if (m_iqpThread.joinable())       m_iqpThread.join();
    if (m_ephThread.joinable())       m_ephThread.join();
    if (m_suviThread.joinable())       m_suviThread.join();
    if (m_audioScanThread.joinable()) m_audioScanThread.join();
    if (m_geoThread.joinable())       m_geoThread.join();
    if (m_moonThread.joinable())      m_moonThread.join();

    for (auto* srv : m_suviSrvs) if (srv) srv->Release();
    m_suviSrvs.clear();
    if (m_moonSrv) { m_moonSrv->Release(); m_moonSrv = nullptr; }

    if (m_pipe.GetState() == PipeClient::State::Connected) {
        (void)m_pipe.Send("{\"cmd\":\"quit\"}");
        m_pipe.Disconnect();
    }
    LogLine("=== TotalControlGUI exit ===");
}

void App::ApplyStyleDark() {
    ImGui::StyleColorsDark();   // baseline reset
    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text]                      = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]              = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    c[ImGuiCol_WindowBg]                  = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);
    c[ImGuiCol_ChildBg]                   = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg]                   = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
    c[ImGuiCol_Border]                    = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    c[ImGuiCol_BorderShadow]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]                   = ImVec4(0.16f, 0.29f, 0.48f, 0.54f);
    c[ImGuiCol_FrameBgHovered]            = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
    c[ImGuiCol_FrameBgActive]             = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    c[ImGuiCol_TitleBg]                   = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
    c[ImGuiCol_TitleBgActive]             = ImVec4(0.16f, 0.29f, 0.48f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]          = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    c[ImGuiCol_MenuBarBg]                 = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    c[ImGuiCol_ScrollbarBg]               = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    c[ImGuiCol_ScrollbarGrab]             = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]      = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]       = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    c[ImGuiCol_CheckMark]                 = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_SliderGrab]                = ImVec4(0.24f, 0.52f, 0.88f, 1.00f);
    c[ImGuiCol_SliderGrabActive]          = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_Button]                    = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
    c[ImGuiCol_ButtonHovered]             = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_ButtonActive]              = ImVec4(0.06f, 0.53f, 0.98f, 1.00f);
    c[ImGuiCol_Header]                    = ImVec4(0.26f, 0.59f, 0.98f, 0.31f);
    c[ImGuiCol_HeaderHovered]             = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    c[ImGuiCol_HeaderActive]              = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_Separator]                 = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    c[ImGuiCol_SeparatorHovered]          = ImVec4(0.10f, 0.40f, 0.75f, 0.78f);
    c[ImGuiCol_SeparatorActive]           = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
    c[ImGuiCol_ResizeGrip]                = ImVec4(0.26f, 0.59f, 0.98f, 0.20f);
    c[ImGuiCol_ResizeGripHovered]         = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    c[ImGuiCol_ResizeGripActive]          = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
    c[ImGuiCol_TabHovered]                = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    c[ImGuiCol_Tab]                       = ImVec4(0.18f, 0.35f, 0.58f, 0.86f);
    c[ImGuiCol_TabSelected]               = ImVec4(0.20f, 0.41f, 0.68f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]       = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_TabDimmed]                 = ImVec4(0.07f, 0.10f, 0.15f, 0.97f);
    c[ImGuiCol_TabDimmedSelected]         = ImVec4(0.14f, 0.26f, 0.42f, 1.00f);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.50f, 0.50f, 0.50f, 0.00f);
    c[ImGuiCol_PlotLines]                 = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]          = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    c[ImGuiCol_PlotHistogram]             = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered]      = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TableHeaderBg]             = ImVec4(0.19f, 0.19f, 0.20f, 1.00f);
    c[ImGuiCol_TableBorderStrong]         = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
    c[ImGuiCol_TableBorderLight]          = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
    c[ImGuiCol_TableRowBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]             = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    c[ImGuiCol_TextLink]                  = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_TextSelectedBg]            = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
    c[ImGuiCol_DragDropTarget]            = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    c[ImGuiCol_NavCursor]                 = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight]     = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]         = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]          = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
}

void App::OnInit(ID3D11Device* d3dDev, ID3D11DeviceContext* d3dCtx) {
    assert(d3dDev != nullptr);
    assert(d3dCtx != nullptr);
    m_d3dDev = d3dDev;
    m_d3dCtx = d3dCtx;

    ImGuiIO& io = ImGui::GetIO();
    m_fontMono  = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 15.0f);
    m_fontLarge = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 24.0f);
    if (!m_fontMono)  m_fontMono  = io.FontDefault;
    if (!m_fontLarge) m_fontLarge = io.FontDefault;
    io.Fonts->Build();

    ApplyStyleDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding   = 0.0f;
    style.FrameRounding    = 3.0f;
    style.FramePadding     = ImVec2(8, 5);
    style.ItemSpacing      = ImVec2(8, 6);
    style.WindowBorderSize = 1.0f;
    style.WindowPadding    = ImVec2(12, 12);

    TriggerSuviFetch();
    TriggerMoonFetch();
    StartStatusThread();
}

// ─── Extra clock row ──────────────────────────────────────────────────────────
//
// Panel layout (content width ~176px):
//
//   HH:MM:SS            [=]   ← coloured time  +  gear button right-aligned
//   [✓]  CODE                 ← checkbox  +  IANA code tag

void App::RenderExtraClock(const char* clockId, const char* popupId,
                            bool& show, std::string& tzIana) {
    // Find current list entry for this IANA name
    int curIdx = TzFindByIana(m_tzList, tzIana);
    const TzEntry& cur = m_tzList[curIdx];

    ImGuiStyle& style = ImGui::GetStyle();

    // ── row 1: time + gear button ─────────────────────────────────────────
    ImGui::PushFont(m_fontMono);

    char timeBuf[12];
    FormatLocalHms(UtcNowMs(), tzIana, timeBuf, sizeof(timeBuf));

    static const ImVec4 kColorHome{ 0.45f, 0.75f, 1.00f, 1.0f };  // soft blue
    static const ImVec4 kColorEcl { 0.40f, 1.00f, 0.80f, 1.0f };  // cyan-green
    ImVec4 timeColor = (clockId[0] == 'H') ? kColorHome : kColorEcl;

    if (!show) {
        ImVec4 dim = timeColor;  dim.w = 0.35f;
        ImGui::TextColored(dim, "%s", timeBuf);
    } else {
        ImGui::TextColored(timeColor, "%s", timeBuf);
    }

    // Gear button — right-aligned on the same line
    {
        char gearId[48];
        snprintf(gearId, sizeof(gearId), "=##gear_%s", popupId);
        float gearW = ImGui::CalcTextSize("=").x
                    + style.FramePadding.x * 2.0f
                    + style.ItemSpacing.x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - gearW + style.ItemSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.18f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.32f, 0.48f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.55f, 0.65f, 0.85f, 1.0f));
        if (ImGui::SmallButton(gearId))
            ImGui::OpenPopup(popupId);
        ImGui::PopStyleColor(3);
    }

    ImGui::PopFont();

    // ── row 2: checkbox + full label (e.g. "Home Time Zone") ────────────────
    bool prevShow = show;
    ImGui::Checkbox(clockId, &show);
    if (show != prevShow) SaveClockSettings();

    // ── row 3: city name — indented to align with checkbox label ─────────
    {
        float indent = ImGui::GetFrameHeight()
                     + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::Indent(indent);
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(ImVec4(0.38f, 0.38f, 0.44f, 1.0f), "%s", cur.code.c_str());
        ImGui::PopFont();
        ImGui::Unindent(indent);
    }

    // ── timezone picker popup ─────────────────────────────────────────────
    ImGui::SetNextWindowSizeConstraints(ImVec2(420, 0), ImVec2(420, 520));
    if (ImGui::BeginPopup(popupId)) {
        ImGui::TextColored(ImVec4(0.55f, 0.65f, 0.85f, 1.0f), "Select timezone");
        ImGui::Separator();
        ImGui::Spacing();

        for (int i = 0; i < static_cast<int>(m_tzList.size()); ++i) {
            bool sel = (i == curIdx);
            char entry[80];
            snprintf(entry, sizeof(entry), "%-22s  %s",
                     m_tzList[i].code.c_str(),
                     m_tzList[i].iana.c_str());
            if (ImGui::Selectable(entry, sel)) {
                tzIana = m_tzList[i].iana;
                SaveClockSettings();
                ImGui::CloseCurrentPopup();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndPopup();
    }
}

// ─── Camera status rendering ──────────────────────────────────────────────────

void App::RenderCameraSection() {
    static const ImVec4 kGray   { 0.40f, 0.40f, 0.45f, 1.0f };
    static const ImVec4 kWhite  { 0.88f, 0.88f, 0.90f, 1.0f };
    static const ImVec4 kGreen  { 0.20f, 0.85f, 0.30f, 1.0f };
    static const ImVec4 kYellow { 0.95f, 0.80f, 0.10f, 1.0f };
    static const ImVec4 kOrange { 1.00f, 0.55f, 0.10f, 1.0f };
    static const ImVec4 kRed    { 0.90f, 0.20f, 0.18f, 1.0f };
    static const ImVec4 kDim    { 0.35f, 0.35f, 0.40f, 1.0f };

    // Snapshot cameras under mutex — render thread must not hold mutex during drawing
    std::vector<CamStatus> cameras;
    {
        std::lock_guard lk(m_camerasMutex);
        cameras = m_cameras;
    }

    static_assert(kMaxCamTracks == 4, "camera table is hardcoded to 4 columns");
    auto CamAt = [&](int ci) -> const CamStatus* {
        return (ci < static_cast<int>(cameras.size())) ? &cameras[ci] : nullptr;
    };

    ImGui::PushFont(m_fontMono);
    if (ImGui::BeginTable("##cam_tbl", 5,
                           ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_SizingFixedFit)) {
        // Column names are set fresh every frame, so the header bar can show
        // the live model name (or "Cam N") — same real header-bar look as
        // the contact-times table (IQP/BE/Loc/GE/T-).
        ImGui::TableSetupColumn("Cam", ImGuiTableColumnFlags_WidthFixed, 42.f);
        char hdrBuf[kMaxCamTracks][24];
        for (int ci = 0; ci < kMaxCamTracks; ++ci) {
            const CamStatus* s = CamAt(ci);
            if (s && !s->model.empty()) snprintf(hdrBuf[ci], sizeof(hdrBuf[ci]), "%s", s->model.c_str());
            else                        snprintf(hdrBuf[ci], sizeof(hdrBuf[ci]), "Cam %d", ci + 1);
            ImGui::TableSetupColumn(hdrBuf[ci], ImGuiTableColumnFlags_WidthStretch);
        }

        // ── Header row: real ImGui header bar (TableHeadersRow style) ──
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
        ImGui::TableHeader(ImGui::TableGetColumnName(0));
        ImGui::PopStyleColor();
        for (int ci = 0; ci < kMaxCamTracks; ++ci) {
            ImGui::TableSetColumnIndex(ci + 1);
            ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
            ImGui::TableHeader(ImGui::TableGetColumnName(ci + 1));
            ImGui::PopStyleColor();
        }

        // Renders one parameter row across all 4 camera columns; getValue maps
        // a connected camera's status to (display text, colour).
        auto Row = [&](const char* label, auto&& getValue) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kGray, "%s", label);
            for (int ci = 0; ci < kMaxCamTracks; ++ci) {
                ImGui::TableSetColumnIndex(ci + 1);
                const CamStatus* s = CamAt(ci);
                if (!s || !s->valid) { ImGui::TextColored(kDim, "-"); continue; }
                auto [val, col] = getValue(*s);
                ImGui::TextColored(col, "%s", val.c_str());
            }
        };

        Row("Batt", [&](const CamStatus& s) -> std::pair<std::string, ImVec4> {
            ImVec4 batCol = kGreen;
            std::string_view lvl = s.batteryLevel;
            if      (lvl.find("1/4") != std::string_view::npos) { batCol = kRed;    }
            else if (lvl.find("1/2") != std::string_view::npos) { batCol = kOrange; }
            else if (lvl.find("3/4") != std::string_view::npos) { batCol = kYellow; }
            bool usb = lvl.find("usb") != std::string_view::npos;
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%%s", s.batteryPct, usb ? " U" : "");
            return { buf, batCol };
        });

        Row("Mode",  [&](const CamStatus& s) { return std::pair{ s.mode.empty()  ? "?" : s.mode,  kWhite }; });
        Row("SS",    [&](const CamStatus& s) { return std::pair{ s.ss.empty()    ? "?" : s.ss,    kWhite }; });
        Row("ISO",   [&](const CamStatus& s) { return std::pair{ s.iso ? std::to_string(s.iso) : "?", kWhite }; });
        Row("f/",    [&](const CamStatus& s) { return std::pair{ s.fnum.empty()  ? "?" : s.fnum,  kWhite }; });
        Row("Focus", [&](const CamStatus& s) { return std::pair{ s.focus.empty() ? "?" : s.focus, kWhite }; });
        Row("Drive", [&](const CamStatus& s) { return std::pair{ s.drive.empty() ? "?" : s.drive, kWhite }; });

        // Cards — remaining/max (shots) and % free when max is known
        auto CardVal = [&](const std::string& status, int remaining, int maxRem)
                -> std::pair<std::string, ImVec4> {
            char buf[48];
            if (remaining > 0 && maxRem > 0) {
                int pct = remaining * 100 / maxRem;
                snprintf(buf, sizeof(buf), "%d/%d %d%%", remaining, maxRem, pct);
            } else if (remaining > 0) {
                snprintf(buf, sizeof(buf), "%d shots", remaining);
            } else if (status == "ok") {
                snprintf(buf, sizeof(buf), "reading...");
            } else {
                snprintf(buf, sizeof(buf), "%s", status.c_str());
            }
            ImVec4 col = (status == "ok") ? kGreen : kRed;
            return { buf, col };
        };
        Row("C1", [&](const CamStatus& s) { return CardVal(s.slot1Status, s.slot1Remaining, s.slot1MaxRem); });
        Row("C2", [&](const CamStatus& s) { return CardVal(s.slot2Status, s.slot2Remaining, s.slot2MaxRem); });

        // Last shoot latency — full stack (SDK + USB + shutter + confirm)
        Row("Shot", [&](const CamStatus& s) -> std::pair<std::string, ImVec4> {
            if (s.lastShotMs < 0) return { "-", kDim };
            char buf[16]; snprintf(buf, sizeof(buf), "%d ms", s.lastShotMs);
            ImVec4 col = s.lastShotMs < 500 ? kGreen : s.lastShotMs < 1500 ? kYellow : kRed;
            return { buf, col };
        });

        ImGui::EndTable();
    }
    ImGui::PopFont();

    if (cameras.empty())
        ImGui::TextColored(kGray, "no cameras connected");
}

// ─── Eclipse time helpers ─────────────────────────────────────────────────────

// Formats a signed millisecond duration as "Xd HH:MM:SS.mmm" (negative = past).
static void FormatCountdown(int64_t diffMs, char* buf, int len) {
    bool neg   = diffMs < 0;
    int64_t a  = neg ? -diffMs : diffMs;
    int days   = static_cast<int>(a / 86400000LL);
    int hh     = static_cast<int>((a % 86400000LL) / 3600000LL);
    int mm     = static_cast<int>((a % 3600000LL)  / 60000LL);
    int ss     = static_cast<int>((a % 60000LL)    / 1000LL);
    int ms     = static_cast<int>(a % 1000LL);
    if (neg)
        snprintf(buf, len, "-%dd %02d:%02d:%02d.%03d", days, hh, mm, ss, ms);
    else
        snprintf(buf, len, "%dd %02d:%02d:%02d.%03d",  days, hh, mm, ss, ms);
}

// ─── Eclipse selector rendering ───────────────────────────────────────────────

void App::RenderEclipseSection() {
    static const ImVec4 kGray  { 0.40f, 0.40f, 0.45f, 1.0f };
    static const ImVec4 kWhite { 0.88f, 0.88f, 0.90f, 1.0f };
    static const ImVec4 kBlueBg{ 0.16f, 0.29f, 0.48f, 0.54f };

    if (m_eclipses.empty()) {
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(kGray, "no data");
        ImGui::PopFont();
        return;
    }

    ImGui::PushFont(m_fontMono);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 2.f));

    if (ImGui::BeginTable("##ecl_tbl", 4,
                           ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Date",        ImGuiTableColumnFlags_WidthFixed, 110.f);
        ImGui::TableSetupColumn("Type",        ImGuiTableColumnFlags_WidthFixed,  70.f);
        ImGui::TableSetupColumn("Duration",    ImGuiTableColumnFlags_WidthFixed,  90.f);
        ImGui::TableSetupColumn("GE location", ImGuiTableColumnFlags_WidthFixed, 140.f);

        // ── Header row: real ImGui header bar — same style as ##cam_tbl/##ct_tbl ──
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int ci = 0; ci < 4; ++ci) {
            ImGui::TableSetColumnIndex(ci);
            ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
            ImGui::TableHeader(ImGui::TableGetColumnName(ci));
            ImGui::PopStyleColor();
        }

        ImGui::TableNextRow();

        // ── Col 0: date-only dropdown ────────────────────────────────────────
        ImGui::TableSetColumnIndex(0);
        char preview[12] = "---";
        if (m_eclipseIdx >= 0 && m_eclipseIdx < static_cast<int>(m_eclipses.size())) {
            const auto& e = m_eclipses[m_eclipseIdx];
            snprintf(preview, sizeof(preview), "%04d-%02d-%02d", e.year, e.month, e.day);
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##ecl_sel", preview, ImGuiComboFlags_HeightLarge)) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_eclipses.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto& e = m_eclipses[i];
                    char lbl[12];
                    snprintf(lbl, sizeof(lbl), "%04d-%02d-%02d", e.year, e.month, e.day);
                    bool sel = (i == m_eclipseIdx);
                    if (ImGui::Selectable(lbl, sel)) {
                        m_eclipseIdx = i;
                        // Auto-load GE location into observer fields
                        m_obsLat = m_eclipses[i].latGe;
                        m_obsLon = m_eclipses[i].lonGe;
                        SyncDecimalToDms();
                        SaveObserverSettings();
                        TriggerIqpFetch();
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        // Remaining columns need a valid selection
        if (m_eclipseIdx < 0 || m_eclipseIdx >= static_cast<int>(m_eclipses.size())) {
            ImGui::EndTable();
            ImGui::PopStyleVar();
            ImGui::PopFont();
            return;
        }

        const auto& e = m_eclipses[m_eclipseIdx];
        char tc = e.type.empty() ? '?' : e.type[0];

        const char* typeName = "Unknown";
        ImVec4 typeCol = kWhite;
        switch (tc) {
        case 'T': typeName = "Total";   typeCol = ImVec4{0.95f, 0.80f, 0.20f, 1.0f}; break;
        case 'A': typeName = "Annular"; typeCol = ImVec4{0.85f, 0.50f, 0.15f, 1.0f}; break;
        case 'H': typeName = "Hybrid";  typeCol = ImVec4{0.80f, 0.75f, 0.20f, 1.0f}; break;
        case 'P': typeName = "Partial"; typeCol = kGray;                               break;
        }

        // ── Col 1: Type ───────────────────────────────────────────────────────
        ImGui::TableSetColumnIndex(1);
        {
            char buf[16]; snprintf(buf, sizeof(buf), "%s", typeName);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, kBlueBg);
            ImGui::PushStyleColor(ImGuiCol_Text, typeCol);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ecl_type", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor(2);
        }

        // ── Col 2: Duration at GE ────────────────────────────────────────────
        ImGui::TableSetColumnIndex(2);
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "%s", e.duration.empty() ? "--" : e.duration.c_str());
            ImGui::PushStyleColor(ImGuiCol_FrameBg, kBlueBg);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ecl_dur", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();
        }

        // ── Col 3: GE location ───────────────────────────────────────────────
        ImGui::TableSetColumnIndex(3);
        {
            char latBuf[10], lonBuf[10], coordBuf[24];
            snprintf(latBuf,   sizeof(latBuf),   "%.1f%c", fabsf(e.latGe), e.latGe >= 0.f ? 'N' : 'S');
            snprintf(lonBuf,   sizeof(lonBuf),   "%.1f%c", fabsf(e.lonGe), e.lonGe >= 0.f ? 'E' : 'W');
            snprintf(coordBuf, sizeof(coordBuf), "%s %s",  latBuf, lonBuf);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, kBlueBg);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ecl_geo", coordBuf, sizeof(coordBuf), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();
        }

        ImGui::EndTable();
    }

    ImGui::PopStyleVar();
    ImGui::PopFont();
}

// ─── Contact times table ─────────────────────────────────────────────────────

static int LocalUtcOffsetHours(const std::string& iana, int year, int month, int day) {
    using namespace std::chrono;
    try {
        const time_zone* tz = locate_zone(iana);
        auto ymd = year_month_day{ std::chrono::year(year),
                                   std::chrono::month(static_cast<unsigned>(month)),
                                   std::chrono::day(static_cast<unsigned>(day)) };
        sys_info info = tz->get_info(sys_days{ymd} + hours{12});
        return static_cast<int>(info.offset.count() / 3600);
    } catch (...) { return 0; }
}

void App::RenderContactTimesSection() {
    static const ImVec4 kGray  {0.40f, 0.40f, 0.45f, 1.0f};
    static const ImVec4 kWhite {0.88f, 0.88f, 0.90f, 1.0f};
    static const ImVec4 kDim   {0.35f, 0.35f, 0.40f, 1.0f};
    static const ImVec4 kGold  {0.95f, 0.80f, 0.20f, 1.0f};

    // Compute local timezone offset for eclipse date
    int offH = 0;
    if (m_eclipseIdx >= 0 && m_eclipseIdx < static_cast<int>(m_eclipses.size())) {
        const auto& ec = m_eclipses[m_eclipseIdx];
        offH = LocalUtcOffsetHours(m_eclTzIana, ec.year, ec.month, ec.day);
    }
    char offLabel[10];
    if (offH > 0)       snprintf(offLabel, sizeof(offLabel), "UTC+%d", offH);
    else if (offH < 0)  snprintf(offLabel, sizeof(offLabel), "UTC%d",  offH);
    else                snprintf(offLabel, sizeof(offLabel), "UTC");

    // Column offsets within content area
    const float kT1  = 26.f;  // UTC time column
    const float kT2  = 98.f;  // local time column

    auto fmtUtc = [](int64_t ms, char* buf, int len) {
        if (ms < 0) { snprintf(buf, len, "--:--:--"); return; }
        int64_t s = ms / 1000;
        snprintf(buf, len, "%02d:%02d:%02d",
                 (int)((s/3600)%24), (int)((s/60)%60), (int)(s%60));
    };
    auto fmtLoc = [&](int64_t ms, char* buf, int len) {
        if (ms < 0) { snprintf(buf, len, "--:--:--"); return; }
        // shift by offset
        int64_t shifted = ms + int64_t(offH) * 3600000LL;
        int64_t s = shifted / 1000;
        snprintf(buf, len, "%02d:%02d:%02d",
                 (int)((s/3600)%24), (int)((s/60)%60), (int)(s%60));
    };

    struct EventRow { const char* lbl; int64_t ms; };

    auto renderSection = [&](const char* srcLabel, const ContactTimes& ct,
                              bool loading, ImVec4 srcCol) {
        // header line
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(srcCol,  "%-3s", srcLabel);
        ImGui::SameLine(kT1);
        ImGui::TextColored(kGray, "UTC");
        ImGui::SameLine(kT2);
        ImGui::TextColored(kGray, "%s", offLabel);
        ImGui::PopFont();

        ImGui::PushFont(m_fontMono);
        if (loading) {
            ImGui::SameLine(); // dummy to keep spacing
            ImGui::NewLine();
            ImGui::TextColored(kDim, "  loading...");
            ImGui::PopFont();
            return;
        }
        if (!ct.apiOk && !ct.valid) {
            ImGui::TextColored(kDim, "  ---");
            ImGui::PopFont();
            return;
        }
        if (!ct.valid) {
            ImGui::TextColored(kDim, "  no eclipse");
            ImGui::PopFont();
            return;
        }

        EventRow rows[] = {
            {"C1",  ct.c1Ms},
            {"C2",  ct.c2Ms},
            {"Max", ct.maxMs},
            {"C3",  ct.c3Ms},
            {"C4",  ct.c4Ms},
        };
        for (auto& r : rows) {
            if (r.ms < 0) continue;
            char t1[12], t2[12];
            fmtUtc(r.ms, t1, sizeof(t1));
            fmtLoc(r.ms, t2, sizeof(t2));
            ImGui::TextColored(kGray,  "%s", r.lbl);
            ImGui::SameLine(kT1);
            ImGui::TextColored(kWhite, "%s", t1);
            ImGui::SameLine(kT2);
            ImGui::TextColored(kDim,   "%s", t2);
        }
        ImGui::PopFont();
    };

    int iqpSt = m_iqpState.load();
    ContactTimes iqpCt;
    { std::lock_guard lk(m_iqpMutex); iqpCt = m_contacts; }

    const char* iqpLabel = (iqpCt.source == ContactSource::BesselApi) ? "IQP API" : "IQP";
    renderSection(iqpLabel, iqpCt, iqpSt == 1,
                  ImVec4{0.45f, 0.75f, 1.00f, 1.0f});   // blue
    ImGui::Spacing();
    renderSection("BE",  m_beResult, false,
                  ImVec4{0.40f, 1.00f, 0.60f, 1.0f});   // green
}

// ─── Sunrise / Sunset from EPH ───────────────────────────────────────────────

int64_t App::FindSunAltCrossing(bool findRise) const {
    assert(static_cast<size_t>(EphBody::Sun) < m_ephSamples.size());
    std::lock_guard lk(m_ephMutex);
    const auto& rows = m_ephSamples[static_cast<size_t>(EphBody::Sun)];
    if (rows.size() < 2) return -1;
    assert(rows.size() <= 2000); // bounded: JPL Horizons returns ≤1440 samples/day
    for (size_t i = 1; i < rows.size(); ++i) {
        double prev = rows[i-1].alt_deg;
        double curr = rows[i].alt_deg;
        bool match = findRise ? (prev < 0.0 && curr >= 0.0) : (prev >= 0.0 && curr < 0.0);
        if (match) {
            double t = prev / (prev - curr);
            return rows[i-1].utc_ms
                 + static_cast<int64_t>(t * double(rows[i].utc_ms - rows[i-1].utc_ms));
        }
    }
    return -1;
}

// ─── Location section (observer DMS + totality status) ───────────────────────

void App::RenderLocationSection() {
    assert(m_fontMono != nullptr);
    static const ImVec4 kGray  {0.40f, 0.40f, 0.45f, 1.0f};
    static const ImVec4 kGreen2{0.15f, 0.90f, 0.35f, 1.0f};
    static const ImVec4 kRed2  {0.95f, 0.22f, 0.18f, 1.0f};
    static const ImVec4 kDim3  {0.40f, 0.40f, 0.45f, 1.0f};
    static const ImVec4 kWhite2{0.80f, 0.85f, 0.90f, 1.0f};

    ImGui::SeparatorText("Time and Location");
    ImGui::PushFont(m_fontMono);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.f, 3.f));

    // Seconds are split into whole + hundredths as two separate fields joined
    // by a literal "." — mirrors the deg/min layout instead of one merged
    // float box (previously "6.34" was a single editable field).
    auto DmsRow = [&](const char* label, DmsCoord& dms, int maxDeg,
                      const char* posLabel, const char* negLabel,
                      const char* idD, const char* idM,
                      const char* idSW, const char* idSF) {
        bool t = false;
        int secW = static_cast<int>(dms.sec);
        int secF = static_cast<int>(std::lround((dms.sec - static_cast<float>(secW)) * 100.f));
        if (secF >= 100) { secF = 0; ++secW; }

        ImGui::SetNextItemWidth(30); ImGui::InputInt(idD, &dms.deg, 0);
        t |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine(0, 1); ImGui::TextColored(kGray, "\xc2\xb0");
        ImGui::SameLine(0, 2); ImGui::SetNextItemWidth(22); ImGui::InputInt(idM, &dms.min, 0);
        t |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine(0, 1); ImGui::TextColored(kGray, "'");
        ImGui::SameLine(0, 2); ImGui::SetNextItemWidth(22); ImGui::InputInt(idSW, &secW, 0);
        t |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine(0, 1); ImGui::TextColored(kGray, ".");
        ImGui::SameLine(0, 1); ImGui::SetNextItemWidth(24); ImGui::InputInt(idSF, &secF, 0);
        t |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine(0, 1); ImGui::TextColored(kGray, "\"");
        ImGui::SameLine(0, 2);
        if (ImGui::Button(dms.pos ? posLabel : negLabel, ImVec2(20.f, 0.f))) {
            dms.pos = !dms.pos; t = true;
        }
        ImGui::SameLine(0, 6); ImGui::TextColored(kGray, "%s", label);
        if (t) {
            dms.deg = std::clamp(dms.deg, 0, maxDeg);
            dms.min = std::clamp(dms.min, 0, 59);
            secW    = std::clamp(secW, 0, 59);
            secF    = std::clamp(secF, 0, 99);
            dms.sec = static_cast<float>(secW) + static_cast<float>(secF) / 100.f;
            SyncDmsToDecimal();
            SaveObserverSettings();
        }
    };

    // One coordinate per line, trailing label — Latitude, then Longitude.
    DmsRow("Latitude",  m_latDms, 90,  "N##latn", "S##latn", "##latd", "##latm", "##latsw", "##latsf");
    DmsRow("Longitude", m_lonDms, 180, "E##lone", "W##lone", "##lond", "##lonm", "##lonsw", "##lonsf");

    ImGui::SetNextItemWidth(100.f);
    ImGui::InputInt("##obs_alt", &m_obsAltM, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveObserverSettings();
    ImGui::SameLine(0, 6); ImGui::TextColored(kGray, "Altitude");

    ImGui::PopStyleVar();
    ImGui::PopFont();

    // ── Consume background elevation+geocode result (if ready) ──────────────
    {
        int geoSt = m_geoState.load();
        if (geoSt == 2 || geoSt == 3) {
            std::string pendingName;
            double elevM = 0.0;
            { std::lock_guard lk(m_geoMutex);
              pendingName = m_geoNamePending; elevM = m_geoElevM; }
            if (!pendingName.empty()) {
                m_obsName = pendingName;
                SaveObserverSettings();
            }

            if (geoSt == 2) {
                m_obsAltM = static_cast<int>(std::lround(elevM));
                SaveObserverSettings();
                m_gmapsStatusMsg.clear();
                m_gmapsStatusIsErr = false;
            } else {
                m_gmapsStatusMsg   = "Wysokosc: blad pobierania (Open-Elevation) - ustaw recznie";
                m_gmapsStatusIsErr = true;
            }
            m_geoState.store(0);
        }
    }

    // ── Set Location from Google Maps URL ────────────────────────────────────
    ImGui::Spacing();
    ImGui::PushFont(m_fontMono);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##gmaps_url", "Paste Google Maps URL",
                              m_gmapsUrlBuf, sizeof(m_gmapsUrlBuf));

    // One button does both: parse the URL (if given) then calculate contact
    // times for the resulting (or unchanged) location. Turns green (same
    // style as "Camera connected") once contacts have been computed.
    bool geoLoading = (m_geoState.load() == 1);
    bool ctLoading  = (m_iqpState.load() == 1);
    bool busy       = geoLoading || ctLoading;
    bool ok         = m_beResult.valid;
    if (ok) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f, 0.55f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.65f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.08f, 0.45f, 0.14f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f,  1.0f,  1.0f,  1.0f));
    }
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button(ctLoading ? "Calculating..." : "SET LOCATION & CALCULATE", ImVec2(-1, 0)))
        ApplyLocationAndCalculate();
    if (busy) ImGui::EndDisabled();
    if (ok) ImGui::PopStyleColor(4);

    // Detected place name — no label, centered, wrapped over up to 2 lines.
    if (!m_obsName.empty()) {
        float avail = ImGui::GetContentRegionAvail().x;
        std::string line1 = m_obsName, line2;

        if (ImGui::CalcTextSize(line1.c_str()).x > avail) {
            size_t splitPos = line1.size();
            while (splitPos > 0 &&
                   ImGui::CalcTextSize(line1.substr(0, splitPos).c_str()).x > avail)
                --splitPos;
            size_t spacePos = line1.rfind(' ', splitPos);
            if (spacePos != std::string::npos && spacePos > 0) splitPos = spacePos;

            line2 = line1.substr(splitPos);
            line1 = line1.substr(0, splitPos);
            size_t s = line2.find_first_not_of(' ');
            line2 = (s == std::string::npos) ? "" : line2.substr(s);

            bool truncated2 = false;
            while (line2.size() > 1 && ImGui::CalcTextSize(line2.c_str()).x > avail) {
                line2.resize(line2.size() - 1);
                truncated2 = true;
            }
            if (truncated2 && line2.size() > 3) line2.replace(line2.size() - 3, 3, "...");
        }

        auto centeredLine = [&](const std::string& s) {
            if (s.empty()) return;
            float tw  = ImGui::CalcTextSize(s.c_str()).x;
            float cur = ImGui::GetCursorPosX();
            ImGui::SetCursorPosX(cur + std::max(0.f, (avail - tw) * 0.5f));
            ImGui::TextColored(kWhite2, "%s", s.c_str());
        };
        centeredLine(line1);
        centeredLine(line2);
    }

    if (!m_gmapsStatusMsg.empty()) {
        ImVec4 col = m_gmapsStatusIsErr ? ImVec4(0.95f, 0.30f, 0.25f, 1.f)
                                        : ImVec4(0.55f, 0.85f, 0.55f, 1.f);
        ImGui::TextColored(col, "%s", m_gmapsStatusMsg.c_str());
    }
    ImGui::PopFont();

    // Totality zone status (moved from Col2 Calculate button area)
    ImGui::Spacing();
    {
        int iqpSt = m_iqpState.load();
        if (iqpSt == 2 || iqpSt == 3) {
            ContactTimes ct;
            { std::lock_guard lk(m_iqpMutex); ct = m_contacts; }
            ImGui::PushFont(m_fontMono);
            const char* msg =
                !ct.apiOk    ? "network error"    :
                !ct.valid    ? "YOU ARE NOT IN TOTALITY ZONE!" :
                ct.c2Ms > 0 ? "YOU ARE IN TOTALITY ZONE!" : "YOU ARE NOT IN TOTALITY ZONE!";
            const ImVec4& col =
                !ct.apiOk    ? kDim3   :
                !ct.valid    ? kRed2   :
                ct.c2Ms > 0 ? kGreen2 : kRed2;
            float textW = ImGui::CalcTextSize(msg).x;
            float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - textW) * 0.5f);
            ImGui::TextColored(col, "%s", msg);
            ImGui::PopFont();
        }
    }
}

// ─── Time section (3 clocks + contact comparison table + countdowns) ──────────

void App::RenderTimeSection() {
    assert(m_fontMono != nullptr);
    static const ImVec4 kGray {0.40f, 0.40f, 0.45f, 1.0f};
    static const ImVec4 kDim  {0.28f, 0.28f, 0.32f, 1.0f};
    static const ImVec4 kGold {0.95f, 0.80f, 0.20f, 1.0f};
    static const ImVec4 kBlue {0.45f, 0.75f, 1.00f, 1.0f};
    static const ImVec4 kCyan {0.40f, 1.00f, 0.80f, 1.0f};

    // "Calculate contact times" moved next to "SET LOCATION" in
    // RenderLocationSection (now merged under the "Time and Location" header).

    int64_t nowMs = UtcNowMs();
    // UTC/Home/Loc clocks moved to the Sky View Simulator status bar
    // (right of "PH=") — more horizontal room there, and it keeps this
    // column short enough to avoid scrolling the timeline table below.

    ImGui::Spacing();

    // ── Contact comparison table: IQP | BE | Loc | GE ────────────────────
    int  iqpSt = m_iqpState.load();
    ContactTimes iqpCt;
    { std::lock_guard lk(m_iqpMutex); iqpCt = m_contacts; }

    int offH = 0;
    if (m_eclipseIdx >= 0 && m_eclipseIdx < static_cast<int>(m_eclipses.size())) {
        const auto& ec = m_eclipses[m_eclipseIdx];
        offH = LocalUtcOffsetHours(m_eclTzIana, ec.year, ec.month, ec.day);
    }

    int64_t sunriseMs = FindSunAltCrossing(true);
    int64_t sunsetMs  = FindSunAltCrossing(false);

    auto FmtHM = [](int64_t ms, char* buf, int len) {
        if (ms <= 0) { snprintf(buf, len, "--:--"); return; }
        int64_t s = ms / 1000;
        snprintf(buf, len, "%02d:%02d", (int)((s/3600)%24), (int)((s/60)%60));
    };
    auto FmtLoc = [&](int64_t ms, char* buf, int len) {
        int64_t shifted = (ms > 0) ? ms + int64_t(offH) * 3600000LL : -1;
        FmtHM(shifted, buf, len);
    };

    ImGui::PushFont(m_fontMono);
    constexpr float kLblW  = 34.f;
    // Sized to fit exactly "12:34" (4 digits + colon) — the widest string
    // FmtHM/FmtLoc ever produce for these four columns.
    const float kDataW = ImGui::CalcTextSize("12:34").x + 6.f;
    if (ImGui::BeginTable("##ct_tbl", 6,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit)) {
        constexpr float kCdW = 150.f;   // "Xd HH:MM:SS.mmm" — widest countdown string
        ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthFixed, kLblW);
        ImGui::TableSetupColumn("IQP", ImGuiTableColumnFlags_WidthFixed, kDataW);
        ImGui::TableSetupColumn("BE",  ImGuiTableColumnFlags_WidthFixed, kDataW);
        ImGui::TableSetupColumn("Loc", ImGuiTableColumnFlags_WidthFixed, kDataW);
        ImGui::TableSetupColumn("GE",  ImGuiTableColumnFlags_WidthFixed, kDataW);
        ImGui::TableSetupColumn("T-",  ImGuiTableColumnFlags_WidthFixed, kCdW);
        ImGui::TableHeadersRow();

        bool loading = (iqpSt == 1);
        // Countdown source: IQP when available, else BE — same fallback the
        // simulator status bar used before these countdowns moved here.
        const ContactTimes& cdSrc = iqpCt.valid ? iqpCt : m_beResult;
        struct CtRow { const char* lbl; int64_t iqp; int64_t be; int64_t ge; int64_t cd; };
        CtRow ctRows[] = {
            { "C1",  iqpCt.c1Ms,  m_beResult.c1Ms,  m_geResult.c1Ms,  cdSrc.c1Ms  },
            { "C2",  iqpCt.c2Ms,  m_beResult.c2Ms,  m_geResult.c2Ms,  cdSrc.c2Ms  },
            { "Max", iqpCt.maxMs, m_beResult.maxMs, m_geResult.maxMs, cdSrc.maxMs },
            { "C3",  iqpCt.c3Ms,  m_beResult.c3Ms,  m_geResult.c3Ms,  cdSrc.c3Ms  },
            { "C4",  iqpCt.c4Ms,  m_beResult.c4Ms,  m_geResult.c4Ms,  cdSrc.c4Ms  },
            { "Rise",sunriseMs,   sunriseMs,          -1,             -1          },
            { "Set", sunsetMs,    sunsetMs,           -1,             -1          },
        };
        static constexpr int kCtRowN = 7;
        static_assert(sizeof(ctRows)/sizeof(ctRows[0]) == kCtRowN);

        for (int ri = 0; ri < kCtRowN; ++ri) {
            const auto& r = ctRows[ri];
            ImGui::TableNextRow();
            char i1[8], i2[8], i3[8], i4[8], i5[32];
            FmtHM(r.iqp,  i1, sizeof(i1));
            FmtHM(r.be,   i2, sizeof(i2));
            FmtLoc(r.iqp, i3, sizeof(i3));
            FmtHM(r.ge,   i4, sizeof(i4));
            if (r.cd > 0) FormatCountdown(r.cd - nowMs, i5, sizeof(i5));
            else          snprintf(i5, sizeof(i5), "--");

            ImGui::TableSetColumnIndex(0); ImGui::TextColored(kGray, "%s", r.lbl);
            ImGui::TableSetColumnIndex(1);
                if (loading) ImGui::TextColored(kDim, "...");
                else         ImGui::TextColored(kBlue, "%s", i1);
            ImGui::TableSetColumnIndex(2); ImGui::TextColored(kCyan, "%s", i2);
            ImGui::TableSetColumnIndex(3);
                if (loading) ImGui::TextColored(kDim, "...");
                else         ImGui::TextColored(kGray, "%s", i3);
            ImGui::TableSetColumnIndex(4); ImGui::TextColored(kDim,  "%s", i4);
            ImGui::TableSetColumnIndex(5); ImGui::TextColored(kGold, "%s", i5);
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();

}

// ─── Hardware section (connection + cameras) ──────────────────────────────────

void App::RenderHardwareSection() {
    assert(m_fontMono != nullptr);

    bool connected = (m_pipe.GetState() == PipeClient::State::Connected);

    // Three buttons side by side: status/connect | Test picture | Disconnect.
    // Fixed 135px each.
    constexpr float kBtnW = 135.0f;
    auto TextW = [&](const char*) { return kBtnW; };
    const ImVec2 btnSzMid  (kBtnW, 28);
    const ImVec2 btnSzRight(kBtnW, 28);

    {
        bool srvRunning = false;
        HANDLE hm = OpenMutexW(SYNCHRONIZE, FALSE, L"TotalControl_DaemonRunning");
        if (hm) { srvRunning = true; CloseHandle(hm); }

        if (connected) {
            m_connecting = false;
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f, 0.55f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.65f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.08f, 0.45f, 0.14f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f,  1.0f,  1.0f,  1.0f));
            ImGui::Button("Camera connected", ImVec2(TextW("Camera connected"), 28));
            ImGui::PopStyleColor(4);
        } else if (m_connecting) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.65f, 0.38f, 0.05f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.44f, 0.08f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f, 0.32f, 0.04f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f,  1.0f,  1.0f,  1.0f));
            ImGui::BeginDisabled();
            ImGui::Button("Connecting...", ImVec2(TextW("Connecting..."), 28));
            ImGui::EndDisabled();
            ImGui::PopStyleColor(4);
        } else {
            if (srvRunning) ImGui::BeginDisabled();
            if (ImGui::Button("Connect camera", ImVec2(TextW("Connect camera"), 28))) {
                LogLine("user: Start TotalControlSRV");
                if (TryLaunchDaemon()) {
                    m_connecting = true;
                    m_reconnectCountdown = 0;
                } else {
                    m_lastResult = "ERROR: cannot launch TotalControlSRV.exe";
                    LogLine(m_lastResult);
                }
            }
            if (srvRunning) ImGui::EndDisabled();
        }
    }

    ImGui::SameLine();
    if (!connected) ImGui::BeginDisabled();
    if (ImGui::Button("Test picture", btnSzMid)) {
        LogLine("user: take test picture");
        const char* req =
            "{\"cmd\":\"shoot\",\"drive\":\"single\","
            "\"ss\":\"1/8000\",\"iso\":100,\"f\":8.0}";
        if (auto res = m_pipe.SendRequest(req)) {
            bool ok  = JStr(*res, "ok") == "true";
            int  lat = JInt(*res, "latency_ms", -1);
            if (ok && lat >= 0 && !m_cameras.empty())
                m_cameras[0].lastShotMs = lat;
            m_lastResult = ok ? std::format("Shot OK — {} ms", lat >= 0 ? lat : 0)
                              : "ERROR: shoot failed";
            LogLine(m_lastResult);
        } else {
            m_lastResult = std::format("ERROR: {}", PipeErrorMessage(res.error()));
            LogLine(m_lastResult);
        }
    }
    if (!connected) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!connected) ImGui::BeginDisabled();
    if (ImGui::Button("Disconnect", btnSzRight)) {
        if (m_seqRun.load()) { StopSeqThread(); m_guiSeqMode.store(GuiSeqMode::Idle); }
        LogLine("user: disconnect & quit SRV");
        (void)m_pipe.Send("{\"cmd\":\"quit\"}");
        m_pipe.Disconnect();
        { std::lock_guard lk(m_camerasMutex); m_cameras.clear(); }
        m_lastResult = "Server stopped.";
        LogLine("SRV quit sent");
    }
    if (!connected) ImGui::EndDisabled();

    ImGui::Spacing();
    RenderCameraSection();
}

// ─── Left column (270px): ECLIPSE / LOCATION / TIME / HARDWARE / EXECUTE ─────

void App::RenderLeftColumn() {
    assert(m_fontMono != nullptr);

    ImGui::SeparatorText("ECLIPSE");
    ImGui::Spacing();
    RenderEclipseSection();

    ImGui::Spacing();
    RenderLocationSection();

    ImGui::Spacing();
    RenderTimeSection();

    ImGui::Spacing();
    RenderHardwareSection();
}

// ─── Menu actions ────────────────────────────────────────────────────────────

void App::NewTimeline() {
    m_tracks.clear();
    InitTracks();
    m_selTrack = -1; m_selBlock = -1;
    m_tlDragging = false; m_tlDragTrack = -1; m_tlDragBlock = -1;
    m_tlViewStart = -1;   // will re-init from contacts on next frame
    m_tlViewEnd   = -1;
    m_tlDirty = true;
    LogLine("Timeline: New");
}

void App::DeleteSelectedBlock() {
    if (m_selTrack < 0 || m_selTrack >= (int)m_tracks.size()) return;
    auto& tr = m_tracks[m_selTrack];
    if (m_selBlock < 0 || m_selBlock >= (int)tr.blocks.size()) return;
    tr.blocks.erase(tr.blocks.begin() + m_selBlock);
    m_selBlock = -1; m_selTrack = -1;
    m_tlDirty = true;
}

void App::DuplicateSelectedBlock() {
    if (m_selTrack < 0 || m_selTrack >= (int)m_tracks.size()) return;
    auto& tr = m_tracks[m_selTrack];
    if (m_selBlock < 0 || m_selBlock >= (int)tr.blocks.size()) return;
    TLBlock copy = tr.blocks[m_selBlock];
    copy.id   = -1;
    copy.atMs += BlockDurMs(copy) + 500; // offset by own duration + 0.5s gap
    tr.blocks.push_back(copy);
    std::sort(tr.blocks.begin(), tr.blocks.end(),
        [](const TLBlock& a, const TLBlock& b){ return a.atMs < b.atMs; });
    // Select the duplicate
    for (int i = 0; i < (int)tr.blocks.size(); ++i) {
        if (tr.blocks[i].atMs == copy.atMs && tr.blocks[i].type == copy.type) {
            m_selBlock = i; break;
        }
    }
    m_tlDirty = true;
}

// Convert UTC ms → ISO 8601 "YYYY-MM-DDTHH:MM:SS.mmmZ"
static std::string UtcMsToIso(int64_t ms) {
    if (ms < 0) return {};
    time_t tt = (time_t)(ms / 1000);
    tm utc{};
    gmtime_s(&utc, &tt);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             utc.tm_year+1900, utc.tm_mon+1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec, (int)(ms%1000));
    return buf;
}

// ─── Named snapshots ─────────────────────────────────────────────────────────

static std::string FormatUtcIso8601(int64_t ms);  // defined in Export section below

// ─── Bracket calibration helpers ─────────────────────────────────────────────

void App::LoadCalibCache() {
    assert(m_configDb.IsOpen());
    m_calibCache.clear();
    auto models = m_configDb.LoadCalibModels();
    assert(models.size() <= 100);  // bounded: realistic number of camera models
    for (const auto& model : models) {
        auto entries = m_configDb.LoadCalibData(model);
        assert(entries.size() <= 1024);
        auto& cm = m_calibCache[model];
        for (const auto& e : entries)
            cm[{e.count, e.ev}] = e.latAvgMs + 50;
    }
    LogLine(std::format("Calibration: {} model(s) loaded into cache",
                        m_calibCache.size()));
}

void App::SeedBuiltinCalib() {
    assert(m_configDb.IsOpen());
    static constexpr char kModel[] = "ILCE-7RM4A";
    if (!m_configDb.LoadCalibData(kModel).empty()) return; // already seeded

    // Measured 2026-07-12: SS=1/125, ISO=100, f/8.0, 10 reps each.
    // lat_max_ms / lat_avg_ms / lat_min_ms
    struct Row { int cnt; const char* ev; int mx; int av; int mn; };
    static constexpr Row kRows[] = {
        {3,"0.3ev", 493,473,448}, {3,"0.5ev", 509,481,449},
        {3,"0.7ev", 510,480,457}, {3,"1.0ev", 507,483,458},
        {3,"2.0ev", 535,499,464}, {3,"3.0ev", 551,528,505},
        {5,"0.3ev", 718,681,650}, {5,"0.5ev", 702,688,672},
        {5,"0.7ev", 719,705,693}, {5,"1.0ev", 734,700,669},
        {5,"2.0ev", 851,831,808}, {5,"3.0ev",1273,1243,1219},
        {9,"0.3ev",1139,1118,1096},{9,"0.5ev",1165,1146,1127},
        {9,"0.7ev",1197,1174,1146},{9,"1.0ev",1330,1303,1264},
    };
    static constexpr int kNumRows = static_cast<int>(std::size(kRows));

    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ui; ui.LowPart = ft.dwLowDateTime; ui.HighPart = ft.dwHighDateTime;
    int64_t nowMs = static_cast<int64_t>(ui.QuadPart / 10000) - 11644473600000LL;

    std::vector<BktCalibEntry> entries;
    entries.reserve(kNumRows);
    for (const auto& r : kRows) {
        BktCalibEntry e;
        e.camModel  = kModel;
        e.count     = r.cnt;
        e.ev        = r.ev;
        e.latMaxMs  = r.mx;
        e.latAvgMs  = r.av;
        e.latMinMs  = r.mn;
        e.reps      = 10;
        e.ss        = "1/125";
        e.createdMs = nowMs;
        entries.push_back(std::move(e));
    }
    m_configDb.SaveCalibData(entries);
    LogLine(std::format("Calibration: seeded {} entries for {}", kNumRows, kModel));
}

void App::SaveCalibFromBuf(const std::string& camModel) {
    assert(!m_seqCalibBuf.empty());
    assert(!camModel.empty());

    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ui; ui.LowPart = ft.dwLowDateTime; ui.HighPart = ft.dwHighDateTime;
    int64_t nowMs = static_cast<int64_t>(ui.QuadPart / 10000) - 11644473600000LL;

    // Group samples by (count, ev) → collect latMs values
    std::map<std::pair<int,std::string>, std::vector<int>> groups;
    assert(m_seqCalibBuf.size() <= 4096);  // bounded: calibration sequence is finite
    for (const auto& s : m_seqCalibBuf)
        groups[{s.count, s.ev}].push_back(s.latMs);

    std::vector<BktCalibEntry> entries;
    entries.reserve(groups.size());
    for (const auto& [key, lats] : groups) {
        assert(!lats.empty());
        BktCalibEntry e;
        e.camModel  = camModel;
        e.count     = key.first;
        e.ev        = key.second;
        e.latMaxMs  = *std::max_element(lats.begin(), lats.end());
        e.latMinMs  = *std::min_element(lats.begin(), lats.end());
        int sum = 0;
        for (int v : lats) sum += v;
        e.latAvgMs  = sum / static_cast<int>(lats.size());
        e.reps      = static_cast<int>(lats.size());
        e.ss        = "1/100";
        e.createdMs = nowMs;
        entries.push_back(std::move(e));
    }
    m_configDb.SaveCalibData(entries);
    LoadCalibCache();
    m_seqCalibBuf.clear();
    m_lastResult = std::format("Calibration saved: {} combos for {}",
                               entries.size(), camModel);
    LogLine(m_lastResult);
}

// Builds the bracket-calibration snapshot once and saves it to the config DB.
// 16 bracket combinations × 3 repetitions = 48 blocks, ~6 min total.
// Base time: 2026-08-12T22:00:00.000Z (well after totality).
void App::CreateCalibrationSnapshot() {
    assert(m_configDb.IsOpen());
    static constexpr char kName[] = "Bracket Calibration";
    if (m_configDb.SnapshotExists(kName)) return;

    // 2026-08-12T22:00:00.000Z  (verified: 1786492800 + 22×3600 = 1786572000s)
    static constexpr int64_t kBaseMs        = 1786572000000LL;
    static constexpr int     kRepIntervalMs = 4000;   // 4 s between reps (same drive code)
    static constexpr int     kGroupGapMs    = 15000;  // 15 s between groups (drive change)
    static constexpr int     kReps          = 3;

    struct Combo { int n; const char* ev; };
    // All 16 cont-bracket combinations supported by ILCE-series cameras.
    // 3-shot: 0.3/0.5/0.7/1.0/2.0/3.0 ev
    // 5-shot: 0.3/0.5/0.7/1.0/2.0/3.0 ev
    // 9-shot: 0.3/0.5/0.7/1.0 ev  (2ev/3ev not available at 9-shot)
    static constexpr Combo kCombos[] = {
        {3,"0.3ev"},{3,"0.5ev"},{3,"0.7ev"},{3,"1.0ev"},{3,"2.0ev"},{3,"3.0ev"},
        {5,"0.3ev"},{5,"0.5ev"},{5,"0.7ev"},{5,"1.0ev"},{5,"2.0ev"},{5,"3.0ev"},
        {9,"0.3ev"},{9,"0.5ev"},{9,"0.7ev"},{9,"1.0ev"},
    };
    static constexpr int kNumCombos = static_cast<int>(std::size(kCombos));

    TLTrack track;
    track.type     = "camera";
    track.cameraId = "";          // model-agnostic: targets camera[0] at runtime
    track.label    = "Calibration Cam0";

    int64_t tMs = kBaseMs;
    for (int g = 0; g < kNumCombos; ++g) {
        const Combo& c = kCombos[g];
        for (int r = 0; r < kReps; ++r) {
            TLBlock blk;
            blk.type       = BlockType::Bracket;
            blk.atMs       = tMs;
            blk.ss         = "1/100";
            blk.iso        = 100;
            blk.fstop      = "8.0";
            blk.count      = c.n;
            blk.ev         = c.ev;
            blk.burstDrive = "cont-hi-plus";
            blk.burstDurMs = 3000;
            blk.audioDurMs = 0;
            blk.snapToPrev = false;
            blk.label      = std::format("{}x{} r{}", c.n, c.ev, r + 1);
            track.blocks.push_back(blk);
            tMs += (r < kReps - 1) ? kRepIntervalMs : kGroupGapMs;
        }
    }

    assert(track.blocks.size() == static_cast<size_t>(kNumCombos * kReps));
    std::vector<TLTrack> tracks = { std::move(track) };
    m_configDb.SaveSnapshot(kName, tracks);
    LogLine(std::format("Snapshot '{}' created: {} blocks, base {}",
                        kName, kNumCombos * kReps,
                        FormatUtcIso8601(kBaseMs)));
}

void App::RenderSnapshotModal() {
    assert(m_configDb.IsOpen());

    // ── Open Timeline modal ────────────────────────────────────────────────
    if (m_showSnapOpen) {
        ImGui::OpenPopup("Open Timeline##snap");
        m_snapList = m_configDb.LoadSnapshotList();
        m_snapSel  = -1;
        m_showSnapOpen = false;
    }
    if (ImGui::BeginPopupModal("Open Timeline##snap",
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Select snapshot to load onto the timeline:");
        ImGui::Separator();

        ImGui::BeginChild("snaplist", ImVec2(800, 280), true);
        assert(m_snapList.size() <= 1024);  // bounded: no realistic user has >1024 snapshots
        for (int i = 0; i < static_cast<int>(m_snapList.size()); ++i) {
            const auto& s = m_snapList[i];
            char ts[32];
            FormatUtcHms(s.createdMs, ts, sizeof(ts));
            std::string label = std::format("{} - {}", s.name, ts);
            bool sel = (m_snapSel == i);
            if (ImGui::Selectable(label.c_str(), sel))
                m_snapSel = i;
        }
        ImGui::EndChild();

        bool canLoad = (m_snapSel >= 0 &&
                        m_snapSel < static_cast<int>(m_snapList.size()));
        if (!canLoad) ImGui::BeginDisabled();
        if (ImGui::Button("Load", ImVec2(100, 0))) {
            auto loaded = m_configDb.LoadSnapshot(m_snapList[m_snapSel].id);
            int totalBlocks = 0;
            for (const auto& tr : loaded) totalBlocks += (int)tr.blocks.size();
            LogLine(std::format("LoadSnapshot: {} tracks, {} blocks",
                                loaded.size(), totalBlocks));
            if (!loaded.empty()) {
                m_tracks  = std::move(loaded);
                m_tlDirty = true;
                m_selTrack = m_selBlock = -1;

                // Auto-fit view to loaded blocks so they are always visible,
                // regardless of where their at_ms falls on the timeline.
                int64_t minMs = INT64_MAX, maxMs = INT64_MIN;
                for (const auto& tr : m_tracks)
                    for (const auto& b : tr.blocks) {
                        if (b.atMs < 0) continue;
                        minMs = std::min(minMs, b.atMs);
                        maxMs = std::max(maxMs, b.atMs + BlockDurMs(b));
                    }
                if (minMs < maxMs) {
                    int64_t pad   = std::max(30000LL, (maxMs - minMs) / 5LL);
                    m_tlViewStart = minMs - pad;
                    m_tlViewEnd   = maxMs + pad;
                }

                m_lastResult = std::format("Loaded snapshot: {} ({} blocks)",
                                           m_snapList[m_snapSel].name, totalBlocks);
                LogLine(m_lastResult);
            }
            ImGui::CloseCurrentPopup();
        }
        if (!canLoad) ImGui::EndDisabled();

        ImGui::SameLine();
        // Delete — only for non-built-in snapshots (allow delete of any)
        if (!canLoad) ImGui::BeginDisabled();
        if (ImGui::Button("Delete", ImVec2(100, 0))) {
            m_configDb.DeleteSnapshot(m_snapList[m_snapSel].id);
            m_snapList = m_configDb.LoadSnapshotList();
            m_snapSel  = -1;
        }
        if (!canLoad) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    // ── Save Timeline As modal ─────────────────────────────────────────────
    if (m_showSnapSaveAs) {
        ImGui::OpenPopup("Save Timeline As##snap");
        // Suggest "<location> (lat, lon)" — still a plain editable field, so
        // the user can correct/replace it freely. The coordinate suffix
        // matters: tl_snapshots.name is UNIQUE and SaveSnapshot()
        // overwrites-by-name on purpose (re-saving under an existing name is
        // a deliberate update), but nearby coordinates often reverse-geocode
        // to the *same* place name — without a differentiator, saves for
        // slightly different positions under that name silently clobbered
        // each other instead of accumulating as separate snapshots. 5
        // decimals (~1 m precision) distinguishes even small position tweaks.
        if (!m_obsName.empty()) {
            snprintf(m_snapNameBuf, sizeof(m_snapNameBuf),
                     "%s (%.5f, %.5f)", m_obsName.c_str(), m_obsLat, m_obsLon);
        } else {
            m_snapNameBuf[0] = '\0';
        }
        m_showSnapSaveAs = false;
    }
    if (ImGui::BeginPopupModal("Save Timeline As##snap",
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Snapshot name:");
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("##snapname", m_snapNameBuf, sizeof(m_snapNameBuf));

        bool canSave = (m_snapNameBuf[0] != '\0' && !m_tracks.empty());
        if (!canSave) ImGui::BeginDisabled();
        if (ImGui::Button("Save", ImVec2(100, 0))) {
            m_configDb.SaveSnapshot(std::string(m_snapNameBuf), m_tracks);
            m_lastResult = std::format("Snapshot saved: {}", m_snapNameBuf);
            LogLine(m_lastResult);
            ImGui::CloseCurrentPopup();
        }
        if (!canSave) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
}

// Formats UTC ms as ISO 8601 with milliseconds: "2026-08-12T20:29:02.100Z"
static std::string FormatUtcIso8601(int64_t ms) {
    using namespace std::chrono;
    auto tp  = system_clock::time_point{milliseconds{ms}};
    auto sec = floor<seconds>(tp);
    int  msec = static_cast<int>(ms % 1000);
    if (msec < 0) msec += 1000;
    return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03d}Z", sec, msec);
}

void App::ExportTimelineJson() {
    assert(!m_tracks.empty());  // must have at least one track before exporting
    // File save dialog
    wchar_t path[MAX_PATH] = L"eclipse_sequence.json";
    OPENFILENAMEW ofn{};
    ofn.lStructSize    = sizeof(ofn);
    ofn.lpstrFilter    = L"JSON\0*.json\0All\0*.*\0";
    ofn.lpstrFile      = path;
    ofn.nMaxFile       = MAX_PATH;
    ofn.lpstrDefExt    = L"json";
    ofn.Flags          = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    // Collect all camera blocks, sorted by time
    std::vector<const TLBlock*> steps;
    for (auto& tr : m_tracks) {
        if (!tr.IsCamera()) continue;
        for (auto& b : tr.blocks)
            if (b.atMs >= 0) steps.push_back(&b);
    }
    std::sort(steps.begin(), steps.end(),
        [](const TLBlock* a, const TLBlock* b){ return a->atMs < b->atMs; });

    // Build JSON
    std::string json = "{\n  \"steps\": [\n";
    for (size_t i = 0; i < steps.size(); ++i) {
        const TLBlock& b = *steps[i];
        std::string at  = UtcMsToIso(b.atMs);
        const char* cmd = (b.type==BlockType::Bracket) ? "bracket"
                        : (b.type==BlockType::Burst)   ? "shoot"
                                                        : "shoot";
        std::string entry = std::format(
            "    {{\n"
            "      \"at\": \"{}\",\n"
            "      \"cmd\": \"{}\",\n"
            "      \"ss\": \"{}\",\n"
            "      \"iso\": {},\n"
            "      \"f\": {}",
            at, cmd, b.ss, b.iso, b.fstop);
        if (b.type == BlockType::Bracket)
            entry += std::format(",\n      \"count\": {},\n      \"ev\": \"{}\"",
                                 b.count, b.ev);
        if (b.type == BlockType::Burst)
            entry += std::format(",\n      \"drive\": \"cont-hi-plus\",\n"
                                 "      \"timeout_ms\": {}", b.burstDurMs + 2000);
        if (!b.label.empty())
            entry += std::format(",\n      \"label\": \"{}\"", b.label);
        entry += "\n    }";
        if (i + 1 < steps.size()) entry += ",";
        json += entry + "\n";
    }
    json += "  ]\n}\n";

    // Write file
    std::ofstream f(path);
    if (f) { f << json; f.close(); }
    std::string pathNarrow;
    for (const wchar_t* p = path; *p; ++p) pathNarrow += static_cast<char>(*p);
    m_lastResult = std::format("Exported {} steps → {}", steps.size(), pathNarrow);
    LogLine(m_lastResult);
}

// ─── Camera config ────────────────────────────────────────────────────────────

void App::MergeCamerasIntoCamConfigs() {
    assert(m_camConfigs.size() == m_showCamCfgWnd.size());
    std::vector<CamStatus> snap;
    { std::lock_guard lk(m_camerasMutex); snap = m_cameras; }

    for (const CamStatus& cs : snap) {
        if (cs.guid.empty() || cs.model.empty()) continue;
        auto it = std::find_if(m_camConfigs.begin(), m_camConfigs.end(),
            [&](const CamConfig& cc){ return cc.guid == cs.guid; });
        if (it == m_camConfigs.end()) {
            CamConfig cc;
            cc.guid    = cs.guid;
            cc.model   = cs.model;
            cc.focalMm = 0;
            cc.applyP  = true;
            m_configDb.SaveCamConfig(cc.guid, cc.model, cc.focalMm, cc.applyP);
            m_camConfigs.push_back(std::move(cc));
            m_showCamCfgWnd.push_back(false);
            LogLine(std::format("CamConfig: registered {} ({})", cs.model, cs.guid));
        }
    }
}

void App::RenderCamConfigWindows() {
    assert(m_camConfigs.size() == m_showCamCfgWnd.size());
    static const ImVec4 kGray {0.40f, 0.40f, 0.45f, 1.0f};
    static const ImVec4 kDim  {0.28f, 0.28f, 0.32f, 1.0f};

    for (int ci = 0; ci < (int)m_camConfigs.size(); ++ci) {
        if (!m_showCamCfgWnd[ci]) continue;
        CamConfig& cc = m_camConfigs[ci];

        std::string wndTitle = cc.model + " \xc2\xb7 " + cc.guid + "##camcfg"
                               + std::to_string(ci);
        ImGui::SetNextWindowSize({420.f, 360.f}, ImGuiCond_FirstUseEver);
        // std::vector<bool> returns proxy, not bool* — use local copy for ImGui::Begin
        bool wndOpen = m_showCamCfgWnd[ci];
        if (!ImGui::Begin(wndTitle.c_str(), &wndOpen)) {
            ImGui::End();
            m_showCamCfgWnd[ci] = wndOpen;
            continue;
        }

        // Snapshot live status for this camera
        CamStatus live;
        { std::lock_guard lk(m_camerasMutex);
          for (const auto& cs : m_cameras)
              if (cs.guid == cc.guid) { live = cs; break; } }
        bool online = live.valid;

        ImGui::PushFont(m_fontMono);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));

        // ── Detected parameters (read-only) ─────────────────────────────────
        ImGui::SeparatorText("Detected Parameters");

        auto Row = [&](const char* key, const std::string& val) {
            ImGui::TextColored(kGray, "%s", key); ImGui::SameLine(90.f);
            if (online) ImGui::TextUnformatted(val.c_str());
            else        ImGui::TextColored(kDim, "-");
        };

        Row("Model",   cc.model);
        Row("GUID",    cc.guid);
        if (online) {
            // Battery bar + percent
            ImGui::TextColored(kGray, "Battery"); ImGui::SameLine(90.f);
            {
                float pct = live.batteryPct / 100.f;
                ImVec4 bc = pct > 0.5f ? ImVec4(0.2f,0.7f,0.3f,1.f)
                           : pct > 0.2f ? ImVec4(0.8f,0.6f,0.1f,1.f)
                                        : ImVec4(0.8f,0.2f,0.2f,1.f);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bc);
                ImGui::ProgressBar(pct, ImVec2(80.f, 0.f));
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::Text("%d%%", live.batteryPct);
            }
            Row("Mode",   live.mode);
            Row("SS",     live.ss);
            Row("ISO",    std::to_string(live.iso));
            Row("f/",     live.fnum);
            Row("Focus",  live.focus);
            Row("Drive",  live.drive);
            {
                auto SlotStr = [](int rem, int maxRem, const std::string& status) {
                    if (rem > 0 && maxRem > 0) {
                        int pct = rem * 100 / maxRem;
                        return std::format("{}  {}/{} shots  ({}%)", status, rem, maxRem, pct);
                    } else if (rem > 0) {
                        return std::format("{}  {} shots", status, rem);
                    } else if (status == "ok") {
                        return status + "  (reading...)";
                    }
                    return status;
                };
                Row("Slot 1", SlotStr(live.slot1Remaining, live.slot1MaxRem, live.slot1Status));
                Row("Slot 2", SlotStr(live.slot2Remaining, live.slot2MaxRem, live.slot2Status));
            }
        } else {
            ImGui::TextColored(kDim, "  (Not connected)");
        }

        ImGui::Spacing();
        // ── Configuration ───────────────────────────────────────────────────
        ImGui::SeparatorText("Configuration");

        ImGui::TextColored(kGray, "Focal Len"); ImGui::SameLine(90.f);
        ImGui::SetNextItemWidth(80.f);
        if (ImGui::InputInt("##focal_cfg", &cc.focalMm, 0, 0)) {
            if (cc.focalMm < 0) cc.focalMm = 0;
            m_configDb.SaveCamConfig(cc.guid, cc.model, cc.focalMm, cc.applyP,
                                     cc.trackMode, cc.horizonAltDeg, cc.horizonAzDeg);
        }
        ImGui::SameLine(); ImGui::TextColored(kGray, "mm");

        ImGui::TextColored(kGray, "Apply P");  ImGui::SameLine(90.f);
        if (ImGui::Checkbox("##applyP_cfg", &cc.applyP))
            m_configDb.SaveCamConfig(cc.guid, cc.model, cc.focalMm, cc.applyP,
                                     cc.trackMode, cc.horizonAltDeg, cc.horizonAzDeg);
        ImGui::SameLine();
        ImGui::TextColored(kDim, cc.applyP ? "corona (P rotation)" : "horizontal (landscape)");

        // ── Track mode ──────────────────────────────────────────────────────
        ImGui::Spacing();
        ImGui::TextColored(kGray, "Track");    ImGui::SameLine(90.f);
        {
            bool changed = false;
            int  tm      = static_cast<int>(cc.trackMode);
            changed |= ImGui::RadioButton("Sun##tm",     &tm, 0); ImGui::SameLine();
            changed |= ImGui::RadioButton("Moon##tm",    &tm, 1); ImGui::SameLine();
            changed |= ImGui::RadioButton("Horizon##tm", &tm, 2);
            if (changed) {
                CamTrackMode newMode = static_cast<CamTrackMode>(tm);
                // Switching TO Horizon: seed from last known Sun position so
                // the frame starts on-target and operator can drag from there.
                if (newMode == CamTrackMode::Horizon
                    && cc.trackMode != CamTrackMode::Horizon) {
                    cc.horizonAltDeg = m_sunAltDeg;
                    cc.horizonAzDeg  = m_sunAzDeg;
                }
                cc.trackMode = newMode;
                m_configDb.SaveCamConfig(cc.guid, cc.model, cc.focalMm, cc.applyP,
                                         cc.trackMode, cc.horizonAltDeg, cc.horizonAzDeg);
            }
        }
        if (cc.trackMode == CamTrackMode::Horizon) {
            ImGui::TextColored(kGray, "  Alt/Az"); ImGui::SameLine(90.f);
            ImGui::TextColored(kDim, "%.1f deg / %.1f deg  (drag frame on simulator)",
                               cc.horizonAltDeg, cc.horizonAzDeg);
        }

        ImGui::PopStyleVar();
        ImGui::PopFont();
        ImGui::End();
        m_showCamCfgWnd[ci] = wndOpen;
    }
}

// ─── Menu bar ─────────────────────────────────────────────────────────────────

void App::RenderMenuBar() {
    // ── File ──────────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Timeline", "Ctrl+N"))
            NewTimeline();

        if (ImGui::MenuItem("Open Timeline\xc2\xb7\xc2\xb7\xc2\xb7", nullptr, false,
                            m_configDb.IsOpen()))
            m_showSnapOpen = true;
        ImGui::Separator();

        ImGui::MenuItem("Save Timeline [FUTURE FUNCTION]", "Ctrl+S", false, false);
        if (ImGui::MenuItem("Save Timeline As\xc2\xb7\xc2\xb7\xc2\xb7", nullptr, false,
                            m_configDb.IsOpen() && !m_tracks.empty()))
            m_showSnapSaveAs = true;
        ImGui::Separator();

        if (ImGui::BeginMenu("Export")) {
            if (ImGui::MenuItem("TotalControl JSON\xc2\xb7\xc2\xb7\xc2\xb7"))
                ExportTimelineJson();
            ImGui::Separator();
            ImGui::MenuItem("CSV\xc2\xb7\xc2\xb7\xc2\xb7 [FUTURE FUNCTION]",                 nullptr, false, false);
            ImGui::MenuItem("SolarEclipseMaestro\xc2\xb7\xc2\xb7\xc2\xb7 [FUTURE FUNCTION]", nullptr, false, false);
            ImGui::MenuItem("EclipseWorkbench\xc2\xb7\xc2\xb7\xc2\xb7 [FUTURE FUNCTION]",    nullptr, false, false);
            ImGui::EndMenu();
        }

        ImGui::MenuItem("Import from JSON\xc2\xb7\xc2\xb7\xc2\xb7 [FUTURE FUNCTION]", nullptr, false, false);
        ImGui::Separator();

        if (ImGui::MenuItem("Quit", "Alt+F4"))
            PostQuitMessage(0);

        ImGui::EndMenu();
    }

    // ── Edit ──────────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("Edit")) {
        bool hasSel = m_selTrack >= 0 && m_selBlock >= 0;

        if (ImGui::MenuItem("Delete Block", "Del", false, hasSel))
            DeleteSelectedBlock();

        if (ImGui::MenuItem("Duplicate Block", "Ctrl+D", false, hasSel))
            DuplicateSelectedBlock();

        ImGui::Separator();
        ImGui::MenuItem("Undo [FUTURE FUNCTION]", "Ctrl+Z", false, false);
        ImGui::MenuItem("Redo [FUTURE FUNCTION]", "Ctrl+Y", false, false);
        ImGui::EndMenu();
    }

    // ── Photo Sequence ────────────────────────────────────────────────────
    if (ImGui::BeginMenu("Photo Sequence")) {
        // Reset Photo Sequence — clears all blocks from every camera track.
        if (ImGui::MenuItem("Reset Photo Sequence")) {
            for (auto& tr : m_tracks) if (tr.IsCamera()) tr.blocks.clear();
            m_tlDirty  = true;
            m_selTrack = m_selBlock = -1;
            m_lastResult = "Photo sequence reset";
            LogLine("Menu: Reset Photo Sequence");
        }
        ImGui::Separator();
        // One picture per minute for partial phase: Single blocks every 1 min
        // from C1-5min to C4+5min on the target camera track (skips C2-C3).
        if (ImGui::MenuItem("One Picture Per Minute (Partial Phase)")) AddPhotoPreset();
        // Bracket series every 3s across totality (C2-C3) on the target track.
        if (ImGui::MenuItem("Add Brackets Photo Series for Total Phase (every 3s)"))
            AddTotalityBracketPreset();
        // One block per bracket-mode dropdown entry (16 ev/count variants) —
        // calibration/verification run, 2s gap from ARM-end to next block.
        if (ImGui::MenuItem("Add All Bracket Variants (calibration)"))
            AddAllBracketVariantsPreset();
        ImGui::Separator();
        ImGui::MenuItem("Validate Timeline [FUTURE FUNCTION]", nullptr, false, false);
        ImGui::MenuItem("Auto-optimize [FUTURE FUNCTION]",     nullptr, false, false);
        ImGui::EndMenu();
    }

    // ── Audio Sequence ────────────────────────────────────────────────────
    if (ImGui::BeginMenu("Audio Sequence")) {
        // Reset Audio Sequence — clears all blocks from every audio track.
        if (ImGui::MenuItem("Reset Audio Sequence")) {
            for (auto& tr : m_tracks) if (tr.IsAudio()) tr.blocks.clear();
            m_tlDirty  = true;
            m_selTrack = m_selBlock = -1;
            m_lastResult = "Audio sequence reset";
            LogLine("Menu: Reset Audio Sequence");
        }
        ImGui::Separator();
        // Load Audio Preset: scan eclipse_audio_XX/ dirs (2-char lang suffix).
        if (ImGui::BeginMenu("Load Audio Preset")) {
            std::wstring exeDir = ExeDir();
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((exeDir + L"\\eclipse_audio_*").c_str(), &fd);
            bool anyLang = false;
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    std::string dns = WToUtf8(fd.cFileName);
                    if (dns.size() != 16) continue;   // must be exactly eclipse_audio_XX
                    std::string tag = dns.substr(14);
                    for (char& c : tag) c = static_cast<char>(
                        std::toupper(static_cast<unsigned char>(c)));
                    if (ImGui::MenuItem(tag.c_str())) LoadAudioPreset(tag);
                    anyLang = true;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            if (!anyLang)
                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1.f), "(no eclipse_audio_XX/ dirs)");
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    // ── Options ───────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("Options")) {
        if (ImGui::MenuItem("API Keys..."))
            m_showOptions = true;

        // Detected cameras — flattened here (was a "Camera Config" submenu).
        ImGui::Separator();
        if (m_camConfigs.empty()) {
            ImGui::TextDisabled("No cameras registered");
            ImGui::TextDisabled("Connect camera to discover");
        }
        for (int ci = 0; ci < (int)m_camConfigs.size(); ++ci) {
            const CamConfig& cc = m_camConfigs[ci];
            bool online = false;
            { std::lock_guard lk(m_camerasMutex);
              for (const auto& cs : m_cameras)
                  if (cs.guid == cc.guid) { online = true; break; } }
            std::string label = std::format("[{}]  {}  {}##camcfgmenu{}",
                online ? "ON" : "OFF",
                cc.model, cc.guid, ci);
            if (ImGui::MenuItem(label.c_str()))
                m_showCamCfgWnd[ci] = true;
        }
        ImGui::EndMenu();
    }

    // ── About ─────────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("About")) {
        if (ImGui::MenuItem("About TotalControl"))
            m_showAbout = true;
        if (ImGui::MenuItem("Open GitHub")) {
            ShellExecuteW(nullptr, L"open",
                L"https://github.com/mrmgrpl/TotalControl",
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::Separator();
        ImGui::MenuItem("TSE 2026-08-12  Burgos/Lerma", nullptr, false, false);
        ImGui::EndMenu();
    }
}

// ─── Options window ───────────────────────────────────────────────────────────

void App::RenderOptionsWindow() {
    assert(true); // invariant: always callable; m_showOptions guards early-out
    if (!m_showOptions) return;

    ImGui::SetNextWindowSize(ImVec2(460, 220), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin("Options##optwin", &m_showOptions, ImGuiWindowFlags_NoCollapse))
    { ImGui::End(); return; }

    static const ImVec4 kGold {0.95f, 0.80f, 0.20f, 1.0f};
    static const ImVec4 kGray {0.55f, 0.55f, 0.60f, 1.0f};
    static const ImVec4 kGreen{0.30f, 0.80f, 0.35f, 1.0f};
    static const ImVec4 kLink {0.45f, 0.75f, 1.00f, 1.0f};

    ImGui::PushFont(m_fontMono);

    // ── IQP API Key ────────────────────────────────────────────────────────────
    ImGui::TextColored(kGold, "IQP API Key  (besselianelements.com)");
    ImGui::TextColored(kGray, "40-char hex key -- contact the besselianelements.com team to obtain one:");
    ImGui::TextColored(kLink, "  https://www.besselianelements.com/");
    ImGui::TextColored(kGray, "No key:   C1-C4 times from local Besselian model (green column).");
    ImGui::TextColored(kGray, "With key: C1-C4 times from IQP API automatically (blue column).");
    ImGui::TextColored(kGray, "You can also visit the site and enter the times manually.");
    ImGui::Spacing();

    ImGuiInputTextFlags keyFlags = m_beKeyVisible
        ? ImGuiInputTextFlags_None
        : ImGuiInputTextFlags_Password;
    ImGui::SetNextItemWidth(310.f);
    bool edited = ImGui::InputText("##be_api_key", m_beApiKeyBuf,
                                   sizeof(m_beApiKeyBuf), keyFlags);
    ImGui::SameLine();
    if (ImGui::Button(m_beKeyVisible ? "Hide" : "Show"))
        m_beKeyVisible = !m_beKeyVisible;
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        std::string newKey = m_beApiKeyBuf;
        SetBeApiKey(newKey);
        if (m_configDb.IsOpen())
            m_configDb.SetSetting("be_api_key", newKey.c_str());
    }
    (void)edited; // InputText result not needed here — Apply is explicit

    // Live status: which API will be used on next Calculate Contacts
    std::string activeKey = GetBeApiKey();
    if (!activeKey.empty()) {
        ImGui::TextColored(kGreen,
            "  BE REST API active  (%s...)",
            activeKey.substr(0, 8).c_str());
    } else {
        ImGui::TextColored(kGray,
            "  Classic IQP API (maps.besselianelements.com)");
    }

    assert(true); // postcondition: all controls rendered
    ImGui::PopFont();
    ImGui::End();
}

// ─── About modal ──────────────────────────────────────────────────────────────

void App::RenderAboutModal() {
    assert(true); // invariant: always callable; m_showAbout guards early-out
    if (!m_showAbout) return;
    ImGui::OpenPopup("About TotalControl");
    ImGui::SetNextWindowSize(ImVec2(580, 620), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal("About TotalControl", &m_showAbout,
                                ImGuiWindowFlags_NoResize))
        return;

    static const ImVec4 kGold {0.95f, 0.80f, 0.20f, 1.0f};
    static const ImVec4 kGray {0.55f, 0.55f, 0.60f, 1.0f};
    static const ImVec4 kLink {0.45f, 0.75f, 1.00f, 1.0f};

    ImGui::PushFont(m_fontLarge);
    ImGui::TextColored(kGold, "TotalControl");
    ImGui::PopFont();
    ImGui::PushFont(m_fontMono);
    ImGui::TextColored(kGray, "Solar Eclipse Photography Controller");
    ImGui::TextColored(kGray, "TSE 2026-08-12  Burgos / Lerma, Spain  (totality 103.9 s)");
    ImGui::PopFont();

    ImGui::Separator();

    // scrollable body (44px reserved for Close button + separator)
    ImGui::BeginChild("##about_body", ImVec2(0, -44.f), false, ImGuiWindowFlags_None);
    ImGui::PushFont(m_fontMono);

    // helper: clickable blue link rendered as MenuItem
    struct S {
        static void Link(const char* label, const wchar_t* url, const ImVec4& col) {
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (ImGui::MenuItem(label))
                ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::PopStyleColor();
        }
    };

    // ── Libraries ─────────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextColored(kGold, "Biblioteki / Libraries");
    ImGui::TextColored(kGray, "  Sony Camera Remote SDK (CrSDK)  Sony Corp.");
    ImGui::TextColored(kGray, "    Camera control, live view, property polling");
    ImGui::Spacing();
    ImGui::TextColored(kGray, "  Dear ImGui v1.91.6  Omar Cornut  (MIT License)");
    S::Link(      "    github.com/ocornut/imgui",    L"https://github.com/ocornut/imgui",  kLink);
    ImGui::Spacing();
    ImGui::TextColored(kGray, "  SQLite 3.53.1  D. Richard Hipp  (Public domain)");
    S::Link(      "    sqlite.org",                  L"https://www.sqlite.org",            kLink);
    ImGui::Spacing();
    ImGui::TextColored(kGray, "  WinHTTP / Direct3D 11 / WIC  Microsoft Windows SDK");
    ImGui::TextColored(kGray, "    HTTPS fetch, GPU rendering, JPEG decode");

    // ── Eclipse Data & Algorithms ──────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextColored(kGold, "Dane zaciemien / Eclipse Data & Algorithms");
    ImGui::TextColored(kGray, "  Fred Espenak  (\"Mr. Eclipse\")  NASA GSFC (ret.)");
    ImGui::TextColored(kGray, "    Exposure model, Besselian elements, 11 898 eclipses");
    S::Link(      "    eclipse.gsfc.nasa.gov",       L"https://eclipse.gsfc.nasa.gov/",    kLink);
    ImGui::Spacing();
    ImGui::TextColored(kGray, "  IQP API -- contact times C1-C4 in real time");
    S::Link(      "    maps.besselianelements.com",  L"https://maps.besselianelements.com",kLink);
    ImGui::Spacing();
    ImGui::TextColored(kGray, "  Jean Meeus  \"Astronomical Algorithms\"");
    ImGui::TextColored(kGray, "    Willmann-Bell 2nd ed. -- Besselian element computation");
    ImGui::Spacing();
    ImGui::TextColored(kGray, "  Greg Miller -- Besselian Elements for Solar Eclipses");
    ImGui::TextColored(kGray, "    C2/C3 contact geometry, local circumstances");
    S::Link(      "    celestialprogramming.com",    L"https://www.celestialprogramming.com/", kLink);

    // ── Solar Imagery & Ephemeris ──────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextColored(kGold, "Obrazy i efemerdy / Solar Imagery & Ephemeris");
    ImGui::TextColored(kGray, "  JPL Horizons  NASA Jet Propulsion Laboratory");
    ImGui::TextColored(kGray, "    Sun, Moon & 5 planets -- positions, angular sizes");
    S::Link(      "    ssd.jpl.nasa.gov/horizons",   L"https://ssd.jpl.nasa.gov/horizons/",kLink);
    ImGui::Spacing();
    ImGui::TextColored(kGray, "  SDO HMI Intensitygram  NASA Goddard SFC");
    ImGui::TextColored(kGray, "    Live solar disc image (latest_1024_HMIIC.jpg)");
    S::Link(      "    sdo.gsfc.nasa.gov",           L"https://sdo.gsfc.nasa.gov/",        kLink);
    ImGui::Spacing();
    ImGui::TextColored(kGray, "  GOES-19 SUVI Fe171  NOAA/NESDIS");
    ImGui::TextColored(kGray, "    EUV corona animation -- 300 frames, 30 fps, 4-min cadence");
    S::Link(      "    star.nesdis.noaa.gov",        L"https://star.nesdis.noaa.gov/",     kLink);
    ImGui::Spacing();
    ImGui::TextColored(kGray, "  IANA Timezone Database -- 598 IANA zones, DST via Windows");
    S::Link(      "    iana.org/time-zones",         L"https://www.iana.org/time-zones",   kLink);

    // ── Acknowledgments ────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextColored(kGold, "Podziekowania / Acknowledgments");
    ImGui::TextColored(kGray, "  Fred Espenak -- 40+ lat tabel zaciemien dla fotografow");
    ImGui::TextColored(kGray, "  Ekipa IQP -- API prognoz zaciemien w czasie rzeczywistym");
    ImGui::TextColored(kGray, "  Omar Cornut -- Dear ImGui, immediate-mode UI bez kompromisow");
    ImGui::TextColored(kGray, "  NASA SDO i NOAA GOES-19 -- otwarte dane sloneczne");
    ImGui::TextColored(kGray, "  Zespol JPL SSD -- serwis efemerydy Horizons");
    ImGui::TextColored(kGray, "  Jean Meeus -- algorytmy astronomiczne ponadczasowe");
    ImGui::TextColored(kGray, "  Open-Elevation API -- darmowa wysokosc n.p.m. z linku Google Maps");

    // ── Author ─────────────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextColored(kGold, "Autor / Author");
    ImGui::TextColored(kGray, "  Andrzej Nowak  (mrmgrpl)");
    S::Link(      "    github.com/mrmgrpl/TotalControl",
                  L"https://github.com/mrmgrpl/TotalControl",                              kLink);

    assert(true); // postcondition: all sections rendered without skipping EndChild/PopFont
    ImGui::PopFont();
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Spacing();
    float btnW = 80.f;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnW) * 0.5f);
    if (ImGui::Button("Close", ImVec2(btnW, 0)))
        m_showAbout = false;

    ImGui::EndPopup();
}

// ─── Main frame ───────────────────────────────────────────────────────────────

void App::OnFrame() {
    ImGuiIO& io = ImGui::GetIO();

    // ── Merge newly connected cameras into persistent config ──────────────
    if (m_configDb.IsOpen()) MergeCamerasIntoCamConfigs();

    // ── Post-scan reload ──────────────────────────────────────────────────
    // If a "Reset Audio Presets" triggered a rescan, reload the preset now.
    if (m_audioScanComplete.exchange(false) && !m_pendingAudioReload.empty()) {
        LoadAudioPreset(m_pendingAudioReload);
        m_lastResult = std::format("Audio presets reloaded ({})", m_pendingAudioReload);
        m_pendingAudioReload.clear();
    }

    // ── Main menu bar ─────────────────────────────────────────────────────
    float menuH = 0.f;
    if (ImGui::BeginMainMenuBar()) {
        menuH = ImGui::GetWindowHeight();
        RenderMenuBar();
        ImGui::EndMainMenuBar();
    }

    // ── Keyboard shortcuts ────────────────────────────────────────────────
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) NewTimeline();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) DuplicateSelectedBlock();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) DeleteSelectedBlock();

    // ── Host window (below menu bar) ──────────────────────────────────────
    ImGui::SetNextWindowPos(ImVec2(0.f, menuH));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - menuH));
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##host", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    // ── About + Options + snapshot modals ────────────────────────────────
    RenderAboutModal();
    RenderOptionsWindow();
    if (m_configDb.IsOpen()) RenderSnapshotModal();

    const float kLeftW = 420.0f;   // Left column: Eclipse/Location/Time/Hardware/Execute
                                    // (300 + ~120 for the T- countdown column in the TIME table)
    const float kInspW = 220.0f;   // Right column: Inspector + Palette
    const float totalH = io.DisplaySize.y - menuH;
    const float totalW = io.DisplaySize.x;

    // Timeline height: separator + full ruler section + clamp(tracks,1,4) rows + padding
    // Ruler section = kMarkerH(14) + kPhaseH(20) + kRelRulerH(22) + kRulerH(85) = 141px
    static constexpr float kTlHdrH   =  26.f;   // SeparatorText "TIMELINE"
    static constexpr float kTlRulerH = 141.f;   // all ruler layers combined
    static constexpr float kTlTrackH =  28.f;   // one track row
    static constexpr float kTlPadH   =  10.f;
    int   nTl        = std::clamp((int)m_tracks.size(), 1, 4);
    float kTimelineH = kTlHdrH + kTlRulerH + float(nTl) * kTlTrackH + kTlPadH;

    const float topH = totalH - kTimelineH;

    // ════════════════════════════════════════════════════════════════════════
    // LEFT COLUMN — Eclipse / Location / Time / Hardware / Execute (270px)
    // ════════════════════════════════════════════════════════════════════════
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.07f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3, 10));
    ImGui::BeginChild("##col_left", ImVec2(kLeftW, topH), false, 0);
    ImGui::PopStyleVar();
    RenderLeftColumn();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ════════════════════════════════════════════════════════════════════════
    // CENTER — Sky View Simulator (auto width = total − left − right)
    // ════════════════════════════════════════════════════════════════════════
    ImGui::SameLine(0, 0);
    const float centerW = totalW - kLeftW - kInspW;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.09f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    ImGui::BeginChild("##col_center", ImVec2(centerW, topH), false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    RenderStatusColumn();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ════════════════════════════════════════════════════════════════════════
    // RIGHT COLUMN — Inspector + Palette (270px)
    // ════════════════════════════════════════════════════════════════════════
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.075f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3, 10));
    ImGui::BeginChild("##col_insp", ImVec2(kInspW, topH), false, 0);
    ImGui::PopStyleVar();
    RenderInspectorColumn();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ════════════════════════════════════════════════════════════════════════
    // TIMELINE — full width at bottom
    // ════════════════════════════════════════════════════════════════════════
    ImGui::SetCursorPos({0.0f, topH});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.04f, 0.055f, 1.0f));
    ImGui::BeginChild("##col_tl", ImVec2(totalW, kTimelineH), false,
                      ImGuiWindowFlags_NoScrollbar);
    RenderTimelineBottom();
    ImGui::EndChild();
    ImGui::PopStyleColor();


    ImGui::End();

    // ── Camera config floating windows (outside host window) ─────────────
    RenderCamConfigWindows();

    // ── persist timeline when changed ─────────────────────────────────────
    if (m_tlDirty && m_configDb.IsOpen()) {
        m_configDb.SaveTimeline(m_tracks);
        m_tlDirty = false;
    }

    // Camera status polling is handled by m_statusThread (background, ~2 s interval).

    // ── silent auto-reconnect ─────────────────────────────────────────────
    const bool connected = (m_pipe.GetState() == PipeClient::State::Connected);
    if (!connected) {
        if (m_reconnectCountdown <= 0) {
            m_pipe.Connect();
            m_reconnectCountdown = 60;
        } else {
            --m_reconnectCountdown;
        }
    }
}

bool App::OnCloseRequest() { return true; }

// ─── Daemon launch ────────────────────────────────────────────────────────────

bool App::TryLaunchDaemon() {
    HANDLE hm = OpenMutexW(SYNCHRONIZE, FALSE, L"TotalControl_DaemonRunning");
    if (hm) { CloseHandle(hm); return true; }

    std::wstring dir = ExeDir();
    std::wstring exe = dir + L"\\TotalControlSRV.exe";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr,
                             FALSE, CREATE_NEW_CONSOLE, nullptr,
                             dir.c_str(), &si, &pi);
    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return true; }
    return false;
}

} // namespace TotalControl
