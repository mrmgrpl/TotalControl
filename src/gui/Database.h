#pragma once
#include "TzEntry.h"
#include "EclipseEntry.h"
#include "BesselCalc.h"
#include "Timeline.h"
#include "EphClient.h"
#include <map>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace TotalControl {

// ─── Camera configuration ─────────────────────────────────────────────────────

enum class CamTrackMode { Sun = 0, Moon = 1, Horizon = 2 };

struct CamConfig {
    std::string  guid;               // primary key — permanent hardware ID
    std::string  model;              // e.g. "ILCE-7RM4A" — recorded once on first detection
    int          focalMm      = 0;   // configured lens focal length (0 = unset → no frame drawn)
    bool         applyP       = true; // true → rotate solar frame by P_rad; false → horizontal
    CamTrackMode trackMode    = CamTrackMode::Sun;
    double       horizonAltDeg = 0.0;   // Alt when trackMode == Horizon
    double       horizonAzDeg  = 180.0; // Az  when trackMode == Horizon
};

// ─── Bracket calibration ─────────────────────────────────────────────────────
// Keyed by (camModel, count) only — NOT ev, NOT ss. Confirmed empirically
// 2026-07-22: once BracketExposureSumMs()'s analytic exposure-time sum is
// subtracted from a raw measured bracket-shoot duration, the residual
// (per-shot mirror/shutter/buffer/USB/SDK overhead) is ev- and ss-independent
// for a given (model, count) — see CLAUDE.md Change log. latAvgMs etc. store
// that residual, not the raw total, so this table has the same shape as
// arm_calibration (which the App.cpp SAVE CALIB code already mirrors).

struct BktCalibEntry {
    std::string camModel;
    int         count     = 0;
    int         latMaxMs  = 0;   // max per-shot overhead residual across reps
    int         latAvgMs  = 0;   // avg per-shot overhead residual (used by BlockDurMs)
    int         latMinMs  = 0;
    int         reps      = 0;   // number of samples averaged
    int64_t     createdMs = 0;
};

// ARM (DriveMode-change) latency — separate from BktCalibEntry's bracket
// *shoot* time above. Keyed by (camModel, count) only, not ev: measured ARM
// latency doesn't show a consistent ev dependency (see docs/
// arm_latency_bionz_whitepaper.md), matching ArmEstMs()'s existing
// count-only formula shape.
struct ArmCalibEntry {
    std::string camModel;
    int         count     = 0;
    int         latMaxMs  = 0;
    int         latAvgMs  = 0;
    int         latMinMs  = 0;
    int         reps      = 0;
    int64_t     createdMs = 0;
};

// Write-buffer-ready calibration — how long after a bracket variant finishes
// firing until the camera genuinely accepts the next DriveMode change,
// measured DIRECTLY (not modeled): one long-budget "arm" probe targeting a
// value different from cache (forcing a real round-trip past the cached
// fast-path) right after the bracket completes, its returned latency_ms IS
// the sample. Keyed by (camModel, count, ev) — unlike ArmCalibEntry/
// BktCalibEntry this is NOT assumed ev-independent: a bigger ev step means
// longer real per-stop exposure times within the bracket's own shooting
// window, which plausibly changes how much the buffer has already drained
// by the time the last shot lands. Same shape as ArmCalibEntry otherwise.
struct WbCalibEntry {
    std::string camModel;
    int         count      = 0;
    std::string ev;           // e.g. "0.5ev" -- matches TLBlock::ev / bracket variant key
    int         readyMaxMs = 0;
    int         readyAvgMs = 0;
    int         readyMinMs = 0;
    int         reps       = 0;
    int64_t     createdMs  = 0;
};

// Card write-speed calibration — one row per camera model (PRIMARY KEY),
// unlike bracket/ARM calibration which are also keyed by count. This is a
// property of the camera+card combo's sustained write throughput, not of any
// particular block's shape, and the SDK has no way to identify which
// physical card is inserted -- an operator who swaps cards must re-run the
// "Card Write-Speed Calibration" preset. Each run overwrites the previous
// value for that model (no averaging across reps, unlike bracket/ARM calib —
// this is a one-off operator-run test, not a multi-variant sweep).
struct CardCalibEntry {
    std::string camModel;
    double      shotsPerSec    = 0.0;
    int         measuredShots  = 0;
    int64_t     measuredMs     = 0;
    int64_t     createdMs      = 0;
};

