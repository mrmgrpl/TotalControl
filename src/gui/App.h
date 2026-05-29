#pragma once
#include "PipeClient.h"
#include "Database.h"
#include <string>
#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>

struct ImFont;

namespace TotalControl {

class App {
public:
    App();
    ~App();
    void OnInit();
    void OnFrame();
    bool OnCloseRequest();

private:
    void TryAutoConnect();
    bool TryLaunchDaemon();
    void LogLine(std::string_view msg);
    void RenderExtraClock(const char* clockId, const char* popupId,
                          bool& show, std::string& tzIana);
    void SaveClockSettings();

    PipeClient m_pipe;

    std::thread       m_connectThread;
    std::atomic<bool> m_connectRun{false};

    int         m_reconnectCountdown = 0;
    std::string m_lastResult;

    bool m_showStyleEditor = false;
    bool m_showDemoWindow  = false;

    ImFont* m_fontMono  = nullptr;
    ImFont* m_fontLarge = nullptr;

    std::ofstream m_logFile;
    std::mutex    m_logMutex;

    // ── Persistent settings (SQLite) ─────────────────────────────────────
    Database    m_db;

    bool        m_showHomeClock = true;
    bool        m_showEclClock  = true;
    std::string m_homeTzIana   = "Europe/Warsaw";   // IANA name, loaded from DB at startup
    std::string m_eclTzIana    = "Europe/Madrid";   // IANA name, loaded from DB at startup
};

} // namespace TotalControl
