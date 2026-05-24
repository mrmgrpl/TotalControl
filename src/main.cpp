#include "CameraController.h"
#include "daemon/PipeServer.h"
#include "daemon/CommandHandler.h"

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ─── Log do pliku ─────────────────────────────────────────────────────────────
static std::ofstream g_logFile;
static std::mutex    g_logMutex;

static std::string WtoU8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static void LogLine(const wchar_t* msg) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t ts[16];
    swprintf_s(ts, L"%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::wstring line = std::wstring(ts) + L"  " + msg;

    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile.is_open()) { g_logFile << WtoU8(line) << "\n"; g_logFile.flush(); }
    wprintf(L"%s\n", line.c_str());
    ::OutputDebugStringW((line + L"\n").c_str());
}

// ─── Pomocnik: katalog exe ────────────────────────────────────────────────────
static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (auto* s = wcsrchr(buf, L'\\')) *(s + 1) = L'\0';
    return buf;
}

// ─── Graceful shutdown on console close / Ctrl+C ─────────────────────────────
static TotalControl::PipeServer*                       g_server       = nullptr;
static std::vector<TotalControl::CameraController*>*   g_cams         = nullptr;
static HANDLE                                          g_shutdownDone = nullptr;

static BOOL WINAPI CtrlHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        LogLine(L"Signal — inicjuję shutdown...");
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
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);

    std::wstring logPath = ExeDir() + L"TotalControlSRV.log";
    g_logFile.open(logPath, std::ios::out | std::ios::trunc | std::ios::binary);
    g_logFile.write("\xEF\xBB\xBF", 3);

    LogLine(L"╔══════════════════════════════════════╗");
    LogLine(L"║  TotalControlSRV daemon              ║");
    LogLine(L"║  pipe: \\\\.\\pipe\\TotalControl         ║");
    LogLine(L"║  zamknij: TotalControlCLI quit       ║");
    LogLine(L"╚══════════════════════════════════════╝");

    // ── Init SDK ──────────────────────────────────────────────────────────────
    LogLine(L"Inicjalizacja SDK...");
    if (!TotalControl::CameraController::InitSDK()) {
        LogLine(L"BŁĄD: SDK::Init");
        return 1;
    }

    // ── Enumeracja kamer ──────────────────────────────────────────────────────
    LogLine(L"Szukam kamer (timeout 5s)...");
    auto cameraList = TotalControl::CameraController::Enumerate(5);
    if (cameraList.empty()) {
        LogLine(L"BŁĄD: brak kamer");
        TotalControl::CameraController::ReleaseSDK();
        return 2;
    }
    {
        wchar_t buf[128];
        swprintf_s(buf, L"Znaleziono %zu kamer(ę/y):", cameraList.size());
        LogLine(buf);
        for (size_t i = 0; i < cameraList.size(); ++i) {
            swprintf_s(buf, L"  [%zu] %s  guid=%s",
                       i, cameraList[i].model.c_str(), cameraList[i].guid.c_str());
            LogLine(buf);
        }
    }

    // ── Połącz z każdą kamerą ─────────────────────────────────────────────────
    std::vector<std::unique_ptr<TotalControl::CameraController>> camOwners;

    for (size_t i = 0; i < cameraList.size(); ++i) {
        const auto& info = cameraList[i];

        // Logger z prefiksem indeksu kamery
        auto logFn = [i](const wchar_t* msg) {
            wchar_t buf[1024];
            swprintf_s(buf, L"[CAM%zu] %s", i, msg);
            LogLine(buf);
        };

        auto cam = std::make_unique<TotalControl::CameraController>(logFn);
        if (!cam->Init()) {
            wchar_t buf[128];
            swprintf_s(buf, L"BŁĄD: Init kamery %zu (%s)", i, info.model.c_str());
            LogLine(buf);
            continue;
        }
        if (!cam->Connect(info.guid.c_str(), 5, 8000)) {
            wchar_t buf[128];
            swprintf_s(buf, L"BŁĄD: Connect kamery %zu (%s)", i, info.model.c_str());
            LogLine(buf);
            cam->Shutdown();
            continue;
        }
        camOwners.push_back(std::move(cam));
    }

    if (camOwners.empty()) {
        LogLine(L"BŁĄD: żadna kamera nie połączona");
        TotalControl::CameraController::ReleaseSDK();
        return 3;
    }

    {
        wchar_t buf[128];
        swprintf_s(buf, L"Połączono %zu/%zu kamer. Daemon gotowy.",
                   camOwners.size(), cameraList.size());
        LogLine(buf);
    }

    // ── Zbuduj wektor wskaźników dla CommandHandler ───────────────────────────
    std::vector<TotalControl::CameraController*> camPtrs;
    for (auto& c : camOwners) camPtrs.push_back(c.get());

    // ── Pipe server ───────────────────────────────────────────────────────────
    TotalControl::CommandHandler handler(camPtrs);

    TotalControl::PipeServer server(
        L"\\\\.\\pipe\\TotalControl",
        [&handler](const std::wstring& req, std::wstring& resp) -> bool {
            LogLine((L"→ " + req).c_str());
            bool cont = handler.Handle(req, resp);
            LogLine((L"← " + resp).c_str());
            return cont;
        }
    );

    g_shutdownDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_server = &server;
    g_cams   = &camPtrs;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    LogLine(L"Nasłuchuję na pipe...");
    server.Run();

    // ── Cleanup ───────────────────────────────────────────────────────────────
    LogLine(L"Zamykam...");
    for (auto& c : camOwners) c->Shutdown();
    TotalControl::CameraController::ReleaseSDK(); // finalny decrement (z InitSDK w main)
    LogLine(L"Daemon zakończony.");
    if (g_shutdownDone) { SetEvent(g_shutdownDone); CloseHandle(g_shutdownDone); }
    g_logFile.close();
    return 0;
}
