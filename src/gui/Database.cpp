#include "Database.h"
#include "sqlite3.h"
#include <windows.h>
#include <cassert>
#include <format>
#include <vector>

namespace TotalControl {

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// ─── Open ─────────────────────────────────────────────────────────────────────

bool Database::Open(const std::wstring& path) {
    assert(!path.empty());  // caller must supply a non-empty file path
    std::string p = WideToUtf8(path);
    if (sqlite3_open(p.c_str(), &m_db) != SQLITE_OK) {
        m_db = nullptr;
        return false;
    }
    sqlite3_exec(m_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    return true;
}

bool Database::OpenReadOnly(const std::wstring& path) {
    assert(!path.empty());  // caller must supply a non-empty file path
    std::string p = WideToUtf8(path);
    int rc = sqlite3_open_v2(p.c_str(), &m_db,
                              SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        m_db = nullptr;
        return false;
    }
    return true;
}

void Database::Close() {
    if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
}

// ─── Exec ─────────────────────────────────────────────────────────────────────

bool Database::Exec(std::string_view sql) {
    if (!m_db) return false;
    char* err = nullptr;
    int rc = sqlite3_exec(m_db, sql.data(), nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

// ─── Settings helpers ─────────────────────────────────────────────────────────

std::string Database::GetSetting(const char* key, const char* def) const {
    assert(key != nullptr && key[0] != '\0');  // key must be a valid non-empty C string
    if (!m_db) return def ? def : "";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(m_db,
        "SELECT value FROM settings WHERE key=?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    std::string result = def ? def : "";
    if (sqlite3_step(st) == SQLITE_ROW)
        result = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return result;
}

void Database::SetSetting(const char* key, const char* value) {
    assert(key != nullptr && key[0] != '\0');  // key must be a valid non-empty C string
    assert(value != nullptr);                  // value must not be null (use "" for empty)
    if (!m_db) return;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(m_db,
        "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?);",
        -1, &st, nullptr);
    sqlite3_bind_text(st, 1, key,   -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

int Database::GetSettingInt(const char* key, int def) const {
    std::string s = GetSetting(key, nullptr);
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

void Database::SetSettingInt(const char* key, int value) {
    SetSetting(key, std::format("{}", value).c_str());
}

// ─── Reference data ───────────────────────────────────────────────────────────

std::vector<TzEntry> Database::LoadTimezones() const {
    std::vector<TzEntry> result;
    if (!m_db) return result;

    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT code, label, iana FROM timezones ORDER BY sort_order, id;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return result;

    while (sqlite3_step(st) == SQLITE_ROW) {
        TzEntry e;
        auto col = [&](int i) -> std::string {
            const auto* p = sqlite3_column_text(st, i);
            return p ? reinterpret_cast<const char*>(p) : "";
        };
        e.code  = col(0);
        e.label = col(1);
        e.iana  = col(2);
        result.push_back(std::move(e));
    }
    sqlite3_finalize(st);
    return result;
}

std::vector<EclipseEntry> Database::LoadEclipses(int fromYear, int toYear) const {
    assert(fromYear <= toYear);
    std::vector<EclipseEntry> out;
    if (!m_db) return out;

    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT year, month, day, eclipse_type, td_ge,"
        "       lat_dd_ge, lng_dd_ge, central_duration, duration_secs, dt"
        " FROM eclipse_besselian"
        " WHERE year BETWEEN ? AND ?"
        " ORDER BY julian_date;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return out;
    sqlite3_bind_int(st, 1, fromYear);
    sqlite3_bind_int(st, 2, toYear);

    auto col = [&](int i) -> std::string {
        const auto* p = sqlite3_column_text(st, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    };

    while (sqlite3_step(st) == SQLITE_ROW) {
        EclipseEntry e;
        e.year         = sqlite3_column_int(st, 0);
        e.month        = sqlite3_column_int(st, 1);
        e.day          = sqlite3_column_int(st, 2);
        e.type         = col(3);
        e.timeGe       = col(4);
        e.latGe        = static_cast<float>(sqlite3_column_double(st, 5));
        e.lonGe        = static_cast<float>(sqlite3_column_double(st, 6));
        e.duration     = col(7);
        e.durationSecs = static_cast<float>(sqlite3_column_double(st, 8));
        e.dt           = static_cast<float>(sqlite3_column_double(st, 9));
        out.push_back(std::move(e));
    }
    sqlite3_finalize(st);
    return out;
}

BesselianElements Database::LoadBesselianElements(int year, int month, int day) const {
    assert(year  >= 1900 && year  <= 2200);  // sane eclipse calendar range
    assert(month >= 1    && month <= 12);
    assert(day   >= 1    && day   <= 31);
    BesselianElements b;
    if (!m_db) return b;

    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT t0, dt,"
        " x0,x1,x2,x3, y0,y1,y2,y3,"
        " d0,d1,d2, mu0,mu1,mu2,"
        " l10,l11,l12, l20,l21,l22,"
        " tan_f1, tan_f2, tmin, tmax"
        " FROM eclipse_besselian"
        " WHERE year=? AND month=? AND day=?"
        " LIMIT 1;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return b;

    sqlite3_bind_int(st, 1, year);
    sqlite3_bind_int(st, 2, month);
    sqlite3_bind_int(st, 3, day);

    if (sqlite3_step(st) == SQLITE_ROW) {
        auto d = [&](int i){ return sqlite3_column_double(st, i); };
        b.year  = year; b.month = month; b.day = day;
        b.t0    = d(0);  b.dt   = d(1);
        b.x0    = d(2);  b.x1   = d(3);  b.x2  = d(4);  b.x3  = d(5);
        b.y0    = d(6);  b.y1   = d(7);  b.y2  = d(8);  b.y3  = d(9);
        b.d0    = d(10); b.d1   = d(11); b.d2  = d(12);
        b.mu0   = d(13); b.mu1  = d(14); b.mu2 = d(15);
        b.l10   = d(16); b.l11  = d(17); b.l12 = d(18);
        b.l20   = d(19); b.l21  = d(20); b.l22 = d(21);
        b.tan_f1= d(22); b.tan_f2= d(23);
        b.tmin  = d(24); b.tmax = d(25);
        b.valid = true;
    }
    sqlite3_finalize(st);
    return b;
}

// ─── Timeline persistence ─────────────────────────────────────────────────────

static constexpr const char* kCreateTlTracks = R"SQL(
    CREATE TABLE IF NOT EXISTS tl_tracks (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        sort_order INTEGER NOT NULL DEFAULT 0,
        type       TEXT    NOT NULL DEFAULT 'camera',
        camera_id  TEXT    NOT NULL DEFAULT '',
        label      TEXT    NOT NULL DEFAULT '',
        focal_mm   INTEGER NOT NULL DEFAULT 0
    );
)SQL";

static constexpr const char* kCreateTlBlocks = R"SQL(
    CREATE TABLE IF NOT EXISTS tl_blocks (
        id            INTEGER PRIMARY KEY AUTOINCREMENT,
        track_id      INTEGER NOT NULL,
        at_ms         INTEGER NOT NULL DEFAULT -1,
        block_type    INTEGER NOT NULL DEFAULT 0,
        ss            TEXT    NOT NULL DEFAULT '1/100',
        iso           INTEGER NOT NULL DEFAULT 100,
        fstop         TEXT    NOT NULL DEFAULT '8.0',
        cnt           INTEGER NOT NULL DEFAULT 5,
        ev            TEXT    NOT NULL DEFAULT '1.0ev',
        burst_drive   TEXT    NOT NULL DEFAULT 'cont-hi-plus',
        burst_dur_ms  INTEGER NOT NULL DEFAULT 3000,
        audio_file    TEXT    NOT NULL DEFAULT '',
        audio_dur_ms  INTEGER NOT NULL DEFAULT 10000,
        label         TEXT    NOT NULL DEFAULT '',
        snap_to_prev  INTEGER NOT NULL DEFAULT 0,
        snap_to_sec   INTEGER NOT NULL DEFAULT 0
    );
)SQL";

void Database::SaveTimeline(const std::vector<TLTrack>& tracks) {
    assert(m_db != nullptr);  // must be open before saving
    assert(tracks.size() <= 64);  // bounded: 64 tracks is more than enough
    if (!m_db) return;

    Exec(kCreateTlTracks);
    Exec(kCreateTlBlocks);
    // Migration: add focal_mm/snap_to_sec if this is an older DB without the
    // column (error ignored if it exists)
    sqlite3_exec(m_db,
        "ALTER TABLE tl_tracks ADD COLUMN focal_mm INTEGER NOT NULL DEFAULT 0;",
        nullptr, nullptr, nullptr);
    sqlite3_exec(m_db,
        "ALTER TABLE tl_blocks ADD COLUMN snap_to_sec INTEGER NOT NULL DEFAULT 0;",
        nullptr, nullptr, nullptr);

    sqlite3_exec(m_db, "BEGIN;",                 nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "DELETE FROM tl_blocks;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "DELETE FROM tl_tracks;", nullptr, nullptr, nullptr);

    for (int si = 0; si < static_cast<int>(tracks.size()); ++si) {
        const TLTrack& tr = tracks[si];
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(m_db,
            "INSERT INTO tl_tracks (sort_order, type, camera_id, label, focal_mm)"
            " VALUES (?,?,?,?,?);",
            -1, &st, nullptr);
        sqlite3_bind_int (st, 1, si);
        sqlite3_bind_text(st, 2, tr.type.c_str(),     -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, tr.cameraId.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, tr.label.c_str(),    -1, SQLITE_STATIC);
        sqlite3_bind_int (st, 5, tr.focalMm);
        sqlite3_step(st);
        sqlite3_finalize(st);
        int64_t trackId = sqlite3_last_insert_rowid(m_db);

        assert(tr.blocks.size() <= 4096);  // bounded: sensible sequence length
        for (const TLBlock& b : tr.blocks) {
            sqlite3_stmt* bs = nullptr;
            sqlite3_prepare_v2(m_db,
                "INSERT INTO tl_blocks"
                " (track_id, at_ms, block_type, ss, iso, fstop,"
                "  cnt, ev, burst_drive, burst_dur_ms,"
                "  audio_file, audio_dur_ms, label, snap_to_prev, snap_to_sec)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
                -1, &bs, nullptr);
            sqlite3_bind_int64(bs,  1, trackId);
            sqlite3_bind_int64(bs,  2, b.atMs);
            sqlite3_bind_int  (bs,  3, static_cast<int>(b.type));
            sqlite3_bind_text (bs,  4, b.ss.c_str(),         -1, SQLITE_STATIC);
            sqlite3_bind_int  (bs,  5, b.iso);
            sqlite3_bind_text (bs,  6, b.fstop.c_str(),      -1, SQLITE_STATIC);
            sqlite3_bind_int  (bs,  7, b.count);
            sqlite3_bind_text (bs,  8, b.ev.c_str(),         -1, SQLITE_STATIC);
            sqlite3_bind_text (bs,  9, b.burstDrive.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int  (bs, 10, b.burstDurMs);
            sqlite3_bind_text (bs, 11, b.audioFile.c_str(),  -1, SQLITE_STATIC);
            sqlite3_bind_int  (bs, 12, b.audioDurMs);
            sqlite3_bind_text (bs, 13, b.label.c_str(),      -1, SQLITE_STATIC);
            sqlite3_bind_int  (bs, 14, b.snapToPrev ? 1 : 0);
            sqlite3_bind_int  (bs, 15, b.snapToSec  ? 1 : 0);
            sqlite3_step(bs);
            sqlite3_finalize(bs);
        }
    }

    sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
}

std::vector<TLTrack> Database::LoadTimeline() const {
    assert(m_db != nullptr);  // must be open before loading
    std::vector<TLTrack> result;
    if (!m_db) return result;

    // Return empty if the table doesn't exist yet (pre-migration DB)
    sqlite3_stmt* chk = nullptr;
    sqlite3_prepare_v2(m_db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name='tl_tracks';",
        -1, &chk, nullptr);
    bool exists = (sqlite3_step(chk) == SQLITE_ROW);
    sqlite3_finalize(chk);
    if (!exists) return result;

    // Migration: add snap_to_sec if this DB predates the column — must run
    // here too (not just in SaveTimeline) since Load happens first at
    // startup, before any Save. Error ignored if the column already exists.
    sqlite3_exec(m_db,
        "ALTER TABLE tl_blocks ADD COLUMN snap_to_sec INTEGER NOT NULL DEFAULT 0;",
        nullptr, nullptr, nullptr);

    auto col = [](sqlite3_stmt* s, int i) -> std::string {
        const auto* p = sqlite3_column_text(s, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    };

    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT id, type, camera_id, label, focal_mm FROM tl_tracks ORDER BY sort_order;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return result;

    while (sqlite3_step(st) == SQLITE_ROW) {
        TLTrack tr;
        tr.id       = sqlite3_column_int64(st, 0);
        tr.type     = col(st, 1);
        tr.cameraId = col(st, 2);
        tr.label    = col(st, 3);
        tr.focalMm  = sqlite3_column_int(st, 4);
        result.push_back(std::move(tr));
    }
    sqlite3_finalize(st);

    for (TLTrack& tr : result) {
        sqlite3_stmt* bs = nullptr;
        rc = sqlite3_prepare_v2(m_db,
            "SELECT id, at_ms, block_type, ss, iso, fstop,"
            "       cnt, ev, burst_drive, burst_dur_ms,"
            "       audio_file, audio_dur_ms, label, snap_to_prev, snap_to_sec"
            " FROM tl_blocks WHERE track_id=? ORDER BY at_ms;",
            -1, &bs, nullptr);
        if (rc != SQLITE_OK) continue;
        sqlite3_bind_int64(bs, 1, tr.id);

        while (sqlite3_step(bs) == SQLITE_ROW) {
            TLBlock b;
            b.id         = sqlite3_column_int64(bs, 0);
            b.atMs       = sqlite3_column_int64(bs, 1);
            b.type       = static_cast<BlockType>(sqlite3_column_int(bs, 2));
            b.ss         = col(bs, 3);
            b.iso        = sqlite3_column_int(bs, 4);
            b.fstop      = col(bs, 5);
            b.count      = sqlite3_column_int(bs, 6);
            b.ev         = col(bs, 7);
            b.burstDrive = col(bs, 8);
            b.burstDurMs = sqlite3_column_int(bs, 9);
            b.audioFile  = col(bs, 10);
            b.audioDurMs = sqlite3_column_int(bs, 11);
            b.label      = col(bs, 12);
            b.snapToPrev = sqlite3_column_int(bs, 13) != 0;
            b.snapToSec  = sqlite3_column_int(bs, 14) != 0;
            tr.blocks.push_back(std::move(b));
        }
        sqlite3_finalize(bs);
    }

    return result;
}

// ─── Named snapshots ─────────────────────────────────────────────────────────

static constexpr const char* kCreateSnapshots = R"SQL(
    CREATE TABLE IF NOT EXISTS tl_snapshots (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        name       TEXT    NOT NULL UNIQUE,
        created_ms INTEGER NOT NULL
    );
    CREATE TABLE IF NOT EXISTS tl_snap_tracks (
        id          INTEGER PRIMARY KEY AUTOINCREMENT,
        snapshot_id INTEGER NOT NULL,
        sort_order  INTEGER NOT NULL DEFAULT 0,
        type        TEXT    NOT NULL DEFAULT 'camera',
        camera_id   TEXT    NOT NULL DEFAULT '',
        label       TEXT    NOT NULL DEFAULT '',
        focal_mm    INTEGER NOT NULL DEFAULT 0
    );
    CREATE TABLE IF NOT EXISTS tl_snap_blocks (
        id           INTEGER PRIMARY KEY AUTOINCREMENT,
        track_id     INTEGER NOT NULL,
        at_ms        INTEGER NOT NULL DEFAULT -1,
        block_type   INTEGER NOT NULL DEFAULT 0,
        ss           TEXT    NOT NULL DEFAULT '1/100',
        iso          INTEGER NOT NULL DEFAULT 100,
        fstop        TEXT    NOT NULL DEFAULT '8.0',
        cnt          INTEGER NOT NULL DEFAULT 5,
        ev           TEXT    NOT NULL DEFAULT '1.0ev',
        burst_drive  TEXT    NOT NULL DEFAULT 'cont-hi-plus',
        burst_dur_ms INTEGER NOT NULL DEFAULT 3000,
        audio_file   TEXT    NOT NULL DEFAULT '',
        audio_dur_ms INTEGER NOT NULL DEFAULT 10000,
        label        TEXT    NOT NULL DEFAULT '',
        snap_to_prev INTEGER NOT NULL DEFAULT 0,
        snap_to_sec  INTEGER NOT NULL DEFAULT 0
    );
)SQL";

void Database::CreateSnapshotTables() {
    assert(m_db != nullptr);  // DB must be open before creating tables
    Exec(kCreateSnapshots);
    // Migration: add focal_mm/snap_to_sec if absent (error ignored if column already exists)
    sqlite3_exec(m_db,
        "ALTER TABLE tl_snap_tracks ADD COLUMN focal_mm INTEGER NOT NULL DEFAULT 0;",
        nullptr, nullptr, nullptr);
    sqlite3_exec(m_db,
        "ALTER TABLE tl_snap_blocks ADD COLUMN snap_to_sec INTEGER NOT NULL DEFAULT 0;",
        nullptr, nullptr, nullptr);
}

void Database::SaveSnapshot(const std::string& name,
                            const std::vector<TLTrack>& tracks) {
    assert(m_db != nullptr);
    assert(!name.empty());    // snapshot name must be non-empty
    assert(tracks.size() <= 64);

    // Get current UTC ms via POSIX time (DB-level helper; App owns the clock)
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ui; ui.LowPart = ft.dwLowDateTime; ui.HighPart = ft.dwHighDateTime;
    int64_t nowMs = static_cast<int64_t>(ui.QuadPart / 10000) - 11644473600000LL;

    sqlite3_exec(m_db, "BEGIN;", nullptr, nullptr, nullptr);

    // Remove old snapshot with the same name (overwrite semantics)
    sqlite3_stmt* del = nullptr;
    sqlite3_prepare_v2(m_db,
        "DELETE FROM tl_snap_blocks WHERE track_id IN"
        " (SELECT id FROM tl_snap_tracks WHERE snapshot_id ="
        "  (SELECT id FROM tl_snapshots WHERE name=?));",
        -1, &del, nullptr);
    sqlite3_bind_text(del, 1, name.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(del); sqlite3_finalize(del);

    sqlite3_stmt* del2 = nullptr;
    sqlite3_prepare_v2(m_db,
        "DELETE FROM tl_snap_tracks WHERE snapshot_id ="
        " (SELECT id FROM tl_snapshots WHERE name=?);",
        -1, &del2, nullptr);
    sqlite3_bind_text(del2, 1, name.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(del2); sqlite3_finalize(del2);

    sqlite3_stmt* del3 = nullptr;
    sqlite3_prepare_v2(m_db,
        "DELETE FROM tl_snapshots WHERE name=?;",
        -1, &del3, nullptr);
    sqlite3_bind_text(del3, 1, name.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(del3); sqlite3_finalize(del3);

    // Insert snapshot header
    sqlite3_stmt* si = nullptr;
    sqlite3_prepare_v2(m_db,
        "INSERT INTO tl_snapshots (name, created_ms) VALUES (?,?);",
        -1, &si, nullptr);
    sqlite3_bind_text (si, 1, name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(si, 2, nowMs);
    sqlite3_step(si); sqlite3_finalize(si);
    int64_t snapId = sqlite3_last_insert_rowid(m_db);

    for (int si2 = 0; si2 < static_cast<int>(tracks.size()); ++si2) {
        const TLTrack& tr = tracks[si2];
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(m_db,
            "INSERT INTO tl_snap_tracks (snapshot_id,sort_order,type,camera_id,label,focal_mm)"
            " VALUES (?,?,?,?,?,?);",
            -1, &st, nullptr);
        sqlite3_bind_int64(st, 1, snapId);
        sqlite3_bind_int  (st, 2, si2);
        sqlite3_bind_text (st, 3, tr.type.c_str(),     -1, SQLITE_STATIC);
        sqlite3_bind_text (st, 4, tr.cameraId.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text (st, 5, tr.label.c_str(),    -1, SQLITE_STATIC);
        sqlite3_bind_int  (st, 6, tr.focalMm);
        sqlite3_step(st); sqlite3_finalize(st);
        int64_t trackId = sqlite3_last_insert_rowid(m_db);

        assert(tr.blocks.size() <= 4096);
        for (const TLBlock& b : tr.blocks) {
            sqlite3_stmt* bs = nullptr;
            sqlite3_prepare_v2(m_db,
                "INSERT INTO tl_snap_blocks"
                " (track_id,at_ms,block_type,ss,iso,fstop,cnt,ev,"
                "  burst_drive,burst_dur_ms,audio_file,audio_dur_ms,label,snap_to_prev,snap_to_sec)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
                -1, &bs, nullptr);
            sqlite3_bind_int64(bs,  1, trackId);
            sqlite3_bind_int64(bs,  2, b.atMs);
            sqlite3_bind_int  (bs,  3, static_cast<int>(b.type));
            sqlite3_bind_text (bs,  4, b.ss.c_str(),         -1, SQLITE_STATIC);
            sqlite3_bind_int  (bs,  5, b.iso);
            sqlite3_bind_text (bs,  6, b.fstop.c_str(),      -1, SQLITE_STATIC);
            sqlite3_bind_int  (bs,  7, b.count);
            sqlite3_bind_text (bs,  8, b.ev.c_str(),         -1, SQLITE_STATIC);
            sqlite3_bind_text (bs,  9, b.burstDrive.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int  (bs, 10, b.burstDurMs);
            sqlite3_bind_text (bs, 11, b.audioFile.c_str(),  -1, SQLITE_STATIC);
            sqlite3_bind_int  (bs, 12, b.audioDurMs);
            sqlite3_bind_text (bs, 13, b.label.c_str(),      -1, SQLITE_STATIC);
            sqlite3_bind_int  (bs, 14, b.snapToPrev ? 1 : 0);
            sqlite3_bind_int  (bs, 15, b.snapToSec  ? 1 : 0);
            sqlite3_step(bs); sqlite3_finalize(bs);
        }
    }

    sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
}

std::vector<SnapshotInfo> Database::LoadSnapshotList() const {
    assert(m_db != nullptr);  // DB must be open to list snapshots
    std::vector<SnapshotInfo> result;
    if (!m_db) return result;
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT id, name, created_ms FROM tl_snapshots ORDER BY created_ms DESC;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return result;
    while (sqlite3_step(st) == SQLITE_ROW) {
        SnapshotInfo s;
        s.id        = sqlite3_column_int64(st, 0);
        const auto* p = sqlite3_column_text(st, 1);
        s.name      = p ? reinterpret_cast<const char*>(p) : "";
        s.createdMs = sqlite3_column_int64(st, 2);
        result.push_back(std::move(s));
    }
    sqlite3_finalize(st);
    return result;
}

std::vector<TLTrack> Database::LoadSnapshot(int64_t id) const {
    assert(m_db != nullptr);  // DB must be open to load snapshot
    assert(id > 0);
    std::vector<TLTrack> result;
    if (!m_db) return result;

    auto col = [](sqlite3_stmt* s, int i) -> std::string {
        const auto* p = sqlite3_column_text(s, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    };

    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT id, type, camera_id, label, focal_mm FROM tl_snap_tracks"
        " WHERE snapshot_id=? ORDER BY sort_order;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return result;
    sqlite3_bind_int64(st, 1, id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        TLTrack tr;
        tr.id       = sqlite3_column_int64(st, 0);
        tr.type     = col(st, 1);
        tr.cameraId = col(st, 2);
        tr.label    = col(st, 3);
        tr.focalMm  = sqlite3_column_int(st, 4);
        result.push_back(std::move(tr));
    }
    sqlite3_finalize(st);

    for (TLTrack& tr : result) {
        sqlite3_stmt* bs = nullptr;
        rc = sqlite3_prepare_v2(m_db,
            "SELECT at_ms,block_type,ss,iso,fstop,cnt,ev,"
            "       burst_drive,burst_dur_ms,audio_file,audio_dur_ms,label,snap_to_prev,snap_to_sec"
            " FROM tl_snap_blocks WHERE track_id=? ORDER BY at_ms;",
            -1, &bs, nullptr);
        if (rc != SQLITE_OK) continue;
        sqlite3_bind_int64(bs, 1, tr.id);
        while (sqlite3_step(bs) == SQLITE_ROW) {
            TLBlock b;
            b.atMs       = sqlite3_column_int64(bs, 0);
            b.type       = static_cast<BlockType>(sqlite3_column_int(bs, 1));
            b.ss         = col(bs, 2);
            b.iso        = sqlite3_column_int(bs, 3);
            b.fstop      = col(bs, 4);
            b.count      = sqlite3_column_int(bs, 5);
            b.ev         = col(bs, 6);
            b.burstDrive = col(bs, 7);
            b.burstDurMs = sqlite3_column_int(bs, 8);
            b.audioFile  = col(bs, 9);
            b.audioDurMs = sqlite3_column_int(bs, 10);
            b.label      = col(bs, 11);
            b.snapToPrev = sqlite3_column_int(bs, 12) != 0;
            b.snapToSec  = sqlite3_column_int(bs, 13) != 0;
            tr.blocks.push_back(std::move(b));
        }
        sqlite3_finalize(bs);
    }
    return result;
}

bool Database::SnapshotExists(const std::string& name) const {
    assert(m_db != nullptr);
    assert(!name.empty());
    if (!m_db) return false;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(m_db,
        "SELECT 1 FROM tl_snapshots WHERE name=? LIMIT 1;",
        -1, &st, nullptr);
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_STATIC);
    bool found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

void Database::DeleteSnapshot(int64_t id) {
    assert(m_db != nullptr);
    assert(id > 0);
    sqlite3_exec(m_db, "BEGIN;", nullptr, nullptr, nullptr);
    // cascade delete blocks → tracks → header
    auto exec1 = [&](const char* sql) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr);
        sqlite3_bind_int64(st, 1, id);
        sqlite3_step(st); sqlite3_finalize(st);
    };
    exec1("DELETE FROM tl_snap_blocks WHERE track_id IN"
          " (SELECT id FROM tl_snap_tracks WHERE snapshot_id=?);");
    exec1("DELETE FROM tl_snap_tracks WHERE snapshot_id=?;");
    exec1("DELETE FROM tl_snapshots WHERE id=?;");
    sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
}

// ─── Bracket calibration ─────────────────────────────────────────────────────

static constexpr const char* kCreateCalib = R"SQL(
    CREATE TABLE IF NOT EXISTS bracket_calibration (
        cam_model   TEXT    NOT NULL,
        count       INTEGER NOT NULL,
        ev          TEXT    NOT NULL,
        lat_max_ms  INTEGER NOT NULL,
        lat_avg_ms  INTEGER NOT NULL,
        lat_min_ms  INTEGER NOT NULL,
        reps        INTEGER NOT NULL DEFAULT 3,
        ss          TEXT    NOT NULL DEFAULT '1/100',
        created_ms  INTEGER NOT NULL,
        PRIMARY KEY (cam_model, count, ev)
    );
)SQL";

void Database::CreateCalibTables() {
    assert(m_db != nullptr);  // DB must be open before creating tables
    Exec(kCreateCalib);
}

void Database::SaveCalibData(const std::vector<BktCalibEntry>& entries) {
    assert(m_db != nullptr);
    assert(entries.size() <= 1024);  // bounded: no camera has >1024 bracket combos
    if (!m_db || entries.empty()) return;
    sqlite3_exec(m_db, "BEGIN;", nullptr, nullptr, nullptr);
    for (const auto& e : entries) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(m_db,
            "INSERT OR REPLACE INTO bracket_calibration"
            " (cam_model,count,ev,lat_max_ms,lat_avg_ms,lat_min_ms,reps,ss,created_ms)"
            " VALUES (?,?,?,?,?,?,?,?,?);",
            -1, &st, nullptr);
        sqlite3_bind_text (st, 1, e.camModel.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int  (st, 2, e.count);
        sqlite3_bind_text (st, 3, e.ev.c_str(),       -1, SQLITE_STATIC);
        sqlite3_bind_int  (st, 4, e.latMaxMs);
        sqlite3_bind_int  (st, 5, e.latAvgMs);
        sqlite3_bind_int  (st, 6, e.latMinMs);
        sqlite3_bind_int  (st, 7, e.reps);
        sqlite3_bind_text (st, 8, e.ss.c_str(),       -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 9, e.createdMs);
        sqlite3_step(st); sqlite3_finalize(st);
    }
    sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
}

std::vector<BktCalibEntry> Database::LoadCalibData(const std::string& camModel) const {
    assert(m_db != nullptr);  // DB must be open to load calibration
    assert(!camModel.empty());
    std::vector<BktCalibEntry> result;
    if (!m_db) return result;

    auto col = [](sqlite3_stmt* s, int i) -> std::string {
        const auto* p = sqlite3_column_text(s, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    };

    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT count,ev,lat_max_ms,lat_avg_ms,lat_min_ms,reps,ss,created_ms"
        " FROM bracket_calibration WHERE cam_model=?"
        " ORDER BY count, ev;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return result;
    sqlite3_bind_text(st, 1, camModel.c_str(), -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW) {
        BktCalibEntry e;
        e.camModel  = camModel;
        e.count     = sqlite3_column_int  (st, 0);
        e.ev        = col(st, 1);
        e.latMaxMs  = sqlite3_column_int  (st, 2);
        e.latAvgMs  = sqlite3_column_int  (st, 3);
        e.latMinMs  = sqlite3_column_int  (st, 4);
        e.reps      = sqlite3_column_int  (st, 5);
        e.ss        = col(st, 6);
        e.createdMs = sqlite3_column_int64(st, 7);
        result.push_back(std::move(e));
    }
    sqlite3_finalize(st);
    return result;
}

std::vector<std::string> Database::LoadCalibModels() const {
    assert(m_db != nullptr);  // DB must be open to list models
    std::vector<std::string> result;
    if (!m_db) return result;
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT DISTINCT cam_model FROM bracket_calibration ORDER BY cam_model;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return result;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const auto* p = sqlite3_column_text(st, 0);
        if (p) result.push_back(reinterpret_cast<const char*>(p));
    }
    sqlite3_finalize(st);
    return result;
}

// ─── JPL Horizons ephemeris cache ────────────────────────────────────────────

static constexpr const char* kCreateEph = R"SQL(
    CREATE TABLE IF NOT EXISTS ephemeris (
        body     TEXT    NOT NULL,
        utc_ms   INTEGER NOT NULL,
        az_deg   REAL    NOT NULL,
        alt_deg  REAL    NOT NULL,
        ang_diam REAL    NOT NULL,
        ra_deg   REAL    NOT NULL,
        dec_deg  REAL    NOT NULL,
        PRIMARY KEY (body, utc_ms)
    );
    CREATE TABLE IF NOT EXISTS eph_meta (
        id           INTEGER PRIMARY KEY,
        eclipse_date TEXT    NOT NULL,
        location     TEXT    NOT NULL,
        fetched_at   INTEGER NOT NULL
    );
)SQL";

void Database::CreateEphTables() {
    assert(m_db != nullptr);  // DB must be open before creating tables
    Exec(kCreateEph);
}

void Database::SaveEphRows(EphBody body, const std::vector<EphRow>& rows) {
    assert(m_db != nullptr);
    assert(rows.size() <= 1500);  // bounded: ≤288 samples/day per body
    if (!m_db || rows.empty()) return;

    const char* bodyName = EphBodyName(body);
    sqlite3_exec(m_db, "BEGIN;", nullptr, nullptr, nullptr);

    // Replace all rows for this body
    sqlite3_stmt* del = nullptr;
    sqlite3_prepare_v2(m_db,
        "DELETE FROM ephemeris WHERE body=?;", -1, &del, nullptr);
    sqlite3_bind_text(del, 1, bodyName, -1, SQLITE_STATIC);
    sqlite3_step(del);
    sqlite3_finalize(del);

    for (const EphRow& r : rows) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(m_db,
            "INSERT OR REPLACE INTO ephemeris"
            " (body, utc_ms, az_deg, alt_deg, ang_diam, ra_deg, dec_deg)"
            " VALUES (?,?,?,?,?,?,?);",
            -1, &st, nullptr);
        sqlite3_bind_text  (st, 1, bodyName,       -1, SQLITE_STATIC);
        sqlite3_bind_int64 (st, 2, r.utc_ms);
        sqlite3_bind_double(st, 3, r.az_deg);
        sqlite3_bind_double(st, 4, r.alt_deg);
        sqlite3_bind_double(st, 5, r.ang_diam_arcmin);
        sqlite3_bind_double(st, 6, r.ra_deg);
        sqlite3_bind_double(st, 7, r.dec_deg);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
}

std::vector<EphRow> Database::LoadEphRows(EphBody body) const {
    assert(m_db != nullptr);  // DB must be open to load ephemeris
    std::vector<EphRow> result;
    if (!m_db) return result;

    // Return empty if table absent (pre-migration DB)
    sqlite3_stmt* chk = nullptr;
    sqlite3_prepare_v2(m_db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name='ephemeris';",
        -1, &chk, nullptr);
    bool exists = (sqlite3_step(chk) == SQLITE_ROW);
    sqlite3_finalize(chk);
    if (!exists) return result;

    const char* bodyName = EphBodyName(body);
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT utc_ms, az_deg, alt_deg, ang_diam, ra_deg, dec_deg"
        " FROM ephemeris WHERE body=? ORDER BY utc_ms;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return result;
    sqlite3_bind_text(st, 1, bodyName, -1, SQLITE_STATIC);

    while (sqlite3_step(st) == SQLITE_ROW) {
        EphRow r;
        r.utc_ms          = sqlite3_column_int64 (st, 0);
        r.az_deg          = sqlite3_column_double(st, 1);
        r.alt_deg         = sqlite3_column_double(st, 2);
        r.ang_diam_arcmin = sqlite3_column_double(st, 3);
        r.ra_deg          = sqlite3_column_double(st, 4);
        r.dec_deg         = sqlite3_column_double(st, 5);
        result.push_back(r);
    }
    sqlite3_finalize(st);
    return result;
}

bool Database::EphemerisExists(const std::string& eclDate,
                               const std::string& location) const {
    assert(m_db != nullptr);
    assert(!eclDate.empty() && !location.empty());
    if (!m_db) return false;

    sqlite3_stmt* chk = nullptr;
    sqlite3_prepare_v2(m_db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name='eph_meta';",
        -1, &chk, nullptr);
    bool exists = (sqlite3_step(chk) == SQLITE_ROW);
    sqlite3_finalize(chk);
    if (!exists) return false;

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(m_db,
        "SELECT 1 FROM eph_meta WHERE eclipse_date=? AND location=? LIMIT 1;",
        -1, &st, nullptr);
    sqlite3_bind_text(st, 1, eclDate.c_str(),   -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, location.c_str(),  -1, SQLITE_STATIC);
    bool found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

void Database::SetEphMeta(const std::string& eclDate, const std::string& location) {
    assert(m_db != nullptr);
    assert(!eclDate.empty() && !location.empty());
    if (!m_db) return;

    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ui; ui.LowPart = ft.dwLowDateTime; ui.HighPart = ft.dwHighDateTime;
    int64_t nowMs = static_cast<int64_t>(ui.QuadPart / 10000) - 11644473600000LL;

    sqlite3_exec(m_db, "BEGIN;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "DELETE FROM eph_meta;", nullptr, nullptr, nullptr);

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(m_db,
        "INSERT INTO eph_meta (eclipse_date, location, fetched_at) VALUES (?,?,?);",
        -1, &st, nullptr);
    sqlite3_bind_text (st, 1, eclDate.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 2, location.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, nowMs);
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
}

// ─── Audio file duration cache ───────────────────────────────────────────────

static constexpr const char* kCreateAudioFiles = R"SQL(
    CREATE TABLE IF NOT EXISTS audio_files (
        lang      TEXT    NOT NULL,
        filename  TEXT    NOT NULL,
        dur_ms    INTEGER NOT NULL,
        PRIMARY KEY (lang, filename)
    );
)SQL";

void Database::CreateAudioFilesTable() {
    assert(m_db != nullptr);
    Exec(kCreateAudioFiles);
}

void Database::SaveAudioFileDur(std::string_view lang,
                                std::string_view filename,
                                int32_t         durMs) {
    assert(m_db != nullptr);
    assert(!lang.empty() && !filename.empty());
    assert(durMs >= 0);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "INSERT OR REPLACE INTO audio_files (lang, filename, dur_ms) VALUES (?,?,?);",
            -1, &st, nullptr) != SQLITE_OK)
        return;   // rule 7: discard only on confirmed error (st is null here, do not bind)
    sqlite3_bind_text(st, 1, lang.data(),     static_cast<int>(lang.size()),     SQLITE_STATIC);
    sqlite3_bind_text(st, 2, filename.data(), static_cast<int>(filename.size()), SQLITE_STATIC);
    sqlite3_bind_int (st, 3, durMs);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

std::map<std::string, int32_t> Database::LoadAudioFileDurs() const {
    assert(m_db != nullptr);
    std::map<std::string, int32_t> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "SELECT lang, filename, dur_ms FROM audio_files;",
            -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        std::string lang = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        std::string fn   = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        int32_t     dur  = sqlite3_column_int(st, 2);
        out[lang + "/" + fn] = dur;
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<std::string> Database::LoadAudioCachedLangs() const {
    assert(m_db != nullptr);
    std::vector<std::string> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "SELECT DISTINCT lang FROM audio_files ORDER BY lang;",
            -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW)
        out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
    sqlite3_finalize(st);
    return out;
}

void Database::ClearAudioFileDurs(std::string_view lang) {
    assert(m_db != nullptr);
    assert(lang.empty() || lang.size() <= 10);  // rule 5: lang tags are short codes ("PL","EN",…)
    if (lang.empty()) {
        sqlite3_exec(m_db, "DELETE FROM audio_files;", nullptr, nullptr, nullptr);
    } else {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(m_db, "DELETE FROM audio_files WHERE lang=?;",
                               -1, &st, nullptr) != SQLITE_OK)
            return;   // rule 7: st is null on error — must not bind
        sqlite3_bind_text(st, 1, lang.data(), static_cast<int>(lang.size()), SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

// ─── Camera configuration ─────────────────────────────────────────────────────

void Database::CreateCamConfigTable() {
    assert(m_db != nullptr);
    Exec(R"SQL(
        CREATE TABLE IF NOT EXISTS camera_config (
            guid             TEXT PRIMARY KEY,
            model            TEXT NOT NULL DEFAULT '',
            focal_mm         INTEGER NOT NULL DEFAULT 0,
            apply_p          INTEGER NOT NULL DEFAULT 1,
            track_mode       INTEGER NOT NULL DEFAULT 0,
            horizon_alt_deg  REAL    NOT NULL DEFAULT 0.0,
            horizon_az_deg   REAL    NOT NULL DEFAULT 180.0
        );
    )SQL");
    // Idempotent migrations for databases created before these columns existed.
    (void)Exec("ALTER TABLE camera_config ADD COLUMN track_mode      INTEGER NOT NULL DEFAULT 0;");
    (void)Exec("ALTER TABLE camera_config ADD COLUMN horizon_alt_deg REAL    NOT NULL DEFAULT 0.0;");
    (void)Exec("ALTER TABLE camera_config ADD COLUMN horizon_az_deg  REAL    NOT NULL DEFAULT 180.0;");
}

void Database::SaveCamConfig(const std::string& guid, const std::string& model,
                              int focalMm, bool applyP,
                              CamTrackMode trackMode,
                              double horizonAltDeg, double horizonAzDeg) {
    assert(m_db != nullptr);
    assert(!guid.empty());
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "INSERT OR REPLACE INTO camera_config"
            " (guid, model, focal_mm, apply_p, track_mode, horizon_alt_deg, horizon_az_deg)"
            " VALUES (?,?,?,?,?,?,?);",
            -1, &st, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text  (st, 1, guid.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_text  (st, 2, model.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int   (st, 3, focalMm);
    sqlite3_bind_int   (st, 4, applyP ? 1 : 0);
    sqlite3_bind_int   (st, 5, static_cast<int>(trackMode));
    sqlite3_bind_double(st, 6, horizonAltDeg);
    sqlite3_bind_double(st, 7, horizonAzDeg);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

std::vector<CamConfig> Database::LoadCamConfigs() const {
    assert(m_db != nullptr);
    std::vector<CamConfig> result;
    if (!m_db) return result;

    sqlite3_stmt* chk = nullptr;
    sqlite3_prepare_v2(m_db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name='camera_config';",
        -1, &chk, nullptr);
    bool exists = (sqlite3_step(chk) == SQLITE_ROW);
    sqlite3_finalize(chk);
    if (!exists) return result;

    auto col = [](sqlite3_stmt* s, int i) -> std::string {
        const auto* p = sqlite3_column_text(s, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    };

    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT guid, model, focal_mm, apply_p, track_mode, horizon_alt_deg, horizon_az_deg"
        " FROM camera_config ORDER BY rowid;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return result;

    while (sqlite3_step(st) == SQLITE_ROW) {
        CamConfig cc;
        cc.guid           = col(st, 0);
        cc.model          = col(st, 1);
        cc.focalMm        = sqlite3_column_int   (st, 2);
        cc.applyP         = sqlite3_column_int   (st, 3) != 0;
        cc.trackMode      = static_cast<CamTrackMode>(sqlite3_column_int(st, 4));
        cc.horizonAltDeg  = sqlite3_column_double(st, 5);
        cc.horizonAzDeg   = sqlite3_column_double(st, 6);
        result.push_back(std::move(cc));
    }
    sqlite3_finalize(st);
    assert(result.size() <= 64);  // sanity: won't have more than 64 cameras in a session
    return result;
}

} // namespace TotalControl
