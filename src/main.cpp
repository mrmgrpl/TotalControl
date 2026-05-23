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
#include <mutex>
#include <string>

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
static TotalControl::PipeServer*       g_server         = nullptr;
static TotalControl::CameraController* g_cam            = nullptr;
static HANDLE                          g_shutdownDone   = nullptr;

static BOOL WINAPI CtrlHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        LogLine(L"Signal — inicjuję shutdown...");
        if (g_cam)    g_cam->RequestShutdown();   // przerywa aktywny Shoot()
        if (g_server) g_server->Stop();            // odblokowuje Run()
        // Czekaj aż main() dokończy cam.Shutdown() — max 5s zanim OS dobije proces
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

    // ── Init + Connect ────────────────────────────────────────────────────────
    TotalControl::CameraController cam([](const wchar_t* msg) { LogLine(msg); });

    LogLine(L"Inicjalizacja SDK...");
    if (!cam.Init()) { LogLine(L"BŁĄD: Init SDK"); return 1; }

    LogLine(L"Szukam kamery (timeout 5s)...");
    if (!cam.Connect(5, 8000)) { LogLine(L"BŁĄD: brak kamery"); return 2; }

    LogLine(L"Kamera połączona. Daemon gotowy.");

    // ── Pipe server ───────────────────────────────────────────────────────────
    TotalControl::CommandHandler handler(cam);

    TotalControl::PipeServer server(
        L"\\\\.\\pipe\\TotalControl",
        [&handler](const std::wstring& req, std::wstring& resp) -> bool {
            LogLine((L"→ " + req).c_str());
            bool cont = handler.Handle(req, resp);
            LogLine((L"← " + resp).c_str());
            return cont;
        }
    );

    // Rejestruj handler konsoli (Ctrl+C, zamknięcie okna, shutdown systemu)
    g_shutdownDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_server = &server;
    g_cam    = &cam;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    LogLine(L"Nasłuchuję na pipe...");
    server.Run();

    // ── Cleanup ───────────────────────────────────────────────────────────────
    LogLine(L"Zamykam...");
    cam.Shutdown();
    LogLine(L"Daemon zakończony — kamera zwolniona.");
    if (g_shutdownDone) { SetEvent(g_shutdownDone); CloseHandle(g_shutdownDone); }
    g_logFile.close();
    return 0;
}
