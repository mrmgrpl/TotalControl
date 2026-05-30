#pragma once
#include "PipeClient.h"
#include "Database.h"
#include "TzEntry.h"
#include <string>
#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

struct ImFont;

namespace TotalControl {

// Camera status polled via {"cmd":"status"} every ~2 s when connected.
struct CamStatus {
    bool        valid        = false;
    std::string guid;            // camera GUID (for routing)
    std::string model;           // e.g. "ILCE-7RM4A"
    int         batteryPct   = 0;
    std::string batteryLevel;    // "full","3/4","1/2","1/4","empty", may contain "+usb"
    std::string mode;            // "M","A","S","P"
    std::string ss;              // "1/1000","2s",...
    int         iso          = 0;
    std::string fnum;            // "8.0","5.6"
    std::string focus;           // "MF","AF-S",...
    std::string drive;           // "single","cont-hi",...
    std::string slot1Status;     // "OK","no-card"
    std::string slot2Status;
    int         slot1Remaining = -1;
    int         slot2Remaining = -1;
    std::string store;           // "MemCard","HostPC"
    int         latencyMs    = -1;  // pipe round-trip for this camera's status poll
};

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
    void EnsureDefaultConfig(const std::wstring& path);

    void PollCameraStatus();          // send "status", parse response
    void RenderCameraSection();       // draw CAMERA section in left panel

    PipeClient m_pipe;

    std::thread       m_connectThread;
    std::atomic<bool> m_connectRun{false};

    int         m_reconnectCountdown = 0;
    int         m_statusCountdown    = 0;  // frames until next status poll
    std::string m_lastResult;

    bool m_showStyleEditor = false;
    bool m_showDemoWindow  = false;

    ImFont* m_fontMono  = nullptr;
    ImFont* m_fontLarge = nullptr;

    std::ofstream m_logFile;
    std::mutex    m_logMutex;

    // ── Databases ─────────────────────────────────────────────────────────────
    Database m_configDb;
    Database m_dataDb;

    // ── Timezone list ─────────────────────────────────────────────────────────
    std::vector<TzEntry> m_tzList;

    // ── Clock settings ────────────────────────────────────────────────────────
    bool        m_showHomeClock = true;
    bool        m_showEclClock  = true;
    std::string m_homeTzIana   = "Europe/Warsaw";
    std::string m_eclTzIana    = "Europe/Madrid";

    // ── Camera status — one entry per connected camera ────────────────────────
    std::vector<CamStatus> m_cameras;
};

} // namespace TotalControl
