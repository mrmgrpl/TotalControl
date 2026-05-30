#pragma once
#include "PipeClient.h"
#include "Database.h"
#include "TzEntry.h"
#include "EclipseEntry.h"
#include "IqpClient.h"
#include "BesselCalc.h"
#include <string>
#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

struct ImFont;

namespace TotalControl {

struct DmsCoord {
    int   deg = 0, min = 0;
    float sec = 0.f;
    bool  pos = true;   // true = N or E
};

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
    int         lastShotMs   = -1;  // latency_ms from last shoot response (full stack)
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
    void SaveObserverSettings();
    void TriggerIqpFetch();
    void EnsureDefaultConfig(const std::wstring& path);

    void PollCameraStatus();
    void RenderCameraSection();
    void RenderEclipseSection();
    void RenderContactTimesSection();

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

    // ── Eclipse selector ──────────────────────────────────────────────────────
    std::vector<EclipseEntry> m_eclipses;
    int                        m_eclipseIdx = -1;

    // ── Observer location ─────────────────────────────────────────────────────
    float m_obsLat  = 0.f;   // decimal degrees N (+) / S (-)
    float m_obsLon  = 0.f;   // decimal degrees E (+) / W (-)
    int   m_obsAltM = 0;     // metres above sea level

    DmsCoord m_latDms, m_lonDms;
    void SyncDecimalToDms();
    void SyncDmsToDecimal();

    // ── Contact times ─────────────────────────────────────────────────────────
    std::thread        m_iqpThread;
    std::mutex         m_iqpMutex;
    ContactTimes       m_contacts;     // IQP result
    ContactTimes       m_beResult;     // Besselian result (sync, always computed)
    std::atomic<int>   m_iqpState{0};  // 0=Idle 1=Loading 2=Ready 3=Error
    float              m_iqpFetchedLat = 1e9f;
    float              m_iqpFetchedLon = 1e9f;
    int                m_iqpFetchedIdx = -2;
};

} // namespace TotalControl
