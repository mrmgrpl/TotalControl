#include "App.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <algorithm>
#include <sstream>
#include <thread>
#include <chrono>

namespace TotalControl {

// ─── JSON helpers (minimal, no dependencies) ──────────────────────────────────

static std::string JStr(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return json.substr(pos + 1, end - pos - 1);
}

static int JInt(const std::string& json, const std::string& key, int def = 0) {
    std::string pat = "\"" + key + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return def;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return def;
    try { return std::stoi(json.substr(pos)); } catch (...) { return def; }
}


// ─── UTC helpers ──────────────────────────────────────────────────────────────

static int64_t UtcNowMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ui;
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(ui.QuadPart / 10000) - 11644473600000LL;
}

static std::string FormatUtcHms(int64_t ms) {
    if (ms <= 0) return "??:??:??.???";
    int64_t s   = ms / 1000;
    int     mss = static_cast<int>(ms % 1000);
    int     hh  = static_cast<int>((s / 3600) % 24);
    int     mm  = static_cast<int>((s / 60) % 60);
    int     ss  = static_cast<int>(s % 60);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", hh, mm, ss, mss);
    return buf;
}

// ─── ExeDir ───────────────────────────────────────────────────────────────────

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    auto slash = s.rfind(L'\\');
    return (slash != std::wstring::npos) ? s.substr(0, slash) : s;
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

// ─── App ──────────────────────────────────────────────────────────────────────

App::App() {
    std::wstring logPath = ExeDir() + L"\\TotalControlGUI.log";
    m_logFile.open(logPath, std::ios::app);  // append mode — accumulate across sessions
    LogLine("=== TotalControlGUI start ===");
}

App::~App() {
    StopPollThread();
    if (m_pipe.GetState() == PipeClient::State::Connected)
        m_pipe.Send("{\"cmd\":\"quit\"}");
    m_pipe.Disconnect();
    LogLine("=== TotalControlGUI exit ===");
}

void App::OnInit() {
    ImGuiIO& io = ImGui::GetIO();

    // Monospace font for data values
    m_fontMono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 15.0f);
    // Fallback: default font
    if (!m_fontMono) m_fontMono = io.FontDefault;

    // Larger font for countdown / key values
    m_fontLarge = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 22.0f);
    if (!m_fontLarge) m_fontLarge = io.FontDefault;

    io.Fonts->Build();

    // Style — dark mission-control theme
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.GrabRounding      = 3.0f;
    style.FramePadding      = ImVec2(6, 4);
    style.ItemSpacing       = ImVec2(8, 5);
    style.WindowBorderSize  = 1.0f;

    // Accent colours: amber/orange for active, red for error
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]        = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_TitleBg]         = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]   = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_Header]          = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_HeaderHovered]   = ImVec4(0.26f, 0.26f, 0.32f, 1.00f);
    colors[ImGuiCol_Button]          = ImVec4(0.20f, 0.20f, 0.28f, 1.00f);
    colors[ImGuiCol_ButtonHovered]   = ImVec4(0.30f, 0.30f, 0.40f, 1.00f);
    colors[ImGuiCol_ButtonActive]    = ImVec4(0.14f, 0.45f, 0.80f, 1.00f);
    colors[ImGuiCol_FrameBg]         = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_CheckMark]       = ImVec4(0.90f, 0.60f, 0.10f, 1.00f);
    colors[ImGuiCol_SliderGrab]      = ImVec4(0.90f, 0.60f, 0.10f, 1.00f);

    StartPollThread();
}

void App::OnFrame() {
    // Full-screen invisible host window
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##host", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove     |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar);

    DrawStatusBar();
    ImGui::Separator();

    const float panelW = io.DisplaySize.x * 0.35f;
    const float contentH = io.DisplaySize.y - 44.0f; // below status bar

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);

    // Left column: connection + camera status
    ImGui::BeginChild("##left", ImVec2(panelW, contentH), false);
    DrawConnectionPanel();
    ImGui::Spacing();
    DrawCameraPanel();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##right", ImVec2(0, contentH), false);
    DrawSequencerPanel();
    ImGui::EndChild();

    ImGui::End();

    // Auto-reconnect pipe only — never auto-launch SRV (user must press "Launch SRV")
    if (m_pipe.GetState() == PipeClient::State::Disconnected) {
        if (m_reconnectCountdown <= 0) {
            if (m_pipe.Connect())
                LogLine("pipe: reconnected");
            m_reconnectCountdown = 60; // retry every ~1s at 60fps
        } else {
            --m_reconnectCountdown;
        }
    }
}

bool App::OnCloseRequest() {
    return true; // allow close
}

// ─── Status bar ───────────────────────────────────────────────────────────────

