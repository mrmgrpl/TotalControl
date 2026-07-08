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

struct BktCalibEntry {
    std::string camModel;
    int         count     = 0;
    std::string ev;
    int         latMaxMs  = 0;   // max latency across reps
    int         latAvgMs  = 0;
    int         latMinMs  = 0;
    int         reps      = 3;   // number of repetitions measured
    std::string ss        = "1/100";
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
