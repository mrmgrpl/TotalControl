#pragma once
#include "PipeClient.h"
#include "Database.h"
#include "Timeline.h"
#include "TzEntry.h"
#include "EclipseEntry.h"
#include "IqpClient.h"
#include "BesselCalc.h"
#include "EphClient.h"
#include "ElevationClient.h"
#include "GeocodeClient.h"
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
    int         slot1MaxRem   = -1;  // max remaining seen this session → used for % calc
    int         slot2MaxRem   = -1;
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
    void RenderOptionsWindow();      // floating Options window (API keys, etc.)
    void RenderSequencerButtons();   // TEST RUN / STOP / RUN / STOP RUN in Col1
    void RenderLeftColumn();         // new single 270px left column
    void RenderLocationSection();    // observer DMS + totality status
    void RenderTimeSection();        // 3 clocks + contact comparison table + countdowns
    void RenderHardwareSection();    // connection + camera status
    int64_t FindSunAltCrossing(bool findRise) const;  // scan EPH for sunrise/sunset
    void ExportTimelineJson();

    // Camera configuration
    void MergeCamerasIntoCamConfigs();
    void RenderCamConfigWindows();
    void NewTimeline();
    void DeleteSelectedBlock();
    void DuplicateSelectedBlock();
    static void ApplyStyleDark();
    bool m_showAbout    = false;
    bool m_showOptions  = false;

    // ── BE REST API key (40 hex chars, stored in Config.db, never in source) ────
    char m_beApiKeyBuf[48] = {};   // InputText buffer; 40 chars + null
    bool m_beKeyVisible    = false; // toggle password mask

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
    bool        m_connecting = false;

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
    float       m_obsLat  = 0.f;
    float       m_obsLon  = 0.f;
    int         m_obsAltM = 0;
    std::string m_obsName;   // region/city name, parsed from a Google Maps URL

    DmsCoord m_latDms, m_lonDms;
    void SyncDecimalToDms();
    void SyncDmsToDecimal();

    // ── Set Location from Google Maps URL ───────────────────────────────────
    char        m_gmapsUrlBuf[512] = {};
    std::string m_gmapsStatusMsg;     // feedback shown under the SET LOCATION button
    bool        m_gmapsStatusIsErr = false;
    std::thread m_geoThread;
    std::mutex  m_geoMutex;
    double      m_geoElevM = 0.0;     // guarded by m_geoMutex
    std::string m_geoNamePending;     // guarded by m_geoMutex; reverse-geocoded place name
    std::atomic<int> m_geoState{0};   // 0=Idle 1=Loading 2=Ready(unconsumed) 3=Error
    void ApplyGoogleMapsUrl();        // parses m_gmapsUrlBuf, updates lat/lon, kicks elevation+geocode fetch
    void ApplyLocationAndCalculate(); // ApplyGoogleMapsUrl (if URL given) + TriggerIqpFetch, one click
    void GeoElevationThreadProc(double lat, double lon); // fetches elevation AND reverse-geocodes name

    // ── Solar view ───────────────────────────────────────────────────────────
    void  RenderSolarView();
    float m_solarP    = 15.2f;  // P₀: solar N pole PA from celestial north (deg) — for display
    float m_solarQ    =  0.f;   // q: parallactic angle; drawing uses (P₀-q)
    float m_solarZoom =  1.f;   // current zoom factor (mouse wheel; 0.2–20)
    double m_sunAltDeg  = 8.0;   // current Sun altitude (deg) — updated each frame in RenderSolarView
    double m_sunAzDeg   = 285.0; // current Sun azimuth  (deg) — updated each frame in RenderSolarView

    // ── GOES-19 SUVI Fe171 animation ─────────────────────────────────────────
    // 300 frames from cdn.star.nesdis.noaa.gov; cadence 4 min; alpha=luminance.
    struct SuviFrame { std::vector<uint8_t> rgba; int w = 0, h = 0; };

    // Alignment calibration — editable live in Inspector panel
    // v2 defaults measured on real GOES-19 imagery 2026-06-28
    float m_suviHalfQ       = 1.5250f;  // image half / disc radius
    float m_suviFooterPx    = 20.f;     // disc centre offset from image centre (px)
    float m_suviCorrRightPx = -24.f;    // additional shift right (image px)
    float m_suviCorrUpPx    =   0.f;    // additional shift up   (image px)

    void TriggerSuviFetch();
    void SuviThreadProc();
    void CreateSuviTextures();   // render thread only — uploads pending frames to D3D11

    ID3D11Device*              m_d3dDev    = nullptr;
    ID3D11DeviceContext*       m_d3dCtx    = nullptr;

    std::vector<SuviFrame>                   m_suviPending;  // decoded, awaiting GPU upload
    std::vector<ID3D11ShaderResourceView*>   m_suviSrvs;     // D3D11 SRV per frame (render thread)
    mutable std::mutex                       m_suviMutex;    // guards m_suviPending
    std::atomic<bool>                        m_suviNewFrames{false};

    int     m_suviCurFrame    = 0;
    float   m_suviAnimTimer   = 0.f;
    float   m_suviAnimFps     = 30.f;   // 30 fps → 10s loop at 300 frames

    std::thread        m_suviThread;
    std::atomic<bool>  m_suviFetching{false};
    std::atomic<int64_t> m_suviFetchedAtMs{0};  // set at completion, not start → interval from end of fetch
    float              m_suviOpacity     = 1.0f;  // 0–1; < 0.05 = hidden (persisted to DB)
    std::string        m_suviChannel     = "Fe171"; // selected SUVI wavelength band (persisted to DB)
    bool               m_suviJustCleared = false; // set by TriggerSuviFetch, resets s_prevSrvN

    // ── Moon texture (static NASA archive photo, fetched once) ───────────────
    // images-assets.nasa.gov/.../GSFC_20171208_Archive_e001982 — cached locally
    // as TotalControlMoon.jpg since this is a fixed archive image, not a live
    // feed like SUVI; no periodic re-fetch.
    void TriggerMoonFetch();
    void MoonThreadProc();
    void CreateMoonTexture();   // render thread only — uploads pending pixels to D3D11

    std::vector<uint8_t> m_moonPending;              // decoded RGBA, guarded by m_moonMutex
    int                  m_moonPendingW = 0, m_moonPendingH = 0;
    float                m_moonPendingCx = 0.f, m_moonPendingCy = 0.f, m_moonPendingR = 1.f;
    std::mutex           m_moonMutex;
    std::atomic<bool>    m_moonNewData{false};
    std::thread          m_moonThread;
    std::atomic<bool>    m_moonFetching{false};

    // Render-thread-owned (set only in CreateMoonTexture, read in RenderSolarView).
    ID3D11ShaderResourceView* m_moonSrv    = nullptr;
    int                       m_moonImgW   = 0, m_moonImgH = 0;
    float                     m_moonDiscCx = 0.f, m_moonDiscCy = 0.f, m_moonDiscR = 1.f;

    float m_moonOpacity = 1.0f;  // 0-1; < 0.05 = flat fallback disc (persisted to DB)

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
    ContactTimes       m_geResult;   // BesselCalc at eclipse GE lat/lon
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

    // Timeline pan drag (grab "Contacts" strip above phase bar, hold, move)
    bool    m_tlPanDragging      = false;
    float   m_tlPanMouseX0       = 0.f;
    int64_t m_tlPanStartViewMs   = 0;
    int64_t m_tlPanStartViewDur  = 0;

    // ── Playhead ──────────────────────────────────────────────────────────────
    // Written by seqThread (during run) or main thread (drag / default init).
    // Read by render thread every frame. Atomic to avoid torn reads.
    std::atomic<int64_t> m_tlPlayheadMs{-1};   // -1 = not yet initialised

    // ── Named timeline snapshots ─────────────────────────────────────────────
    void CreateCalibrationSnapshot();  // idempotent — skips if already exists
    void RenderSnapshotModal();        // ImGui modal: open / save-as / delete
    void LoadAudioPreset(std::string_view lang); // populate audio track from eclipse_audio_<LANG>/

    // Background audio file scanner — probes MP3 durations via MCI and caches to DB.
    void ScanAudioFilesAsync();       // start or restart background scan
    void AudioScanThreadProc();       // thread body

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

    // Snap to Seconds: rounds so the OFFSET FROM THE NEAREST CONTACT
    // (C1/C2/C3/C4) is a whole number of seconds — matching the Relative
    // ruler row — not so atMs itself lands on a whole UTC second (contacts
    // themselves have sub-second precision, e.g. C2=18:28:53.700).
    // Falls back to rounding to the nearest whole UTC second when no
    // contact times are available yet.
    int64_t SnapMsToRelativeSecond(int64_t ms);

    // ── Execution log ────────────────────────────────────────────────────────
    // Sequence counter reset at each TEST RUN / RUN start.
    // Written by main thread (StartSeqThread), incremented by seqThread.
    // Thread-safe: thread start provides happens-before guarantee.
    int m_execSeqNum = 0;

    // ── GUI sequencer state ───────────────────────────────────────────────────
    std::atomic<GuiSeqMode> m_guiSeqMode{GuiSeqMode::Idle};

    // Per-track "next unfired block" index for up to 4 camera tracks.
    // Written only by m_seqThread while running; read by main thread only after join.
    static constexpr int kMaxCamTracks  = 4;
    static constexpr int kMaxAudioTracks = 2;
    int m_seqNextBlock[kMaxCamTracks]    = {};
    // m_audioNextBlock is written by m_audioSeqThread during the run (live progress) and
    // may be read by the render thread concurrently for display — must be atomic.
    std::atomic<int> m_audioNextBlock[kMaxAudioTracks]{};

    // Snapshot of playhead and real-time at the last Start/Resume
    int64_t m_testPlayheadAtStart = -1;
    int64_t m_testStartRealMs     = -1;

    std::thread       m_seqThread;
    std::thread       m_audioSeqThread;   // independent audio-only tick loop
    std::atomic<bool> m_seqRun{false};

    void AudioSeqThreadProc(GuiSeqMode mode, int64_t playheadStartMs, int64_t realStartMs);

    // ── Audio file duration cache ─────────────────────────────────────────────
    // Populated by AudioScanThreadProc; read by LoadAudioPreset + Inspector.
    // Key: "LANG/filename.mp3" (e.g. "PL/01_pre_c1_10min.mp3")
    std::map<std::string, int32_t> m_audioDurCache;
    std::mutex                     m_audioDurMutex;
    std::thread                    m_audioScanThread;
    std::atomic<int>               m_audioScanProgress{0};    // files probed so far
    std::atomic<int>               m_audioScanTotal{0};       // total files found
    std::atomic<bool>              m_audioScanComplete{false}; // set by scan thread on finish
    std::string                    m_pendingAudioReload;       // lang to reload after scan; main-thread only

    // ── Camera config ─────────────────────────────────────────────────────────
    std::vector<CamConfig> m_camConfigs;
    std::vector<bool>      m_showCamCfgWnd;    // one flag per m_camConfigs entry
    int                    m_dragHorizonCamIdx = -1; // index of camera being dragged in Horizon mode

    // ── Live View overlay on solar simulator ──────────────────────────────────
    // Frames arrive via named SHM (TotalControl_LV_<ci>), decoded JPEG→RGBA by
    // m_lvThread, uploaded to D3D11 by CreateLvTextures (render thread only),
    // rendered in RenderSolarView as alpha-blended quad matching camera FOV rect.
    bool   m_lvEnabled[kMaxCamTracks] = {};       // derived: true when m_lvOpacity[ci] >= 0.05
    float  m_lvOpacity[kMaxCamTracks] = {};       // per-camera opacity 0–1 (persisted to DB)

    struct LvFrame { std::vector<uint8_t> rgba; int w = 0, h = 0; };
    LvFrame                         m_lvPending[kMaxCamTracks];
    bool                            m_lvNewData[kMaxCamTracks] = {};
    mutable std::mutex              m_lvMutex;
    std::atomic<bool>               m_lvNewFrames { false };
    ID3D11ShaderResourceView*       m_lvSrv[kMaxCamTracks]  = {};
    int                             m_lvW[kMaxCamTracks]    = {};
    int                             m_lvH[kMaxCamTracks]    = {};

    std::thread       m_lvThread;
    std::atomic<bool> m_lvThreadRun { false };

    void StartLvThread();
    void StopLvThread();
    void LvThreadProc();
    void CreateLvTextures();   // render thread only

    // ── Photo preset ─────────────────────────────────────────────────────────
    void AddPhotoPreset();
    void AddTotalityBracketPreset();
    void AddAllBracketVariantsPreset();
    int  m_presetTargetTrack = 0;  // camera track index that receives AddPhotoPreset blocks
};

} // namespace TotalControl