void App::DrawStatusBar() {
    // UTC clock
    int64_t now = UtcNowMs();
    std::string utc = "UTC  " + FormatUtcHms(now);

    // Connection indicator
    PipeClient::State connState = m_pipe.GetState();
    const char* connLabel = "SRV  DISCONNECTED";
    ImVec4 connColor = ImVec4(0.85f, 0.20f, 0.20f, 1.0f);
    if (connState == PipeClient::State::Connected) {
        connLabel = "SRV  CONNECTED";
        connColor = ImVec4(0.20f, 0.85f, 0.30f, 1.0f);
    }

    ImGui::PushFont(m_fontMono);
    ImGui::TextColored(ImVec4(0.90f, 0.75f, 0.20f, 1.0f), "%s", utc.c_str());
    ImGui::SameLine(0, 30);
    ImGui::TextColored(connColor, "%s", connLabel);
    ImGui::SameLine(0, 30);
    ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.60f, 1.0f), "TotalControl 2026  |  Sony ILCE-7RM4A  |  TSE 2026-08-12 Burgos");
    ImGui::PopFont();
}

// ─── Connection panel ─────────────────────────────────────────────────────────

void App::DrawConnectionPanel() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
    ImGui::BeginChild("##conn", ImVec2(0, 100), true);

    ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.75f, 1.0f), "DAEMON");
    ImGui::Separator();
    ImGui::Spacing();

    PipeClient::State st = m_pipe.GetState();

    if (st == PipeClient::State::Connected) {
        ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.30f, 1.0f), "Connected to TotalControlSRV");
        if (ImGui::Button("Disconnect")) {
            LogLine("user: disconnect");
            StopPollThread();
            m_pipe.Disconnect();
            StartPollThread();
        }
    } else {
        ImGui::TextColored(ImVec4(0.85f, 0.40f, 0.20f, 1.0f), "Not connected");
        ImGui::SameLine();
        if (ImGui::Button("Connect")) {
            if (m_pipe.Connect())
                LogLine("user: connect OK");
            else
                LogLine("user: connect failed");
        }
        ImGui::SameLine();
        if (ImGui::Button("Launch SRV")) {
            LogLine("user: launch SRV");
            if (TryLaunchDaemon())
                m_reconnectCountdown = 90; // give SRV ~1.5s to start before reconnect
            else
                LogLine("launch SRV: CreateProcess failed");
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ─── Camera status panel ──────────────────────────────────────────────────────

void App::DrawCameraPanel() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
    ImGui::BeginChild("##cam", ImVec2(0, 340), true);

    ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.75f, 1.0f), "CAMERA STATUS");
    ImGui::Separator();
    ImGui::Spacing();

    CameraStatus s;
    {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        s = m_camStatus;
    }

    if (!s.valid) {
        ImGui::TextDisabled("No camera data");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    auto row = [&](const char* label, const std::string& val, ImVec4 col = ImVec4(0.85f, 0.85f, 0.85f, 1.0f)) {
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.60f, 1.0f), "%-12s", label);
        ImGui::SameLine();
        ImGui::TextColored(col, "%s", val.c_str());
        ImGui::PopFont();
    };

    // Color coding for important values
    auto ssColor = [&]() {
        // Warn if shutter speed is very slow (might cause blur)
        return ImVec4(0.90f, 0.75f, 0.20f, 1.0f);
    };

    row("Battery",  s.battery,  s.battery.find("low") != std::string::npos
                                ? ImVec4(1.0f, 0.3f, 0.2f, 1.0f)
                                : ImVec4(0.30f, 0.85f, 0.40f, 1.0f));
    row("Mode",     s.mode);
    row("Shutter",  s.ss,       ssColor());
    row("ISO",      s.iso,      ImVec4(0.90f, 0.75f, 0.20f, 1.0f));
    row("f/",       s.fnumber,  ImVec4(0.90f, 0.75f, 0.20f, 1.0f));
    row("Focus",    s.focus);
    row("Drive",    s.drive);
    ImGui::Spacing();
    row("Card 1",   s.card1,    s.card1 == "ok" ? ImVec4(0.30f, 0.85f, 0.40f, 1.0f)
                                                 : ImVec4(0.85f, 0.30f, 0.20f, 1.0f));
    row("Card 2",   s.card2,    s.card2 == "ok" ? ImVec4(0.30f, 0.85f, 0.40f, 1.0f)
                                                 : ImVec4(0.85f, 0.30f, 0.20f, 1.0f));
    row("Store",    s.store);
    row("WB",       s.wb);

    if (!s.error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.2f, 1.0f), "ERR: %s", s.error.c_str());
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ─── Sequencer panel ──────────────────────────────────────────────────────────

