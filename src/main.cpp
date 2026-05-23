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

// ─── main ─────────────────────────────────────────────────────────────────────
int main() {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);

    std::wstring logPath = ExeDir() + L"tc_daemon.log";
    g_logFile.open(logPath, std::ios::out | std::ios::trunc | std::ios::binary);
    g_logFile.write("\xEF\xBB\xBF", 3);

    LogLine(L"╔══════════════════════════════════════╗");
    LogLine(L"║  TotalControl daemon                 ║");
    LogLine(L"║  pipe: \\\\.\\pipe\\TotalControl         ║");
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

    LogLine(L"Nasłuchuję na pipe...");
    server.Run();

    // ── Cleanup ───────────────────────────────────────────────────────────────
    LogLine(L"Zamykam...");
    cam.Shutdown();
    LogLine(L"Daemon zakończony.");
    g_logFile.close();
    return 0;
}