// Buffer capacity — complementary to CardCalibEntry: how many shots fit
// before the camera falls back to card-write-limited speed, not the
// steady-state write throughput itself. Same one-row-per-model, no-averaging
// rationale (operator-run one-off test, re-run after a card swap).
struct BufferCapacityEntry {
    std::string camModel;
    int         totalShots          = 0;   // shots fired during the test
    int         bufferCapacityShots = 0;   // shots before the slowdown (== totalShots if none seen)
    double      fastFps             = 0.0;
    double      slowFps             = 0.0; // == fastFps if no slowdown observed
    bool        slowdownObserved    = false;
    int64_t     createdMs           = 0;
};

// Drive-mode nominal fps — one row per (camModel, drive). Real, millisecond-
// precision measurement (from AnalyzeBufferCapacity's per-shot capture
// timestamps, the same fastFps already returned by buffer_capacity_calib)
// instead of App::DriveFpsEstimate()'s old hardcoded 3/5/8/10 guess table.
// Feeds App::BlockShotCount()/PredictedShotOffsetsMs()'s pre-run PREDICTION
// of how many shots a Burst/BufferCapacity block will fire -- a live run
// always corrects with the real captures count regardless, so this only
// affects Timeline block-width display and the WR/ARM bar predictions drawn
// before a sequence actually runs. Same one-row-per-key, no-averaging
// rationale as CardCalibEntry (operator-run one-off test, re-run after a
// card or lens swap that changes real continuous-drive throughput).
struct DriveFpsEntry {
    std::string camModel;
    std::string drive;      // "cont-lo" | "cont-mid" | "cont-hi" | "cont-hi-plus"
    double      fps        = 0.0;
    int64_t     createdMs  = 0;
};

// Sensor physical size — one row per camera model (PRIMARY KEY). Real
// manufacturer spec in mm, not a calibration measurement — ships as factory
// reference data in TotalControlDefaultConfig.db (same distribution
// mechanism as bracket_calibration/arm_calibration), replacing the old
// hardcoded 35.9x24.0mm constant used for every model's FOV/frame overlay
// in the Solar Simulator. App::SensorSizeMmFor() falls back to that same
// 35.9x24.0mm default for any model not present in this table.
struct SensorSizeEntry {
    std::string camModel;
    double      widthMm   = 0.0;
    double      heightMm  = 0.0;
    int64_t     createdMs = 0;
};

// ─── Named snapshot ───────────────────────────────────────────────────────────

struct SnapshotInfo {
    int64_t     id        = 0;
    std::string name;
    int64_t     createdMs = 0;
};

class Database {
public:
    // Open for read/write (creates file if missing).
    bool Open(const std::wstring& path);

    // Open existing file read-only. Returns false if file doesn't exist.
    bool OpenReadOnly(const std::wstring& path);

    void Close();
    bool IsOpen() const noexcept { return m_db != nullptr; }

    ~Database() { Close(); }

    // Execute arbitrary SQL (DDL, multi-statement, no results needed).
    bool Exec(std::string_view sql);

    // settings table helpers
    std::string GetSetting(const char* key, const char* def = "") const;
    void        SetSetting(const char* key, const char* value);
    int         GetSettingInt(const char* key, int def = 0) const;
    void        SetSettingInt(const char* key, int value);

    // reference data queries
    std::vector<TzEntry>     LoadTimezones() const;
    std::vector<EclipseEntry> LoadEclipses(int fromYear = 2026, int toYear = 2036) const;
    BesselianElements        LoadBesselianElements(int year, int month, int day) const;

    // timeline persistence (TotalControlConfig.db)
    void                     SaveTimeline(const std::vector<TLTrack>& tracks);
    std::vector<TLTrack>     LoadTimeline() const;

