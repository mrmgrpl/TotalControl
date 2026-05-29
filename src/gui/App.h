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
    bool TryLaunchDaemon();
    void LogLine(std::string_view msg);
    void RenderExtraClock(const char* clockId, const char* popupId,
                          bool& show, std::string& tzIana);
    void SaveClockSettings();

    // Creates TotalControlDefaultConfig.db at given path if it does not exist.
    // Populates it with factory-default settings via embedded SQL.
    void EnsureDefaultConfig(const std::wstring& path);

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

    // ── Databases ────────────────────────────────────────────────────────────
    Database m_configDb;  // TotalControlConfig.db       — active user settings
    Database m_dataDb;    // TotalControlData.db          — reference data (read-only)
    //       TotalControlDefaultConfig.db — factory defaults, not kept open

    // ── Settings loaded from m_configDb ──────────────────────────────────────
    bool        m_showHomeClock = true;
    bool        m_showEclClock  = true;
    std::string m_homeTzIana   = "Europe/Warsaw";
    std::string m_eclTzIana    = "Europe/Madrid";
};

} // namespace TotalControl
