#include "App.h"
#include "TzEntry.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <windows.h>
#include <chrono>
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
    )SQL");
}

// ─── Persistent settings ──────────────────────────────────────────────────────

void App::SaveClockSettings() {
    m_configDb.SetSettingInt("show_home_clock", m_showHomeClock ? 1 : 0);
    m_configDb.SetSettingInt("show_ecl_clock",  m_showEclClock  ? 1 : 0);
    m_configDb.SetSetting("home_tz_iana", m_homeTzIana.c_str());
    m_configDb.SetSetting("ecl_tz_iana",  m_eclTzIana.c_str());
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
        LogLine(std::format("Config DB: home={}  ecl={}", m_homeTzIana, m_eclTzIana));
    } else {
        LogLine("WARNING: cannot open TotalControlConfig.db — settings not persisted");
    }

    // ── TotalControlData.db — reference data (read-only, optional) ────────
    std::wstring dataPath = dir + L"\\TotalControlData.db";
    if (std::filesystem::exists(dataPath)) {
        if (m_dataDb.OpenReadOnly(dataPath)) {
            m_tzList = m_dataDb.LoadTimezones();
            LogLine(std::format("Data DB: opened — {} timezones loaded", m_tzList.size()));
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
}

App::~App() {
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

    const float sideW  = 200.0f;
    const float totalH = io.DisplaySize.y;
    const float rightW = io.DisplaySize.x - sideW;

    bool connected = (m_pipe.GetState() == PipeClient::State::Connected);

    // ════════════════════════════════════════════════════════════════════════
    // LEFT PANEL
    // ════════════════════════════════════════════════════════════════════════
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.07f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 14));
    ImGui::BeginChild("##side", ImVec2(sideW, totalH), false,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    // ── app name ─────────────────────────────────────────────────────────
    ImGui::PushFont(m_fontMono);
    ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.92f, 1.0f), "TotalControl");
    ImGui::PopFont();

    ImGui::Spacing();

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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── connection status ─────────────────────────────────────────────────
    ImGui::PushFont(m_fontMono);
    if (connected)
        ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.30f, 1.0f), "● connected");
    else
        ImGui::TextColored(ImVec4(0.75f, 0.22f, 0.18f, 1.0f), "○ no server");
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── buttons ───────────────────────────────────────────────────────────
    const ImVec2 btnSz(-1, 38);

    {
        bool srvRunning = false;
        HANDLE hm = OpenMutexW(SYNCHRONIZE, FALSE, L"TotalControl_DaemonRunning");
        if (hm) { srvRunning = true; CloseHandle(hm); }

        if (srvRunning) ImGui::BeginDisabled();
        if (ImGui::Button("Connect cameras", btnSz)) {
            LogLine("user: Start TotalControlSRV");
            if (TryLaunchDaemon()) {
                m_lastResult = "SRV launched — connecting...";
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
        std::string resp;
        if (auto res = m_pipe.SendRequest(req)) {
            m_lastResult = *res;
            LogLine(std::format("shoot: {}", *res));
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
        m_lastResult = "Server stopped.";
        LogLine("SRV quit sent");
    }
    if (!connected) ImGui::EndDisabled();

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