    // bracket calibration (TotalControlConfig.db) — per-model empirical durations
    void                       CreateCalibTables();
    void                       SaveCalibData(const std::vector<BktCalibEntry>& entries);
    std::vector<BktCalibEntry> LoadCalibData(const std::string& camModel) const;
    std::vector<std::string>   LoadCalibModels() const;

    // ARM (DriveMode-change) latency calibration (TotalControlConfig.db) —
    // per-model, per-count. Feeds App::ArmEstMs() the same way LoadCalibData
    // feeds App::BlockDurMs().
    void                       CreateArmCalibTable();
    void                       SaveArmCalibData(const std::vector<ArmCalibEntry>& entries);
    std::vector<ArmCalibEntry> LoadArmCalibData(const std::string& camModel) const;
    std::vector<std::string>   LoadArmCalibModels() const;

    // Write-buffer-ready calibration (TotalControlConfig.db) — per-model,
    // per-count, per-ev; feeds App::ComputeWrMsPerBlock()'s Bracket-boundary
    // case directly as a measured lookup, instead of the leaky-bucket
    // occupancy/drain-rate formula used for Burst/BufferCapacity. See
    // WbCalibEntry comment above.
    void                      CreateWbCalibTable();
    void                      SaveWbCalibData(const std::vector<WbCalibEntry>& entries);
    std::vector<WbCalibEntry> LoadWbCalibData(const std::string& camModel) const;
    std::vector<std::string>  LoadWbCalibModels() const;

    // Card write-speed calibration (TotalControlConfig.db) — one row per
    // model, feeds App::WrEstMs() the way LoadArmCalibData feeds ArmEstMs().
    // This is the POST-HOLD drain rate (precise ARM-confirm-latency timing,
    // after the shutter has stopped) -- see ConcurrentWriteCalibEntry below
    // for why that's a physically different, generally SLOWER number than
    // the concurrent-write rate.
    void            CreateCardCalibTable();
    bool            SaveCardCalib(const CardCalibEntry& entry);  // false = sqlite3_step didn't return SQLITE_DONE
    std::vector<CardCalibEntry> LoadCardCalibAll() const;

    // Concurrent write-speed calibration (TotalControlConfig.db) — one row
    // per model. Added 2026-08-02 after hardware evidence (operator-run
    // rehearsal) showed the card's write throughput WHILE the shutter is
    // still actively firing (capture pipeline and background write sharing
    // I/O) is measurably SLOWER than the throughput measured by draining an
    // already-full buffer AFTER shooting stops (CardCalibEntry above) --
    // using the post-hold rate for the "does this block's own hold overflow
    // the buffer" calculation under-predicted real backlog by several
    // seconds. Measured via a long (default 45s) held burst that
    // deliberately stays saturated well past the inflection point
    // (buffer_capacity_calib with stop_on_slowdown=false), averaging many
    // post-inflection intervals for a statistically stable sustained rate
    // -- same AnalyzeBufferCapacity slow_fps mechanism as
    // BufferCapacityEntry, just run long enough to be trustworthy (the
    // short buffer-capacity/drive-fps tests only see a handful of
    // post-inflection samples, too noisy to use as a calibration input on
    // their own -- confirmed varying 3.3-4.9 shots/s run to run).
    struct ConcurrentWriteCalibEntry {
        std::string camModel;
        double      shotsPerSec   = 0.0;
        int         sampleShots   = 0;   // post-inflection shots averaged
        int64_t     measuredMs    = 0;
        int64_t     createdMs     = 0;
    };
    void            CreateConcurrentWriteCalibTable();
    bool            SaveConcurrentWriteCalib(const ConcurrentWriteCalibEntry& entry);
    std::vector<ConcurrentWriteCalibEntry> LoadConcurrentWriteCalibAll() const;

    // Buffer capacity calibration (TotalControlConfig.db) — one row per model.
    void            CreateBufferCapacityTable();
    bool            SaveBufferCapacity(const BufferCapacityEntry& entry);  // false = sqlite3_step didn't return SQLITE_DONE
    std::vector<BufferCapacityEntry> LoadBufferCapacityAll() const;

