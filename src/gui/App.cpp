#include "App.h"
#include "TzEntry.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <windows.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string_view>

namespace TotalControl {

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    auto slash = s.rfind(L'\\');
    return (slash != std::wstring::npos) ? s.substr(0, slash) : s;
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

    // IQP: asynchronous, no BE fallback (results shown side-by-side)
    m_iqpState.store(1);
    if (m_iqpThread.joinable()) m_iqpThread.detach();

    auto* statePtr   = &m_iqpState;
    auto* mutexPtr   = &m_iqpMutex;
    auto* contactPtr = &m_contacts;
    auto* cfgDb      = &m_configDb;

    m_iqpThread = std::thread([id, lat, lon, y, mo, d,
                                statePtr, mutexPtr, contactPtr, cfgDb]() {
        auto ct = FetchContactTimes(id, lat, lon, y, mo, d);
        // Persist refreshed key so it survives next app restart
        std::string newKey = GetCurrentApiKey();
        if (!newKey.empty())
            cfgDb->SetSetting("iqp_api_key", newKey.c_str());
        { std::lock_guard lk(*mutexPtr); *contactPtr = ct; }
        statePtr->store(ct.apiOk ? 2 : 3);
    });
    LogLine(std::format("IQP+BE triggered: {} lat={:.4f} lon={:.4f}  BE valid={}",
                        id, lat, lon, m_beResult.valid));
}

// ─── App lifecycle ────────────────────────────────────────────────────────────

App::App() {
    std::wstring dir = ExeDir();

    m_logFile.open(dir + L"\\TotalControlGUI.log", std::ios::app);
    LogLine("=== TotalControlGUI start ===");

    // ── TotalControlDefaultConfig.db — factory defaults ───────────────────
    std::wstring defaultCfgPath = dir + L"\\TotalControlDefaultConfig.db";
    EnsureDefaultConfig(defaultCfgPath);

    // ── TotalControlConfig.db — active config ─────────────────────────────
    std::wstring configPath = dir + L"\\TotalControlConfig.db";
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
        m_obsAltM = m_configDb.GetSettingInt("obs_alt_m", 0);
        SyncDecimalToDms();

        // Load cached IQP API key (may be newer than compile-time default)
        std::string savedKey = m_configDb.GetSetting("iqp_api_key", "");
        if (!savedKey.empty()) SetApiKey(savedKey);

        LogLine(std::format("Config DB: home={}  ecl={}  obs={:.4f},{:.4f} {}m",
                            m_homeTzIana, m_eclTzIana, m_obsLat, m_obsLon, m_obsAltM));
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
        m_eclipseIdx = static_cast<int>(m_eclipses.size()) - 1; // fallback: last
        for (int i = 0; i < static_cast<int>(m_eclipses.size()); ++i) {
            if (m_eclipses[i].DateInt() >= today) { m_eclipseIdx = i; break; }
        }
        const auto& e = m_eclipses[m_eclipseIdx];
        LogLine(std::format("Default eclipse: {}-{:02d}-{:02d} {}",
                            e.year, e.month, e.day, e.type));
    }
}

App::~App() {
    if (m_iqpThread.joinable()) m_iqpThread.join();
    if (m_pipe.GetState() == PipeClient::State::Connected) {
        (void)m_pipe.Send("{\"cmd\":\"quit\"}");
        m_pipe.Disconnect();
    }
    LogLine("=== TotalControlGUI exit ===");
}

void App::OnInit() {
    ImGuiIO& io = ImGui::GetIO();
    m_fontMono  = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 15.0f);
    m_fontLarge = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 24.0f);
    if (!m_fontMono)  m_fontMono  = io.FontDefault;
    if (!m_fontLarge) m_fontLarge = io.FontDefault;
    io.Fonts->Build();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding   = 0.0f;
    style.FrameRounding    = 3.0f;
    style.FramePadding     = ImVec2(8, 5);
    style.ItemSpacing      = ImVec2(8, 6);
    style.WindowBorderSize = 1.0f;
    style.WindowPadding    = ImVec2(12, 12);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.09f, 0.09f, 0.10f, 1.0f);
    c[ImGuiCol_ChildBg]       = ImVec4(0.09f, 0.09f, 0.10f, 1.0f);
    c[ImGuiCol_Button]        = ImVec4(0.18f, 0.18f, 0.24f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.28f, 0.38f, 1.0f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.14f, 0.45f, 0.80f, 1.0f);
    c[ImGuiCol_FrameBg]       = ImVec4(0.14f, 0.14f, 0.18f, 1.0f);
    c[ImGuiCol_Separator]     = ImVec4(0.22f, 0.22f, 0.28f, 1.0f);
    c[ImGuiCol_Border]        = ImVec4(0.22f, 0.22f, 0.28f, 1.0f);
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

