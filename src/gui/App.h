#pragma once
#include "PipeClient.h"
#include "Database.h"
#include "Timeline.h"
#include "TzEntry.h"
#include "EclipseEntry.h"
#include "IqpClient.h"
#include "BesselCalc.h"
#include "EphClient.h"
#include <string>
#include <array>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

struct ImFont;

// Forward declarations — avoid pulling in d3d11.h into every TU
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace TotalControl {

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
    void OnInit(ID3D11Device* d3dDev, ID3D11DeviceContext* d3dCtx);
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
    void RenderSequencerButtons();   // TEST RUN / STOP / RUN / STOP RUN in Col1
    void ExportTimelineJson();
    void NewTimeline();
    void DeleteSelectedBlock();
    void DuplicateSelectedBlock();
    static void ApplyStyleDark();
    bool m_showAbout = false;

    // ── Background status thread ──────────────────────────────────────────────
    void StartStatusThread();
    void StopStatusThread();
    void StatusThreadProc();

    // ── GUI sequencer thread ──────────────────────────────────────────────────
    // Sequencer mode: Idle → TestRunning ↔ TestPaused; Idle → Running
    enum class GuiSeqMode : int { Idle, TestRunning, TestPaused, Running };

    static std::string BuildBlockCmd(const TLBlock& blk, int camIdx);
    void StartSeqThread(GuiSeqMode mode);
    void StopSeqThread();          // pause/stop — joins thread
    void SeqThreadProc(GuiSeqMode mode,
                       int64_t playheadStartMs,
                       int64_t realStartMs);

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

    // ── Camera status (written by m_statusThread, read by render thread) ──────
    std::thread       m_statusThread;
    std::atomic<bool> m_statusRun{false};
    std::mutex        m_camerasMutex;        // guards m_cameras
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

    // ── Solar view ───────────────────────────────────────────────────────────
    void  RenderSolarView();
    float m_solarP    = 15.2f;  // P₀: solar N pole PA from celestial north (deg) — for display
    float m_solarQ    =  0.f;   // q: parallactic angle; drawing uses (P₀-q)
    float m_solarZoom =  1.f;   // current zoom factor (mouse wheel; 0.2–20)

    // ── SDO live solar image ─────────────────────────────────────────────────
    // Downloaded from sdo.gsfc.nasa.gov, decoded JPEG → D3D11 texture.
    void TriggerSdoFetch();
    void SdoThreadProc(std::wstring cachePath);
    void CreateSdoTexture();     // render thread only — creates D3D11 SRV

    ID3D11Device*              m_d3dDev    = nullptr;
    ID3D11DeviceContext*       m_d3dCtx    = nullptr;
    ID3D11ShaderResourceView*  m_sdoSrv    = nullptr;  // null until first decode
    std::thread                m_sdoThread;
    std::atomic<bool>          m_sdoFetching{false};
    std::atomic<bool>          m_sdoNewData{false};    // pixel buffer ready
    mutable std::mutex         m_sdoPixelMutex;
    std::vector<uint8_t>       m_sdoPixels;            // decoded RGBA (under mutex)
    int                        m_sdoTexW = 0, m_sdoTexH = 0;
    int64_t                    m_sdoFetchedAtMs = 0;   // UtcNowMs() of last attempt

    // ── JPL Horizons ephemeris ────────────────────────────────────────────────
    void TriggerEphFetch();
    void EphThreadProc(std::string eclDate, std::string locKey,
                       std::wstring configPath,
                       double lat, double lon, double altM);
    EphRow InterpEphAt(EphBody body, int64_t utcMs) const;
    static double ComputeP0(double raSun_deg,  double decSun_deg);
    static double ComputeQ (double raSun_deg,  double decSun_deg,
                             double lat_deg,   double lon_deg, int64_t utcMs);
    static double ComputeMoonV(double raMoon_deg, double decMoon_deg);

    std::wstring              m_configPath;   // path to TotalControlConfig.db
    std::thread               m_ephThread;
    std::atomic<bool>         m_ephFetching{false};
    mutable std::mutex        m_ephMutex;
    // Indexed by EphBody int; populated by EphThreadProc, read by render thread.
    std::array<std::vector<EphRow>, static_cast<size_t>(EphBody::Count)> m_ephSamples;

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
    bool                 m_tlDirty    = false;
    int                  m_selTrack    = -1;
    int                  m_selBlock    = -1;
    int64_t              m_tlViewStart = -1;
    int64_t              m_tlViewEnd   = -1;

    // Drag-to-move existing blocks
    bool    m_tlDragging    = false;
    int     m_tlDragTrack   = -1;
    int     m_tlDragBlock   = -1;
    int64_t m_tlDragStartMs = -1;
    float   m_tlDragMouseX0 = 0.f;
    float   m_tlScreenTopY  = 0.f;

    // Playhead drag (grab triangle, hold, move)
    bool    m_tlPhDragging  = false;

    // ── Playhead ──────────────────────────────────────────────────────────────
    // Written by seqThread (during run) or main thread (drag / default init).
    // Read by render thread every frame. Atomic to avoid torn reads.
    std::atomic<int64_t> m_tlPlayheadMs{-1};   // -1 = not yet initialised

    // ── Named timeline snapshots ─────────────────────────────────────────────
    void CreateCalibrationSnapshot();  // idempotent — skips if already exists
    void RenderSnapshotModal();        // ImGui modal: open / save-as / delete

    bool                      m_showSnapOpen   = false;   // open-timeline modal
    bool                      m_showSnapSaveAs = false;   // save-as modal
    std::vector<SnapshotInfo> m_snapList;
    int                       m_snapSel        = -1;
    char                      m_snapNameBuf[128] = {};

    // ── Bracket calibration ──────────────────────────────────────────────────
    // Per-model lookup: camModel → { (count,ev) → lat_max_ms+10 }
    // Loaded from DB at startup; reloaded after SaveCalibFromBuf.
    std::map<std::string,
             std::map<std::pair<int,std::string>, int>> m_calibCache;

    struct SeqCalibSample { int count = 0; std::string ev; int latMs = 0; };
    // Samples collected during the current/last TEST RUN or RUN (Bracket only).
    // Written by seqThread; read by main thread only after thread join.
    std::vector<SeqCalibSample> m_seqCalibBuf;

    void LoadCalibCache();                              // DB → m_calibCache
    void SeedBuiltinCalib();                            // seed ILCE-7RM4A if absent
    void SaveCalibFromBuf(const std::string& camModel); // buf → DB + reload cache

    // BlockDurMs: member function (not static) so it can access m_calibCache.
    // camModel = track's cameraId; empty = use first available calibration.
    int64_t BlockDurMs(const TLBlock& b, std::string_view camModel = {}) const;

    // ── Execution log ────────────────────────────────────────────────────────
    // Sequence counter reset at each TEST RUN / RUN start.
    // Written by main thread (StartSeqThread), incremented by seqThread.
    // Thread-safe: thread start provides happens-before guarantee.
    int m_execSeqNum = 0;

    // ── GUI sequencer state ───────────────────────────────────────────────────
    std::atomic<GuiSeqMode> m_guiSeqMode{GuiSeqMode::Idle};

    // Per-track "next unfired block" index for up to 4 camera tracks.
    // Written only by m_seqThread while running; read by main thread only after join.
    static constexpr int kMaxCamTracks = 4;
    int m_seqNextBlock[kMaxCamTracks] = {};

    // Snapshot of playhead and real-time at the last Start/Resume
    int64_t m_testPlayheadAtStart = -1;
    int64_t m_testStartRealMs     = -1;

    std::thread       m_seqThread;
    std::atomic<bool> m_seqRun{false};
};

} // namespace TotalControl
