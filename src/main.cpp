#include "CameraController.h"
#include "SequencerEngine.h"
#include "daemon/PipeServer.h"
#include "daemon/CommandHandler.h"

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ─── File log ─────────────────────────────────────────────────────────────────
static std::ofstream g_logFile;
static std::mutex    g_logMutex;
static bool          g_dotMode = false;  // true when last console output was "."

static std::string WtoU8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// File + console + DebugView. Breaks line first if console was in dot mode.
static void LogLine(const wchar_t* msg) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t ts[16];
    swprintf_s(ts, L"%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::wstring line = std::wstring(ts) + L"  " + msg;

    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile.is_open()) { g_logFile << WtoU8(line) << "\n"; g_logFile.flush(); }
    if (g_dotMode) { wprintf(L"\n"); g_dotMode = false; }
    wprintf(L"%s\n", line.c_str());
    ::OutputDebugStringW((line + L"\n").c_str());
}

// File only (no console) — for seq_status polling.
static void LogFileOnly(const wchar_t* msg) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t ts[16];
    swprintf_s(ts, L"%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::wstring line = std::wstring(ts) + L"  " + msg;
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile.is_open()) { g_logFile << WtoU8(line) << "\n"; g_logFile.flush(); }
}

// Prints '.' to console without newline — avoids seq_status log flood.
static void ConsoleDot() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_dotMode = true;
    wprintf(L".");
    fflush(stdout);
}

// File + console, no timestamp — used for the startup logo banner.
static void LogBanner(const wchar_t* msg) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile.is_open()) { g_logFile << WtoU8(msg) << "\n"; g_logFile.flush(); }
    wprintf(L"%s\n", msg);
    ::OutputDebugStringW((std::wstring(msg) + L"\n").c_str());
}

// Timestamp log in bright red — for critical startup warnings.
static void LogWarning(const wchar_t* msg) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t ts[16];
    swprintf_s(ts, L"%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::wstring line = std::wstring(ts) + L"  " + msg;
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile.is_open()) { g_logFile << WtoU8(line) << "\n"; g_logFile.flush(); }
    if (g_dotMode) { wprintf(L"\n"); g_dotMode = false; }
    wprintf(L"\033[91m%s\033[0m\n", line.c_str());  // bright red on console
    ::OutputDebugStringW((line + L"\n").c_str());
}

// ─── Exe directory helper ────────────────────────────────────────────────────
static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return L"";
    wchar_t* const sep = wcsrchr(buf, L'\\');
    if (sep != nullptr) sep[1] = L'\0';  // array indexing, not pointer arithmetic
    return buf;
}

// ─── Version ─────────────────────────────────────────────────────────────────
static constexpr wchar_t kVersion[] = L"2026.05.28";

// ─── Startup progress bar ─────────────────────────────────────────────────────
// Two separate phases: camera search and camera connect, each targeting 25 s = 100%.
static constexpr int     kEnumTimeoutSec  = 5;      // Enumerate() timeout
static constexpr int     kConnectTimeoutMs = 8000;  // Connect() callback timeout
static constexpr int64_t kExpectedPhaseMs = 25000;  // 100% target per phase

static std::atomic<bool>                     g_progressActive{false};
static std::chrono::steady_clock::time_point g_progStart;

static void RenderProgress(int64_t elapsedMs) {
    constexpr int W = 36;
    double frac = static_cast<double>(elapsedMs) / static_cast<double>(kExpectedPhaseMs);
    if (frac > 1.0) frac = 1.0;
    const int filled = static_cast<int>(frac * W + 0.5);
    wchar_t bar[W + 4];
    bar[0] = L'[';
    for (int i = 0; i < W; ++i)
        bar[1 + i] = (i < filled) ? L'█' : L'░';
    bar[1 + W] = L']';
    bar[2 + W] = L'\0';
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_dotMode = true;
    wprintf(L"\r  %s %3d%%  %2lld s", bar,
            static_cast<int>(frac * 100.0 + 0.5), elapsedMs / 1000LL);
    fflush(stdout);
}

static void ClearProgressLine() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    wprintf(L"\r%-80s\r", L"");  // erase progress line
    g_dotMode = false;
    fflush(stdout);
}

// ─── Singleton mutex — prevents a second SRV instance ────────
static HANDLE g_singletonMutex = nullptr;

// ─── Graceful shutdown on console close / Ctrl+C ─────────────────────────────
static TotalControl::PipeServer*                       g_server       = nullptr;
static std::vector<TotalControl::CameraController*>*   g_cams         = nullptr;
static TotalControl::SequencerEngine*                  g_seq          = nullptr;
static HANDLE                                          g_shutdownDone = nullptr;