// ─── Camera status polling ────────────────────────────────────────────────────

void App::PollCameraStatus() {
    // 1. Get list of connected cameras
    auto listRes = m_pipe.SendRequest(R"({"cmd":"list_cameras"})");
    if (!listRes) { m_cameras.clear(); return; }

    auto guids = JStrAll(*listRes, "guid");
    if (guids.empty()) { m_cameras.clear(); return; }

    // 2. Query status for each camera; keep vector in GUID order
    std::vector<CamStatus> next;
    next.reserve(guids.size());

    for (auto& guid : guids) {
        std::string req = std::format(R"({{"cmd":"status","cam":"{}"}})", guid);
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

        // Preserve lastShotMs across polls
        for (auto& prev : m_cameras)
            if (prev.guid == guid) { s.lastShotMs = prev.lastShotMs; break; }

        next.push_back(std::move(s));
    }
    m_cameras = std::move(next);
}

// ─── Camera status rendering ──────────────────────────────────────────────────

void App::RenderCameraSection() {
    static const ImVec4 kGray   { 0.40f, 0.40f, 0.45f, 1.0f };
    static const ImVec4 kWhite  { 0.88f, 0.88f, 0.90f, 1.0f };
    static const ImVec4 kGreen  { 0.20f, 0.85f, 0.30f, 1.0f };
    static const ImVec4 kYellow { 0.95f, 0.80f, 0.10f, 1.0f };
    static const ImVec4 kOrange { 1.00f, 0.55f, 0.10f, 1.0f };
    static const ImVec4 kRed    { 0.90f, 0.20f, 0.18f, 1.0f };

    if (m_cameras.empty()) {
        ImGui::TextColored(kGray, "no cameras");
        return;
    }

    // Value column starts at fixed x offset within panel content
    const float kValX = 52.0f;

    auto Row = [&](const char* label, const char* value,
                   ImVec4 valCol = kWhite) {
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(kGray, "%s", label);
        ImGui::SameLine(kValX);
        ImGui::TextColored(valCol, "%s", value);
        ImGui::PopFont();
    };

    for (int ci = 0; ci < static_cast<int>(m_cameras.size()); ++ci) {
        const CamStatus& s = m_cameras[ci];

        if (ci > 0) ImGui::Spacing();

        // Camera header: model name or index fallback
        ImGui::PushFont(m_fontMono);
        const char* modelStr = s.model.empty() ? "Camera" : s.model.c_str();
        ImGui::TextColored(kWhite, "[%d] %s", ci + 1, modelStr);
        ImGui::PopFont();

        if (!s.valid) {
            ImGui::TextColored(kRed, "    disconnected");
            continue;
        }

        // Battery — coloured bar + percentage
        {
            ImVec4 batCol = kGreen;
            int bars = 4;
            std::string_view lvl = s.batteryLevel;
            if      (lvl.find("1/4")  != std::string_view::npos) { bars = 1; batCol = kRed;    }
            else if (lvl.find("1/2")  != std::string_view::npos) { bars = 2; batCol = kOrange; }
            else if (lvl.find("3/4")  != std::string_view::npos) { bars = 3; batCol = kYellow; }

            bool usb = lvl.find("usb") != std::string_view::npos;
            char barStr[8];
            for (int b = 0; b < 4; ++b) barStr[b] = (b < bars) ? '|' : '.';
            barStr[4] = '\0';

            char batBuf[24];
            snprintf(batBuf, sizeof(batBuf), "%s %d%%%s",
                     barStr, s.batteryPct, usb ? " USB" : "");
            Row("Batt", batBuf, batCol);
        }

        // Exposure
        Row("Mode",  s.mode.empty()  ? "?" : s.mode.c_str());
        Row("SS",    s.ss.empty()    ? "?" : s.ss.c_str());

        char isoBuf[12];
        snprintf(isoBuf, sizeof(isoBuf), "%d", s.iso);
        Row("ISO",   s.iso ? isoBuf : "?");
        Row("f/",    s.fnum.empty()  ? "?" : s.fnum.c_str());
        Row("Focus", s.focus.empty() ? "?" : s.focus.c_str());
        Row("Drive", s.drive.empty() ? "?" : s.drive.c_str());

        // Cards
        auto CardRow = [&](const char* lbl, const std::string& status, int remaining) {
            char buf[32];
            if (remaining >= 0)
                snprintf(buf, sizeof(buf), "%s  %d", status.c_str(), remaining);
            else
                snprintf(buf, sizeof(buf), "%s", status.c_str());
            ImVec4 col = (status == "OK") ? kGreen : kRed;
            Row(lbl, buf, col);
        };

        CardRow("C1", s.slot1Status.empty() ? "?" : s.slot1Status, s.slot1Remaining);
        CardRow("C2", s.slot2Status.empty() ? "?" : s.slot2Status, s.slot2Remaining);

        // Last shoot latency — full stack (SDK + USB + shutter + confirm)
        if (s.lastShotMs >= 0) {
            char shotBuf[16];
            snprintf(shotBuf, sizeof(shotBuf), "%d ms", s.lastShotMs);
            ImVec4 shotCol = s.lastShotMs < 500  ? kGreen
                           : s.lastShotMs < 1500 ? kYellow
                                                 : kRed;
            Row("Shot", shotBuf, shotCol);
        }
    }
}