    // Drive-mode fps calibration (TotalControlConfig.db) — one row per
    // (model, drive), feeds App::DriveFpsEstimate().
    void            CreateDriveFpsTable();
    bool            SaveDriveFps(const DriveFpsEntry& entry);  // false = sqlite3_step didn't return SQLITE_DONE
    std::vector<DriveFpsEntry> LoadDriveFpsAll() const;

    // Sensor physical size (TotalControlConfig.db) — one row per model,
    // factory reference data (ships in TotalControlDefaultConfig.db).
    void            CreateSensorSizeTable();
    bool            SaveSensorSize(const SensorSizeEntry& entry);  // false = sqlite3_step didn't return SQLITE_DONE
    std::vector<SensorSizeEntry> LoadSensorSizeAll() const;

    // named timeline snapshots (TotalControlConfig.db)
    void                      CreateSnapshotTables();
    void                      SaveSnapshot(const std::string& name,
                                           const std::vector<TLTrack>& tracks);
    std::vector<SnapshotInfo> LoadSnapshotList() const;
    std::vector<TLTrack>      LoadSnapshot(int64_t id) const;
    bool                      SnapshotExists(const std::string& name) const;
    void                      DeleteSnapshot(int64_t id);

    // JPL Horizons ephemeris cache (TotalControlConfig.db)
    // eclipse_date: "YYYY-Mon-DD"; location: "lat,lon" key string.
    void                 CreateEphTables();
    void                 SaveEphRows(EphBody body, const std::vector<EphRow>& rows);
    std::vector<EphRow>  LoadEphRows(EphBody body) const;
    bool                 EphemerisExists(const std::string& eclDate,
                                         const std::string& location) const;
    void                 SetEphMeta(const std::string& eclDate,
                                    const std::string& location);

    // camera_config table (TotalControlConfig.db)
    void                   CreateCamConfigTable();
    void                   SaveCamConfig(const std::string& guid, const std::string& model,
                                         int focalMm, bool applyP,
                                         CamTrackMode trackMode = CamTrackMode::Sun,
                                         double horizonAltDeg = 0.0,
                                         double horizonAzDeg  = 180.0);
    std::vector<CamConfig> LoadCamConfigs() const;
    void                   DeleteCamConfig(const std::string& guid);

    // Delta T (IERS Earth-orientation bulletin) cache (TotalControlConfig.db)
    // Keyed by eclipseDate "YYYY-MM-DD". Refreshed at most once per 24h by
    // App's background fetch — see IersDeltaTClient. fetchedAtMs lets the
    // caller decide staleness; the cached value itself is always usable as
    // a fallback even when stale (better than the static Espenak catalog
    // value — see Change log 2026-07-21, Alessandro/besselianelements.com).
    struct DeltaTCache {
        double  dtSeconds   = 0.0;
        bool    predicted   = false;  // IERS Bulletin A forecast vs Bulletin B measured
        int64_t fetchedAtMs = 0;
    };
    void CreateDeltaTTable();
    void SaveDeltaT(const std::string& eclipseDate, double dtSeconds, bool predicted);
    bool LoadDeltaT(const std::string& eclipseDate, DeltaTCache& out) const;

    // Audio file duration cache (TotalControlConfig.db)
    // lang: uppercase 2-char tag e.g. "PL", "EN"; filename: bare name.
    // Key in returned map: "LANG/filename.mp3".
    void                              CreateAudioFilesTable();
    void                              SaveAudioFileDur(std::string_view lang,
                                                       std::string_view filename,
                                                       int32_t durMs);
    std::map<std::string, int32_t>    LoadAudioFileDurs() const;
    // Returns sorted list of language tags that already have rows in the table.
    std::vector<std::string>          LoadAudioCachedLangs() const;
    // Deletes all rows for given lang; pass empty to delete ALL rows.
    void                              ClearAudioFileDurs(std::string_view lang = {});

private:
    sqlite3* m_db = nullptr;
};

} // namespace TotalControl
