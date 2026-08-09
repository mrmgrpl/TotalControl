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
    std::string shutterType;   // "mechanical","electronic","auto","" — see resonance/vibration note in CLAUDE.md
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

    // ── Delta T (IERS Earth-orientation bulletin) ──────────────────────────────
    // Espenak's bundled catalog dt is a static, years-old prediction; IERS
    // revises the real forecast weekly as the eclipse date approaches (see
    // Change log 2026-07-21 — Alessandro/besselianelements.com found
    // TotalControl's Espenak dt for TSE2026 was 6s stale, shifting C2/C3 by
    // the same amount). Checked at startup and whenever the selected eclipse
    // changes (TriggerIqpFetch); re-fetched at most once per 24h per date.
    struct DeltaTState {
        double      dtSeconds   = 0.0;
        bool        valid       = false;  // do we have ANY usable IERS value for forDate
        bool        predicted   = false;  // IERS Bulletin A forecast vs Bulletin B measured
        int64_t     fetchedAtMs = 0;
        std::string forDate;              // "YYYY-MM-DD"
    };
    void TriggerDeltaTFetch();
    // Overrides bel.dt with the cached/fetched IERS value for (y,mo,d) if one
    // is available; leaves the catalog's own dt untouched otherwise (only
    // fallback: no IERS value has EVER been obtained for this date).
    void ApplyDeltaTOverride(BesselianElements& bel, int y, int mo, int d) const;
    static constexpr int64_t kDeltaTRefreshMs = 24LL * 3600 * 1000;
    mutable std::mutex m_deltaTMutex;
    DeltaTState         m_deltaTState;
    std::thread          m_deltaTThread;
    // Snapshot of bel.dt AFTER ApplyDeltaTOverride, taken in TriggerIqpFetch —
    // the actual value used for the last BE/GE calc (IERS override or catalog
    // fallback, whichever applied). Shown in the Solar Simulator status bar;
    // avoids re-deriving/re-querying the same decision every frame.
    double m_effectiveDeltaT = 0.0;

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
    void RenderWhatsNewModal();      // separate modal, opened via About menu -> What's New
    void RenderCameraSetupModal();   // separate modal, opened via About menu -> Camera Setup
    void RenderOptionsWindow();      // floating Options window (API keys, etc.)
    void RenderDriveFpsCalibWindow();  // floating drive-fps calibration window
    void RenderCloseConfirmModal();  // "close TotalControl?" guard against accidental exit
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
    void RenderAddSimCameraModal();  // "Add Simulated Camera" popup (Options menu)
    void AddSimulatedCamera(const std::string& model);
    void RemoveCamConfig(int ci);    // removes an offline camera_config entry + its window flag
    void NewTimeline();
    // Keeps C2/C3-anchored blocks correct as the observer location (and
    // therefore contact times) changes -- see TLAnchor in Timeline.h. Called
    // once per frame from RenderTimelineBottom(), before anything reads atMs.
    void ResyncTimelineAnchors();
    void DeleteSelectedBlock();
    void DuplicateSelectedBlock();
    void ExtendSelectionArrow(int dir);   // dir: +1 = right, -1 = left
    void SnapshotForUndo();               // call right before a mutating Timeline action
    void UndoLastTimelineAction();        // Ctrl+Z
    void DistributeSelectedBlocksEvenly(); // "Distribute Evenly" button (Block Inspector)

    // Multi-select bulk edit: after the Inspector writes a new value for
    // capture parameter `field` onto the primary selected block, this pushes
    // the same value onto every OTHER block in m_multiSel. Position (atMs),
    // id, and label are per-block identity, not capture parameters, so
    // callers only ever pass ss/iso/fstop/count/ev/burstDrive/burstDurMs.
    template <typename T>
    void PropagateBlockField(T TLBlock::* field, const T& value) {
        for (const auto& [ti, bi] : m_multiSel) {
            if (ti == m_selTrack && bi == m_selBlock) continue;
            if (ti < 0 || ti >= static_cast<int>(m_tracks.size())) continue;
            auto& blocks = m_tracks[ti].blocks;
            if (bi < 0 || bi >= static_cast<int>(blocks.size())) continue;
            blocks[bi].*field = value;
        }
    }
    static void ApplyStyleDark();
    void RenderMarkdownBody(const std::string& md); // minimal # / ## / "- " renderer
    bool m_showAbout       = false;
    bool m_showWhatsNew    = false;
    bool m_showCameraSetup = false;
    bool m_showOptions     = false;
    bool m_showDriveFpsCalibWnd = false;
    // ── Close confirmation guard ────────────────────────────────────────────
    bool m_showCloseConfirm = false; // WM_CLOSE deferred to this modal instead of closing immediately
    bool m_closeConfirmed   = false; // set true right before re-issuing the real close
    std::string m_whatsNewMd;      // raw contents of CHANGELOG.md, loaded lazily
    bool        m_whatsNewLoaded = false;

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

    // Up to 4 simultaneous camera tracks. Declared here (rather than with the
    // rest of the sequencer state below) because m_seqPipe/m_seqThread need it
    // for their array bounds.
    static constexpr int kMaxCamTracks  = 4;

    static std::string BuildBlockCmd(const TLBlock& blk, int camIdx);
    void StartSeqThread(GuiSeqMode mode);
    void StopSeqThread();          // pause/stop — joins all per-camera threads
    // One thread per camera track, each with its own pipe connection
    // (m_seqPipe[camIdx]), so a multi-second bracket/ARM call on one camera
    // never blocks the due-block check or firing for a different camera.
    void SeqCamThreadProc(int camIdx,
                          TLTrack* track,
                          GuiSeqMode mode,
                          int64_t playheadStartMs,
                          int64_t realStartMs);

    PipeClient m_pipe;
    // Dedicated pipe connections for the sequencer threads — separate from
    // m_pipe (used by the main/status thread) and from each other, so
    // SendRequest's internal per-connection mutex can't serialise one
    // camera's ARM/shoot behind another's.
    PipeClient m_seqPipe[kMaxCamTracks];

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
    mutable std::mutex m_camerasMutex;        // guards m_cameras -- mutable: CamModelForPos() is const
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
    // Explicit user choice of which engine drives Timeline generation and the
    // T- countdown (falls back to the other if the chosen one is invalid) --
    // 0 = IQP, 1 = BE. Persisted as "primary_contact_src" in Config.db.
    int                m_primaryContactSrc = 0;
    std::atomic<int>   m_iqpState{0};  // 0=Idle 1=Loading 2=Ready 3=Error
    float              m_iqpFetchedLat = 1e9f;
    float              m_iqpFetchedLon = 1e9f;
    int                m_iqpFetchedIdx = -2;

    // ── Timeline editor ───────────────────────────────────────────────────────
    std::vector<TLTrack> m_tracks;
    bool                 m_tlDirty    = false;
    int                  m_selTrack    = -1;
    int                  m_selBlock    = -1;
    // Ctrl+click multi-selection: (track, block) pairs, includes the primary
    // (m_selTrack, m_selBlock) whenever it's non-empty. Empty = plain single
    // selection (or nothing selected) -- everything single-select already did
    // keeps working unchanged; multi-select is additive on top of it.
    std::vector<std::pair<int, int>> m_multiSel;
    // Shift+Arrow range selection: anchor is the block the mouse originally
    // selected (stays fixed); extent is the far end, moved one block per
    // arrow press (see ExtendSelectionArrow). Reset to -1 whenever a plain
    // mouse click picks a new primary block (RenderTimelineBottom).
    int                  m_shiftSelAnchorTrack  = -1;
    int                  m_shiftSelAnchorBlock  = -1;
    int                  m_shiftSelExtentBlock  = -1;
    // Ctrl+Z undo: single last-known-good snapshot of ALL tracks, taken right
    // before a mutating Timeline action (delete/duplicate/drag/drop-insert).
    // Deliberately NOT a full history stack (explicit request to keep this
    // simple) -- one level only, no redo.
    std::vector<TLTrack> m_undoTracks;
    bool                 m_undoValid  = false;
    int64_t              m_tlViewStart = -1;
    int64_t              m_tlViewEnd   = -1;
    // Last C2/C3 seen by ResyncTimelineAnchors() -- lets it tell "contacts
    // just changed, re-derive every anchored block's atMs" apart from
    // "nothing changed, only backfill any still-unanchored blocks".
    int64_t               m_tlAnchorC2Ms = -1;
    int64_t               m_tlAnchorC3Ms = -1;
    // Set by the offline-track remove button (RenderTimelineBottom); applied at
    // the top of the NEXT frame's RenderTimelineBottom() call, before nT/camSnap
    // are captured — erasing m_tracks mid-render would desync the several loops
    // in that function that all snapshot m_tracks.size() once per frame.
    int                  m_pendingTrackRemoval = -1;

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
    // Per-model lookup: camModel → { count → overhead_avg_ms+50 }. Keyed by
    // count only, NOT ev — see BktCalibEntry comment (Database.h) for why:
    // the exposure-time component is now computed exactly by
    // BracketExposureSumMs(), so only the per-shot hardware overhead needs
    // empirical calibration, and that's ev-independent. Same shape as
    // m_armCalibCache. Loaded from DB at startup; reloaded after SaveCalibFromBuf.
    std::map<std::string, std::map<int, int>> m_calibCache;

    // camModel is recorded per-sample (not assumed from m_cameras[0]) because
    // all kMaxCamTracks camera threads push into the same buffer concurrently
    // — a run with 4 different camera models mixes their samples together
    // otherwise (see Change log). ss is recorded so SaveCalibFromBuf can
    // subtract each sample's own analytic exposure time before averaging —
    // this makes samples from any ss (including an SS Sweep run) safe to
    // fold into calibration, instead of being a corruption risk.
    struct SeqCalibSample { int count = 0; std::string ev; std::string ss; int latMs = 0; std::string camModel; };
    // Samples collected during the current/last TEST RUN or RUN (Bracket only).
    // Written by up to kMaxCamTracks sequencer threads concurrently — guarded
    // by m_seqCalibMtx; read by main thread only after all threads join.
    std::vector<SeqCalibSample> m_seqCalibBuf;

    void LoadCalibCache();                              // DB → m_calibCache
    // Saves + removes only camModel's samples from m_seqCalibBuf — other
    // models' still-unsaved samples are left intact for their own button.
    void SaveCalibFromBuf(const std::string& camModel);

    // BlockDurMs: member function (not static) so it can access m_calibCache.
    // camModel = track's cameraId; empty = use first available calibration.
    int64_t BlockDurMs(const TLBlock& b, std::string_view camModel = {}) const;

    // ── ARM (DriveMode-change) calibration ───────────────────────────────────
    // Mirrors the bracket-duration calibration above exactly, but keyed by
    // (camModel, count) only — see docs/arm_latency_bionz_whitepaper.md for
    // why ev isn't part of the key. Measured on real hardware 2026-07-21:
    // ArmEstMs()'s original formula was derived from ILCE-7RM4A alone and
    // over-estimates BIONZ-XR bodies (ILCE-7SM3/7M4) by 8-12x.
    std::map<std::string, std::map<int, int>> m_armCalibCache;  // camModel -> count -> estMs
    struct ArmCalibSample { int count = 0; int latMs = 0; std::string camModel; };
    std::vector<ArmCalibSample> m_armCalibBuf;  // guarded by m_seqCalibMtx, same as m_seqCalibBuf

    void LoadArmCalibCache();                    // DB -> m_armCalibCache
    // Saves + removes only camModel's ARM samples from m_armCalibBuf.
    void SaveArmCalibFromBuf(const std::string& camModel);
    // ArmEstMs: member function (not static) so it can access m_armCalibCache.
    // camModel = track's cameraId; empty/unmatched -> falls back to the
    // original ILCE-7RM4A-derived formula.
    int64_t ArmEstMs(const TLBlock& b, std::string_view camModel = {}) const;

    // ── Card write-speed calibration ─────────────────────────────────────────
    // One scalar per model (no per-count keying, no averaging across reps —
    // see CardCalibEntry in Database.h). Derived from the Buffer Capacity
    // test's post-slowdown sustained fps (see m_bufferCapacityBuf below and
    // RenderSequencerButtons' auto-save block) rather than measured directly
    // — a standalone measurement existed earlier but was retired, see
    // CommandHandler.cpp's buffer_capacity_calib handler for why.
    //
    // This is the POST-HOLD drain rate (precise ARM-confirm-latency timing,
    // measured after the shutter has fully stopped). See
    // m_concurrentWriteCalibCache immediately below for the physically
    // different, generally SLOWER concurrent-write rate, which applies
    // instead while a block's own shutter is still held.
    std::map<std::string, double> m_cardCalibCache;  // camModel -> shots/sec

    // ── Concurrent write-speed calibration ───────────────────────────────────
    // Card write throughput WHILE the shutter is still actively firing (the
    // capture pipeline and the background write both competing for the same
    // I/O) -- see ConcurrentWriteCalibEntry in Database.h for the full
    // rationale and how it's measured. Confirmed for real on hardware
    // 2026-08-02 via the camera's own buffer readout: a 15s cont-lo hold
    // that fired ~74 shots left only 31 pending in the buffer at release --
    // i.e. concurrent draining is real and non-negligible (~43 shots over
    // 15s, ≈2.9/s), not zero (an earlier same-day "~76 shots, ~all
    // unwritten" reading turned out to be from a burst silently running
    // 2s/~10 shots longer than configured -- see BuildBlockCmd's Burst-
    // branch fix -- so it wasn't representative). Used for the "drain
    // during this block's own hold" step in ComputeWrMsPerBlock/
    // AdvanceOccupancyThroughBlock; m_cardCalibCache (post-hold) remains
    // correct for the trailing wait-after-shooting-stops step and for
    // genuinely idle gaps between blocks. Falls back to m_cardCalibCache
    // when not yet calibrated for a model, so WR predictions degrade
    // gracefully rather than going to zero.
    std::map<std::string, double> m_concurrentWriteCalibCache;  // camModel -> shots/sec
    void LoadConcurrentWriteCalibCache();
    // Returns the concurrent rate if calibrated, else falls back to the
    // post-hold rate (m_cardCalibCache), else 0 -- single lookup point so
    // every WR caller degrades the same way.
    double ConcurrentWriteFpsFor(std::string_view camModel) const;

    // ── Buffer capacity calibration ──────────────────────────────────────────
    // How many shots fit before the camera's rate slows (buffer full). Also
    // the source of card write-speed calibration above, via the post-
    // slowdown sustained fps -- see RenderSequencerButtons' auto-save block.
    struct BufferCapacitySample {
        std::string camModel;
        int         totalShots          = 0;
        int         bufferCapacityShots = 0;
        double      fastFps             = 0.0;
        double      slowFps             = 0.0;
        bool        slowdownObserved    = false;
        // Precise post-burst ARM-drain measurement (ms), piggybacked onto
        // the SAME burst that produced the fields above -- see
        // FireBufferCapacityWriteSpeedTest(). -1 = not attempted (no
        // slowdown observed, so nothing to drain-time) or the ARM call
        // failed; the auto-save block falls back to slowFps in that case
        // (matches pre-2026-08-01 behavior, e.g. for a BufferCapacity block
        // fired from an old saved Timeline via SeqCamThreadProc, which
        // never sets this field).
        int         preciseDrainMs      = -1;
    };
    std::vector<BufferCapacitySample> m_bufferCapacityBuf;

    // ── Buffer Capacity + Write-Speed Calibration (combined, direct-fire) ───
    // Merged 2026-08-01: the held burst that determines buffer capacity
    // already leaves the buffer pegged at that capacity when it stops (if a
    // slowdown was observed), so the precise post-burst ARM-drain
    // measurement -- previously a separate "Write-Rate Calibration" step
    // that required capacity to already be known from a prior run --
    // piggybacks directly on THIS SAME burst instead of needing a second
    // one. One click, one burst, both numbers auto-saved. If no slowdown is
    // observed within the hold (some camera/card combos -- e.g. ILCE-7M4 +
    // CFexpress, confirmed by operator -- never fill their buffer even at
    // HI+), that's a valid, useful result on its own: the card keeps up
    // with the camera, no write bottleneck exists, and there is nothing to
    // time a drain for -- reported as such, not as a failure.
    int                m_bufCapCalibCamIdx  = 0;   // index into m_cameras
    std::thread        m_bufCapCalibThread;
    std::atomic<bool>  m_bufCapCalibRunning{false};
    void FireBufferCapacityWriteSpeedTest();  // "Run Test" button
    // "Set" (arm only, no shoot) -- sends a plain "arm" command (drive
    // cont-hi-plus + ss/iso/f) so the drive-mode change and its buffer-clear
    // wait (up to kDriveModeVerifyMs) happen and get cached BEFORE "Run
    // Test" starts the timed hold. Without this, the FIRST "Run Test" click
    // after the camera was on a different drive mode pays that confirm
    // delay out of its own hold window, shortening the actual burst and
    // corrupting the buffer-capacity inflection measurement (same failure
    // mode the old manual write-rate tool's "Set" button existed to avoid --
    // see Change log 2026-07-26).
    std::atomic<bool> m_bufCapCalibArming{false};
    std::atomic<int>  m_bufCapCalibArmResult{-2};  // -2 = no attempt yet, -1 = failed, >=0 = latency ms
    void FireBufferCapacityArm();  // "Set" button
    bool               m_showBufCapCalibWnd = false;
    void RenderBufferCapacityCalibWindow();
    // Structured result for RenderBufferCapacityCalibWindow's own display --
    // m_lastResult stays the shared, single-line, log-style status string
    // used app-wide, but the operator looking at THIS window wants the
    // numbers laid out as separate readable fields, not one dense sentence.
    // Populated by the auto-save block (RenderSequencerButtons) right
    // alongside m_lastResult.
    struct BufCapCalibResultDisplay {
        bool        valid                 = false;
        std::string camModel;
        int         bufferCapacityShots   = 0;
        int         totalShots            = 0;
        double      fastFps               = 0.0;
        bool        slowdownObserved      = false;
        bool        precise               = false;  // true = piggybacked ARM drain, false = rough slowFps fallback
        double      writeSpeedShotsPerSec = 0.0;
        bool        dbSaved               = false;
    };
    BufCapCalibResultDisplay m_bufCapCalibResult;

    // ── Concurrent Write-Rate Calibration (advanced, long-hold) ──────────────
    // See m_concurrentWriteCalibCache comment above for the physical
    // rationale. Deliberately a MUCH longer hold than the combined test
    // above (default 45s vs that test's 30s ceiling that stops on first
    // detection) -- this one must stay saturated well past the inflection
    // point to accumulate enough post-inflection samples for a stable
    // average (a short hold's handful of samples varied 3.3-4.9 shots/s
    // run to run on real hardware, unusable as a calibration input).
    // Shares m_bufCapCalibCamIdx (same camera picker) but needs its own
    // running/result state since it's a separate, much longer operation.
    int                m_concurWriteCalibHoldSec = 45;
    std::atomic<bool>  m_concurWriteCalibRunning{false};
    void FireConcurrentWriteRateTest();  // "Run Test" button (advanced section)
    struct ConcurWriteResultDisplay {
        bool        valid            = false;
        std::string camModel;
        int         totalShots       = 0;
        int         bufferCapacityShots = 0;
        double      fastFps          = 0.0;
        double      concurrentFps    = 0.0;  // AnalyzeBufferCapacity's slow_fps, long-hold-averaged
        bool        slowdownObserved = false;
        bool        dbSaved          = false;
    };
    ConcurWriteResultDisplay m_concurWriteCalibResult;
    // Pending result handoff from the background thread -- drained and
    // saved directly inside RenderBufferCapacityCalibWindow() each frame
    // it's open (self-contained, unlike m_bufferCapacityBuf's shared
    // RenderSequencerButtons auto-save path, which this test must NOT go
    // through -- it saves to a different table and must not overwrite
    // buffer_capacity_calibration/card_write_calibration with its own,
    // deliberately-long-hold numbers).
    struct ConcurWritePending {
        bool        haveResult          = false;
        std::string camModel;
        int         totalShots          = 0;
        int         bufferCapacityShots = 0;
        double      fastFps             = 0.0;
        double      concurrentFps       = 0.0;
        bool        slowdownObserved    = false;
    };
    ConcurWritePending m_concurWritePending;  // guarded by m_seqCalibMtx

    // ── WB (Write-Buffer) Calibration ────────────────────────────────────────
    // No dedicated test/thread/UI -- collected as a side effect of running
    // "1. Bracket ARM Calibration" via the normal TEST RUN/RUN buttons, same
    // as m_armCalibBuf. An earlier direct-fire probe design (own thread,
    // arm-then-shoot loop firing immediately with no settle gap) broke the
    // camera's bracket sequencing on real hardware 2026-08-02 -- only 1/N
    // shots fired whenever the arm needed real busy-retry recovery, even
    // though the property itself read back confirmed. Reusing the proven,
    // already-paced sequencer path (SeqCamThreadProc's sendArm lambda)
    // avoids that regime entirely. See ArmCalibSample above -- same
    // measurement, additionally keyed by ev (arm_calibration is count-only).
    struct WbCalibSample { int count = 0; std::string ev; int latMs = 0; std::string camModel; };
    std::vector<WbCalibSample> m_wbCalibBuf;  // guarded by m_seqCalibMtx, same as m_armCalibBuf
    // Aggregates m_wbCalibBuf (min/avg/max per (count,ev)) and saves to
    // wb_calibration -- called alongside SaveArmCalibFromBuf from the same
    // per-model "SAVE CALIB" action, no separate button.
    void SaveWbCalibFromBuf(const std::string& camModel);

    // camModel -> (count,ev) -> ready_avg_ms. Loaded from DB at startup;
    // reloaded after SaveWbCalibFromBuf(). Empty (or missing key) means
    // "not yet calibrated for this variant" -- ComputeWrMsPerBlock falls
    // back to the leaky-bucket formula in that case, same graceful-
    // degradation pattern as every other calibration cache.
    std::map<std::string, std::map<std::pair<int, std::string>, int>> m_wbCalibCache;
    void LoadWbCalibCache();  // DB -> m_wbCalibCache
    // Returns the measured ready time if calibrated for (camModel,count,ev),
    // else -1 -- single lookup point so ComputeWrMsPerBlock's fallback logic
    // stays in one place.
    int WbReadyMsFor(std::string_view camModel, int count, const std::string& ev) const;

    // camModel -> buffer_capacity_shots (most recent one-off measurement).
    // Used only as ComputeWrMsPerBlock's occupancy ceiling clamp.
    std::map<std::string, int> m_bufferCapacityCache;

    void LoadCardCalibCache();       // DB -> m_cardCalibCache
    void LoadBufferCapacityCache();  // DB -> m_bufferCapacityCache

    // ── Drive-mode fps calibration ────────────────────────────────────────────
    // camModel -> drive -> real measured fps (millisecond precision, from
    // AnalyzeBufferCapacity's per-shot capture timestamps -- see DriveFpsEntry
    // in Database.h for why this replaces DriveFpsEstimate()'s old hardcoded
    // guess table). Populated via "Measure <drive> FPS" in the Write-Rate
    // Calibration window, which runs buffer_capacity_calib for a short,
    // non-overflowing hold and saves its returned fast_fps directly.
    std::map<std::string, std::map<std::string, double>> m_driveFpsCalibCache;
    void LoadDriveFpsCalibCache();   // DB -> m_driveFpsCalibCache

    // ── Sensor physical size ──────────────────────────────────────────────────
    // camModel -> {widthMm, heightMm}, factory reference data (see
    // SensorSizeEntry in Database.h) -- replaces the old hardcoded
    // 35.9x24.0mm constant used for every model's FOV/frame overlay in the
    // Solar Simulator (RenderSolarView's camera-frame/Live-View/label loops).
    std::map<std::string, std::pair<float, float>> m_sensorSizeCache;
    void LoadSensorSizeCache();      // DB -> m_sensorSizeCache
    // Real sensor size if known for camModel; else the Sony full-frame
    // default (35.9 x 24.0mm) that every camera used before this table
    // existed. Member (not static) so it can consult m_sensorSizeCache.
    std::pair<float, float> SensorSizeMmFor(std::string_view camModel) const;
    // Real fps if calibrated for (camModel, drive); else the old rough guess.
    // Member (not static) so it can consult m_driveFpsCalibCache. Only ever
    // feeds PRE-RUN prediction (BlockShotCount / PredictedShotOffsetsMs) --
    // a live run always corrects with the real captures count instead.
    float DriveFpsEstimate(const std::string& drive, std::string_view camModel) const;
    // How many frames a block is predicted to fire (exact for Single/
    // Bracket; fps x duration estimate for Burst/BufferCapacity, via
    // DriveFpsEstimate) -- feeds ComputeWrMsPerBlock's occupancy walk.
    int BlockShotCount(const TLBlock& b, std::string_view camModel) const;
    // Predicted per-shot timestamp offsets (ms, relative to a block's own
    // start) for the live buffer-occupancy leaky-bucket model. Bracket reuses
    // BracketExposureSumMs's exact per-stop physics; Burst/BufferCapacity
    // fall back to flat DriveFpsEstimate spacing (no analytic schedule
    // possible for continuous drive) -- a real capture count still corrects
    // the total once a block's response lands, just not the intra-block shape.
    std::vector<int64_t> PredictedShotOffsetsMs(const TLBlock& b, std::string_view camModel) const;
    // Advances a buffer-occupancy checkpoint (shots, atMs) through block
    // blk's predicted per-shot schedule, applying the leaky-bucket
    // recurrence at each shot up to uptoMs. writeFps should be the
    // CONCURRENT (while-shooting) rate -- see m_concurrentWriteCalibCache
    // comment -- since every caller walks a span that's still inside or
    // immediately after this block's own active shooting. If realShotCount
    // >= 0, the predicted list is truncated/extended to that count first --
    // see App.cpp definition for full detail.
    void AdvanceOccupancyThroughBlock(const TLBlock& blk, int64_t blockStartMs,
                                       int realShotCount, double writeFps,
                                       double capacityShots, int64_t uptoMs,
                                       double& shots, int64_t& atMs,
                                       std::string_view camModel) const;

    // ── Drive-mode fps calibration UI state ───────────────────────────────────
    // Companion measurement to write-rate: how many frames/sec each drive
    // mode (LO/MID/HI/HI+) actually fires, BEFORE the buffer starts
    // throttling it -- needed because DriveFpsEstimate's old hardcoded guess
    // (whole-number 3/5/8/10) is used to predict Burst block widths on the
    // Timeline, and a wrong fps there means the predicted block length never
    // matches what the camera actually does, independent of the write-rate
    // question entirely. Uses buffer_capacity_calib's own fast_fps (already
    // millisecond-precision, from real per-shot capture timestamps) with a
    // short (non-overflowing) hold -- one button per drive, no manual
    // "add sample" step needed since fast_fps is already a clean, direct
    // measurement.
    //
    // m_driveFpsCalibDurSec is an operator-adjustable UI field, NOT a fixed
    // constant: a flat 10s (the original guess) overflows HI+'s buffer on
    // real hardware (HI+ ~10fps x 10s = ~100 shots vs. a 65-shot capacity,
    // confirmed 2026-07-26 -- the camera audibly/visibly dropped into
    // write-limited slow mode mid-test, contaminating the very baseline this
    // test exists to measure cleanly). FireDriveFpsSample() additionally
    // clamps the ACTUAL duration used, per drive, against known/estimated
    // fps and buffer_capacity_shots (with a safety margin) so a bad manual
    // value can't overflow the buffer regardless of what's typed here.
    int                  m_driveFpsCalibDurSec = 5;
    std::thread          m_driveFpsCalibThread;  // separate from m_bufCapCalibThread -- independent operation
    std::atomic<bool>    m_driveFpsCalibRunning{false};
    int                  m_driveFpsCalibBusyIdx = -1;  // which of the 4 drives is currently running, -1 = none
    std::atomic<double>  m_driveFpsCalibResult{-1.0};  // -1 = no pending result, else measured fps
    void FireDriveFpsSample(int driveIdx);  // "Measure <drive> FPS" button
    // "Set" per drive (arm only, no shoot) -- same reasoning as
    // FireBufferCapacityArm: buffer_capacity_calib's own drive-mode change
    // (if the camera was on a different drive) pays its confirm delay out
    // of the SAME timed hold FireDriveFpsSample uses to measure fast_fps,
    // silently shortening that hold and corrupting the very first
    // measurement for each drive (operator-reported 2026-08-01: "pierwszy
    // raz jest jałowy" -- the first run for a drive is wasted/void). One
    // arm state shared across all 4 drives (only one calibration op can be
    // in flight at a time on this camera/pipe anyway); m_driveFpsArmBusyIdx
    // says which drive is currently arming.
    std::atomic<bool>    m_driveFpsArming{false};
    int                  m_driveFpsArmBusyIdx = -1;
    std::atomic<int>     m_driveFpsArmResult{-2};  // -2 = no attempt yet, -1 = failed, >=0 = latency ms
    void FireDriveFpsArm(int driveIdx);  // "Set" button, per drive

    // ── Card write-speed calibration ─────────────────────────────────────────
    // Historical note: this used to be a standalone "Write-Rate Calibration"
    // window/mechanism (manual 4-speed burst-and-visually-read-the-camera
    // tool 2026-07-26 to 2026-08-01, then an automatic "Large-Backlog Test"
    // needing a known buffer_capacity_shots as input). Both were removed
    // 2026-08-01, merged into the combined "Buffer Capacity + Write-Speed
    // Calibration" (m_bufCapCalibCamIdx and friends, below) -- see that
    // comment for why the merge is physically sound and what happened to
    // the manual path. card_write_calibration itself is still just one
    // scalar per model (no per-count keying) -- see CardCalibEntry in
    // Database.h.
    // WrEstMs: card-write-buffer drain time for a given virtual occupancy
    // (shots currently estimated "pending write") at this camera's
    // calibrated sustained write rate. Returns 0 when camModel isn't
    // calibrated yet or occupancy is already 0.
    int64_t WrEstMs(std::string_view camModel, double occupancyShots) const;
    // Walks `tr`'s blocks left-to-right maintaining a continuous virtual
    // card-write-buffer occupancy (shots pending write): each block adds its
    // own shot count and drains at the camera's calibrated CONCURRENT write
    // rate for the duration its own shutter is held (see
    // m_concurrentWriteCalibCache comment -- real and non-negligible,
    // confirmed via the camera's own buffer readout), then at the POST-HOLD
    // sustained write rate through any real idle gap to the next block,
    // clamped to the calibrated buffer capacity ceiling. The occupancy is
    // carried across EVERY block
    // (including same-param runs where no ARM happens) rather than being
    // reset at group boundaries — a WR wait (long enough to drain occupancy
    // to 0) is only surfaced right before a block that needs an ARM
    // (BlockParamsDiffer), since that's the only point a camera-side command
    // can actually be rejected by a still-full buffer. Bounded by
    // tr.blocks.size(), no recursion. Returned vector is indexed 1:1 with
    // tr.blocks; entries for blocks that don't trigger a transition are 0.
    // Shared by every piece of code that needs to know "how long is the
    // WR+ARM zone after block bi" — the main Timeline render pass, drag
    // snap-to-prev, click hit-testing, the overlap check, and the
    // selection-outline extent — so all of them agree with what the gray WR
    // bar actually shows.
    //
    // Replaces an earlier group-based model (sum shots since the last ARM,
    // compare against real elapsed group time) that could badly underpredict
    // an isolated slow-drive burst — confirmed on real hardware 2026-07-25:
    // a 25s cont-lo burst was predicted to need ~0 WR but the camera actually
    // rejected the next property change for ~35s.
    std::vector<int64_t> ComputeWrMsPerBlock(const TLTrack& tr,
                                              std::string_view camModel) const;

    // ── Live buffer occupancy (TEMPORARY diagnostic) ─────────────────────────
    // Real-time running estimate of shots pending write, separate from
    // ComputeWrMsPerBlock's pre-run PREDICTION over the whole static timeline.
    // Displayed live in the camera status table (replaces the "Mode" row,
    // which is always "M" and therefore not operator-relevant) so it can be
    // visually checked against real hardware behavior (e.g. a video
    // recording of the access light) while the occupancy model itself is
    // still being validated -- see Change log 2026-07-26.
    //
    // Modeled as a leaky-bucket queue (+1 per shot, continuous drain at the
    // camera's calibrated CONCURRENT write rate while shooting -- see
    // m_concurrentWriteCalibCache comment -- or the post-hold rate once
    // idle) walked through each block's PREDICTED PER-SHOT schedule
    // (App.cpp's PredictedShotOffsetsMs / AdvanceOccupancyThroughBlock)
    // rather than a flat per-block average --
    // a flat average loses the real, often wildly uneven intra-block shape
    // (e.g. a slow symmetric bracket's per-stop exposure times: 3.2s, 1.6s,
    // 0.8s, 0.4s, 0.2s, ...) and the resulting error compounds over time.
    // `shots`/`shotsAtMs` is the last exactly-computed checkpoint (walked up
    // to a real shot time or the current in-flight interpolation point);
    // real capture counts (Bracket/Single always know their exact count;
    // Burst/BufferCapacity from the response's "captures" field) replace the
    // prediction the moment a block's response lands, so drift can only
    // accumulate WITHIN one in-flight block, never across the whole session.
    struct LiveOccupancy {
        double  shots     = 0.0;  // occupancy at shotsAtMs (last exact checkpoint)
        int64_t shotsAtMs = 0;    // UtcNowMs() at that checkpoint

        // Block currently executing (no response yet) -- SeqCamThreadProc
        // stores a copy here right before sending so CurrentBufferOccupancy
        // can regenerate its predicted per-shot schedule on demand. Cleared
        // once the real response (success or error) lands.
        bool    inFlight        = false;
        TLBlock inFlightBlock;
        int64_t inFlightStartMs = 0;
    };
    std::array<LiveOccupancy, kMaxCamTracks> m_liveOccupancy{};
    mutable std::mutex                       m_liveOccupancyMtx;

    // Current estimated occupancy for camIdx, decayed from the last tracked
    // event to now at camModel's calibrated write rate. Returns -1 when
    // camModel has no card_write_calibration yet (nothing to show).
    double CurrentBufferOccupancy(int camIdx, std::string_view camModel) const;

    // Resolves a camera track's live model name for BlockDurMs()/ArmEstMs()
    // per-model lookups. Two overloads: by position among camera tracks only
    // (0-based, top-to-bottom -- same numbering as BuildBlockCmd's camIdx and
    // TrackLabel/TrackColor in RenderTimelineBottom), or by raw track index
    // into m_tracks (converts to camPos first). Returns "" if unresolvable
    // (camera not yet connected/reporting a model) -- callers already treat
    // an empty camModel as "use the generic/fallback formula".
    std::string CamModelForPos(int camPos) const;
    std::string CamModelForTrackIndex(int ti) const;

    // True if a camera with this GUID is in the current live m_cameras
    // snapshot (i.e. actually connected right now) -- used to hide Solar
    // Simulator elements (frame overlays, LV sliders) for a CamConfig entry
    // whose camera has since been disconnected/returned. CamConfig entries
    // themselves are never deleted (see camera_config table comment in
    // Database.h) -- this only gates rendering.
    bool IsCameraOnline(const std::string& guid) const;

    // Snap to Seconds: rounds so the OFFSET FROM THE NEAREST CONTACT
    // (C1/C2/C3/C4) is a whole number of seconds — matching the Relative
    // ruler row — not so atMs itself lands on a whole UTC second (contacts
    // themselves have sub-second precision, e.g. C2=18:28:53.700).
    // Falls back to rounding to the nearest whole UTC second when no
    // contact times are available yet.
    int64_t SnapMsToRelativeSecond(int64_t ms);

    // Snapshots m_contacts (thread-safe) and returns whichever engine the
    // user picked as "primary" (m_primaryContactSrc), falling back to the
    // other engine if the chosen one isn't valid yet. Used everywhere
    // Timeline generation / countdowns need a single, explicit answer
    // instead of biasing toward IQP silently.
    ContactTimes PrimaryContacts();

    // ── Execution log ────────────────────────────────────────────────────────
    // Sequence counter reset at each TEST RUN / RUN start.
    // One per-camera thread now increments this concurrently — must be atomic.
    std::atomic<int> m_execSeqNum{0};

    // ── GUI sequencer state ───────────────────────────────────────────────────
    std::atomic<GuiSeqMode> m_guiSeqMode{GuiSeqMode::Idle};

    // Per-track "next unfired block" index for up to kMaxCamTracks camera tracks.
    // Each index is written only by its own SeqCamThreadProc(camIdx) while
    // running (no two threads touch the same slot); read by main thread only
    // after all threads have joined.
    static constexpr int kMaxAudioTracks = 2;
    int m_seqNextBlock[kMaxCamTracks]    = {};
    // m_audioNextBlock is written by m_audioSeqThread during the run (live progress) and
    // may be read by the render thread concurrently for display — must be atomic.
    std::atomic<int> m_audioNextBlock[kMaxAudioTracks]{};

    // Snapshot of playhead and real-time at the last Start/Resume
    int64_t m_testPlayheadAtStart = -1;
    int64_t m_testStartRealMs     = -1;

    // One sequencer thread per camera track (see SeqCamThreadProc), joined in
    // StopSeqThread(). Unused slots (fewer than kMaxCamTracks camera tracks)
    // are left non-joinable.
    std::thread       m_seqThread[kMaxCamTracks];
    std::thread       m_audioSeqThread;   // independent audio-only tick loop
    std::atomic<bool> m_seqRun{false};
    // Guards m_seqCalibBuf, which per-camera sequencer threads now push to concurrently.
    std::mutex        m_seqCalibMtx;

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

    // "Add Simulated Camera" (Options menu) -- lets the operator register a
    // camera_config row (model + focal length, drives the Solar Simulator FOV
    // frame + a Timeline track) without ever physically connecting hardware,
    // e.g. for pre-trip planning. Guid is synthesized ("SIM-<model>-<n>") so
    // it never collides with a real CrSDK GUID.
    bool        m_showAddSimCam = false;
    char        m_simCamModelBuf[64] = {};

    // ── Live View overlay on solar simulator ──────────────────────────────────
    // Frames arrive via named SHM, decoded JPEG→RGBA by m_lvThread, uploaded to
    // D3D11 by CreateLvTextures (render thread only), rendered in
    // RenderSolarView as alpha-blended quad matching camera FOV rect.
    // m_lvEnabled/m_lvOpacity/m_focusMagLevel/m_lvSrv/m_lvPending/m_lvNewData
    // are all indexed by "ci" = position in m_camConfigs (persistent,
    // GUID-keyed identity -- stable across sessions and camera-count
    // changes). That index is NOT the same as the live SRV camera-list
    // position (CameraController::StartLiveView's camIdx / SRV's
    // CommandHandler::CamIndex()), which only depends on THIS session's
    // connection order -- the two coincide only by coincidence (e.g. a
    // single camera, or cameras always connecting in first-seen order).
    // m_lvLiveCamIdx[ci] bridges the two: refreshed every frame by
    // MergeCamerasIntoCamConfigs() (guid match against the live m_cameras
    // snapshot), -1 when that config slot's camera isn't currently
    // connected. Anything that talks to the wire protocol (the "cam" field
    // sent to SRV, or the TotalControl_LV_<N> SHM name) must go through
    // m_lvLiveCamIdx[ci], never send/read "ci" directly -- see the bug this
    // fixed: with >1 camera and a discovery order different from live
    // connection order, one camera's LV feed showed up under a different
    // camera's slot.
    bool   m_lvEnabled[kMaxCamTracks] = {};       // derived: true when m_lvOpacity[ci] >= 0.05
    float  m_lvOpacity[kMaxCamTracks] = {};       // per-camera opacity 0–1 (persisted to DB)
    std::atomic<int> m_lvLiveCamIdx[kMaxCamTracks]; // ci -> live SRV camIdx, -1 = offline; read by background m_lvThread

    // Focus Magnifier (CrDeviceProperty_Focus_Magnifier_Setting) — needed because
    // the camera only auto-triggers its live-view zoom when it senses a native
    // lens's electronic focus ring turning. Passive/manual optics (e.g. a
    // telescope) never send that signal, so this has to be set remotely instead.
    // Confirmed on real hardware (ILCE-7RM4A, 2026-07-20). Button index into
    // RenderSolarView's kFmLabels/kFmValues: 0=Off / 1=x1.0 / 2=x5.9 / 3=x11.9
    // — see CommandHandler.cpp DecodePropValue for the raw wire encoding.
    int    m_focusMagLevel[kMaxCamTracks] = { 1, 1, 1, 1 };

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
    void AddBracketArmCalibrationPreset(int reps);
    void AddShutterSpeedSweepPreset();
    int  m_presetTargetTrack = 0;  // camera track index that receives generated preset blocks

    // "Single Picture Preset Generator..." / "Bracket Set Generator..."
    // (Photo Sequence menu) -- configurable batch block generators, replace
    // the old fixed "One Picture Per Minute" / "Add Brackets Photo Series"
    // presets. Each repeats at intervalSec within ONE contact window at a
    // time, picked by which of the 3 range buttons (C1-C2/C2-C3/C3-C4) the
    // operator clicks -- not a checkbox multi-select, so the window it
    // generated into is unambiguous. Two separate small windows rather than
    // one combined dialog -- less crowded, and Single/Bracket are
    // independent choices anyway. No Burst generator: a repeating
    // held-shutter block has no practical use (explicit product decision,
    // 2026-08-01).
    enum class GenPresetRange { C1C2, C2C3, C3C4 };
    struct GenPresetRangeCfg {
        int         intervalSec = 60;
        std::string ss          = "1/100";
        int         iso         = 100;
        std::string fstop       = "8.0";
    };
    bool               m_showSinglePresetWnd   = false;
    bool               m_showBracketPresetWnd  = false;
    GenPresetRangeCfg  m_genPresetSingle;
    GenPresetRangeCfg  m_genPresetBracket;
    std::string        m_genPresetBracketEv    = "1.0ev";
    int                m_genPresetBracketCount = 5;
    void RenderSinglePresetModal();
    void RenderBracketPresetModal();
    void GenerateRangePresetBlocks(GenPresetRangeCfg& cfg, BlockType type, GenPresetRange range);
    void ClearRangePresetBlocks(GenPresetRange range);
    void RenderPresetTargetTrackCombo(std::vector<TLTrack>& tracks, int& presetTargetTrack);
};

} // namespace TotalControl