// ─── Eclipse time helpers ─────────────────────────────────────────────────────

// Returns UTC ms (Unix epoch) of greatest eclipse.
// td_ge is Terrestrial Time "HH:MM:SS[.s]"; dt = TT−UTC in seconds.
// Returns INT64_MIN on parse failure.
static int64_t EclipseGeUtcMs(const EclipseEntry& e) {
    using namespace std::chrono;
    int hh = 0, mm = 0;
    float fs = 0.f;
    if (sscanf_s(e.timeGe.c_str(), "%d:%d:%f", &hh, &mm, &fs) < 2)
        return INT64_MIN;
    auto ymd = year_month_day{ year(e.year), month(static_cast<unsigned>(e.month)),
                               day(static_cast<unsigned>(e.day)) };
    auto dp    = sys_days{ ymd };
    int64_t datMs = duration_cast<milliseconds>(dp.time_since_epoch()).count();
    int64_t timMs = (int64_t(hh) * 3600 + int64_t(mm) * 60) * 1000
                  + static_cast<int64_t>(fs * 1000.f);
    int64_t dtMs  = static_cast<int64_t>(e.dt * 1000.f);  // TT − UTC → subtract
    return datMs + timMs - dtMs;
}

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

    if (m_eclipses.empty()) {
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(kGray, "no data");
        ImGui::PopFont();
        return;
    }

    // Combo preview: YYYY-MM-DD  T
    char preview[20] = "---";
    if (m_eclipseIdx >= 0 && m_eclipseIdx < static_cast<int>(m_eclipses.size())) {
        const auto& e = m_eclipses[m_eclipseIdx];
        char tc = e.type.empty() ? '?' : e.type[0];
        snprintf(preview, sizeof(preview), "%04d-%02d-%02d  %c",
                 e.year, e.month, e.day, tc);
    }

    ImGui::PushFont(m_fontMono);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##ecl_sel", preview, ImGuiComboFlags_HeightLarge)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_eclipses.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& e = m_eclipses[i];
                char tc = e.type.empty() ? '?' : e.type[0];
                char lbl[20];
                snprintf(lbl, sizeof(lbl), "%04d-%02d-%02d  %c",
                         e.year, e.month, e.day, tc);
                bool sel = (i == m_eclipseIdx);
                if (ImGui::Selectable(lbl, sel)) {
                    m_eclipseIdx = i;
                    TriggerIqpFetch();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopFont();

    // Details for selected eclipse
    if (m_eclipseIdx < 0 || m_eclipseIdx >= static_cast<int>(m_eclipses.size()))
        return;

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

    const float kValX = 42.0f;

    ImGui::PushFont(m_fontMono);

    // Type + duration
    ImGui::TextColored(typeCol, "%s", typeName);
    if (!e.duration.empty()) {
        ImGui::SameLine(kValX);
        ImGui::TextColored(kGray, "%s", e.duration.c_str());
    }

    // Location of maximum
    char latBuf[10], lonBuf[10];
    snprintf(latBuf, sizeof(latBuf), "%.1f%c", fabsf(e.latGe), e.latGe >= 0.f ? 'N' : 'S');
    snprintf(lonBuf, sizeof(lonBuf), "%.1f%c", fabsf(e.lonGe), e.lonGe >= 0.f ? 'E' : 'W');
    ImGui::TextColored(kGray, "Max");
    ImGui::SameLine(kValX);
    char coordBuf[24];
    snprintf(coordBuf, sizeof(coordBuf), "%s %s", latBuf, lonBuf);
    ImGui::TextColored(kWhite, "%s", coordBuf);

    ImGui::PopFont();

    // ── Observer location (DMS) ───────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushFont(m_fontMono);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.f, 3.f));

    // Helper: render one DMS row (label on its own line, fields on next line).
    // posLabel/negLabel: "N"/"S" or "E"/"W"
    auto DmsRow = [&](const char* label,
                      DmsCoord&   dms,
                      int         maxDeg,
                      const char* posLabel,
                      const char* negLabel,
                      const char* idD, const char* idM, const char* idS) {
        ImGui::TextColored(kGray, "%s", label);
        bool t = false;

        ImGui::SetNextItemWidth(38);
        ImGui::InputInt(idD, &dms.deg, 0);
        t |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::SameLine(0, 1); ImGui::TextColored(kGray, "°");
        ImGui::SameLine(0, 2); ImGui::SetNextItemWidth(26);
        ImGui::InputInt(idM, &dms.min, 0);
        t |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::SameLine(0, 1); ImGui::TextColored(kGray, "'");
        ImGui::SameLine(0, 2); ImGui::SetNextItemWidth(52);
        ImGui::InputFloat(idS, &dms.sec, 0.f, 0.f, "%.2f");
        t |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::SameLine(0, 1); ImGui::TextColored(kGray, "\"");
        ImGui::SameLine(0, 2);
        if (ImGui::Button(dms.pos ? posLabel : negLabel, ImVec2(22.f, 0.f))) {
            dms.pos = !dms.pos; t = true;
        }

        if (t) {
            dms.deg = std::clamp(dms.deg, 0, maxDeg);
            dms.min = std::clamp(dms.min, 0, 59);
            dms.sec = std::clamp(dms.sec, 0.f, 59.99f);
            SyncDmsToDecimal();
            SaveObserverSettings();
        }
    };

    DmsRow("Latitude",  m_latDms, 90,  "N##latn", "S##latn",
           "##latd", "##latm", "##lats");
    DmsRow("Longitude", m_lonDms, 180, "E##lone", "W##lone",
           "##lond", "##lonm", "##lons");

    // Altitude
    ImGui::TextColored(kGray, "Altitude");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputInt("##obs_alt", &m_obsAltM, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveObserverSettings();

    ImGui::Spacing();
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

    renderSection("IQP", iqpCt, iqpSt == 1,
                  ImVec4{0.45f, 0.75f, 1.00f, 1.0f});   // blue
    ImGui::Spacing();
    renderSection("BE",  m_beResult, false,
                  ImVec4{0.40f, 1.00f, 0.60f, 1.0f});   // green
}

// ─── Main frame ───────────────────────────────────────────────────────────────

void App::OnFrame() {
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##host", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    const float colW   = 200.0f;
    const float colW2  = 400.0f;
    const float sideW  = colW + colW2;
    const float totalH = io.DisplaySize.y;
    const float rightW = io.DisplaySize.x - sideW;

    bool connected = (m_pipe.GetState() == PipeClient::State::Connected);

    // ════════════════════════════════════════════════════════════════════════
    // COLUMN 1 — Hardware (TIME / CONNECTION / CAMERA STATUS)
    // ════════════════════════════════════════════════════════════════════════
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.07f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 14));
    ImGui::BeginChild("##col_hw", ImVec2(colW, totalH), false, 0);
    ImGui::PopStyleVar();

    // ── app name ─────────────────────────────────────────────────────────
    ImGui::PushFont(m_fontMono);
    ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.92f, 1.0f), "TotalControl");
    ImGui::PopFont();

    // ════════════════════════════════
    // Section: TIME
    // ════════════════════════════════
    ImGui::SeparatorText("TIME");

    // ── UTC clock (always on, no options) ─────────────────────────────────
    {
        char buf[20];
        FormatUtcHms(UtcNowMs(), buf, sizeof(buf));
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(ImVec4(0.90f, 0.75f, 0.20f, 1.0f), "%s", buf);
        ImGui::TextColored(ImVec4(0.40f, 0.40f, 0.45f, 1.0f), "Universal Time Zone");
        ImGui::PopFont();
    }

    ImGui::Spacing();

    // ── Home clock ────────────────────────────────────────────────────────
    RenderExtraClock("Home Time Zone##home", "##popup_home", m_showHomeClock, m_homeTzIana);

    ImGui::Spacing();

    // ── Local / eclipse clock ─────────────────────────────────────────────
    RenderExtraClock("Local Time Zone##ecl", "##popup_ecl",  m_showEclClock,  m_eclTzIana);

    // ── Countdowns ────────────────────────────────────────────────────────────
    if (m_eclipseIdx >= 0 && m_eclipseIdx < static_cast<int>(m_eclipses.size())) {
        const auto& ec = m_eclipses[m_eclipseIdx];
        char tc = ec.type.empty() ? '?' : ec.type[0];
        ImVec4 cdCol = (tc == 'T') ? ImVec4{0.95f, 0.80f, 0.20f, 1.0f}
                     : (tc == 'A') ? ImVec4{0.85f, 0.50f, 0.15f, 1.0f}
                     : (tc == 'H') ? ImVec4{0.80f, 0.75f, 0.20f, 1.0f}
                                   : ImVec4{0.55f, 0.55f, 0.60f, 1.0f};
        static const ImVec4 kDim {0.35f, 0.35f, 0.40f, 1.0f};
        static const ImVec4 kGray{0.40f, 0.40f, 0.45f, 1.0f};

        ImGui::Spacing();
        ImGui::PushFont(m_fontMono);

        // Row 1 — GE (always visible, eclipse-axis reference)
        int64_t geMs = EclipseGeUtcMs(ec);
        if (geMs != INT64_MIN) {
            char cdBuf[32];
            FormatCountdown(geMs - UtcNowMs(), cdBuf, sizeof(cdBuf));
            ImGui::TextColored(kGray, "GE");
            ImGui::SameLine(25.0f);
            ImGui::TextColored(cdCol, "%s", cdBuf);
        }

        // Rows C1/C2/Max/C3/C4 from IQP or Besselian
        {
            int iqpSt = m_iqpState.load();
            if (iqpSt == 1) {
                ImGui::TextColored(kGray, "C1");
                ImGui::SameLine(25.0f);
                ImGui::TextColored(kDim, "loading...");
            } else if (iqpSt == 2 || iqpSt == 3) {
                ContactTimes ct;
                { std::lock_guard lk(m_iqpMutex); ct = m_contacts; }

                if (!ct.valid && ct.apiOk) {
                    ImGui::TextColored(kGray, "C1");
                    ImGui::SameLine(25.0f);
                    ImGui::TextColored(kDim, "no eclipse");
                } else if (ct.valid) {
                    // Source label shown once on C1 line
                    const char* srcLabel = (ct.source == ContactSource::IQP) ? "IQP" : "BE";

                    // Helper: format UTC ms → "HH:MM:SS"
                    auto fmtUtc = [](int64_t ms, char* buf, int len) {
                        if (ms < 0) { snprintf(buf, len, "--:--:--"); return; }
                        int64_t s  = ms / 1000;
                        int hh = static_cast<int>((s / 3600) % 24);
                        int mm = static_cast<int>((s / 60)   % 60);
                        int sc = static_cast<int>(s % 60);
                        snprintf(buf, len, "%02d:%02d:%02d", hh, mm, sc);
                    };

                    struct Row { const char* lbl; int64_t ms; };
                    Row rows[] = {
                        {"C1",  ct.c1Ms},
                        {"C2",  ct.c2Ms},
                        {"Max", ct.maxMs},
                        {"C3",  ct.c3Ms},
                        {"C4",  ct.c4Ms},
                    };
                    bool first = true;
                    for (auto& r : rows) {
                        if (r.ms < 0) continue;
                        char tbuf[12];
                        fmtUtc(r.ms, tbuf, sizeof(tbuf));
                        ImGui::TextColored(kGray, "%s", r.lbl);
                        ImGui::SameLine(30.0f);
                        ImGui::TextColored(cdCol, "%s", tbuf);
                        if (first) {
                            ImGui::SameLine();
                            ImGui::TextColored(kDim, " %s", srcLabel);
                            first = false;
                        }
                    }
                } else {
                    ImGui::TextColored(kGray, "C1");
                    ImGui::SameLine(25.0f);
                    ImGui::TextColored(kDim, "err");
                }
            }
        }

        ImGui::PopFont();
    }

    // ════════════════════════════════
    // Section: CONNECTION
    // ════════════════════════════════
    ImGui::SeparatorText("CONNECTION");

    ImGui::PushFont(m_fontMono);
    if (connected)
        ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.30f, 1.0f), "● connected");
    else
        ImGui::TextColored(ImVec4(0.75f, 0.22f, 0.18f, 1.0f), "○ no server");
    ImGui::PopFont();

    ImGui::Spacing();

    const ImVec2 btnSz(-1, 34);

    {
        bool srvRunning = false;
        HANDLE hm = OpenMutexW(SYNCHRONIZE, FALSE, L"TotalControl_DaemonRunning");
        if (hm) { srvRunning = true; CloseHandle(hm); }

        if (srvRunning) ImGui::BeginDisabled();
        if (ImGui::Button("Connect cameras", btnSz)) {
            LogLine("user: Start TotalControlSRV");
            if (TryLaunchDaemon()) {
                m_lastResult = "SRV launched...";
                m_reconnectCountdown = 0;
            } else {
                m_lastResult = "ERROR: cannot launch TotalControlSRV.exe";
                LogLine(m_lastResult);
            }
        }
        if (srvRunning) ImGui::EndDisabled();
    }

    ImGui::Spacing();

    if (!connected) ImGui::BeginDisabled();
    if (ImGui::Button("Test picture", btnSz)) {
        LogLine("user: take test picture");
        const char* req =
            "{\"cmd\":\"shoot\",\"drive\":\"single\","
            "\"ss\":\"1/8000\",\"iso\":100,\"f\":8.0}";
        if (auto res = m_pipe.SendRequest(req)) {
            bool ok  = JStr(*res, "ok") == "true";
            int  lat = JInt(*res, "latency_ms", -1);
            // Store shot latency in camera[0] (Test picture targets camera[0])
            if (ok && lat >= 0 && !m_cameras.empty())
                m_cameras[0].lastShotMs = lat;
            m_lastResult = ok
                ? std::format("Shot OK — {} ms", lat >= 0 ? lat : 0)
                : "ERROR: shoot failed";
            LogLine(m_lastResult);
        } else {
            m_lastResult = std::format("ERROR: {}", PipeErrorMessage(res.error()));
            LogLine(m_lastResult);
        }
    }
    if (!connected) ImGui::EndDisabled();

    ImGui::Spacing();

    if (!connected) ImGui::BeginDisabled();
    if (ImGui::Button("Disconnect cameras", btnSz)) {
        LogLine("user: disconnect & quit SRV");
        (void)m_pipe.Send("{\"cmd\":\"quit\"}");
        m_pipe.Disconnect();
        m_cameras.clear();
        m_lastResult = "Server stopped.";
        LogLine("SRV quit sent");
    }
    if (!connected) ImGui::EndDisabled();

    // ════════════════════════════════
    // Section: CAMERA STATUS
    // ════════════════════════════════
    ImGui::Spacing();
    ImGui::SeparatorText("CAMERA STATUS");
    ImGui::Spacing();
    RenderCameraSection();

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ════════════════════════════════════════════════════════════════════════
    // COLUMN 2 — Eclipse (selector / observer / calculate / contact times)
    // ════════════════════════════════════════════════════════════════════════
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.065f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 14));
    ImGui::BeginChild("##col_ecl", ImVec2(colW2, totalH), false, 0);
    ImGui::PopStyleVar();

    // ECLIPSE section
    ImGui::SeparatorText("ECLIPSE");
    ImGui::Spacing();
    RenderEclipseSection();

    // Calculate button
    ImGui::Spacing();
    {
        int iqpSt = m_iqpState.load();
        bool loading = (iqpSt == 1);
        if (loading) ImGui::BeginDisabled();
        if (ImGui::Button(loading ? "Calculating..." : "Calculate Contacts", ImVec2(-1, 34)))
            TriggerIqpFetch();
        if (loading) ImGui::EndDisabled();
    }

    // Contact times table
    ImGui::Spacing();
    ImGui::SeparatorText("CONTACTS");
    ImGui::Spacing();
    RenderContactTimesSection();

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ════════════════════════════════════════════════════════════════════════
    // RIGHT AREA
    // ════════════════════════════════════════════════════════════════════════
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::BeginChild("##main", ImVec2(rightW, totalH), false,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    // ── Eclipse zone status banner ────────────────────────────────────────────
    {
        int iqpSt = m_iqpState.load();
        if (iqpSt == 1 || iqpSt == 2) {
            static const ImVec4 kGreen { 0.15f, 0.90f, 0.35f, 1.0f };
            static const ImVec4 kRed   { 0.95f, 0.22f, 0.18f, 1.0f };
            static const ImVec4 kDim2  { 0.40f, 0.40f, 0.45f, 1.0f };

            ImGui::PushFont(m_fontLarge);
            if (iqpSt == 1) {
                ImGui::TextColored(kDim2, "checking location...");
            } else {
                ContactTimes ct;
                { std::lock_guard lk(m_iqpMutex); ct = m_contacts; }
                if (!ct.apiOk) {
                    ImGui::TextColored(kDim2, "network error");
                } else if (!ct.valid) {
                    ImGui::TextColored(kRed,  "NO ECLIPSE VISIBLE HERE");
                } else if (ct.c2Ms > 0) {
                    ImGui::TextColored(kGreen, "YOU ARE IN THE TOTALITY ZONE");
                } else {
                    ImGui::TextColored(kRed,   "YOU ARE OUTSIDE TOTALITY ZONE");
                }
            }
            ImGui::PopFont();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }
    }

    if (!m_lastResult.empty()) {
        ImGui::PushFont(m_fontMono);
        bool ok = m_lastResult.find("ERROR") == std::string::npos;
        ImGui::TextColored(
            ok ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
               : ImVec4(1.0f,  0.35f, 0.25f, 1.0f),
            "%s", m_lastResult.c_str());
        ImGui::PopFont();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushFont(m_fontMono);
    ImGui::TextColored(ImVec4(0.40f, 0.40f, 0.45f, 1.0f), "dev tools");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::Checkbox("Style Editor", &m_showStyleEditor);
    ImGui::SameLine();
    ImGui::Checkbox("Demo Window",  &m_showDemoWindow);

    ImGui::EndChild();

    if (m_showStyleEditor) {
        ImGui::Begin("Style Editor", &m_showStyleEditor);
        ImGui::ShowStyleEditor();
        ImGui::End();
    }
    if (m_showDemoWindow)
        ImGui::ShowDemoWindow(&m_showDemoWindow);

    ImGui::End();

    // ── camera status polling (~every 2 s when connected) ────────────────
    if (connected) {
        if (m_statusCountdown <= 0) {
            PollCameraStatus();
            m_statusCountdown = 120;  // ~2 s at 60 fps
        } else {
            --m_statusCountdown;
        }
    } else {
        m_cameras.clear();
        m_statusCountdown = 0;
    }

    // ── silent auto-reconnect ─────────────────────────────────────────────
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