void App::DrawSequencerPanel() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
    ImGui::BeginChild("##seq", ImVec2(0, 0), true);

    ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.75f, 1.0f), "SEQUENCER");
    ImGui::Separator();
    ImGui::Spacing();

    SeqStatus ss;
    {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        ss = m_seqStatus;
    }

    if (!ss.active) {
        ImGui::TextDisabled("No sequence loaded");
    } else {
        ImGui::PushFont(m_fontLarge);
        // Countdown to next step
        if (ss.nextAtMs > 0) {
            int64_t nowMs = UtcNowMs();
            int64_t delta = ss.nextAtMs - nowMs;
            char cdbuf[32];
            if (delta > 0) {
                int64_t absMs = delta;
                int hh = static_cast<int>(absMs / 3600000);
                int mm = static_cast<int>((absMs % 3600000) / 60000);
                int sc = static_cast<int>((absMs % 60000) / 1000);
                int ms = static_cast<int>(absMs % 1000);
                if (hh > 0)
                    snprintf(cdbuf, sizeof(cdbuf), "T-%02d:%02d:%02d.%03d", hh, mm, sc, ms);
                else
                    snprintf(cdbuf, sizeof(cdbuf), "T-%02d:%02d.%03d", mm, sc, ms);
                ImGui::TextColored(ImVec4(0.90f, 0.75f, 0.20f, 1.0f), "%s", cdbuf);
            } else {
                snprintf(cdbuf, sizeof(cdbuf), "T+%03lldms", (long long)(-delta));
                ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.30f, 1.0f), "%s", cdbuf);
            }
        }
        ImGui::PopFont();

        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.75f, 1.0f),
            "State: %s   Steps: %d / %d",
            ss.state.c_str(), ss.stepsDone, ss.stepsTotal);
        if (!ss.nextLabel.empty())
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f),
                "Next: %s", ss.nextLabel.c_str());
        ImGui::PopFont();

        ImGui::Spacing();
        if (ImGui::Button("Stop Sequence")) {
            std::string resp;
            m_pipe.SendRequest("{\"cmd\":\"seq_stop\"}", resp);
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ─── Poll thread ──────────────────────────────────────────────────────────────

void App::StartPollThread() {
    m_pollRun.store(true);
    m_pollThread = std::thread([this] { PollStatus(); });
}

void App::StopPollThread() {
    m_pollRun.store(false);
    if (m_pollThread.joinable()) m_pollThread.join();
}

void App::PollStatus() {
    PipeClient::State lastState = PipeClient::State::Disconnected;

    while (m_pollRun.load()) {
        // Log state transitions
        PipeClient::State curState = m_pipe.GetState();
        if (curState != lastState) {
            LogLine(curState == PipeClient::State::Connected
                ? "pipe: connected" : "pipe: disconnected");
            lastState = curState;
        }

        if (curState == PipeClient::State::Connected) {
            std::string resp;

            // Camera status
            if (m_pipe.SendRequest("{\"cmd\":\"status\"}", resp)) {
                CameraStatus cs;
                cs.valid    = JStr(resp, "ok") == "true" || resp.find("\"battery\"") != std::string::npos;
                cs.guid     = JStr(resp, "cam");
                cs.battery  = JStr(resp, "battery");
                cs.mode     = JStr(resp, "mode");
                cs.ss       = JStr(resp, "ss");
                cs.iso      = JStr(resp, "iso");
                cs.fnumber  = JStr(resp, "f");
                cs.focus    = JStr(resp, "focus");
                cs.drive    = JStr(resp, "drive");
                cs.card1    = JStr(resp, "card1");
                cs.card2    = JStr(resp, "card2");
                cs.store    = JStr(resp, "store");
                cs.wb       = JStr(resp, "wb");
                cs.cam_time = JStr(resp, "cam_time");
                cs.error    = JStr(resp, "error");
                if (!cs.battery.empty()) cs.valid = true;

                std::lock_guard<std::mutex> lk(m_statusMutex);
                m_camStatus = cs;
            }

            // Seq status
            if (m_pipe.SendRequest("{\"cmd\":\"seq_status\"}", resp)) {
                SeqStatus sq;
                sq.state      = JStr(resp, "state");
                sq.active     = (sq.state == "running");
                sq.stepsDone  = JInt(resp, "steps_done");
                sq.stepsTotal = JInt(resp, "steps_total");
                sq.nextLabel  = JStr(resp, "next_label");
                std::string nextAt = JStr(resp, "next_at");
                // next_at is ISO-8601 string; just store for display — countdown computed from pipe ms field
                sq.nextAtMs   = 0;
                std::string nextAtMs = JStr(resp, "next_at_ms");
                if (!nextAtMs.empty()) {
                    try { sq.nextAtMs = std::stoll(nextAtMs); } catch (...) {}
                }

                std::lock_guard<std::mutex> lk(m_statusMutex);
                m_seqStatus = sq;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// ─── Daemon launch ────────────────────────────────────────────────────────────

bool App::TryLaunchDaemon() {
    // Check singleton mutex — daemon may already be running
    HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, L"TotalControl_DaemonRunning");
    if (hMutex) {
        CloseHandle(hMutex);
        return true; // already running, just need to reconnect
    }

    std::wstring dir  = ExeDir();
    std::wstring exe  = dir + L"\\TotalControlSRV.exe";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(
        exe.c_str(), nullptr,
        nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE,
        nullptr, dir.c_str(),
        &si, &pi);

    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    return false;
}

} // namespace TotalControl
