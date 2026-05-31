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

// ─── Timeline block types ─────────────────────────────────────────────────────

enum class BlockType : int { Single = 0, Burst = 1, Bracket = 2, Audio = 3 };

struct TLBlock {
    int64_t     id         = -1;       // DB id (-1 = unsaved)
    BlockType   type       = BlockType::Single;
    int64_t     atMs       = -1;       // absolute UTC ms

    // Camera block params
    std::string ss         = "1/100";
    int         iso        = 100;
    std::string fstop      = "8.0";
    int         count      = 5;        // bracket: shot count
    std::string ev         = "1ev";    // bracket: EV step
    float       burstFps   = 4.4f;    // burst: fps (ILCE-7RM4A hi+ cRAW)
    int32_t     burstDurMs = 3000;    // burst: duration ms

    // Audio block params
    std::string audioFile;
    int32_t     audioDurMs = 10000;   // default 10 s, updated from file

    // Common
    std::string label;
    bool        snapToPrev = false;
};

struct TLTrack {
    int                  id       = -1;
    std::string          type;          // "camera" | "audio"
    std::string          cameraId;      // e.g. "ILCE-7RM4A"
    std::string          label;         // display name
    std::vector<TLBlock> blocks;

    bool IsCamera() const { return type == "camera"; }
    bool IsAudio()  const { return type == "audio";  }
};

// ─── Other shared structs ─────────────────────────────────────────────────────

struct DmsCoord {
    int   deg = 0, min = 0;
    float sec = 0.f;
    bool  pos = true;   // true = N or E
};

struct CamStatus {
    bool        valid        = false;
    std::string guid;
    std::string model;
    int         batteryPct   = 0;
    std::string batteryLevel;
    std::string mode;
    std::string ss;
    int         iso          = 0;
    std::string fnum;
    std::string focus;
    std::string drive;
    std::string slot1Status;
    std::string slot2Status;
    int         slot1Remaining = -1;
    int         slot2Remaining = -1;
    std::string store;
    int         lastShotMs   = -1;
};

// ─── App ──────────────────────────────────────────────────────────────────────

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

    // Phase 3 — Timeline editor
    void InitTracks();
    void RenderStatusColumn();
    void RenderInspectorColumn();
    void RenderTimelineBottom();
    void RenderMenuBar();
    void RenderAboutModal();
    void ExportTimelineJson();
    void NewTimeline();
    void DeleteSelectedBlock();
    void DuplicateSelectedBlock();
    static void ApplyStyleDark();
    bool m_showAbout = false;

    PipeClient m_pipe;

    std::thread       m_connectThread;
    std::atomic<bool> m_connectRun{false};

    int         m_reconnectCountdown = 0;
    int         m_statusCountdown    = 0;
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

    // ── Camera status ─────────────────────────────────────────────────────────
    std::vector<CamStatus> m_cameras;

    // ── Eclipse selector ──────────────────────────────────────────────────────
    std::vector<EclipseEntry> m_eclipses;
    int                        m_eclipseIdx = -1;

    // ── Observer location ─────────────────────────────────────────────────────
    float m_obsLat  = 0.f;
    float m_obsLon  = 0.f;
    int   m_obsAltM = 0;

    DmsCoord m_latDms, m_lonDms;
    void SyncDecimalToDms();
    void SyncDmsToDecimal();

    // ── Contact times ─────────────────────────────────────────────────────────
    std::thread        m_iqpThread;
    std::mutex         m_iqpMutex;
    ContactTimes       m_contacts;
    ContactTimes       m_beResult;
    std::atomic<int>   m_iqpState{0};  // 0=Idle 1=Loading 2=Ready 3=Error
    float              m_iqpFetchedLat = 1e9f;
    float              m_iqpFetchedLon = 1e9f;
    int                m_iqpFetchedIdx = -2;

    // ── Timeline editor ───────────────────────────────────────────────────────
    std::vector<TLTrack> m_tracks;
    int                  m_selTrack    = -1;
    int                  m_selBlock    = -1;
    int64_t              m_tlViewStart = -1;   // visible range (UTC ms)
    int64_t              m_tlViewEnd   = -1;

    // Drag-to-move: mouse-drag existing blocks; release above timeline = delete
    bool    m_tlDragging    = false;
    int     m_tlDragTrack   = -1;
    int     m_tlDragBlock   = -1;
    int64_t m_tlDragStartMs = -1;
    float   m_tlDragMouseX0 = 0.f;
    float   m_tlScreenTopY  = 0.f;  // top-Y of timeline child in screen coords
};

} // namespace TotalControl