static BOOL WINAPI CtrlHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        LogLine(L"Signal — initiating shutdown...");
        if (g_seq)  g_seq->Stop();
        if (g_cams)
            for (auto* c : *g_cams) c->RequestShutdown();
        if (g_server) g_server->Stop();
        if (g_shutdownDone) WaitForSingleObject(g_shutdownDone, 5000);
        return TRUE;
    default:
        return FALSE;
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main() {
    // JSON numbers always use '.' as the decimal separator. Without this, a
    // Polish (or other comma-decimal) system locale makes std::stof/std::stod
    // stop parsing at the '.' — e.g. "0.3" silently becomes 0.0 — corrupting
    // bracket EV steps, f-number, shutter speed, and every other JSON float.
    std::setlocale(LC_ALL, "C");
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);

    // Enable ANSI colour codes (required for LogWarning red output on Windows 10+)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode))
        SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    const std::wstring logPath = ExeDir() + L"TotalControlSRV.log";
    g_logFile.open(logPath, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!g_logFile.is_open()) {
        ::OutputDebugStringW(L"WARN: cannot open log file\n");
    } else {
        g_logFile.write("\xEF\xBB\xBF", 3);
    }

    LogBanner(L"        _______      _        _  _____            _             _  ");
    LogBanner(L"       |__   __|    | |      | |/ ____|          | |           | | ");
    LogBanner(L"          | |  ___  | |_ __ _| | |     ___  _ __ | |_ _ __ ___ | | ");
    LogBanner(L"          | | / _ \\ | __/ _` | | |    / _ \\| '_ \\| __| '__/ _ \\| | ");
    LogBanner(L"          | || (_) || || (_| | | |___| (_) | | | | | | | | (_) | | ");
    LogBanner(L"          |_| \\___/ |_| \\__,_|_|\\_________/|_| |_|_| |_|  \\___/|_| ");
    LogBanner(L"                                                                   ");	
    {
        wchar_t buf[128];
        swprintf_s(buf, L"  v%s  |  pipe: \\\\.\\pipe\\TotalControl  |  stop: TotalControlCLI quit", kVersion);
        LogBanner(buf);
    }
    LogBanner(L"");

    // ── Singleton — reject second instance of SRV ─────────────────────────────
    assert(g_singletonMutex == nullptr);  // main() must not be called more than once in a process
    g_singletonMutex = CreateMutexW(nullptr, TRUE, L"TotalControl_DaemonRunning");
    if (g_singletonMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        LogLine(L"ERROR: daemon already running — second instance rejected");
        if (g_singletonMutex) CloseHandle(g_singletonMutex);
        return 4;
    }

    // ── Init SDK — no progress bar, completes in < 1 s ───────────────────────
    LogLine(L"Initializing SDK...");
    if (!TotalControl::CameraController::InitSDK()) {
        LogLine(L"ERROR: SDK::Init failed");
        return 1;
    }

    // ── Camera search — progress bar (100% = 25 s) ───────────────────────────
    LogLine(L"Searching for cameras...");
    g_progressActive = true;
    g_progStart = std::chrono::steady_clock::now();
    std::thread progressSearch([&]() {
        while (g_progressActive) {
            const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - g_progStart).count();
            RenderProgress(ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    });

    auto cameraList = TotalControl::CameraController::Enumerate(kEnumTimeoutSec);

    g_progressActive = false;
    progressSearch.join();
    ClearProgressLine();

    if (cameraList.empty()) {
        LogLine(L"ERROR: no cameras found");
        TotalControl::CameraController::ReleaseSDK();
        return 2;
    }
    {
        wchar_t buf[128];
        swprintf_s(buf, L"Found %zu camera(s):", cameraList.size());
        LogLine(buf);
        for (size_t i = 0; i < cameraList.size(); ++i) {
            swprintf_s(buf, L"  [%zu] %s  guid=%s",
                       i, cameraList[i].model.c_str(), cameraList[i].guid.c_str());
            LogLine(buf);
        }
    }

    // ── Camera connect — progress bar (100% = 25 s) ──────────────────────────
    std::vector<std::unique_ptr<TotalControl::CameraController>> camOwners;

    g_progressActive = true;
    g_progStart = std::chrono::steady_clock::now();
    std::thread progressConnect([&]() {
        while (g_progressActive) {
            const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - g_progStart).count();
            RenderProgress(ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    });

    for (size_t i = 0; i < cameraList.size(); ++i) {
        const auto& info = cameraList[i];

        auto logFn = [i](const wchar_t* msg) {
            wchar_t buf[1024];
            swprintf_s(buf, L"[CAM%zu] %s", i, msg);
            LogLine(buf);
        };

        auto cam = std::make_unique<TotalControl::CameraController>(logFn);
        if (!cam->Init()) {
            wchar_t buf[128];
            swprintf_s(buf, L"ERROR: camera Init failed [%zu] %s", i, info.model.c_str());
            LogLine(buf);
            continue;
        }
        if (!cam->Connect(info.guid.c_str(), kEnumTimeoutSec, kConnectTimeoutMs)) {
            wchar_t buf[128];
            swprintf_s(buf, L"ERROR: camera Connect failed [%zu] %s", i, info.model.c_str());
            LogLine(buf);
            cam->Shutdown();
            continue;
        }
        camOwners.push_back(std::move(cam));
    }

    g_progressActive = false;
    progressConnect.join();
    ClearProgressLine();

    if (camOwners.empty()) {
        LogLine(L"ERROR: no cameras connected");
        TotalControl::CameraController::ReleaseSDK();
        return 3;
    }

    {
        wchar_t buf[128];
        swprintf_s(buf, L"Connected %zu/%zu camera(s). Daemon ready.",
                   camOwners.size(), cameraList.size());
        LogLine(buf);
    }

    // ── Build pointer vector for CommandHandler ───────────────────────────
    std::vector<TotalControl::CameraController*> camPtrs;
    for (auto& c : camOwners) camPtrs.push_back(c.get());

    // ── Sequencer + CommandHandler ────────────────────────────────────────────
    TotalControl::CommandHandler handler(camPtrs);

    TotalControl::SequencerEngine seq(LogLine);
    seq.SetDispatch([&handler](const std::wstring& req, std::wstring& resp) {
        return handler.Handle(req, resp);
    });
    handler.SetSequencer(&seq);

    // ── Pipe server ───────────────────────────────────────────────────────────
    TotalControl::PipeServer server(
        L"\\\\.\\pipe\\TotalControl",
        [&handler](const std::wstring& req, std::wstring& resp) -> bool {
            const bool isStatus = req.find(L"\"seq_status\"") != std::wstring::npos;
            if (isStatus) {
                LogFileOnly((L"→ " + req).c_str());
                bool cont = handler.Handle(req, resp);
                LogFileOnly((L"← " + resp).c_str());
                ConsoleDot();
                return cont;
            }
            LogLine((L"→ " + req).c_str());
            bool cont = handler.Handle(req, resp);
            LogLine((L"← " + resp).c_str());
            return cont;
        }
    );

    g_shutdownDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_shutdownDone == nullptr) LogLine(L"WARN: CreateEventW failed");
    g_server = &server;
    g_cams   = &camPtrs;
    g_seq    = &seq;
    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE))
        LogLine(L"WARN: SetConsoleCtrlHandler failed");

    LogLine(L"Listening on pipe...");

    // ── Startup status check — verify settings and log camera state ──────────
    static constexpr int kCardWarnThresh = 900;  // warn if remaining <= 900

    for (size_t i = 0; i < camPtrs.size(); ++i) {
        TotalControl::CameraStatus cs = camPtrs[i]->GetStatus();
        const std::wstring& camGuid = camPtrs[i]->Guid();

        // ── Cam / host time formatting ─────────────────────────────────────────
        std::wstring camTimeDisp = L"n/a (USB — not supported)";
        wchar_t hostTimeDisp[32] = {};
        {
            const auto& ct = cs.camTime; const auto& ca = cs.camTimeArea;
            if (ct.size() >= 15) {
                camTimeDisp = ct.substr(0,4)+L'-'+ct.substr(4,2)+L'-'+ct.substr(6,2)
                            +L'T'+ct.substr(9,2)+L':'+ct.substr(11,2)+L':'+ct.substr(13,2);
                if (ct.size() > 16) {
                    wchar_t ms[8]; swprintf_s(ms, L".%c00", ct[16]);
                    camTimeDisp += ms;
                }
                camTimeDisp += (ca.size() >= 5) ? ca.substr(0,3)+L':'+ca.substr(3,2) : L"Z";
            }
            // Host UTC always available (system clock snapshot from GetStatus)
            const time_t t = static_cast<time_t>(cs.camTimeHostMs / 1000);
            struct tm utc{}; gmtime_s(&utc, &t);
            swprintf_s(hostTimeDisp, L"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                       utc.tm_year+1900, utc.tm_mon+1, utc.tm_mday,
                       utc.tm_hour, utc.tm_min, utc.tm_sec,
                       static_cast<int>(cs.camTimeHostMs % 1000));
        }

        // ── Startup status table ───────────────────────────────────────────────
        wchar_t row[256];
        swprintf_s(row, L"[CAM%zu] %s  guid=%s", i, cs.model.c_str(), camGuid.c_str());
        LogBanner(row);
        // f-number: manual integer+decimal to avoid locale comma (Polish: 5,6 → need 5.6)
        const int fInt = static_cast<int>(cs.fNumber);
        const int fDec = static_cast<int>((cs.fNumber - static_cast<float>(fInt)) * 10.f + 0.5f);
        swprintf_s(row, L"  Battery  : %d%% (%-8s)  Mode : %-4s  SS : %-10s  ISO : %-6d  f/%d.%d",
                   cs.batteryPct, cs.batteryLevel.c_str(),
                   cs.exposureMode.c_str(), cs.shutterSpeed.c_str(),
                   cs.iso, fInt, fDec);
        LogBanner(row);
        swprintf_s(row, L"  Focus    : %-14s  Area : %-12s  Drive : %s",
                   cs.focusMode.c_str(), cs.focusArea.c_str(), cs.driveMode.c_str());
        LogBanner(row);
        // remaining==0 at startup = SDK not yet reported (timing); mark as "?"
        auto CardDesc = [](int rem, const std::wstring& st) -> std::wstring {
            if (rem > 0)         return std::format(L"{} shots  [{}]", rem, st);
            if (st == L"no-card") return L"no card  [no-card]";
            return std::format(L"? shots  [{}]  (reading...)", st);
        };
        swprintf_s(row, L"  Card 1   : %-30s  Card 2 : %s",
                   CardDesc(cs.remainingShots, cs.slot1Status).c_str(),
                   CardDesc(cs.slot2Remaining, cs.slot2Status).c_str());
        LogBanner(row);
        swprintf_s(row, L"  Store    : %-8s  File : %-10s  Size : %-4s  WB : %-12s  Meter : %s",
                   cs.storeDestination.c_str(), cs.fileType.c_str(), cs.imageSize.c_str(),
                   cs.whiteBalance.c_str(), cs.metering.c_str());
        LogBanner(row);
        swprintf_s(row, L"  Cam time : %s", camTimeDisp.c_str());
        LogBanner(row);
        swprintf_s(row, L"  Host UTC : %s", hostTimeDisp);
        LogBanner(row);

        // ── Exposure mode — must be M ──────────────────────────────────────────
        if (cs.exposureMode != L"M") {
            wchar_t buf[128];
            swprintf_s(buf, L"*** WARNING: camera [%zu] is in mode=%s, not M! ***",
                       i, cs.exposureMode.c_str());
            LogWarning(L"╔══════════════════════════════════════════════════════╗");
            LogWarning(buf);
            LogWarning(L"║  Set camera dial to M and restart the server.       ║");
            LogWarning(L"╚══════════════════════════════════════════════════════╝");
        }

        // ── Battery ───────────────────────────────────────────────────────────
        if (cs.batteryPct > 0 && cs.batteryPct < 90) {
            wchar_t buf[128];
            swprintf_s(buf, L"*** WARNING: battery %d%% — charge to 100%% before eclipse! ***",
                       cs.batteryPct);
            LogWarning(buf);
        }

        // ── Card capacity — warn if remaining <= 900 ──────────────────────────
        // "no-card" only when both slot_status AND remaining==0 confirm it
        // (slot_status alone is unreliable on ILCE-7RM4A — returns 0 even with card present)
        // remaining==0 with status=="ok" means SDK hasn't delivered the count yet (startup
        // timing) — treat as "unknown" and skip the low-capacity warning to avoid false alarm.
        if (cs.slot1Status == L"no-card" && cs.remainingShots == 0) {
            LogWarning(L"*** WARNING: card slot 1 — no card inserted! ***");
        } else if (cs.remainingShots > 0 && cs.remainingShots <= kCardWarnThresh) {
            wchar_t buf[128];
            swprintf_s(buf, L"*** WARNING: card 1 — only %d shots remaining! ***",
                       cs.remainingShots);
            LogWarning(buf);
        }
        if (cs.slot2Status == L"no-card" && cs.slot2Remaining == 0) {
            LogWarning(L"*** WARNING: card slot 2 — no card inserted! ***");
        } else if (cs.slot2Remaining > 0 && cs.slot2Remaining <= kCardWarnThresh) {
            wchar_t buf[128];
            swprintf_s(buf, L"*** WARNING: card 2 — only %d shots remaining! ***",
                       cs.slot2Remaining);
            LogWarning(buf);
        }
    }

    server.Run();

    // ── Cleanup ───────────────────────────────────────────────────────────────
    LogLine(L"Shutting down...");
    for (auto& c : camOwners) c->Shutdown();
    TotalControl::CameraController::ReleaseSDK(); // final decrement matching InitSDK in main()
    LogLine(L"Daemon stopped.");
    if (g_shutdownDone) { SetEvent(g_shutdownDone); CloseHandle(g_shutdownDone); }
    if (g_singletonMutex) { ReleaseMutex(g_singletonMutex); CloseHandle(g_singletonMutex); }
    g_logFile.close();
    return 0;
}
