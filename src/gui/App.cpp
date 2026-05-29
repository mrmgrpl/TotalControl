#include "App.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <windows.h>
#include <cstdio>
#include <chrono>
#include <thread>

namespace TotalControl {

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    auto slash = s.rfind(L'\\');
    return (slash != std::wstring::npos) ? s.substr(0, slash) : s;
}

static int64_t UtcNowMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ui;
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(ui.QuadPart / 10000) - 11644473600000LL;
}

static void FormatUtcHms(int64_t ms, char* buf, int bufLen) {
    int64_t s   = ms / 1000;
    int     mss = static_cast<int>(ms % 1000);
    int     hh  = static_cast<int>((s / 3600) % 24);
    int     mm  = static_cast<int>((s / 60) % 60);
    int     sc  = static_cast<int>(s % 60);
    snprintf(buf, bufLen, "%02d:%02d:%02d.%03d", hh, mm, sc, mss);
}

// ─── Logging ─────────────────────────────────────────────────────────────────

void App::LogLine(const char* msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[32];
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::lock_guard<std::mutex> lk(m_logMutex);
    if (m_logFile.is_open()) {
        m_logFile << ts << "  " << msg << "\n";
        m_logFile.flush();
    }
}

// ─── App lifecycle ────────────────────────────────────────────────────────────

App::App() {
    std::wstring logPath = ExeDir() + L"\\TotalControlGUI.log";
    m_logFile.open(logPath, std::ios::app);
    LogLine("=== TotalControlGUI start ===");
}

App::~App() {
    if (m_pipe.GetState() == PipeClient::State::Connected) {
        std::string resp;
        m_pipe.SendRequest("{\"cmd\":\"quit\"}", resp);
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

void App::OnFrame() {
    ImGuiIO& io = ImGui::GetIO();

    // ── full-screen host (no decoration, no padding) ─────────────────────────
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##host", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    const float sideW    = 200.0f;
    const float totalH   = io.DisplaySize.y;
    const float rightW   = io.DisplaySize.x - sideW;

    bool connected = (m_pipe.GetState() == PipeClient::State::Connected);

    // ════════════════════════════════════════════════════════════════════════
    // LEFT PANEL
    // ════════════════════════════════════════════════════════════════════════
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.07f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 14));
    ImGui::BeginChild("##side", ImVec2(sideW, totalH), false,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    // ── app name ─────────────────────────────────────────────────────────────
    ImGui::PushFont(m_fontMono);
    ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.92f, 1.0f), "TotalControl");
    ImGui::PopFont();

    ImGui::Spacing();

    // ── UTC clock (updates every frame) ──────────────────────────────────────
    {
        char clockBuf[20];
        FormatUtcHms(UtcNowMs(), clockBuf, sizeof(clockBuf));
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(ImVec4(0.90f, 0.75f, 0.20f, 1.0f), "%s", clockBuf);
        ImGui::TextColored(ImVec4(0.40f, 0.40f, 0.45f, 1.0f), "UTC");
        ImGui::PopFont();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── connection status ─────────────────────────────────────────────────────
    ImGui::PushFont(m_fontMono);
    if (connected)
        ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.30f, 1.0f), "● connected");
    else
        ImGui::TextColored(ImVec4(0.75f, 0.22f, 0.18f, 1.0f), "○ no server");
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── buttons ───────────────────────────────────────────────────────────────
    const ImVec2 btnSz(-1, 38);  // full panel width

    // Button 1 — Start TotalControlSRV
    {
        bool srvRunning = false;
        HANDLE hm = OpenMutexW(SYNCHRONIZE, FALSE, L"TotalControl_DaemonRunning");
        if (hm) { srvRunning = true; CloseHandle(hm); }

        if (srvRunning) ImGui::BeginDisabled();
        if (ImGui::Button("Start SRV", btnSz)) {
            LogLine("user: Start TotalControlSRV");
            if (TryLaunchDaemon()) {
                m_lastResult = "SRV launched — connecting...";
                m_reconnectCountdown = 0;
            } else {
                m_lastResult = "ERROR: cannot launch TotalControlSRV.exe";
                LogLine(m_lastResult.c_str());
            }
        }
        if (srvRunning) ImGui::EndDisabled();
    }

    ImGui::Spacing();

    // Button 2 — Take test picture
    if (!connected) ImGui::BeginDisabled();
    if (ImGui::Button("Test picture", btnSz)) {
        LogLine("user: take test picture");
        const char* req =
            "{\"cmd\":\"shoot\",\"drive\":\"single\","
            "\"ss\":\"1/8000\",\"iso\":100,\"f\":8.0}";
        std::string resp;
        if (m_pipe.SendRequest(req, resp)) {
            m_lastResult = resp;
            LogLine(("shoot: " + resp).c_str());
        } else {
            m_lastResult = "ERROR: pipe send failed";
            LogLine(m_lastResult.c_str());
        }
    }
    if (!connected) ImGui::EndDisabled();

    ImGui::Spacing();

    // Button 3 — Disconnect and close server
    if (!connected) ImGui::BeginDisabled();
    if (ImGui::Button("Disconnect & quit", btnSz)) {
        LogLine("user: disconnect & quit SRV");
        std::string resp;
        m_pipe.SendRequest("{\"cmd\":\"quit\"}", resp);
        m_pipe.Disconnect();
        m_lastResult = "Server stopped.";
        LogLine("SRV quit sent");
    }
    if (!connected) ImGui::EndDisabled();

    ImGui::EndChild();
    ImGui::PopStyleColor(); // ChildBg

    // ════════════════════════════════════════════════════════════════════════
    // RIGHT AREA  (content will grow here in future steps)
    // ════════════════════════════════════════════════════════════════════════
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::BeginChild("##main", ImVec2(rightW, totalH), false,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    // last result feedback
    if (!m_lastResult.empty()) {
        ImGui::PushFont(m_fontMono);
        bool ok = m_lastResult.find("ERROR") == std::string::npos;
        ImGui::TextColored(
            ok ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
               : ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
            "%s", m_lastResult.c_str());
        ImGui::PopFont();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── dev tools toggles ────────────────────────────────────────────────────
    ImGui::PushFont(m_fontMono);
    ImGui::TextColored(ImVec4(0.40f, 0.40f, 0.45f, 1.0f), "dev tools");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::Checkbox("Style Editor", &m_showStyleEditor);
    ImGui::SameLine();
    ImGui::Checkbox("Demo Window",  &m_showDemoWindow);

    ImGui::EndChild();

    // ── floating ImGui tool windows ──────────────────────────────────────────
    if (m_showStyleEditor) {
        ImGui::Begin("Style Editor", &m_showStyleEditor);
        ImGui::ShowStyleEditor();
        ImGui::End();
    }
    if (m_showDemoWindow)
        ImGui::ShowDemoWindow(&m_showDemoWindow);

    ImGui::End();

    // ── silent auto-reconnect ────────────────────────────────────────────────
    if (!connected) {
        if (m_reconnectCountdown <= 0) {
            m_pipe.Connect();
            m_reconnectCountdown = 60;
        } else {
            --m_reconnectCountdown;
        }
    }
}

bool App::OnCloseRequest() {
    return true;
}

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
