#include "App.h"
#include "TzEntry.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <windows.h>
#include <cassert>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>

namespace TotalControl {

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    auto slash = s.rfind(L'\\');
    return (slash != std::wstring::npos) ? s.substr(0, slash) : s;
}

// Returns UTC milliseconds since Unix epoch using high-resolution Windows clock.
static int64_t UtcNowMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ui;
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(ui.QuadPart / 10000) - 11644473600000LL;
}

// Formats HH:MM:SS.mmm for the UTC clock (millisecond precision).
static void FormatUtcHms(int64_t ms, char* buf, int len) {
    int64_t s  = ms / 1000;
    int     ms3 = static_cast<int>(ms % 1000);
    int     hh  = static_cast<int>((s / 3600) % 24);
    int     mm  = static_cast<int>((s / 60)   % 60);
    int     sc  = static_cast<int>(s % 60);
    snprintf(buf, len, "%02d:%02d:%02d.%03d", hh, mm, sc, ms3);
}

// Formats HH:MM:SS for a local clock identified by an IANA timezone name.
// Uses std::chrono::locate_zone (C++20) — DST is handled by the MSVC runtime
// which maps IANA names to Windows registry zones (updated by Windows Update).
// Falls back to "--:--:--" for unknown or invalid timezone names.
static void FormatLocalHms(int64_t utcMs, const std::string& ianaName,
                            char* buf, int len) {
    using namespace std::chrono;
    try {
        const time_zone* tz = locate_zone(ianaName);
        auto sys_tp = system_clock::time_point{milliseconds{utcMs}};
        auto zoned  = zoned_time{tz, sys_tp};
        auto local  = zoned.get_local_time();
        auto day_start = floor<days>(local);
        hh_mm_ss tod{local - day_start};
        snprintf(buf, len, "%02d:%02d:%02d",
            static_cast<int>(tod.hours().count()),
            static_cast<int>(tod.minutes().count()),
            static_cast<int>(tod.seconds().count()));
    } catch (...) {
        snprintf(buf, len, "--:--:--");
    }
}

// ─── JSON field helpers (no external library) ────────────────────────────────

// Extract first string or numeric value for key from flat/shallow JSON.
static std::string JStr(std::string_view j, std::string_view key) {
    std::string needle = std::string("\"") + std::string(key) + "\":";
    auto p = j.find(needle);
    if (p == std::string_view::npos) return {};
    p += needle.size();
    while (p < j.size() && j[p] == ' ') ++p;
    if (p >= j.size()) return {};
    if (j[p] == '"') {
        ++p;
        auto e = j.find('"', p);
        return e == std::string_view::npos ? std::string{} : std::string(j.substr(p, e - p));
    }
    auto e = j.find_first_of(",}", p);
    return std::string(j.substr(p, e == std::string_view::npos ? j.size() - p : e - p));
}

static int JInt(std::string_view j, std::string_view key, int def = 0) {
    auto s = JStr(j, key);
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

// Extract all values of a repeated string key (e.g. "guid" inside cameras array).
static std::vector<std::string> JStrAll(std::string_view j, std::string_view key) {
    std::vector<std::string> out;
    std::string needle = std::string("\"") + std::string(key) + "\":\"";
    size_t p = 0;
    while ((p = j.find(needle, p)) != std::string_view::npos) {
        p += needle.size();
        auto e = j.find('"', p);
        if (e != std::string_view::npos) { out.emplace_back(j.substr(p, e - p)); p = e; }
    }
    return out;
}

// ─── Logging ─────────────────────────────────────────────────────────────────

void App::LogLine(std::string_view msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::lock_guard lk(m_logMutex);
    if (m_logFile.is_open()) {
        m_logFile << std::format("{:02d}:{:02d}:{:02d}.{:03d}  {}\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        m_logFile.flush();
    }
}

// ─── Default config ───────────────────────────────────────────────────────────

void App::EnsureDefaultConfig(const std::wstring& path) {
    if (std::filesystem::exists(path)) return;

    Database db;
    if (!db.Open(path)) return;

    db.Exec(R"SQL(
        CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);
        INSERT OR IGNORE INTO schema_version VALUES (1);

        CREATE TABLE IF NOT EXISTS settings (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        INSERT OR IGNORE INTO settings VALUES ('show_home_clock', '1');
        INSERT OR IGNORE INTO settings VALUES ('show_ecl_clock',  '1');
        INSERT OR IGNORE INTO settings VALUES ('home_tz_iana', 'Europe/Warsaw');
        INSERT OR IGNORE INTO settings VALUES ('ecl_tz_iana',  'Europe/Madrid');
        INSERT OR IGNORE INTO settings VALUES ('obs_lat',   '0.0');
        INSERT OR IGNORE INTO settings VALUES ('obs_lon',   '0.0');
        INSERT OR IGNORE INTO settings VALUES ('obs_alt_m', '0');
    )SQL");
}

// ─── Persistent settings ──────────────────────────────────────────────────────

void App::SaveClockSettings() {
    m_configDb.SetSettingInt("show_home_clock", m_showHomeClock ? 1 : 0);
    m_configDb.SetSettingInt("show_ecl_clock",  m_showEclClock  ? 1 : 0);
    m_configDb.SetSetting("home_tz_iana", m_homeTzIana.c_str());
    m_configDb.SetSetting("ecl_tz_iana",  m_eclTzIana.c_str());
}

void App::SaveObserverSettings() {
    m_configDb.SetSetting("obs_lat",   std::format("{:.6f}", m_obsLat).c_str());
    m_configDb.SetSetting("obs_lon",   std::format("{:.6f}", m_obsLon).c_str());
    m_configDb.SetSettingInt("obs_alt_m", m_obsAltM);
}

static float DmsToDecimal(const DmsCoord& d) {
    float v = d.deg + d.min / 60.f + d.sec / 3600.f;
    return d.pos ? v : -v;
}

static void DecimalToDms(float decimal, DmsCoord& d) {
    d.pos = decimal >= 0.f;
    float a = fabsf(decimal);
    d.deg = static_cast<int>(a);
    float r = (a - d.deg) * 60.f;
    d.min = static_cast<int>(r);
    d.sec = roundf((r - d.min) * 60.f * 100.f) / 100.f;
    if (d.sec >= 60.f) { d.sec = 0.f; if (++d.min >= 60) { d.min = 0; ++d.deg; } }
}

void App::SyncDecimalToDms() {
    DecimalToDms(m_obsLat, m_latDms);
    DecimalToDms(m_obsLon, m_lonDms);
}

void App::SyncDmsToDecimal() {
    m_obsLat = DmsToDecimal(m_latDms);
    m_obsLon = DmsToDecimal(m_lonDms);
}

void App::TriggerIqpFetch() {
    if (m_eclipseIdx < 0 || m_eclipseIdx >= static_cast<int>(m_eclipses.size())) return;

    const auto& e = m_eclipses[m_eclipseIdx];
    float lat = m_obsLat, lon = m_obsLon;
    int   altM = m_obsAltM;
    int   y = e.year, mo = e.month, d = e.day;
    std::string id = BuildEclipseId(e.type.empty() ? 'T' : e.type[0], y, mo, d);

    m_iqpFetchedLat = lat;
    m_iqpFetchedLon = lon;
    m_iqpFetchedIdx = m_eclipseIdx;

    // Besselian: synchronous (fast, always available immediately)
    BesselianElements bel = m_dataDb.LoadBesselianElements(y, mo, d);
    m_beResult = CalcBesselian(bel, lat, lon, static_cast<double>(altM));

    // IQP: asynchronous, no BE fallback (results shown side-by-side)
    m_iqpState.store(1);
    if (m_iqpThread.joinable()) m_iqpThread.detach();

    auto* statePtr   = &m_iqpState;
    auto* mutexPtr   = &m_iqpMutex;
    auto* contactPtr = &m_contacts;
    auto* cfgDb      = &m_configDb;

    m_iqpThread = std::thread([id, lat, lon, y, mo, d,
                                statePtr, mutexPtr, contactPtr, cfgDb]() {
        auto ct = FetchContactTimes(id, lat, lon, y, mo, d);
        // Persist refreshed key so it survives next app restart
        std::string newKey = GetCurrentApiKey();
        if (!newKey.empty())
            cfgDb->SetSetting("iqp_api_key", newKey.c_str());
        { std::lock_guard lk(*mutexPtr); *contactPtr = ct; }
        statePtr->store(ct.apiOk ? 2 : 3);
    });
    LogLine(std::format("IQP+BE triggered: {} lat={:.4f} lon={:.4f}  BE valid={}",
                        id, lat, lon, m_beResult.valid));
}

// ─── Camera overhead model (ILCE-7RM4A) ──────────────────────────────────────

// Parse shutter speed string → milliseconds.
// "1/1000" → 1, "2s" or "2" → 2000, "0.6" → 600
static int64_t ParseSsMs(const std::string& ss) {
    if (ss.empty()) return 0;
    auto slash = ss.find('/');
    if (slash != std::string::npos) {
        int num = 1, den = 1000;
        sscanf_s(ss.c_str(), "%d/%d", &num, &den);
        return den > 0 ? int64_t(num) * 1000LL / den : 0;
    }
    double v = 0.0;
    sscanf_s(ss.c_str(), "%lf", &v);
    return int64_t(v * 1000.0);
}

// Conservative overhead per shot (USB latency + buffer + SDK confirm).
// Measured: 303 ms; conservative: 350 ms.
static constexpr int64_t kCamOverheadMs = 350;

// Returns true if blocks a and b require an ARM command between them
// (different drive mode, shutter speed, ISO, or f-stop).
static bool BlockParamsDiffer(const TLBlock& a, const TLBlock& b) {
    assert(a.type != BlockType::Audio && b.type != BlockType::Audio);
    if (a.type != b.type)                                      return true;
    if (a.ss != b.ss || a.fstop != b.fstop || a.iso != b.iso) return true;
    if (a.type == BlockType::Bracket) return a.ev != b.ev || a.count != b.count;
    if (a.type == BlockType::Burst)   return a.burstDrive != b.burstDrive;
    return false;
}

// Conservative estimate of ARM overhead after block b fires (RAW write to card +
// SetPropAndVerify(DriveMode) confirm). Derived from ILCE-7RM4A measurements:
// 3-shot ~1800 ms, 5-shot ~2000 ms, 9-shot ~2030 ms. Hard ceiling: 2100 ms.
static int64_t ArmEstMs(const TLBlock& b) {
    assert(b.type != BlockType::Audio);
    if (b.type == BlockType::Bracket)
        return std::min(2100LL, 1000LL + static_cast<int64_t>(b.count) * 300LL);
    return 1800LL;
}

int64_t App::BlockDurMs(const TLBlock& b, std::string_view camModel) const {
    assert(b.type == BlockType::Audio || !b.ss.empty());  // camera blocks must have a shutter speed
    assert(b.type != BlockType::Bracket || b.count >= 1); // bracket must have at least 1 shot
    assert(b.type != BlockType::Burst   || b.burstDurMs > 0);
    assert(b.type != BlockType::Audio   || b.audioDurMs > 0);
    int64_t ssMs = ParseSsMs(b.ss);
    switch (b.type) {
    case BlockType::Single:
        return ssMs + kCamOverheadMs;
    case BlockType::Bracket: {
        // Calibration lookup: prefer named model, fall back to first available.
        const std::map<std::pair<int,std::string>, int>* table = nullptr;
        if (!camModel.empty()) {
            auto it = m_calibCache.find(std::string(camModel));
            if (it != m_calibCache.end()) table = &it->second;
        }
        if (!table && !m_calibCache.empty())
            table = &m_calibCache.begin()->second;
        if (table) {
            auto jt = table->find({b.count, b.ev});
            if (jt != table->end()) {
                // jt->second = lat_max_ms + 10 (10ms safety margin already included)
                // For SS > calibration baseline (1/100 = 10ms): add per-shot SS cost.
                int64_t ssExtra = (ssMs > 10LL) ? int64_t(b.count) * (ssMs - 10LL) : 0LL;
                return jt->second + ssExtra;
            }
        }
        // No calibration — fall back to formula
        return int64_t(b.count) * (ssMs + kCamOverheadMs);
    }
    case BlockType::Burst:
        return b.burstDurMs;
    case BlockType::Audio:
        return b.audioDurMs;
    default:
        return 0;
    }
}

static ImU32 BlockColor(BlockType t) {
    switch (t) {
    case BlockType::Single:  return IM_COL32( 40, 160,  70, 220);
    case BlockType::Burst:   return IM_COL32( 50, 120, 200, 220);
    case BlockType::Bracket: return IM_COL32(200, 130,  30, 220);
    case BlockType::Audio:   return IM_COL32(140,  80, 200, 220);
    default:                 return IM_COL32(100, 100, 100, 220);
    }
}

static const char* BlockTypeName(BlockType t) {
    switch (t) {
    case BlockType::Single:  return "Single";
    case BlockType::Burst:   return "Burst";
    case BlockType::Bracket: return "Bracket";
    case BlockType::Audio:   return "Audio";
    default:                 return "?";
    }
}

// ─── Rotated text helper ──────────────────────────────────────────────────────

// Renders text at `anchor` rotated 90° CCW: first char at anchor.y, text flows
// downward; glyphs readable by tilting head to the left.
static void AddTextRotated90CCW(ImDrawList* dl, ImFont* font, float sz,
                                 ImVec2 anchor, ImU32 col, const char* text) {
    assert(font != nullptr && sz > 0.f);
    assert(text != nullptr);
    float s   = sz / font->FontSize;
    float cur = 0.f;
    for (const char* p = text; *p; ++p) {
        const ImFontGlyph* g = font->FindGlyph(
            static_cast<ImWchar>(static_cast<unsigned char>(*p)));
        if (!g) continue;
        if (g->Visible) {
            // 90° CCW visual (screen Y-down): glyph (x,y) → screen (y, -x) from anchor;
            // text flows upward from anchor, reads bottom-to-top.
            ImVec2 p1{anchor.x + g->Y0 * s, anchor.y - cur - g->X0 * s};
            ImVec2 p2{anchor.x + g->Y0 * s, anchor.y - cur - g->X1 * s};
            ImVec2 p3{anchor.x + g->Y1 * s, anchor.y - cur - g->X1 * s};
            ImVec2 p4{anchor.x + g->Y1 * s, anchor.y - cur - g->X0 * s};
            dl->AddImageQuad(font->ContainerAtlas->TexID,
                             p1, p2, p3, p4,
                             {g->U0, g->V0}, {g->U1, g->V0},
                             {g->U1, g->V1}, {g->U0, g->V1},
                             col);
        }
        cur += g->AdvanceX * s;
    }
}

// ─── Background status thread ────────────────────────────────────────────────
//
// Polls camera list + per-camera status every ~2 s.
// Writes m_cameras under m_camerasMutex; never touches the render thread.
// PipeClient::SendRequest is internally mutex-protected — safe to call from here.

void App::StartStatusThread() {
    assert(!m_statusRun.load());
    m_statusRun.store(true);
    m_statusThread = std::thread([this]() { StatusThreadProc(); });
}

void App::StopStatusThread() {
    assert(m_statusThread.joinable() || !m_statusRun.load());
    m_statusRun.store(false);
    if (m_statusThread.joinable()) m_statusThread.join();
}

void App::StatusThreadProc() {
    assert(m_statusRun.load());
    while (m_statusRun.load()) {
        if (m_pipe.GetState() == PipeClient::State::Connected) {
            auto listRes = m_pipe.SendRequest(R"({"cmd":"list_cameras"})");
            if (listRes) {
                auto guids = JStrAll(*listRes, "guid");
                std::vector<CamStatus> next;
                next.reserve(guids.size());
                for (auto& guid : guids) {
                    std::string req =
                        std::format(R"({{"cmd":"status","cam":"{}"}})", guid);
                    auto res = m_pipe.SendRequest(req);
                    if (!res) continue;
                    CamStatus s;
                    s.valid          = JStr(*res, "connected") == "true";
                    s.guid           = guid;
                    s.model          = JStr(*res, "model");
                    s.batteryPct     = JInt(*res, "battery");
                    s.batteryLevel   = JStr(*res, "battery_level");
                    s.mode           = JStr(*res, "mode");
                    s.ss             = JStr(*res, "ss");
                    s.iso            = JInt(*res, "iso");
                    s.fnum           = JStr(*res, "f");
                    s.focus          = JStr(*res, "focus");
                    s.drive          = JStr(*res, "drive");
                    s.slot1Status    = JStr(*res, "slot1_status");
                    s.slot2Status    = JStr(*res, "slot2_status");
                    s.slot1Remaining = JInt(*res, "remaining",       -1);
                    s.slot2Remaining = JInt(*res, "slot2_remaining", -1);
                    s.store          = JStr(*res, "store");
                    // Preserve lastShotMs across polls
                    {
                        std::lock_guard lk(m_camerasMutex);
                        for (auto& prev : m_cameras)
                            if (prev.guid == guid) { s.lastShotMs = prev.lastShotMs; break; }
                    }
                    next.push_back(std::move(s));
                }
                std::lock_guard lk(m_camerasMutex);
                m_cameras = std::move(next);
            }
        } else {
            std::lock_guard lk(m_camerasMutex);
            m_cameras.clear();
        }
        // Interruptible 2 s sleep — wakes every 10 ms to check m_statusRun
        static constexpr int kPollIntervals = 200;  // 200 × 10 ms = 2 s
        for (int i = 0; i < kPollIntervals && m_statusRun.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ─── GUI sequencer ────────────────────────────────────────────────────────────
//
// BuildBlockCmd: builds a pipe JSON command string for one timeline block.
// camIdx is the 0-based camera-track index (used as "cam":<index> in the pipe protocol).

/*static*/ std::string App::BuildBlockCmd(const TLBlock& blk, int camIdx) {
    assert(camIdx >= 0 && camIdx < kMaxCamTracks);
    assert(!blk.ss.empty() || blk.type == BlockType::Audio);

    if (blk.type == BlockType::Audio) return {};  // audio blocks have no pipe command

    std::string cam = std::to_string(camIdx);

    if (blk.type == BlockType::Single) {
        return std::format(
            "{{\"cmd\":\"shoot\",\"drive\":\"single\","
            "\"ss\":\"{}\",\"iso\":{},\"f\":{},\"cam\":\"{}\"}}",
            blk.ss, blk.iso, blk.fstop, cam);
    }
    if (blk.type == BlockType::Bracket) {
        return std::format(
            "{{\"cmd\":\"bracket\",\"mode\":\"cont\","
            "\"ss\":\"{}\",\"iso\":{},\"f\":{},"
            "\"ev\":\"{}\",\"count\":{},\"cam\":\"{}\"}}",
            blk.ss, blk.iso, blk.fstop, blk.ev, blk.count, cam);
    }
    if (blk.type == BlockType::Burst) {
        int timeoutMs = blk.burstDurMs + 2000;
        return std::format(
            "{{\"cmd\":\"shoot\",\"drive\":\"{}\","
            "\"ss\":\"{}\",\"iso\":{},\"f\":{},"
            "\"timeout_ms\":{},\"cam\":\"{}\"}}",
            blk.burstDrive, blk.ss, blk.iso, blk.fstop, timeoutMs, cam);
    }
    return {};
}

// SeqThreadProc: core sequencer loop.
// mode       — TestRunning or Running
// playheadStartMs — UTC ms of playhead when this run started/resumed
// realStartMs     — real UTC ms at start/resume (for simulated time calculation)
void App::SeqThreadProc(GuiSeqMode mode,
                        int64_t   playheadStartMs,
                        int64_t   realStartMs) {
    assert(m_seqRun.load());
    assert(playheadStartMs > 0);
    assert(realStartMs > 0);

    // Local copy of per-track next-block indices for this run segment
    int nextBlock[kMaxCamTracks];
    for (int i = 0; i < kMaxCamTracks; ++i)
        nextBlock[i] = m_seqNextBlock[i];

    // ── Pre-arm deduplication ─────────────────────────────────────────────────
    // ARM is sent after a block fires only when the NEXT block has different
    // params (BlockParamsDiffer). Identical consecutive blocks skip the ARM —
    // the camera is already configured from the initial arm or the previous ARM.

    auto sendArm = [&](const TLBlock& blk, int camIdx) {
        assert(camIdx >= 0 && camIdx < kMaxCamTracks);
        if (blk.type == BlockType::Audio) return;

        std::string armCmd = std::format(
            "{{\"cmd\":\"arm\",\"cam\":\"{}\",\"ss\":\"{}\",\"iso\":{},\"f\":\"{}\"",
            camIdx, blk.ss, blk.iso, blk.fstop);

        switch (blk.type) {
        case BlockType::Single:
            armCmd += ",\"drive\":\"single\"";
            break;
        case BlockType::Burst:
            armCmd += std::format(",\"drive\":\"{}\"", blk.burstDrive);
            break;
        case BlockType::Bracket:
            armCmd += std::format(",\"ev\":\"{}\",\"count\":{},\"mode\":\"cont\"",
                                  blk.ev, blk.count);
            break;
        default: return;
        }
        armCmd += "}";

        auto res = m_pipe.SendRequest(armCmd);
        bool ok  = res && JStr(*res, "ok") == "true";
        int  lat = res ? JInt(*res, "latency_ms", -1) : -1;
        LogLine(std::format("ARM cam={} ok={} lat={} type={} ss={} ev={} count={}",
            camIdx, ok ? 1 : 0, lat,
            BlockTypeName(blk.type), blk.ss, blk.ev, blk.count));
    };

    // ── Initial arm: arm the first upcoming block for each track ─────────────
    // Happens before the first tick; arm duration is excluded from simulated time
    // by shifting realStartMs forward so the playhead doesn't jump.
    {
        int camIdx = 0;
        for (int ti = 0;
             ti < static_cast<int>(m_tracks.size()) && camIdx < kMaxCamTracks;
             ++ti) {
            if (!m_tracks[ti].IsCamera()) continue;
            int ni = nextBlock[camIdx];
            const auto& blks = m_tracks[ti].blocks;
            while (ni < static_cast<int>(blks.size()) && blks[ni].atMs < 0) ++ni;
            if (ni < static_cast<int>(blks.size()))
                sendArm(blks[ni], camIdx);
            ++camIdx;
        }
    }
    // Absorb arm latency: start the simulated clock from now so that arm time
    // is not counted as elapsed playhead time.
    realStartMs = UtcNowMs();

    while (m_seqRun.load()) {
        int64_t nowReal = UtcNowMs();
        int64_t simMs   = (mode == GuiSeqMode::TestRunning)
                        ? (playheadStartMs + (nowReal - realStartMs))
                        : nowReal;

        // Update displayed playhead (atomic — render thread reads every frame)
        m_tlPlayheadMs.store(simMs);

        // Iterate camera tracks in timeline order (up to kMaxCamTracks)
        int camIdx = 0;
        for (int ti = 0;
             ti < static_cast<int>(m_tracks.size()) && camIdx < kMaxCamTracks;
             ++ti) {
            if (!m_tracks[ti].IsCamera()) continue;
            auto& tr = m_tracks[ti];
            int&  ni = nextBlock[camIdx];

            // Fire all blocks whose scheduled time has passed.
            // simMs is refreshed at the top of every iteration: SendRequest blocks for
            // 1–3s per bracket/burst, and the stale pre-call simMs would cause all
            // subsequent blocks that became due during that wait to fire instantly.
            while (ni < static_cast<int>(tr.blocks.size())) {
                int64_t nowInner = UtcNowMs();
                simMs = (mode == GuiSeqMode::TestRunning)
                      ? (playheadStartMs + (nowInner - realStartMs))
                      : nowInner;
                m_tlPlayheadMs.store(simMs);

                const TLBlock& blk = tr.blocks[ni];
                if (blk.atMs < 0)    { ++ni; continue; }  // invalid block
                if (blk.atMs > simMs) break;               // not yet

                std::string cmd = BuildBlockCmd(blk, camIdx);

                int     seqNum      = ++m_execSeqNum;
                int64_t firedRealMs = UtcNowMs();
                int64_t driftMs     = firedRealMs - blk.atMs;

                if (!cmd.empty()) {
                    if (auto res = m_pipe.SendRequest(cmd)) {
                        int64_t latMs = UtcNowMs() - firedRealMs;
                        bool ok  = JStr(*res, "ok") == "true";
                        int  lat = JInt(*res, "latency_ms", -1);
                        // lat from SRV = camera execution time; latMs = pipe round-trip
                        // Use whichever is available; they differ by ~10ms pipe overhead.
                        int64_t logLat = (lat >= 0) ? lat : latMs;
                        if (ok && lat >= 0) {
                            std::lock_guard lk(m_camerasMutex);
                            if (camIdx < static_cast<int>(m_cameras.size()))
                                m_cameras[camIdx].lastShotMs = lat;
                        }
                        std::string lbl = blk.label.empty() ? cmd.substr(0, 40) : blk.label;
                        LogLine(std::format(
                            "SEQ seq={} cam={} ok={} lat={} drift={}"
                            " type={} count={} ev={} ss={} lbl={}",
                            seqNum, camIdx, ok ? 1 : 0, logLat, driftMs,
                            BlockTypeName(blk.type), blk.count, blk.ev, blk.ss, lbl));
                        // Collect bracket samples for post-run calibration save
                        if (ok && blk.type == BlockType::Bracket && logLat > 0)
                            m_seqCalibBuf.push_back({blk.count, blk.ev,
                                                     static_cast<int>(logLat)});
                        m_lastResult = std::format("SEQ cam{} {} {}",
                            camIdx, ok ? "OK" : "FAIL",
                            blk.label.empty() ? blk.ss : blk.label);
                    } else {
                        std::string errMsg(PipeErrorMessage(res.error()));
                        LogLine(std::format("SEQ seq={} cam={} ok=-1 err={}",
                            seqNum, camIdx, errMsg));
                        m_lastResult = std::format("SEQ ERROR cam{}: {}", camIdx, errMsg);
                    }
                } else {
                    // Audio / empty block — no pipe call, log for timeline record
                    LogLine(std::format(
                        "SEQ seq={} cam={} ok=0 lat=0 drift={} type={} lbl={}",
                        seqNum, camIdx, driftMs, BlockTypeName(blk.type),
                        blk.label.empty() ? "-" : blk.label));
                }
                // Advance playhead to post-block time before ARM starts.
                // Without this, the playhead freezes for shoot+ARM duration and
                // then jumps; with it, the freeze only covers the ARM phase.
                {
                    int64_t nowPost  = UtcNowMs();
                    int64_t simPost  = (mode == GuiSeqMode::TestRunning)
                                     ? (playheadStartMs + (nowPost - realStartMs))
                                     : nowPost;
                    m_tlPlayheadMs.store(simPost);
                }

                ++ni;
                m_seqNextBlock[camIdx] = ni;  // persist resume index

                // Pre-arm the next block so the camera is ready before it fires.
                // ARM is skipped when the next block has identical params (BlockParamsDiffer).
                {
                    int niNext = ni;
                    const auto& blks = tr.blocks;
                    while (niNext < static_cast<int>(blks.size()) &&
                           blks[niNext].atMs < 0)
                        ++niNext;
                    if (niNext < static_cast<int>(blks.size()) &&
                        blk.type != BlockType::Audio &&
                        BlockParamsDiffer(blk, blks[niNext]))
                        sendArm(blks[niNext], camIdx);
                }
            }
            ++camIdx;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void App::StartSeqThread(GuiSeqMode mode) {
    assert(!m_seqRun.load());
    assert(mode == GuiSeqMode::TestRunning || mode == GuiSeqMode::Running);

    int64_t nowReal       = UtcNowMs();
    int64_t playheadStart = m_tlPlayheadMs.load();

    if (playheadStart < 0) {
        // Fallback: if playhead not set, default to now (shouldn't happen normally)
        playheadStart = nowReal;
        m_tlPlayheadMs.store(playheadStart);
    }

    m_testPlayheadAtStart = playheadStart;
    m_testStartRealMs     = nowReal;

    m_execSeqNum = 0;
    m_seqCalibBuf.clear();
    LogLine(std::format("SEQ_START mode={}",
        (mode == GuiSeqMode::TestRunning) ? "test" : "run"));

    m_seqRun.store(true);
    m_guiSeqMode.store(mode);
    m_seqThread = std::thread([this, mode, playheadStart, nowReal]() {
        SeqThreadProc(mode, playheadStart, nowReal);
    });
}

void App::StopSeqThread() {
    m_seqRun.store(false);
    if (m_seqThread.joinable()) m_seqThread.join();
    // m_seqNextBlock[] already updated by SeqThreadProc during run — preserved for resume
}

// ─── Track initialization ─────────────────────────────────────────────────────

void App::InitTracks() {
    if (!m_tracks.empty()) return;

    TLTrack cam1;
    cam1.type     = "camera";
    cam1.cameraId = "ILCE-7RM4A";
    cam1.label    = "Cam1 ILCE-7RM4A";
    m_tracks.push_back(std::move(cam1));

    TLTrack audio;
    audio.type  = "audio";
    audio.label = "Audio";
    m_tracks.push_back(std::move(audio));
}

// ─── Sequencer buttons (Col1 — TEST RUN / RUN) ───────────────────────────────
//
// TEST buttons (yellow/orange):  TEST RUN starts/resumes from playhead; STOP pauses.
// RUN  buttons (red):            RUN fires on real UTC time; STOP RUN stops.
// During any active run: timeline editing and interactive pipe buttons are locked.

void App::RenderSequencerButtons() {
    assert(m_fontMono != nullptr);
    assert(m_fontLarge != nullptr);

    static const ImVec4 kGray   {0.40f, 0.40f, 0.45f, 1.0f};
    static const ImVec4 kDim    {0.28f, 0.28f, 0.32f, 1.0f};
    static const ImVec4 kGreen  {0.20f, 0.85f, 0.30f, 1.0f};
    // TEST palette — yellow/amber
    static const ImVec4 kTestBg    {0.35f, 0.28f, 0.04f, 1.0f};
    static const ImVec4 kTestHover {0.60f, 0.48f, 0.06f, 1.0f};
    static const ImVec4 kTestText  {1.00f, 0.82f, 0.20f, 1.0f};
    // RUN palette — red
    static const ImVec4 kRunBg     {0.38f, 0.06f, 0.06f, 1.0f};
    static const ImVec4 kRunHover  {0.65f, 0.10f, 0.10f, 1.0f};
    static const ImVec4 kRunText   {1.00f, 0.35f, 0.35f, 1.0f};
    // STOP palette — neutral dark
    static const ImVec4 kStopBg    {0.22f, 0.22f, 0.26f, 1.0f};
    static const ImVec4 kStopHover {0.35f, 0.35f, 0.40f, 1.0f};

    GuiSeqMode sm   = m_guiSeqMode.load();
    bool connected  = (m_pipe.GetState() == PipeClient::State::Connected);
    bool hasBlocks  = false;
    for (auto& tr : m_tracks)
        if (tr.IsCamera() && !tr.blocks.empty()) { hasBlocks = true; break; }

    const ImVec2 kBtnSz(-1, 30);

    ImGui::Spacing();
    ImGui::SeparatorText("SEQUENCER");

    ImGui::PushFont(m_fontMono);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

    // ── Playhead display ─────────────────────────────────────────────────
    {
        int64_t ph = m_tlPlayheadMs.load();
        if (ph > 0) {
            int64_t sv = ph / 1000;
            char buf[20];
            snprintf(buf, sizeof(buf), "\xe2\x96\xb6 %02d:%02d:%02d.%03d",
                     (int)((sv/3600)%24),(int)((sv/60)%60),(int)(sv%60),(int)(ph%1000));
            ImVec4 phCol = (sm == GuiSeqMode::TestRunning) ? kGreen : kTestText;
            ImGui::TextColored(phCol, "%s", buf);
        } else {
            ImGui::TextColored(kDim, "\xe2\x96\xb6 --:--:--");
        }
    }

    ImGui::Spacing();

    // ── TEST RUN / STOP ───────────────────────────────────────────────────
    bool testRunning = (sm == GuiSeqMode::TestRunning);
    bool testPaused  = (sm == GuiSeqMode::TestPaused);
    bool anyRun      = (sm == GuiSeqMode::Running);

    // TEST RUN  (disabled: no blocks, no connection, RUN is active)
    bool canTestStart = connected && hasBlocks && !anyRun && !testRunning;
    if (!canTestStart) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        kTestBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kTestHover);
    ImGui::PushStyleColor(ImGuiCol_Text,          kTestText);
    const char* testBtnLabel = testPaused
        ? "\xe2\x96\xb6 RESUME TEST"
        : "\xe2\x96\xb6 TEST RUN";
    if (ImGui::Button(testBtnLabel, kBtnSz)) {
        if (!testPaused) {
            // Fresh start: reset per-track indices to blocks at/after playhead
            int64_t ph     = m_tlPlayheadMs.load();
            int     camIdx = 0;
            for (int ti = 0;
                 ti < static_cast<int>(m_tracks.size()) && camIdx < kMaxCamTracks;
                 ++ti) {
                if (!m_tracks[ti].IsCamera()) continue;
                int ni = 0;
                for (; ni < static_cast<int>(m_tracks[ti].blocks.size()); ++ni)
                    if (m_tracks[ti].blocks[ni].atMs >= ph) break;
                m_seqNextBlock[camIdx++] = ni;
            }
        }
        StartSeqThread(GuiSeqMode::TestRunning);
        LogLine("Sequencer: TEST RUN started");
    }
    ImGui::PopStyleColor(3);
    if (!canTestStart) ImGui::EndDisabled();

    // STOP TEST  (enabled only when TestRunning or TestPaused)
    bool canTestStop = testRunning || testPaused;
    if (!canTestStop) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        kStopBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kStopHover);
    if (ImGui::Button("\xe2\x96\xa0 STOP TEST", kBtnSz)) {
        if (testRunning) {
            StopSeqThread();
            m_guiSeqMode.store(GuiSeqMode::TestPaused);
            LogLine("Sequencer: TEST paused");
        } else if (testPaused) {
            // Full reset to idle
            m_guiSeqMode.store(GuiSeqMode::Idle);
            m_tlPlayheadMs.store(-1);    // resets to C2-45s on next frame
            for (int i = 0; i < kMaxCamTracks; ++i) m_seqNextBlock[i] = 0;
            LogLine("Sequencer: TEST reset to idle");
        }
    }
    ImGui::PopStyleColor(2);
    if (!canTestStop) ImGui::EndDisabled();

    ImGui::Spacing();

    // ── RUN (production — real UTC time) ─────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.50f,0.15f,0.15f,1.0f));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    bool canRun = connected && hasBlocks && !anyRun && !testRunning;
    if (!canRun) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        kRunBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRunHover);
    ImGui::PushStyleColor(ImGuiCol_Text,          kRunText);
    if (ImGui::Button("\xe2\x96\xb6 RUN", kBtnSz)) {
        // RUN always starts from beginning of all tracks
        for (int i = 0; i < kMaxCamTracks; ++i) m_seqNextBlock[i] = 0;
        StartSeqThread(GuiSeqMode::Running);
        LogLine("Sequencer: RUN started (production)");
        m_lastResult = "RUN started — real UTC timing";
    }
    ImGui::PopStyleColor(3);
    if (!canRun) ImGui::EndDisabled();

    bool canStopRun = anyRun;
    if (!canStopRun) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        kStopBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kStopHover);
    ImGui::PushStyleColor(ImGuiCol_Text,          kRunText);
    if (ImGui::Button("\xe2\x96\xa0 STOP RUN", kBtnSz)) {
        StopSeqThread();
        m_guiSeqMode.store(GuiSeqMode::Idle);
        LogLine("Sequencer: RUN stopped");
        m_lastResult = "RUN stopped";
    }
    ImGui::PopStyleColor(3);
    if (!canStopRun) ImGui::EndDisabled();

    // ── Save calibration (visible after any run that produced bracket samples) ─
    bool idle = (sm == GuiSeqMode::Idle || sm == GuiSeqMode::TestPaused);
    if (idle && !m_seqCalibBuf.empty()) {
        ImGui::Spacing();
        static const ImVec4 kCalibBg    {0.06f, 0.24f, 0.38f, 1.0f};
        static const ImVec4 kCalibHover {0.10f, 0.40f, 0.65f, 1.0f};
        static const ImVec4 kCalibText  {0.45f, 0.85f, 1.00f, 1.0f};

        // Determine camera model from first connected camera, or "Unknown"
        std::string camModel;
        {
            std::lock_guard lk(m_camerasMutex);
            if (!m_cameras.empty() && !m_cameras[0].model.empty())
                camModel = m_cameras[0].model;
        }
        if (camModel.empty()) camModel = "Unknown";

        std::string btnLbl = std::format(
            "SAVE CALIB: {} ({} smp)", camModel, m_seqCalibBuf.size());
        ImGui::PushStyleColor(ImGuiCol_Button,        kCalibBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kCalibHover);
        ImGui::PushStyleColor(ImGuiCol_Text,          kCalibText);
        if (ImGui::Button(btnLbl.c_str(), kBtnSz))
            SaveCalibFromBuf(camModel);
        ImGui::PopStyleColor(3);
    }

    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::Spacing();
}

// ─── Status column (middle-right of top area) ─────────────────────────────────

void App::RenderStatusColumn() {
    static const ImVec4 kGray {0.40f, 0.40f, 0.45f, 1.0f};

    int iqpSt = m_iqpState.load();
    if (iqpSt == 1 || iqpSt == 2) {
        static const ImVec4 kGreen {0.15f, 0.90f, 0.35f, 1.0f};
        static const ImVec4 kRed   {0.95f, 0.22f, 0.18f, 1.0f};
        static const ImVec4 kDim2  {0.40f, 0.40f, 0.45f, 1.0f};
        ImGui::PushFont(m_fontLarge);
        if (iqpSt == 1) {
            ImGui::TextColored(kDim2, "checking location...");
        } else {
            ContactTimes ct;
            { std::lock_guard lk(m_iqpMutex); ct = m_contacts; }
            if (!ct.apiOk)      ImGui::TextColored(kDim2,  "network error");
            else if (!ct.valid) ImGui::TextColored(kRed,   "NO ECLIPSE VISIBLE HERE");
            else if (ct.c2Ms>0) ImGui::TextColored(kGreen, "YOU ARE IN THE TOTALITY ZONE");
            else                ImGui::TextColored(kRed,   "YOU ARE OUTSIDE TOTALITY ZONE");
        }
        ImGui::PopFont();
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    }

    if (!m_lastResult.empty()) {
        ImGui::PushFont(m_fontMono);
        bool ok = m_lastResult.find("ERROR") == std::string::npos;
        ImGui::TextColored(ok ? ImVec4(0.55f,0.85f,0.55f,1.f)
                              : ImVec4(1.0f, 0.35f,0.25f,1.f),
                           "%s", m_lastResult.c_str());
        ImGui::PopFont();
    }

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 28.0f);
    ImGui::PushFont(m_fontMono);
    ImGui::TextColored(kGray, "dev:");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::Checkbox("Style##se", &m_showStyleEditor);
    ImGui::SameLine();
    ImGui::Checkbox("Demo##dw",  &m_showDemoWindow);
}

// ─── Inspector + Palette column ───────────────────────────────────────────────

void App::RenderInspectorColumn() {
    static const ImVec4 kGray {0.40f, 0.40f, 0.45f, 1.0f};
    static const ImVec4 kDim  {0.28f, 0.28f, 0.32f, 1.0f};

    ImGui::SeparatorText("INSPECTOR");

    bool hasSel = m_selTrack >= 0 && m_selTrack < (int)m_tracks.size()
               && m_selBlock >= 0 && m_selBlock < (int)m_tracks[m_selTrack].blocks.size();

    if (!hasSel) {
        ImGui::PushFont(m_fontMono);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 6));
        ImGui::TextColored(kDim, "Select block on");
        ImGui::TextColored(kDim, "TimeLine to adjust");
        ImGui::TextColored(kDim, "photo parameters");
        ImGui::PopStyleVar();
        ImGui::PopFont();
    } else {
        TLBlock& b = m_tracks[m_selTrack].blocks[m_selBlock];
        ImGui::PushFont(m_fontMono);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));

        // Type label (colour-coded)
        ImU32  tc = BlockColor(b.type);
        ImVec4 tcv{float(tc&0xff)/255.f, float((tc>>8)&0xff)/255.f,
                   float((tc>>16)&0xff)/255.f, 1.f};
        ImGui::TextColored(kGray, "Type"); ImGui::SameLine(52);
        ImGui::TextColored(tcv, "%s", BlockTypeName(b.type));

        if (b.type != BlockType::Audio) {
            char buf[32];
            ImGui::TextColored(kGray, "SS");   ImGui::SameLine(52);
            ImGui::SetNextItemWidth(-1);
            snprintf(buf, sizeof(buf), "%s", b.ss.c_str());
            if (ImGui::InputText("##ss", buf, sizeof(buf))) { b.ss = buf; m_tlDirty = true; }

            ImGui::TextColored(kGray, "ISO");  ImGui::SameLine(52);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##iso", &b.iso, 0)) m_tlDirty = true;

            ImGui::TextColored(kGray, "f/");   ImGui::SameLine(52);
            ImGui::SetNextItemWidth(-1);
            snprintf(buf, sizeof(buf), "%s", b.fstop.c_str());
            if (ImGui::InputText("##fstop", buf, sizeof(buf))) { b.fstop = buf; m_tlDirty = true; }
        }

        if (b.type == BlockType::Bracket) {
            // 16 valid modes matching CrSDK + camera menu.
            // 2.0ev / 3.0ev support x3 and x5 only (SDK limitation — no 9-pic).
            struct BrkMode { const char* label; const char* ev; int count; };
            static const BrkMode kBrk[] = {
                {"0.3ev \xc3\x97 3", "0.3ev",  3},
                {"0.3ev \xc3\x97 5", "0.3ev",  5},
                {"0.3ev \xc3\x97 9", "0.3ev",  9},
                {"0.5ev \xc3\x97 3", "0.5ev",  3},
                {"0.5ev \xc3\x97 5", "0.5ev",  5},
                {"0.5ev \xc3\x97 9", "0.5ev",  9},
                {"0.7ev \xc3\x97 3", "0.7ev",  3},
                {"0.7ev \xc3\x97 5", "0.7ev",  5},
                {"0.7ev \xc3\x97 9", "0.7ev",  9},
                {"1.0ev \xc3\x97 3", "1.0ev",  3},
                {"1.0ev \xc3\x97 5", "1.0ev",  5},
                {"1.0ev \xc3\x97 9", "1.0ev",  9},
                {"2.0ev \xc3\x97 3", "2.0ev",  3},
                {"2.0ev \xc3\x97 5", "2.0ev",  5},
                {"3.0ev \xc3\x97 3", "3.0ev",  3},
                {"3.0ev \xc3\x97 5", "3.0ev",  5},
            };
            static constexpr int kBrkN = 16;

            int idx = 10; // default: 1.0ev x5
            for (int i = 0; i < kBrkN; ++i)
                if (b.ev == kBrk[i].ev && b.count == kBrk[i].count) { idx = i; break; }

            ImGui::TextColored(kGray, "Bracket"); ImGui::SameLine(52);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##brk", kBrk[idx].label)) {
                for (int i = 0; i < kBrkN; ++i) {
                    if (i > 0 && strcmp(kBrk[i].ev, kBrk[i-1].ev) != 0)
                        ImGui::Separator();
                    if (ImGui::Selectable(kBrk[i].label, i == idx)) {
                        b.ev    = kBrk[i].ev;
                        b.count = kBrk[i].count;
                        m_tlDirty = true;
                    }
                    if (i == idx) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        if (b.type == BlockType::Burst) {
            // Drive mode combo — maps to CrSDK drive mode strings
            struct BurstMode { const char* label; const char* drive; float fps; };
            static const BurstMode kModes[] = {
                {"HI+", "cont-hi-plus", 10.0f},
                {"HI",  "cont-hi",       8.0f},
                {"MID", "cont-mid",      5.0f},
                {"LO",  "cont-lo",       3.0f},
            };
            int modeIdx = 0;
            for (int i = 0; i < 4; ++i)
                if (b.burstDrive == kModes[i].drive) { modeIdx = i; break; }

            ImGui::TextColored(kGray, "Drive");  ImGui::SameLine(52);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##bdrive", kModes[modeIdx].label)) {
                for (int i = 0; i < 4; ++i) {
                    char lbl[24];
                    snprintf(lbl, sizeof(lbl), "%-4s  (~%.0f fps)",
                             kModes[i].label, kModes[i].fps);
                    if (ImGui::Selectable(lbl, i == modeIdx)) {
                        b.burstDrive = kModes[i].drive;
                        m_tlDirty = true;
                    }
                    if (i == modeIdx) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::TextColored(kGray, "Dur s");  ImGui::SameLine(52);
            ImGui::SetNextItemWidth(-1);
            float ds = b.burstDurMs / 1000.f;
            if (ImGui::InputFloat("##bdur", &ds, 0.f, 0.f, "%.1f")) {
                b.burstDurMs = int32_t(ds * 1000.f);
                m_tlDirty = true;
            }

            // Informational: estimated frame count
            float fps  = kModes[modeIdx].fps;
            int   nFrm = std::max(1, (int)(fps * ds + 0.5f));
            ImGui::TextColored(kDim, "~%d frames @ %.0f fps", nFrm, fps);
        }

        if (b.type == BlockType::Audio) {
            ImGui::TextColored(kGray, "File"); ImGui::SameLine(52);
            char fileBuf[256];
            snprintf(fileBuf, sizeof(fileBuf), "%s",
                     b.audioFile.empty() ? "(none)" : b.audioFile.c_str());
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##af", fileBuf, sizeof(fileBuf), ImGuiInputTextFlags_ReadOnly);
            if (ImGui::Button("Browse...", ImVec2(-1,0))) {
                wchar_t fbuf[MAX_PATH] = {};
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.lpstrFilter = L"Audio MP3\0*.mp3\0All\0*.*\0";
                ofn.lpstrFile   = fbuf;
                ofn.nMaxFile    = MAX_PATH;
                ofn.Flags       = OFN_FILEMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    b.audioFile.clear();
                    for (wchar_t wc : std::wstring(fbuf))
                        b.audioFile += static_cast<char>(wc);
                }
            }
        }

        // Label
        {
            char lbuf[64];
            ImGui::TextColored(kGray, "Label"); ImGui::SameLine(52);
            ImGui::SetNextItemWidth(-1);
            snprintf(lbuf, sizeof(lbuf), "%s", b.label.c_str());
            if (ImGui::InputText("##lbl", lbuf, sizeof(lbuf))) { b.label = lbuf; m_tlDirty = true; }
        }

        bool prevSnap = b.snapToPrev;
        ImGui::Checkbox("Snap to prev##snp", &b.snapToPrev);
        if (b.snapToPrev != prevSnap) {
            if (b.snapToPrev && m_selBlock > 0) {
                auto& prev = m_tracks[m_selTrack].blocks[m_selBlock - 1];
                b.atMs = prev.atMs + BlockDurMs(prev);
                if (prev.type != BlockType::Audio && b.type != BlockType::Audio &&
                    BlockParamsDiffer(prev, b))
                    b.atMs += ArmEstMs(prev);
            }
            m_tlDirty = true;
        }
        ImGui::Spacing();

        // Computed duration
        int64_t dur = BlockDurMs(b);
        char durBuf[24];
        if (dur < 1000) snprintf(durBuf,sizeof(durBuf), "%lldms", dur);
        else            snprintf(durBuf,sizeof(durBuf), "%.2fs", dur/1000.0);
        ImGui::TextColored(kDim, "Dur:"); ImGui::SameLine(52);
        ImGui::TextColored(kGray, "%s", durBuf);

        // UTC time
        if (b.atMs > 0) {
            int64_t s = b.atMs / 1000;
            char atBuf[16];
            snprintf(atBuf, sizeof(atBuf), "%02d:%02d:%02d.%03d",
                     (int)((s/3600)%24),(int)((s/60)%60),(int)(s%60),(int)(b.atMs%1000));
            ImGui::TextColored(kDim, "At:");  ImGui::SameLine(52);
            ImGui::TextColored(kGray, "%s", atBuf);
        }

        ImGui::PopStyleVar();
        ImGui::PopFont();
    }

    // ── Palette ────────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("PALETTE");
    ImGui::Spacing();

    struct PE { BlockType type; const char* name; ImU32 col; };
    PE pal[] = {
        {BlockType::Single,  "Single",  IM_COL32( 40,160, 70,255)},
        {BlockType::Burst,   "Burst",   IM_COL32( 50,120,200,255)},
        {BlockType::Bracket, "Bracket", IM_COL32(200,130, 30,255)},
        {BlockType::Audio,   "Audio",   IM_COL32(140, 80,200,255)},
    };

    for (auto& pe : pal) {
        ImGui::PushID((int)pe.type);
        float    pw  = ImGui::GetContentRegionAvail().x;
        ImVec2   pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##pb", {pw, 34.0f});
        bool     hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, {pos.x+pw, pos.y+34.f},
                          hov ? IM_COL32(45,45,58,255) : IM_COL32(20,20,28,255), 4.f);
        dl->AddRectFilled({pos.x+5, pos.y+8}, {pos.x+17, pos.y+26}, pe.col, 2.f);
        dl->AddText({pos.x+22, pos.y+9}, IM_COL32(200,200,215,255), pe.name);
        if (hov) dl->AddRect(pos, {pos.x+pw, pos.y+34.f}, pe.col, 4.f, 0, 1.f);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            const char* pid = (pe.type == BlockType::Audio) ? "TL_AUD_BLOCK" : "TL_CAM_BLOCK";
            ImGui::SetDragDropPayload(pid, &pe.type, sizeof(BlockType));
            ImGui::PushFont(m_fontMono);
            ImVec4 cv{float(pe.col&0xff)/255.f, float((pe.col>>8)&0xff)/255.f,
                      float((pe.col>>16)&0xff)/255.f, 1.f};
            ImGui::TextColored(cv, "%s", pe.name);
            ImGui::PopFont();
            ImGui::EndDragDropSource();
        }
        ImGui::Spacing();
        ImGui::PopID();
    }
}

// ─── Timeline (bottom, full width) ────────────────────────────────────────────

void App::RenderTimelineBottom() {
    static const float kLabelW  = 140.0f;
    static const float kMarkerH =  14.0f;
    static const float kPhaseH  =  20.0f;
    static const float kRulerH  =  85.0f;
    static const float kTrackH  =  28.0f;

    ImVec2 winPos  = ImGui::GetWindowPos();
    float  winW    = ImGui::GetWindowWidth();
    float  cntW    = winW - kLabelW;   // content width (right of labels)
    m_tlScreenTopY = winPos.y;         // remember for drag-out-to-delete

    ContactTimes ct;
    { std::lock_guard lk(m_iqpMutex); ct = m_contacts; }
    if (!ct.valid) ct = m_beResult;

    if (!ct.valid) {
        ImGui::SetCursorPos({kLabelW + 8.f, 8.f});
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored({0.28f,0.28f,0.32f,1.f}, "Calculate contacts to show timeline");
        ImGui::PopFont();
        return;
    }

    // Initialise view range once
    if (m_tlViewStart < 0) {
        m_tlViewStart = ct.c1Ms - 5LL*60000;
        m_tlViewEnd   = ct.c4Ms + 5LL*60000;
    }
    int64_t vDur = m_tlViewEnd - m_tlViewStart;
    if (vDur <= 0) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = winPos.y;

    auto toPx = [&](int64_t ms) -> float {
        return winPos.x + kLabelW + float(ms - m_tlViewStart) / float(vDur) * cntW;
    };

    // ── Phase bar ─────────────────────────────────────────────────────────
    float phaseY = y + kMarkerH;
    dl->AddRectFilled({winPos.x, phaseY}, {winPos.x + kLabelW, phaseY + kPhaseH},
                      IM_COL32(10,10,14,255));

    int64_t c1=ct.c1Ms, c2=ct.c2Ms, mx=ct.maxMs, c3=ct.c3Ms, c4=ct.c4Ms;

    if (c2>0 && c3>0) {
        struct PZ { int64_t s, e; ImU32 col; const char* lbl; };
        PZ zones[] = {
            {m_tlViewStart, c2-10000, IM_COL32(28,28,36,255), "Partial"},
            {c2-10000, c2-2000,  IM_COL32(110,90,15,255),    "Bailey's"},
            {c2-2000,  c2+2000,  IM_COL32(190,185,145,255),  "D.Ring"},
            {c2+2000,  c2+45000, IM_COL32(115,48,68,255),    "Chrom"},
            {c2+45000, mx-20000, IM_COL32(80,60,8,255),      "Corona"},
            {mx-20000, mx+20000, IM_COL32(150,118,18,255),   "Max"},
            {mx+20000, c3-45000, IM_COL32(80,60,8,255),      "Corona"},
            {c3-45000, c3-2000,  IM_COL32(115,48,68,255),    "Chrom"},
            {c3-2000,  c3+2000,  IM_COL32(190,185,145,255),  "D.Ring"},
            {c3+2000,  c3+10000, IM_COL32(110,90,15,255),    "Bailey's"},
            {c3+10000, m_tlViewEnd, IM_COL32(28,28,36,255),  "Partial"},
        };
        for (auto& z : zones) {
            if (z.s >= z.e) continue;
            float px1 = std::max(toPx(z.s), winPos.x+kLabelW);
            float px2 = std::min(toPx(z.e), winPos.x+winW);
            if (px2 <= px1) continue;
            dl->AddRectFilled({px1,phaseY},{px2,phaseY+kPhaseH}, z.col);
            float zw = px2-px1;
            if (zw > 30.f) {
                float tw = ImGui::CalcTextSize(z.lbl).x;
                dl->AddText({px1+(zw-tw)*0.5f, phaseY+3.f},
                            IM_COL32(180,168,138,155), z.lbl);
            }
        }
    } else {
        dl->AddRectFilled({winPos.x+kLabelW,phaseY},{winPos.x+winW,phaseY+kPhaseH},
                          IM_COL32(28,28,36,255));
    }
    dl->AddRect({winPos.x+kLabelW,phaseY},{winPos.x+winW,phaseY+kPhaseH},
                IM_COL32(40,40,52,255));

    // Contact markers
    struct CM { int64_t ms; const char* lbl; ImU32 col; };
    CM cms[] = {
        {c1,"C1",  IM_COL32(140,140,165,255)},
        {c2,"C2",  IM_COL32(255,210, 50,255)},
        {mx,"Max", IM_COL32(255,255,255,255)},
        {c3,"C3",  IM_COL32(255,210, 50,255)},
        {c4,"C4",  IM_COL32(140,140,165,255)},
    };
    for (auto& m : cms) {
        if (m.ms < 0) continue;
        float px = toPx(m.ms);
        if (px < winPos.x+kLabelW || px > winPos.x+winW) continue;
        dl->AddLine({px,phaseY},{px,phaseY+kPhaseH}, m.col, 1.5f);
        dl->AddText({px+2.f, y+1.f}, m.col, m.lbl);
    }

    // ── Ruler: ticks + rotated UTC labels ────────────────────────────────
    float rulerY = phaseY + kPhaseH;
    dl->AddRectFilled({winPos.x, rulerY}, {winPos.x + winW, rulerY + kRulerH},
                      IM_COL32(10, 10, 14, 255));

    // Collect ticks: phase transitions + 60s interval marks.
    // Fixed-size stack array (no heap alloc): NASA rule 3.
    struct RulerTick { int64_t ms; ImU32 col; bool isPhase; };
    static constexpr int kMaxRulerTicks = 256;
    RulerTick ticks[kMaxRulerTicks];
    int numTicks = 0;

    auto addTick = [&](int64_t ms, ImU32 col, bool phase) {
        assert(numTicks <= kMaxRulerTicks);
        if (numTicks < kMaxRulerTicks && ms >= m_tlViewStart && ms <= m_tlViewEnd)
            ticks[numTicks++] = {ms, col, phase};
    };

    // Phase transition times
    if (c2 > 0 && c3 > 0) {
        constexpr ImU32 kC   = IM_COL32(255, 210,  50, 200);
        constexpr ImU32 kMax = IM_COL32(255, 255, 255, 180);
        constexpr ImU32 kC14 = IM_COL32(140, 140, 165, 160);
        constexpr ImU32 kSub = IM_COL32(130, 110,  70, 140);
        if (c1 > 0)  addTick(c1,          kC14,  true);
        addTick(c2 - 10000, kSub,  true);
        addTick(c2 -  2000, kSub,  true);
        addTick(c2,         kC,    true);
        addTick(c2 +  2000, kSub,  true);
        addTick(c2 + 45000, kSub,  true);
        addTick(mx - 20000, kSub,  true);
        addTick(mx,         kMax,  true);
        addTick(mx + 20000, kSub,  true);
        addTick(c3 - 45000, kSub,  true);
        addTick(c3 -  2000, kSub,  true);
        addTick(c3,         kC,    true);
        addTick(c3 +  2000, kSub,  true);
        addTick(c3 + 10000, kSub,  true);
        if (c4 > 0)  addTick(c4,          kC14,  true);
    }

    // 60-second ticks — only when >= 20px apart (avoid clutter at wide views)
    float px60s = (vDur > 0) ? 60000.f * cntW / static_cast<float>(vDur) : 0.f;
    if (px60s >= 20.f) {
        int64_t t60 = 60000LL;
        int64_t ft  = ((m_tlViewStart / t60) + 1) * t60;
        // Bounded: max 300 iterations covers 5 h; zoom clamp is 4 h
        static constexpr int k60Max = 300;
        int loop60 = 0;
        for (int64_t t = ft; t < m_tlViewEnd && loop60 < k60Max; t += t60, ++loop60) {
            float tx = toPx(t);
            bool nearPhase = false;
            for (int i = 0; i < numTicks; ++i) {
                if (fabsf(toPx(ticks[i].ms) - tx) < 8.f) { nearPhase = true; break; }
            }
            if (!nearPhase)
                addTick(t, IM_COL32(55, 55, 80, 180), false);
        }
    }

    // Sort by time so greedy left-to-right label placement works
    std::sort(ticks, ticks + numTicks,
              [](const RulerTick& a, const RulerTick& b){ return a.ms < b.ms; });

    // Draw ticks + rotated labels
    ImFont* lblFont  = m_fontMono ? m_fontMono : ImGui::GetIO().Fonts->Fonts[0];
    constexpr float kLblSz   = 15.0f;  // native m_fontMono size
    constexpr float kTickH   =  7.0f;  // tick line height
    // Minimum x-distance between adjacent labels (≈ font line height)
    constexpr float kMinLblDx = 16.0f;
    float lastLabelX = -9999.f;

    for (int i = 0; i < numTicks; ++i) {
        const RulerTick& tk = ticks[i];
        float tx = toPx(tk.ms);
        if (tx < winPos.x + kLabelW || tx > winPos.x + winW) continue;

        float th = tk.isPhase ? kTickH : kTickH * 0.55f;
        dl->AddLine({tx, rulerY}, {tx, rulerY + th}, tk.col, 1.f);

        if (tx - lastLabelX >= kMinLblDx) {
            int64_t sv = tk.ms / 1000;
            char lb[12];
            snprintf(lb, sizeof(lb), "%02d:%02d:%02d",
                     (int)((sv / 3600) % 24), (int)((sv / 60) % 60), (int)(sv % 60));
            AddTextRotated90CCW(dl, lblFont, kLblSz,
                                {tx, rulerY + kRulerH - 2.f}, tk.col, lb);
            lastLabelX = tx;
        }
    }

    // ── Tracks ────────────────────────────────────────────────────────────
    float tracksY = rulerY + kRulerH;
    int   nT      = (int)m_tracks.size();
    float tracksH = nT * kTrackH;

    // ── Drag-to-move update (runs every frame while LMB held) ────────────────
    if (m_tlDragTrack >= 0 && m_tlDragBlock >= 0
        && m_tlDragTrack < nT
        && m_tlDragBlock < (int)m_tracks[m_tlDragTrack].blocks.size()) {

        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
            m_tlDragging = true;
            float   dx  = ImGui::GetMousePos().x - m_tlDragMouseX0;
            int64_t dMs = int64_t(dx / cntW * vDur);
            m_tracks[m_tlDragTrack].blocks[m_tlDragBlock].atMs =
                std::max(m_tlDragStartMs + dMs, m_tlViewStart);
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (m_tlDragging) {
                ImVec2 mp  = ImGui::GetMousePos();
                auto&  trk = m_tracks[m_tlDragTrack];

                if (mp.y < m_tlScreenTopY) {
                    // Released above timeline → delete block
                    trk.blocks.erase(trk.blocks.begin() + m_tlDragBlock);
                    m_selTrack = -1; m_selBlock = -1;
                } else {
                    // Finalize: sort by time
                    int64_t   movedMs = trk.blocks[m_tlDragBlock].atMs;
                    BlockType movedTy = trk.blocks[m_tlDragBlock].type;
                    std::sort(trk.blocks.begin(), trk.blocks.end(),
                        [](const TLBlock& a, const TLBlock& b){ return a.atMs < b.atMs; });
                    // Update selection index after sort
                    for (int i = 0; i < (int)trk.blocks.size(); ++i) {
                        if (trk.blocks[i].atMs == movedMs && trk.blocks[i].type == movedTy) {
                            m_selBlock = i; break;
                        }
                    }
                    // Apply snap-to-prev if enabled
                    if (m_selBlock > 0 && trk.blocks[m_selBlock].snapToPrev) {
                        auto& prev = trk.blocks[m_selBlock - 1];
                        auto& cur  = trk.blocks[m_selBlock];
                        cur.atMs = prev.atMs + BlockDurMs(prev);
                        if (prev.type != BlockType::Audio && cur.type != BlockType::Audio &&
                            BlockParamsDiffer(prev, cur))
                            cur.atMs += ArmEstMs(prev);
                    }
                }
                m_tlDirty = true;
            }
            m_tlDragging    = false;
            m_tlDragTrack   = -1;
            m_tlDragBlock   = -1;
            m_tlDragStartMs = -1;
        }
    }

    // ── Click detection: select block + initiate drag state ──────────────────
    if (!m_tlDragging && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mp = ImGui::GetMousePos();
        bool hit = false;
        for (int ti = 0; ti < nT && !hit; ++ti) {
            float ty = tracksY + ti*kTrackH;
            if (mp.y < ty || mp.y >= ty+kTrackH) continue;
            for (int bi = 0; bi < (int)m_tracks[ti].blocks.size(); ++bi) {
                auto& blk = m_tracks[ti].blocks[bi];
                float bx  = toPx(blk.atMs);
                int64_t blkEnd = blk.atMs + BlockDurMs(blk);
                // Extend hit box to cover ARM extension zone.
                if (blk.type != BlockType::Audio &&
                    bi + 1 < (int)m_tracks[ti].blocks.size()) {
                    const TLBlock& nxt = m_tracks[ti].blocks[bi + 1];
                    if (nxt.atMs >= 0 && BlockParamsDiffer(blk, nxt))
                        blkEnd += ArmEstMs(blk);
                }
                float bx2 = toPx(blkEnd);
                if (mp.x >= bx && mp.x <= bx2) {
                    m_selTrack = ti; m_selBlock = bi;
                    // Arm drag state (actual drag starts after threshold)
                    m_tlDragTrack   = ti;
                    m_tlDragBlock   = bi;
                    m_tlDragStartMs = blk.atMs;
                    m_tlDragMouseX0 = mp.x;
                    hit = true; break;
                }
            }
        }
        if (!hit) {
            bool inArea = mp.x >= winPos.x+kLabelW && mp.x <= winPos.x+winW
                       && mp.y >= tracksY && mp.y < tracksY+tracksH;
            if (inArea) { m_selTrack = -1; m_selBlock = -1; }
        }
    }

    // Per-track DnD drop targets.
    // Camera tracks accept "TL_CAM_BLOCK", audio track accepts "TL_AUD_BLOCK".
    // dropTargetTi records which track is the active hover target so we can draw
    // the highlight rect in pass 2 (after track backgrounds, so it's not covered).
    int dropTargetTi = -1;
    for (int ti = 0; ti < nT; ++ti) {
        float      ty  = tracksY + ti * kTrackH;
        TLTrack&   tr  = m_tracks[ti];
        const char* pid = tr.IsCamera() ? "TL_CAM_BLOCK" : "TL_AUD_BLOCK";

        ImGui::SetCursorScreenPos({winPos.x + kLabelW, ty});
        char dropId[24]; snprintf(dropId, sizeof(dropId), "##tl_drop_%d", ti);
        ImGui::InvisibleButton(dropId, {cntW, kTrackH});

        // ImGui's RenderDragDropTargetRect writes to window->DrawList before track
        // backgrounds are drawn, so it gets covered. Push transparent to suppress it;
        // we draw our own rect in pass 2.
        ImGui::PushStyleColor(ImGuiCol_DragDropTarget, IM_COL32(0, 0, 0, 0));
        if (ImGui::BeginDragDropTarget()) {
            dropTargetTi = ti;
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(pid)) {
                BlockType bt   = *(const BlockType*)p->Data;
                float     relX = ImGui::GetMousePos().x - (winPos.x + kLabelW);
                int64_t   atMs = std::max(
                    m_tlViewStart + int64_t(relX / cntW * vDur), m_tlViewStart);
                TLBlock nb; nb.type = bt; nb.atMs = atMs;
                tr.blocks.push_back(nb);
                std::sort(tr.blocks.begin(), tr.blocks.end(),
                    [](const TLBlock& a, const TLBlock& b){ return a.atMs < b.atMs; });
                m_selTrack = ti;
                auto it = std::find_if(tr.blocks.begin(), tr.blocks.end(),
                    [&](const TLBlock& blk){ return blk.atMs==atMs && blk.type==bt; });
                m_selBlock = (int)(it - tr.blocks.begin());
                m_tlDirty = true;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopStyleColor();
    }

    // Draw tracks — pass 1: backgrounds, block fills, labels
    // 2-px gap at the bottom of each track (ty2-2 instead of ty2) separates rows visually.
    for (int ti = 0; ti < nT; ++ti) {
        TLTrack& tr  = m_tracks[ti];
        float ty     = tracksY + ti * kTrackH;
        float tyFill = ty + kTrackH - 2.f;   // bottom of visible content (2px gap below)

        // Label column
        dl->AddRectFilled({winPos.x, ty}, {winPos.x+kLabelW-2, tyFill},
                          IM_COL32(14, 14, 22, 255));
        ImU32 lc = tr.IsAudio() ? IM_COL32(150,100,210,255) : IM_COL32(120,148,172,255);
        dl->AddText({winPos.x+4.f, ty+7.f}, lc, tr.label.c_str());

        // Track content bg
        ImU32 bg = (ti % 2 == 0) ? IM_COL32(14,14,20,255) : IM_COL32(12,12,18,255);
        dl->AddRectFilled({winPos.x+kLabelW, ty}, {winPos.x+winW, tyFill}, bg);

        // Block fills + text labels (no selection border here — drawn in pass 2)
        for (int bi = 0; bi < (int)tr.blocks.size(); ++bi) {
            TLBlock& blk = tr.blocks[bi];
            if (blk.atMs < 0) continue;
            float bx  = toPx(blk.atMs);
            float bx2 = toPx(blk.atMs + BlockDurMs(blk));
            bx2 = std::max(bx2, bx + 3.f);
            float cx1 = winPos.x + kLabelW, cx2 = winPos.x + winW;
            if (bx2 < cx1 || bx > cx2) continue;
            float dx  = std::max(bx, cx1);
            float dx2 = std::min(bx2, cx2);
            bool  dragged = (m_tlDragging && m_tlDragTrack==ti && m_tlDragBlock==bi);
            bool  willDel = dragged && (ImGui::GetMousePos().y < m_tlScreenTopY);
            ImU32 col     = willDel ? IM_COL32(200,40,40,200) : BlockColor(blk.type);
            dl->AddRectFilled({dx, ty+2}, {dx2, tyFill}, col, 2.f);
            if (dx2-dx > 20.f && !blk.label.empty())
                dl->AddText({dx+4.f, ty+6.f}, IM_COL32(240,240,240,215), blk.label.c_str());

            // ARM extension: dim bar showing camera write+arm time that follows a
            // block when the next block has different params.
            if (!tr.IsAudio() && bi + 1 < (int)tr.blocks.size()) {
                const TLBlock& nxt = tr.blocks[bi + 1];
                if (nxt.atMs >= 0 && blk.type != BlockType::Audio &&
                    BlockParamsDiffer(blk, nxt)) {
                    float bxA  = toPx(blk.atMs + BlockDurMs(blk));
                    float bxA2 = toPx(blk.atMs + BlockDurMs(blk) + ArmEstMs(blk));
                    bxA2 = std::max(bxA2, bxA + 2.f);
                    float dxA  = std::max(bxA,  cx1);
                    float dxA2 = std::min(bxA2, cx2);
                    if (dxA2 > dxA) {
                        dl->AddRectFilled({dxA, ty+4.f}, {dxA2, tyFill-2.f},
                                          IM_COL32(160, 90, 20, 90));
                        if (dxA2 - dxA > 28.f)
                            dl->AddText({dxA + 3.f, ty + 6.f},
                                        IM_COL32(200, 130, 60, 160), "ARM");
                    }
                }
            }
        }
    }

    // Draw tracks — pass 2: selection/drag borders on top of everything (incl. track gaps)
    for (int ti = 0; ti < nT; ++ti) {
        const TLTrack& tr = m_tracks[ti];
        float ty     = tracksY + ti * kTrackH;
        float tyFill = ty + kTrackH - 2.f;
        for (int bi = 0; bi < (int)tr.blocks.size(); ++bi) {
            bool sel     = (m_selTrack == ti && m_selBlock == bi);
            bool dragged = (m_tlDragging && m_tlDragTrack == ti && m_tlDragBlock == bi);
            if (!sel && !dragged) continue;
            const TLBlock& blk = tr.blocks[bi];
            if (blk.atMs < 0) continue;
            float bx  = toPx(blk.atMs);
            int64_t selEnd = blk.atMs + BlockDurMs(blk);
            if (blk.type != BlockType::Audio && bi + 1 < (int)tr.blocks.size()) {
                const TLBlock& nxt = tr.blocks[bi + 1];
                if (nxt.atMs >= 0 && BlockParamsDiffer(blk, nxt))
                    selEnd += ArmEstMs(blk);
            }
            float bx2 = toPx(selEnd);
            bx2 = std::max(bx2, bx + 3.f);
            float cx1 = winPos.x + kLabelW, cx2 = winPos.x + winW;
            if (bx2 < cx1 || bx > cx2) continue;
            float dx  = std::max(bx, cx1);
            float dx2 = std::min(bx2, cx2);
            bool  willDel = dragged && (ImGui::GetMousePos().y < m_tlScreenTopY);
            ImU32 rimCol  = willDel ? IM_COL32(255,60,60,255) : IM_COL32(255,220,60,255);
            dl->AddRect({dx-1, ty+1}, {dx2+1, tyFill+1}, rimCol, 2.f, 0, 2.f);
        }
    }

    // DnD drop target highlight — drawn after all track backgrounds so it's not covered.
    // Full-height border around the compatible target track row.
    if (dropTargetTi >= 0) {
        assert(dropTargetTi < nT);
        float ty     = tracksY + dropTargetTi * kTrackH;
        float tyFill = ty + kTrackH - 2.f;
        dl->AddRect({winPos.x + kLabelW + 1.f, ty + 1.f},
                    {winPos.x + winW - 1.f,    tyFill},
                    IM_COL32(255, 220, 60, 230), 0.f, 0, 2.f);
    }

    // "now" cursor
    int64_t nowMs = UtcNowMs();
    if (nowMs >= m_tlViewStart && nowMs <= m_tlViewEnd) {
        float nx = toPx(nowMs);
        dl->AddLine({nx,phaseY},{nx,tracksY+tracksH}, IM_COL32(210,70,70,175), 1.5f);
        dl->AddText({nx+2.f, y+1.f}, IM_COL32(210,70,70,255), "now");
    }

    // ── Playhead ──────────────────────────────────────────────────────────
    // Initialise playhead to C2 - 45 s when contacts first become available.
    {
        int64_t ph = m_tlPlayheadMs.load();
        if (ph < 0 && c2 > 0) {
            ph = c2 - 45000LL;
            m_tlPlayheadMs.store(ph);
        }
        if (ph >= m_tlViewStart && ph <= m_tlViewEnd) {
            float px = toPx(ph);
            // Cyan line from ruler top to bottom of tracks
            dl->AddLine({px, rulerY}, {px, tracksY + tracksH},
                        IM_COL32(80, 220, 255, 200), 1.5f);
            // Triangle marker at ruler top
            dl->AddTriangleFilled(
                {px,       rulerY + 6.f},
                {px - 6.f, rulerY},
                {px + 6.f, rulerY},
                IM_COL32(80, 220, 255, 230));
            // Time label just above triangle
            int64_t sv = ph / 1000;
            char phLbl[12];
            snprintf(phLbl, sizeof(phLbl), "%02d:%02d:%02d",
                     (int)((sv/3600)%24), (int)((sv/60)%60), (int)(sv%60));
            dl->AddText({px + 4.f, rulerY}, IM_COL32(80, 220, 255, 220), phLbl);
        }
    }

    // ── Click on ruler → set playhead (only when sequencer is idle/paused) ──
    {
        GuiSeqMode sm = m_guiSeqMode.load();
        bool canDrag  = (sm == GuiSeqMode::Idle || sm == GuiSeqMode::TestPaused);
        if (canDrag && ImGui::IsWindowHovered()
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ImVec2 mp = ImGui::GetMousePos();
            // Ruler area: between rulerY and rulerY + kRulerH, right of label column
            if (mp.x > winPos.x + kLabelW && mp.x < winPos.x + winW
                && mp.y >= rulerY && mp.y < rulerY + kRulerH) {
                float relX  = mp.x - (winPos.x + kLabelW);
                int64_t tMs = m_tlViewStart + int64_t(relX / cntW * vDur);
                m_tlPlayheadMs.store(std::clamp(tMs, m_tlViewStart, m_tlViewEnd));
                // Reset per-track next-block indices so resume picks correct start
                if (sm == GuiSeqMode::TestPaused) {
                    int64_t ph = m_tlPlayheadMs.load();
                    int camIdx = 0;
                    for (int ti = 0;
                         ti < static_cast<int>(m_tracks.size()) && camIdx < kMaxCamTracks;
                         ++ti) {
                        if (!m_tracks[ti].IsCamera()) { continue; }
                        // Find first block at or after new playhead position
                        int ni = 0;
                        for (; ni < static_cast<int>(m_tracks[ti].blocks.size()); ++ni)
                            if (m_tracks[ti].blocks[ni].atMs >= ph) break;
                        m_seqNextBlock[camIdx++] = ni;
                    }
                }
            }
        }
    }

    // ── Scroll wheel zoom ─────────────────────────────────────────────────
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (std::fabs(wheel) > 0.01f) {
            ImVec2  mp   = ImGui::GetMousePos();
            float   relX = std::clamp((mp.x-(winPos.x+kLabelW))/cntW, 0.f, 1.f);
            int64_t fms  = m_tlViewStart + int64_t(relX * vDur);
            float   fac  = (wheel > 0) ? 0.75f : 1.333f;
            int64_t nd   = std::clamp(int64_t(float(vDur)*fac),
                                      10LL*1000LL, 4LL*3600000LL);
            m_tlViewStart = fms - int64_t(relX * nd);
            m_tlViewEnd   = m_tlViewStart + nd;
        }
    }
}

// ─── App lifecycle ────────────────────────────────────────────────────────────

App::App() {
    std::wstring dir = ExeDir();

    m_logFile.open(dir + L"\\TotalControlGUI.log", std::ios::app);
    LogLine("=== TotalControlGUI start ===");

    // Route IQP HTTP diagnostics through the same log file (called from bg thread).
    SetIqpLogger([this](std::string_view msg) { LogLine(msg); });

    // ── TotalControlDefaultConfig.db — factory defaults ───────────────────
    std::wstring defaultCfgPath = dir + L"\\TotalControlDefaultConfig.db";
    EnsureDefaultConfig(defaultCfgPath);

    // ── TotalControlConfig.db — active config ─────────────────────────────
    std::wstring configPath = dir + L"\\TotalControlConfig.db";
    if (!std::filesystem::exists(configPath)) {
        std::filesystem::copy_file(defaultCfgPath, configPath);
        LogLine("First run: TotalControlConfig.db created from defaults");
    }

    if (m_configDb.Open(configPath)) {
        m_showHomeClock = m_configDb.GetSettingInt("show_home_clock", 1) != 0;
        m_showEclClock  = m_configDb.GetSettingInt("show_ecl_clock",  1) != 0;
        std::string home = m_configDb.GetSetting("home_tz_iana", "");
        std::string ecl  = m_configDb.GetSetting("ecl_tz_iana",  "");
        if (!home.empty()) m_homeTzIana = home;
        if (!ecl.empty())  m_eclTzIana  = ecl;

        try { m_obsLat = std::stof(m_configDb.GetSetting("obs_lat", "0")); } catch (...) {}
        try { m_obsLon = std::stof(m_configDb.GetSetting("obs_lon", "0")); } catch (...) {}
        m_obsAltM = m_configDb.GetSettingInt("obs_alt_m", 0);
        SyncDecimalToDms();

        // Load cached IQP API key (may be newer than compile-time default)
        std::string savedKey = m_configDb.GetSetting("iqp_api_key", "");
        if (!savedKey.empty()) SetApiKey(savedKey);

        LogLine(std::format("Config DB: home={}  ecl={}  obs={:.4f},{:.4f} {}m",
                            m_homeTzIana, m_eclTzIana, m_obsLat, m_obsLon, m_obsAltM));

        // Migration: create timeline tables if they don't exist yet
        m_configDb.Exec(R"SQL(
            CREATE TABLE IF NOT EXISTS tl_tracks (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                sort_order INTEGER NOT NULL DEFAULT 0,
                type       TEXT    NOT NULL DEFAULT 'camera',
                camera_id  TEXT    NOT NULL DEFAULT '',
                label      TEXT    NOT NULL DEFAULT ''
            );
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
                snap_to_prev  INTEGER NOT NULL DEFAULT 0
            );
        )SQL");

        // Migration: create named snapshot tables if they don't exist yet
        m_configDb.CreateSnapshotTables();

        // Bracket calibration — create tables, seed built-in data, warm cache
        m_configDb.CreateCalibTables();
        SeedBuiltinCalib();
        LoadCalibCache();

        // Load saved timeline (overrides InitTracks defaults when data exists)
        auto saved = m_configDb.LoadTimeline();
        if (!saved.empty()) {
            m_tracks = std::move(saved);
            LogLine(std::format("Timeline loaded: {} tracks from DB", m_tracks.size()));
        }

        // Seed calibration snapshot once (idempotent)
        CreateCalibrationSnapshot();
    } else {
        LogLine("WARNING: cannot open TotalControlConfig.db — settings not persisted");
    }

    // ── TotalControlData.db — reference data (read-only, optional) ────────
    std::wstring dataPath = dir + L"\\TotalControlData.db";
    if (std::filesystem::exists(dataPath)) {
        if (m_dataDb.OpenReadOnly(dataPath)) {
            m_tzList   = m_dataDb.LoadTimezones();
            m_eclipses = m_dataDb.LoadEclipses();
            LogLine(std::format("Data DB: {} timezones, {} eclipses loaded",
                                m_tzList.size(), m_eclipses.size()));
        } else {
            LogLine("WARNING: TotalControlData.db found but could not be opened");
        }
    } else {
        LogLine("Data DB: TotalControlData.db not found — skipped");
    }

    // Fallback: if DB had no timezones, use hardcoded list
    if (m_tzList.empty()) {
        m_tzList = TzFallbackList();
        LogLine(std::format("Timezones: using fallback ({} entries)", m_tzList.size()));
    }

    // Default eclipse: nearest future (UTC date comparison)
    if (!m_eclipses.empty()) {
        SYSTEMTIME st{};
        GetSystemTime(&st);
        int today = st.wYear * 10000 + st.wMonth * 100 + st.wDay;
        m_eclipseIdx = static_cast<int>(m_eclipses.size()) - 1;
        for (int i = 0; i < static_cast<int>(m_eclipses.size()); ++i) {
            if (m_eclipses[i].DateInt() >= today) { m_eclipseIdx = i; break; }
        }
        const auto& e = m_eclipses[m_eclipseIdx];
        LogLine(std::format("Default eclipse: {}-{:02d}-{:02d} {}",
                            e.year, e.month, e.day, e.type));
    }

    InitTracks();
}

App::~App() {
    // Stop sequencer first so it releases the pipe before we quit
    if (m_seqRun.load()) StopSeqThread();
    m_guiSeqMode.store(GuiSeqMode::Idle);

    StopStatusThread();

    if (m_iqpThread.joinable()) m_iqpThread.join();

    if (m_pipe.GetState() == PipeClient::State::Connected) {
        (void)m_pipe.Send("{\"cmd\":\"quit\"}");
        m_pipe.Disconnect();
    }
    LogLine("=== TotalControlGUI exit ===");
}

void App::ApplyStyleDark() {
    ImGui::StyleColorsDark();   // baseline reset
    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text]                      = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]              = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    c[ImGuiCol_WindowBg]                  = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);
    c[ImGuiCol_ChildBg]                   = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg]                   = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
    c[ImGuiCol_Border]                    = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    c[ImGuiCol_BorderShadow]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]                   = ImVec4(0.16f, 0.29f, 0.48f, 0.54f);
    c[ImGuiCol_FrameBgHovered]            = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
    c[ImGuiCol_FrameBgActive]             = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    c[ImGuiCol_TitleBg]                   = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
    c[ImGuiCol_TitleBgActive]             = ImVec4(0.16f, 0.29f, 0.48f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]          = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    c[ImGuiCol_MenuBarBg]                 = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    c[ImGuiCol_ScrollbarBg]               = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    c[ImGuiCol_ScrollbarGrab]             = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]      = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]       = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    c[ImGuiCol_CheckMark]                 = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_SliderGrab]                = ImVec4(0.24f, 0.52f, 0.88f, 1.00f);
    c[ImGuiCol_SliderGrabActive]          = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_Button]                    = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
    c[ImGuiCol_ButtonHovered]             = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_ButtonActive]              = ImVec4(0.06f, 0.53f, 0.98f, 1.00f);
    c[ImGuiCol_Header]                    = ImVec4(0.26f, 0.59f, 0.98f, 0.31f);
    c[ImGuiCol_HeaderHovered]             = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    c[ImGuiCol_HeaderActive]              = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_Separator]                 = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    c[ImGuiCol_SeparatorHovered]          = ImVec4(0.10f, 0.40f, 0.75f, 0.78f);
    c[ImGuiCol_SeparatorActive]           = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
    c[ImGuiCol_ResizeGrip]                = ImVec4(0.26f, 0.59f, 0.98f, 0.20f);
    c[ImGuiCol_ResizeGripHovered]         = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    c[ImGuiCol_ResizeGripActive]          = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
    c[ImGuiCol_TabHovered]                = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    c[ImGuiCol_Tab]                       = ImVec4(0.18f, 0.35f, 0.58f, 0.86f);
    c[ImGuiCol_TabSelected]               = ImVec4(0.20f, 0.41f, 0.68f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]       = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_TabDimmed]                 = ImVec4(0.07f, 0.10f, 0.15f, 0.97f);
    c[ImGuiCol_TabDimmedSelected]         = ImVec4(0.14f, 0.26f, 0.42f, 1.00f);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.50f, 0.50f, 0.50f, 0.00f);
    c[ImGuiCol_PlotLines]                 = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]          = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    c[ImGuiCol_PlotHistogram]             = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered]      = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TableHeaderBg]             = ImVec4(0.19f, 0.19f, 0.20f, 1.00f);
    c[ImGuiCol_TableBorderStrong]         = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
    c[ImGuiCol_TableBorderLight]          = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
    c[ImGuiCol_TableRowBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]             = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    c[ImGuiCol_TextLink]                  = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_TextSelectedBg]            = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
    c[ImGuiCol_DragDropTarget]            = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    c[ImGuiCol_NavCursor]                 = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight]     = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]         = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]          = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
}

void App::OnInit() {
    ImGuiIO& io = ImGui::GetIO();
    m_fontMono  = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 15.0f);
    m_fontLarge = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 24.0f);
    if (!m_fontMono)  m_fontMono  = io.FontDefault;
    if (!m_fontLarge) m_fontLarge = io.FontDefault;
    io.Fonts->Build();

    ApplyStyleDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding   = 0.0f;
    style.FrameRounding    = 3.0f;
    style.FramePadding     = ImVec2(8, 5);
    style.ItemSpacing      = ImVec2(8, 6);
    style.WindowBorderSize = 1.0f;
    style.WindowPadding    = ImVec2(12, 12);

    StartStatusThread();
}

// ─── Extra clock row ──────────────────────────────────────────────────────────
//
// Panel layout (content width ~176px):
//
//   HH:MM:SS            [=]   ← coloured time  +  gear button right-aligned
//   [✓]  CODE                 ← checkbox  +  IANA code tag

void App::RenderExtraClock(const char* clockId, const char* popupId,
                            bool& show, std::string& tzIana) {
    // Find current list entry for this IANA name
    int curIdx = TzFindByIana(m_tzList, tzIana);
    const TzEntry& cur = m_tzList[curIdx];

    ImGuiStyle& style = ImGui::GetStyle();

    // ── row 1: time + gear button ─────────────────────────────────────────
    ImGui::PushFont(m_fontMono);

    char timeBuf[12];
    FormatLocalHms(UtcNowMs(), tzIana, timeBuf, sizeof(timeBuf));

    static const ImVec4 kColorHome{ 0.45f, 0.75f, 1.00f, 1.0f };  // soft blue
    static const ImVec4 kColorEcl { 0.40f, 1.00f, 0.80f, 1.0f };  // cyan-green
    ImVec4 timeColor = (clockId[0] == 'H') ? kColorHome : kColorEcl;

    if (!show) {
        ImVec4 dim = timeColor;  dim.w = 0.35f;
        ImGui::TextColored(dim, "%s", timeBuf);
    } else {
        ImGui::TextColored(timeColor, "%s", timeBuf);
    }

    // Gear button — right-aligned on the same line
    {
        char gearId[48];
        snprintf(gearId, sizeof(gearId), "=##gear_%s", popupId);
        float gearW = ImGui::CalcTextSize("=").x
                    + style.FramePadding.x * 2.0f
                    + style.ItemSpacing.x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - gearW + style.ItemSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.18f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.32f, 0.48f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.55f, 0.65f, 0.85f, 1.0f));
        if (ImGui::SmallButton(gearId))
            ImGui::OpenPopup(popupId);
        ImGui::PopStyleColor(3);
    }

    ImGui::PopFont();

    // ── row 2: checkbox + full label (e.g. "Home Time Zone") ────────────────
    bool prevShow = show;
    ImGui::Checkbox(clockId, &show);
    if (show != prevShow) SaveClockSettings();

    // ── row 3: city name — indented to align with checkbox label ─────────
    {
        float indent = ImGui::GetFrameHeight()
                     + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::Indent(indent);
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(ImVec4(0.38f, 0.38f, 0.44f, 1.0f), "%s", cur.code.c_str());
        ImGui::PopFont();
        ImGui::Unindent(indent);
    }

    // ── timezone picker popup ─────────────────────────────────────────────
    ImGui::SetNextWindowSizeConstraints(ImVec2(420, 0), ImVec2(420, 520));
    if (ImGui::BeginPopup(popupId)) {
        ImGui::TextColored(ImVec4(0.55f, 0.65f, 0.85f, 1.0f), "Select timezone");
        ImGui::Separator();
        ImGui::Spacing();

        for (int i = 0; i < static_cast<int>(m_tzList.size()); ++i) {
            bool sel = (i == curIdx);
            char entry[80];
            snprintf(entry, sizeof(entry), "%-22s  %s",
                     m_tzList[i].code.c_str(),
                     m_tzList[i].iana.c_str());
            if (ImGui::Selectable(entry, sel)) {
                tzIana = m_tzList[i].iana;
                SaveClockSettings();
                ImGui::CloseCurrentPopup();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndPopup();
    }
}

// ─── Camera status rendering ──────────────────────────────────────────────────

void App::RenderCameraSection() {
    static const ImVec4 kGray   { 0.40f, 0.40f, 0.45f, 1.0f };
    static const ImVec4 kWhite  { 0.88f, 0.88f, 0.90f, 1.0f };
    static const ImVec4 kGreen  { 0.20f, 0.85f, 0.30f, 1.0f };
    static const ImVec4 kYellow { 0.95f, 0.80f, 0.10f, 1.0f };
    static const ImVec4 kOrange { 1.00f, 0.55f, 0.10f, 1.0f };
    static const ImVec4 kRed    { 0.90f, 0.20f, 0.18f, 1.0f };

    // Snapshot cameras under mutex — render thread must not hold mutex during drawing
    std::vector<CamStatus> cameras;
    {
        std::lock_guard lk(m_camerasMutex);
        cameras = m_cameras;
    }

    if (cameras.empty()) {
        ImGui::TextColored(kGray, "no cameras");
        return;
    }

    // Value column starts at fixed x offset within panel content
    const float kValX = 52.0f;

    auto Row = [&](const char* label, const char* value,
                   ImVec4 valCol = kWhite) {
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(kGray, "%s", label);
        ImGui::SameLine(kValX);
        ImGui::TextColored(valCol, "%s", value);
        ImGui::PopFont();
    };

    for (int ci = 0; ci < static_cast<int>(cameras.size()); ++ci) {
        const CamStatus& s = cameras[ci];

        if (ci > 0) ImGui::Spacing();

        // Camera header: model name or index fallback
        ImGui::PushFont(m_fontMono);
        const char* modelStr = s.model.empty() ? "Camera" : s.model.c_str();
        ImGui::TextColored(kWhite, "[%d] %s", ci + 1, modelStr);
        ImGui::PopFont();

        if (!s.valid) {
            ImGui::TextColored(kRed, "    disconnected");
            continue;
        }

        // Battery — coloured bar + percentage
        {
            ImVec4 batCol = kGreen;
            int bars = 4;
            std::string_view lvl = s.batteryLevel;
            if      (lvl.find("1/4")  != std::string_view::npos) { bars = 1; batCol = kRed;    }
            else if (lvl.find("1/2")  != std::string_view::npos) { bars = 2; batCol = kOrange; }
            else if (lvl.find("3/4")  != std::string_view::npos) { bars = 3; batCol = kYellow; }

            bool usb = lvl.find("usb") != std::string_view::npos;
            char barStr[8];
            for (int b = 0; b < 4; ++b) barStr[b] = (b < bars) ? '|' : '.';
            barStr[4] = '\0';

            char batBuf[24];
            snprintf(batBuf, sizeof(batBuf), "%s %d%%%s",
                     barStr, s.batteryPct, usb ? " USB" : "");
            Row("Batt", batBuf, batCol);
        }

        // Exposure
        Row("Mode",  s.mode.empty()  ? "?" : s.mode.c_str());
        Row("SS",    s.ss.empty()    ? "?" : s.ss.c_str());

        char isoBuf[12];
        snprintf(isoBuf, sizeof(isoBuf), "%d", s.iso);
        Row("ISO",   s.iso ? isoBuf : "?");
        Row("f/",    s.fnum.empty()  ? "?" : s.fnum.c_str());
        Row("Focus", s.focus.empty() ? "?" : s.focus.c_str());
        Row("Drive", s.drive.empty() ? "?" : s.drive.c_str());

        // Cards
        auto CardRow = [&](const char* lbl, const std::string& status, int remaining) {
            char buf[32];
            if (remaining >= 0)
                snprintf(buf, sizeof(buf), "%s  %d", status.c_str(), remaining);
            else
                snprintf(buf, sizeof(buf), "%s", status.c_str());
            ImVec4 col = (status == "OK") ? kGreen : kRed;
            Row(lbl, buf, col);
        };

        CardRow("C1", s.slot1Status.empty() ? "?" : s.slot1Status, s.slot1Remaining);
        CardRow("C2", s.slot2Status.empty() ? "?" : s.slot2Status, s.slot2Remaining);

        // Last shoot latency — full stack (SDK + USB + shutter + confirm)
        if (s.lastShotMs >= 0) {
            char shotBuf[16];
            snprintf(shotBuf, sizeof(shotBuf), "%d ms", s.lastShotMs);
            ImVec4 shotCol = s.lastShotMs < 500  ? kGreen
                           : s.lastShotMs < 1500 ? kYellow
                                                 : kRed;
            Row("Shot", shotBuf, shotCol);
        }
    }
}

// ─── Eclipse time helpers ─────────────────────────────────────────────────────

// Returns UTC ms (Unix epoch) of greatest eclipse.
// td_ge is Terrestrial Time "HH:MM:SS[.s]"; dt = TT−UTC in seconds.
// Returns INT64_MIN on parse failure.
static int64_t EclipseGeUtcMs(const EclipseEntry& e) {
    using namespace std::chrono;
    int hh = 0, mm = 0;
    float fs = 0.f;
    if (sscanf_s(e.timeGe.c_str(), "%d:%d:%f", &hh, &mm, &fs) < 2)
        return INT64_MIN;
    auto ymd = year_month_day{ year(e.year), month(static_cast<unsigned>(e.month)),
                               day(static_cast<unsigned>(e.day)) };
    auto dp    = sys_days{ ymd };
    int64_t datMs = duration_cast<milliseconds>(dp.time_since_epoch()).count();
    int64_t timMs = (int64_t(hh) * 3600 + int64_t(mm) * 60) * 1000
                  + static_cast<int64_t>(fs * 1000.f);
    int64_t dtMs  = static_cast<int64_t>(e.dt * 1000.f);  // TT − UTC → subtract
    return datMs + timMs - dtMs;
}

// Formats a signed millisecond duration as "Xd HH:MM:SS.mmm" (negative = past).
static void FormatCountdown(int64_t diffMs, char* buf, int len) {
    bool neg   = diffMs < 0;
    int64_t a  = neg ? -diffMs : diffMs;
    int days   = static_cast<int>(a / 86400000LL);
    int hh     = static_cast<int>((a % 86400000LL) / 3600000LL);
    int mm     = static_cast<int>((a % 3600000LL)  / 60000LL);
    int ss     = static_cast<int>((a % 60000LL)    / 1000LL);
    int ms     = static_cast<int>(a % 1000LL);
    if (neg)
        snprintf(buf, len, "-%dd %02d:%02d:%02d.%03d", days, hh, mm, ss, ms);
    else
        snprintf(buf, len, "%dd %02d:%02d:%02d.%03d",  days, hh, mm, ss, ms);
}

// ─── Eclipse selector rendering ───────────────────────────────────────────────

void App::RenderEclipseSection() {
    static const ImVec4 kGray  { 0.40f, 0.40f, 0.45f, 1.0f };
    static const ImVec4 kWhite { 0.88f, 0.88f, 0.90f, 1.0f };

    if (m_eclipses.empty()) {
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(kGray, "no data");
        ImGui::PopFont();
        return;
    }

    // Combo preview: YYYY-MM-DD  T
    char preview[20] = "---";
    if (m_eclipseIdx >= 0 && m_eclipseIdx < static_cast<int>(m_eclipses.size())) {
        const auto& e = m_eclipses[m_eclipseIdx];
        char tc = e.type.empty() ? '?' : e.type[0];
        snprintf(preview, sizeof(preview), "%04d-%02d-%02d  %c",
                 e.year, e.month, e.day, tc);
    }

    ImGui::PushFont(m_fontMono);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##ecl_sel", preview, ImGuiComboFlags_HeightLarge)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_eclipses.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& e = m_eclipses[i];
                char tc = e.type.empty() ? '?' : e.type[0];
                char lbl[20];
                snprintf(lbl, sizeof(lbl), "%04d-%02d-%02d  %c",
                         e.year, e.month, e.day, tc);
                bool sel = (i == m_eclipseIdx);
                if (ImGui::Selectable(lbl, sel)) {
                    m_eclipseIdx = i;
                    TriggerIqpFetch();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopFont();

    // Details for selected eclipse
    if (m_eclipseIdx < 0 || m_eclipseIdx >= static_cast<int>(m_eclipses.size()))
        return;

    const auto& e = m_eclipses[m_eclipseIdx];
    char tc = e.type.empty() ? '?' : e.type[0];

    const char* typeName = "Unknown";
    ImVec4 typeCol = kWhite;
    switch (tc) {
    case 'T': typeName = "Total";   typeCol = ImVec4{0.95f, 0.80f, 0.20f, 1.0f}; break;
    case 'A': typeName = "Annular"; typeCol = ImVec4{0.85f, 0.50f, 0.15f, 1.0f}; break;
    case 'H': typeName = "Hybrid";  typeCol = ImVec4{0.80f, 0.75f, 0.20f, 1.0f}; break;
    case 'P': typeName = "Partial"; typeCol = kGray;                               break;
    }

    const float kValX = 42.0f;

    ImGui::PushFont(m_fontMono);

    // Type + duration
    ImGui::TextColored(typeCol, "%s", typeName);
    if (!e.duration.empty()) {
        ImGui::SameLine(kValX);
        ImGui::TextColored(kGray, "%s", e.duration.c_str());
    }

    // Location of maximum
    char latBuf[10], lonBuf[10];
    snprintf(latBuf, sizeof(latBuf), "%.1f%c", fabsf(e.latGe), e.latGe >= 0.f ? 'N' : 'S');
    snprintf(lonBuf, sizeof(lonBuf), "%.1f%c", fabsf(e.lonGe), e.lonGe >= 0.f ? 'E' : 'W');
    ImGui::TextColored(kGray, "Max");
    ImGui::SameLine(kValX);
    char coordBuf[24];
    snprintf(coordBuf, sizeof(coordBuf), "%s %s", latBuf, lonBuf);
    ImGui::TextColored(kWhite, "%s", coordBuf);

    ImGui::PopFont();

    // ── Observer location (DMS) ───────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushFont(m_fontMono);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.f, 3.f));

    // Helper: render one DMS row (label on its own line, fields on next line).
    // posLabel/negLabel: "N"/"S" or "E"/"W"
    auto DmsRow = [&](const char* label,
                      DmsCoord&   dms,
                      int         maxDeg,
                      const char* posLabel,
                      const char* negLabel,
                      const char* idD, const char* idM, const char* idS) {
        ImGui::TextColored(kGray, "%s", label);
        bool t = false;

        ImGui::SetNextItemWidth(38);
        ImGui::InputInt(idD, &dms.deg, 0);
        t |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::SameLine(0, 1); ImGui::TextColored(kGray, "°");
        ImGui::SameLine(0, 2); ImGui::SetNextItemWidth(26);
        ImGui::InputInt(idM, &dms.min, 0);
        t |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::SameLine(0, 1); ImGui::TextColored(kGray, "'");
        ImGui::SameLine(0, 2); ImGui::SetNextItemWidth(52);
        ImGui::InputFloat(idS, &dms.sec, 0.f, 0.f, "%.2f");
        t |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::SameLine(0, 1); ImGui::TextColored(kGray, "\"");
        ImGui::SameLine(0, 2);
        if (ImGui::Button(dms.pos ? posLabel : negLabel, ImVec2(22.f, 0.f))) {
            dms.pos = !dms.pos; t = true;
        }

        if (t) {
            dms.deg = std::clamp(dms.deg, 0, maxDeg);
            dms.min = std::clamp(dms.min, 0, 59);
            dms.sec = std::clamp(dms.sec, 0.f, 59.99f);
            SyncDmsToDecimal();
            SaveObserverSettings();
        }
    };

    DmsRow("Latitude",  m_latDms, 90,  "N##latn", "S##latn",
           "##latd", "##latm", "##lats");
    DmsRow("Longitude", m_lonDms, 180, "E##lone", "W##lone",
           "##lond", "##lonm", "##lons");

    // Altitude
    ImGui::TextColored(kGray, "Altitude");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputInt("##obs_alt", &m_obsAltM, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveObserverSettings();

    ImGui::Spacing();
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

// ─── Contact times table ─────────────────────────────────────────────────────

static int LocalUtcOffsetHours(const std::string& iana, int year, int month, int day) {
    using namespace std::chrono;
    try {
        const time_zone* tz = locate_zone(iana);
        auto ymd = year_month_day{ std::chrono::year(year),
                                   std::chrono::month(static_cast<unsigned>(month)),
                                   std::chrono::day(static_cast<unsigned>(day)) };
        sys_info info = tz->get_info(sys_days{ymd} + hours{12});
        return static_cast<int>(info.offset.count() / 3600);
    } catch (...) { return 0; }
}

void App::RenderContactTimesSection() {
    static const ImVec4 kGray  {0.40f, 0.40f, 0.45f, 1.0f};
    static const ImVec4 kWhite {0.88f, 0.88f, 0.90f, 1.0f};
    static const ImVec4 kDim   {0.35f, 0.35f, 0.40f, 1.0f};
    static const ImVec4 kGold  {0.95f, 0.80f, 0.20f, 1.0f};

    // Compute local timezone offset for eclipse date
    int offH = 0;
    if (m_eclipseIdx >= 0 && m_eclipseIdx < static_cast<int>(m_eclipses.size())) {
        const auto& ec = m_eclipses[m_eclipseIdx];
        offH = LocalUtcOffsetHours(m_eclTzIana, ec.year, ec.month, ec.day);
    }
    char offLabel[10];
    if (offH > 0)       snprintf(offLabel, sizeof(offLabel), "UTC+%d", offH);
    else if (offH < 0)  snprintf(offLabel, sizeof(offLabel), "UTC%d",  offH);
    else                snprintf(offLabel, sizeof(offLabel), "UTC");

    // Column offsets within content area
    const float kT1  = 26.f;  // UTC time column
    const float kT2  = 98.f;  // local time column

    auto fmtUtc = [](int64_t ms, char* buf, int len) {
        if (ms < 0) { snprintf(buf, len, "--:--:--"); return; }
        int64_t s = ms / 1000;
        snprintf(buf, len, "%02d:%02d:%02d",
                 (int)((s/3600)%24), (int)((s/60)%60), (int)(s%60));
    };
    auto fmtLoc = [&](int64_t ms, char* buf, int len) {
        if (ms < 0) { snprintf(buf, len, "--:--:--"); return; }
        // shift by offset
        int64_t shifted = ms + int64_t(offH) * 3600000LL;
        int64_t s = shifted / 1000;
        snprintf(buf, len, "%02d:%02d:%02d",
                 (int)((s/3600)%24), (int)((s/60)%60), (int)(s%60));
    };

    struct EventRow { const char* lbl; int64_t ms; };

    auto renderSection = [&](const char* srcLabel, const ContactTimes& ct,
                              bool loading, ImVec4 srcCol) {
        // header line
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(srcCol,  "%-3s", srcLabel);
        ImGui::SameLine(kT1);
        ImGui::TextColored(kGray, "UTC");
        ImGui::SameLine(kT2);
        ImGui::TextColored(kGray, "%s", offLabel);
        ImGui::PopFont();

        ImGui::PushFont(m_fontMono);
        if (loading) {
            ImGui::SameLine(); // dummy to keep spacing
            ImGui::NewLine();
            ImGui::TextColored(kDim, "  loading...");
            ImGui::PopFont();
            return;
        }
        if (!ct.apiOk && !ct.valid) {
            ImGui::TextColored(kDim, "  ---");
            ImGui::PopFont();
            return;
        }
        if (!ct.valid) {
            ImGui::TextColored(kDim, "  no eclipse");
            ImGui::PopFont();
            return;
        }

        EventRow rows[] = {
            {"C1",  ct.c1Ms},
            {"C2",  ct.c2Ms},
            {"Max", ct.maxMs},
            {"C3",  ct.c3Ms},
            {"C4",  ct.c4Ms},
        };
        for (auto& r : rows) {
            if (r.ms < 0) continue;
            char t1[12], t2[12];
            fmtUtc(r.ms, t1, sizeof(t1));
            fmtLoc(r.ms, t2, sizeof(t2));
            ImGui::TextColored(kGray,  "%s", r.lbl);
            ImGui::SameLine(kT1);
            ImGui::TextColored(kWhite, "%s", t1);
            ImGui::SameLine(kT2);
            ImGui::TextColored(kDim,   "%s", t2);
        }
        ImGui::PopFont();
    };

    int iqpSt = m_iqpState.load();
    ContactTimes iqpCt;
    { std::lock_guard lk(m_iqpMutex); iqpCt = m_contacts; }

    renderSection("IQP", iqpCt, iqpSt == 1,
                  ImVec4{0.45f, 0.75f, 1.00f, 1.0f});   // blue
    ImGui::Spacing();
    renderSection("BE",  m_beResult, false,
                  ImVec4{0.40f, 1.00f, 0.60f, 1.0f});   // green
}

// ─── Menu actions ────────────────────────────────────────────────────────────

void App::NewTimeline() {
    m_tracks.clear();
    InitTracks();
    m_selTrack = -1; m_selBlock = -1;
    m_tlDragging = false; m_tlDragTrack = -1; m_tlDragBlock = -1;
    m_tlViewStart = -1;   // will re-init from contacts on next frame
    m_tlViewEnd   = -1;
    m_tlDirty = true;
    LogLine("Timeline: New");
}

void App::DeleteSelectedBlock() {
    if (m_selTrack < 0 || m_selTrack >= (int)m_tracks.size()) return;
    auto& tr = m_tracks[m_selTrack];
    if (m_selBlock < 0 || m_selBlock >= (int)tr.blocks.size()) return;
    tr.blocks.erase(tr.blocks.begin() + m_selBlock);
    m_selBlock = -1; m_selTrack = -1;
    m_tlDirty = true;
}

void App::DuplicateSelectedBlock() {
    if (m_selTrack < 0 || m_selTrack >= (int)m_tracks.size()) return;
    auto& tr = m_tracks[m_selTrack];
    if (m_selBlock < 0 || m_selBlock >= (int)tr.blocks.size()) return;
    TLBlock copy = tr.blocks[m_selBlock];
    copy.id   = -1;
    copy.atMs += BlockDurMs(copy) + 500; // offset by own duration + 0.5s gap
    tr.blocks.push_back(copy);
    std::sort(tr.blocks.begin(), tr.blocks.end(),
        [](const TLBlock& a, const TLBlock& b){ return a.atMs < b.atMs; });
    // Select the duplicate
    for (int i = 0; i < (int)tr.blocks.size(); ++i) {
        if (tr.blocks[i].atMs == copy.atMs && tr.blocks[i].type == copy.type) {
            m_selBlock = i; break;
        }
    }
    m_tlDirty = true;
}

// Convert UTC ms → ISO 8601 "YYYY-MM-DDTHH:MM:SS.mmmZ"
static std::string UtcMsToIso(int64_t ms) {
    if (ms < 0) return {};
    time_t tt = (time_t)(ms / 1000);
    tm utc{};
    gmtime_s(&utc, &tt);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             utc.tm_year+1900, utc.tm_mon+1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec, (int)(ms%1000));
    return buf;
}

// ─── Named snapshots ─────────────────────────────────────────────────────────

static std::string FormatUtcIso8601(int64_t ms);  // defined in Export section below

// ─── Bracket calibration helpers ─────────────────────────────────────────────

void App::LoadCalibCache() {
    assert(m_configDb.IsOpen());
    m_calibCache.clear();
    auto models = m_configDb.LoadCalibModels();
    assert(models.size() <= 100);  // bounded: realistic number of camera models
    for (const auto& model : models) {
        auto entries = m_configDb.LoadCalibData(model);
        assert(entries.size() <= 1024);
        auto& cm = m_calibCache[model];
        for (const auto& e : entries)
            cm[{e.count, e.ev}] = e.latMaxMs + 10;
    }
    LogLine(std::format("Calibration: {} model(s) loaded into cache",
                        m_calibCache.size()));
}

void App::SeedBuiltinCalib() {
    assert(m_configDb.IsOpen());
    static constexpr char kModel[] = "ILCE-7RM4A";
    if (!m_configDb.LoadCalibData(kModel).empty()) return; // already seeded

    // Measured 2026-06-07: SS=1/100, ISO=100, f/8.0, 3 reps each.
    // lat_max_ms / lat_avg_ms / lat_min_ms
    struct Row { int cnt; const char* ev; int mx; int av; int mn; };
    static constexpr Row kRows[] = {
        {3,"0.3ev", 485,483,481}, {3,"0.5ev", 508,496,472},
        {3,"0.7ev", 530,506,478}, {3,"1.0ev", 518,490,469},
        {3,"2.0ev", 528,521,517}, {3,"3.0ev", 571,554,536},
        {5,"0.3ev", 725,706,677}, {5,"0.5ev", 688,686,683},
        {5,"0.7ev", 725,710,701}, {5,"1.0ev", 746,740,732},
        {5,"2.0ev", 892,886,878}, {5,"3.0ev",1412,1383,1364},
        {9,"0.3ev",1157,1150,1144},{9,"0.5ev",1177,1173,1170},
        {9,"0.7ev",1248,1206,1179},{9,"1.0ev",1374,1367,1359},
    };
    static constexpr int kNumRows = static_cast<int>(std::size(kRows));

    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ui; ui.LowPart = ft.dwLowDateTime; ui.HighPart = ft.dwHighDateTime;
    int64_t nowMs = static_cast<int64_t>(ui.QuadPart / 10000) - 11644473600000LL;

    std::vector<BktCalibEntry> entries;
    entries.reserve(kNumRows);
    for (const auto& r : kRows) {
        BktCalibEntry e;
        e.camModel  = kModel;
        e.count     = r.cnt;
        e.ev        = r.ev;
        e.latMaxMs  = r.mx;
        e.latAvgMs  = r.av;
        e.latMinMs  = r.mn;
        e.reps      = 3;
        e.ss        = "1/100";
        e.createdMs = nowMs;
        entries.push_back(std::move(e));
    }
    m_configDb.SaveCalibData(entries);
    LogLine(std::format("Calibration: seeded {} entries for {}", kNumRows, kModel));
}

void App::SaveCalibFromBuf(const std::string& camModel) {
    assert(!m_seqCalibBuf.empty());
    assert(!camModel.empty());

    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ui; ui.LowPart = ft.dwLowDateTime; ui.HighPart = ft.dwHighDateTime;
    int64_t nowMs = static_cast<int64_t>(ui.QuadPart / 10000) - 11644473600000LL;

    // Group samples by (count, ev) → collect latMs values
    std::map<std::pair<int,std::string>, std::vector<int>> groups;
    assert(m_seqCalibBuf.size() <= 4096);  // bounded: calibration sequence is finite
    for (const auto& s : m_seqCalibBuf)
        groups[{s.count, s.ev}].push_back(s.latMs);

    std::vector<BktCalibEntry> entries;
    entries.reserve(groups.size());
    for (const auto& [key, lats] : groups) {
        assert(!lats.empty());
        BktCalibEntry e;
        e.camModel  = camModel;
        e.count     = key.first;
        e.ev        = key.second;
        e.latMaxMs  = *std::max_element(lats.begin(), lats.end());
        e.latMinMs  = *std::min_element(lats.begin(), lats.end());
        int sum = 0;
        for (int v : lats) sum += v;
        e.latAvgMs  = sum / static_cast<int>(lats.size());
        e.reps      = static_cast<int>(lats.size());
        e.ss        = "1/100";
        e.createdMs = nowMs;
        entries.push_back(std::move(e));
    }
    m_configDb.SaveCalibData(entries);
    LoadCalibCache();
    m_seqCalibBuf.clear();
    m_lastResult = std::format("Calibration saved: {} combos for {}",
                               entries.size(), camModel);
    LogLine(m_lastResult);
}

// Builds the bracket-calibration snapshot once and saves it to the config DB.
// 16 bracket combinations × 3 repetitions = 48 blocks, ~6 min total.
// Base time: 2026-08-12T22:00:00.000Z (well after totality).
void App::CreateCalibrationSnapshot() {
    assert(m_configDb.IsOpen());
    static constexpr char kName[] = "Bracket Calibration";
    if (m_configDb.SnapshotExists(kName)) return;

    // 2026-08-12T22:00:00.000Z  (verified: 1786492800 + 22×3600 = 1786572000s)
    static constexpr int64_t kBaseMs        = 1786572000000LL;
    static constexpr int     kRepIntervalMs = 4000;   // 4 s between reps (same drive code)
    static constexpr int     kGroupGapMs    = 15000;  // 15 s between groups (drive change)
    static constexpr int     kReps          = 3;

    struct Combo { int n; const char* ev; };
    // All 16 cont-bracket combinations supported by ILCE-series cameras.
    // 3-shot: 0.3/0.5/0.7/1.0/2.0/3.0 ev
    // 5-shot: 0.3/0.5/0.7/1.0/2.0/3.0 ev
    // 9-shot: 0.3/0.5/0.7/1.0 ev  (2ev/3ev not available at 9-shot)
    static constexpr Combo kCombos[] = {
        {3,"0.3ev"},{3,"0.5ev"},{3,"0.7ev"},{3,"1.0ev"},{3,"2.0ev"},{3,"3.0ev"},
        {5,"0.3ev"},{5,"0.5ev"},{5,"0.7ev"},{5,"1.0ev"},{5,"2.0ev"},{5,"3.0ev"},
        {9,"0.3ev"},{9,"0.5ev"},{9,"0.7ev"},{9,"1.0ev"},
    };
    static constexpr int kNumCombos = static_cast<int>(std::size(kCombos));

    TLTrack track;
    track.type     = "camera";
    track.cameraId = "";          // model-agnostic: targets camera[0] at runtime
    track.label    = "Calibration Cam0";

    int64_t tMs = kBaseMs;
    for (int g = 0; g < kNumCombos; ++g) {
        const Combo& c = kCombos[g];
        for (int r = 0; r < kReps; ++r) {
            TLBlock blk;
            blk.type       = BlockType::Bracket;
            blk.atMs       = tMs;
            blk.ss         = "1/100";
            blk.iso        = 100;
            blk.fstop      = "8.0";
            blk.count      = c.n;
            blk.ev         = c.ev;
            blk.burstDrive = "cont-hi-plus";
            blk.burstDurMs = 3000;
            blk.audioDurMs = 0;
            blk.snapToPrev = false;
            blk.label      = std::format("{}x{} r{}", c.n, c.ev, r + 1);
            track.blocks.push_back(blk);
            tMs += (r < kReps - 1) ? kRepIntervalMs : kGroupGapMs;
        }
    }

    assert(track.blocks.size() == static_cast<size_t>(kNumCombos * kReps));
    std::vector<TLTrack> tracks = { std::move(track) };
    m_configDb.SaveSnapshot(kName, tracks);
    LogLine(std::format("Snapshot '{}' created: {} blocks, base {}",
                        kName, kNumCombos * kReps,
                        FormatUtcIso8601(kBaseMs)));
}

void App::RenderSnapshotModal() {
    assert(m_configDb.IsOpen());

    // ── Open Timeline modal ────────────────────────────────────────────────
    if (m_showSnapOpen) {
        ImGui::OpenPopup("Open Timeline##snap");
        m_snapList = m_configDb.LoadSnapshotList();
        m_snapSel  = -1;
        m_showSnapOpen = false;
    }
    if (ImGui::BeginPopupModal("Open Timeline##snap",
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Select snapshot to load onto the timeline:");
        ImGui::Separator();

        ImGui::BeginChild("snaplist", ImVec2(420, 200), true);
        assert(m_snapList.size() <= 1024);  // bounded: no realistic user has >1024 snapshots
        for (int i = 0; i < static_cast<int>(m_snapList.size()); ++i) {
            const auto& s = m_snapList[i];
            char ts[32];
            FormatUtcHms(s.createdMs, ts, sizeof(ts));
            std::string label = std::format("{} — {}", s.name, ts);
            bool sel = (m_snapSel == i);
            if (ImGui::Selectable(label.c_str(), sel))
                m_snapSel = i;
        }
        ImGui::EndChild();

        bool canLoad = (m_snapSel >= 0 &&
                        m_snapSel < static_cast<int>(m_snapList.size()));
        if (!canLoad) ImGui::BeginDisabled();
        if (ImGui::Button("Load", ImVec2(100, 0))) {
            auto loaded = m_configDb.LoadSnapshot(m_snapList[m_snapSel].id);
            int totalBlocks = 0;
            for (const auto& tr : loaded) totalBlocks += (int)tr.blocks.size();
            LogLine(std::format("LoadSnapshot: {} tracks, {} blocks",
                                loaded.size(), totalBlocks));
            if (!loaded.empty()) {
                m_tracks  = std::move(loaded);
                m_tlDirty = true;
                m_selTrack = m_selBlock = -1;

                // Auto-fit view to loaded blocks so they are always visible,
                // regardless of where their at_ms falls on the timeline.
                int64_t minMs = INT64_MAX, maxMs = INT64_MIN;
                for (const auto& tr : m_tracks)
                    for (const auto& b : tr.blocks) {
                        if (b.atMs < 0) continue;
                        minMs = std::min(minMs, b.atMs);
                        maxMs = std::max(maxMs, b.atMs + BlockDurMs(b));
                    }
                if (minMs < maxMs) {
                    int64_t pad   = std::max(30000LL, (maxMs - minMs) / 5LL);
                    m_tlViewStart = minMs - pad;
                    m_tlViewEnd   = maxMs + pad;
                }

                m_lastResult = std::format("Loaded snapshot: {} ({} blocks)",
                                           m_snapList[m_snapSel].name, totalBlocks);
                LogLine(m_lastResult);
            }
            ImGui::CloseCurrentPopup();
        }
        if (!canLoad) ImGui::EndDisabled();

        ImGui::SameLine();
        // Delete — only for non-built-in snapshots (allow delete of any)
        if (!canLoad) ImGui::BeginDisabled();
        if (ImGui::Button("Delete", ImVec2(100, 0))) {
            m_configDb.DeleteSnapshot(m_snapList[m_snapSel].id);
            m_snapList = m_configDb.LoadSnapshotList();
            m_snapSel  = -1;
        }
        if (!canLoad) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    // ── Save Timeline As modal ─────────────────────────────────────────────
    if (m_showSnapSaveAs) {
        ImGui::OpenPopup("Save Timeline As##snap");
        m_snapNameBuf[0] = '\0';
        m_showSnapSaveAs = false;
    }
    if (ImGui::BeginPopupModal("Save Timeline As##snap",
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Snapshot name:");
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("##snapname", m_snapNameBuf, sizeof(m_snapNameBuf));

        bool canSave = (m_snapNameBuf[0] != '\0' && !m_tracks.empty());
        if (!canSave) ImGui::BeginDisabled();
        if (ImGui::Button("Save", ImVec2(100, 0))) {
            m_configDb.SaveSnapshot(std::string(m_snapNameBuf), m_tracks);
            m_lastResult = std::format("Snapshot saved: {}", m_snapNameBuf);
            LogLine(m_lastResult);
            ImGui::CloseCurrentPopup();
        }
        if (!canSave) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
}

// Formats UTC ms as ISO 8601 with milliseconds: "2026-08-12T20:29:02.100Z"
static std::string FormatUtcIso8601(int64_t ms) {
    using namespace std::chrono;
    auto tp  = system_clock::time_point{milliseconds{ms}};
    auto sec = floor<seconds>(tp);
    int  msec = static_cast<int>(ms % 1000);
    if (msec < 0) msec += 1000;
    return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03d}Z", sec, msec);
}

void App::ExportTimelineJson() {
    assert(!m_tracks.empty());  // must have at least one track before exporting
    // File save dialog
    wchar_t path[MAX_PATH] = L"eclipse_sequence.json";
    OPENFILENAMEW ofn{};
    ofn.lStructSize    = sizeof(ofn);
    ofn.lpstrFilter    = L"JSON\0*.json\0All\0*.*\0";
    ofn.lpstrFile      = path;
    ofn.nMaxFile       = MAX_PATH;
    ofn.lpstrDefExt    = L"json";
    ofn.Flags          = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    // Collect all camera blocks, sorted by time
    std::vector<const TLBlock*> steps;
    for (auto& tr : m_tracks) {
        if (!tr.IsCamera()) continue;
        for (auto& b : tr.blocks)
            if (b.atMs >= 0) steps.push_back(&b);
    }
    std::sort(steps.begin(), steps.end(),
        [](const TLBlock* a, const TLBlock* b){ return a->atMs < b->atMs; });

    // Build JSON
    std::string json = "{\n  \"steps\": [\n";
    for (size_t i = 0; i < steps.size(); ++i) {
        const TLBlock& b = *steps[i];
        std::string at  = UtcMsToIso(b.atMs);
        const char* cmd = (b.type==BlockType::Bracket) ? "bracket"
                        : (b.type==BlockType::Burst)   ? "shoot"
                                                        : "shoot";
        std::string entry = std::format(
            "    {{\n"
            "      \"at\": \"{}\",\n"
            "      \"cmd\": \"{}\",\n"
            "      \"ss\": \"{}\",\n"
            "      \"iso\": {},\n"
            "      \"f\": {}",
            at, cmd, b.ss, b.iso, b.fstop);
        if (b.type == BlockType::Bracket)
            entry += std::format(",\n      \"count\": {},\n      \"ev\": \"{}\"",
                                 b.count, b.ev);
        if (b.type == BlockType::Burst)
            entry += std::format(",\n      \"drive\": \"cont-hi-plus\",\n"
                                 "      \"timeout_ms\": {}", b.burstDurMs + 2000);
        if (!b.label.empty())
            entry += std::format(",\n      \"label\": \"{}\"", b.label);
        entry += "\n    }";
        if (i + 1 < steps.size()) entry += ",";
        json += entry + "\n";
    }
    json += "  ]\n}\n";

    // Write file
    std::ofstream f(path);
    if (f) { f << json; f.close(); }
    std::string pathNarrow;
    for (const wchar_t* p = path; *p; ++p) pathNarrow += static_cast<char>(*p);
    m_lastResult = std::format("Exported {} steps → {}", steps.size(), pathNarrow);
    LogLine(m_lastResult);
}

// ─── Menu bar ─────────────────────────────────────────────────────────────────

void App::RenderMenuBar() {
    static const ImVec4 kDim {0.45f, 0.45f, 0.50f, 1.0f};

    // ── File ──────────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Timeline", "Ctrl+N"))
            NewTimeline();

        if (ImGui::MenuItem("Open Timeline\xc2\xb7\xc2\xb7\xc2\xb7", nullptr, false,
                            m_configDb.IsOpen()))
            m_showSnapOpen = true;
        ImGui::Separator();

        ImGui::MenuItem("Save Timeline",       "Ctrl+S", false, false); // future
        if (ImGui::MenuItem("Save Timeline As\xc2\xb7\xc2\xb7\xc2\xb7", nullptr, false,
                            m_configDb.IsOpen() && !m_tracks.empty()))
            m_showSnapSaveAs = true;
        ImGui::Separator();

        if (ImGui::BeginMenu("Export")) {
            if (ImGui::MenuItem("TotalControl JSON\xc2\xb7\xc2\xb7\xc2\xb7"))
                ExportTimelineJson();
            ImGui::Separator();
            ImGui::Separator();
            ImGui::TextColored(kDim, "  (future)");
            ImGui::MenuItem("CSV\xc2\xb7\xc2\xb7\xc2\xb7",                    nullptr, false, false);
            ImGui::MenuItem("SolarEclipseMaestro\xc2\xb7\xc2\xb7\xc2\xb7",    nullptr, false, false);
            ImGui::MenuItem("EclipseWorkbench\xc2\xb7\xc2\xb7\xc2\xb7",       nullptr, false, false);
            ImGui::EndMenu();
        }

        ImGui::MenuItem("Import from JSON\xc2\xb7\xc2\xb7\xc2\xb7", nullptr, false, false); // future
        ImGui::Separator();

        if (ImGui::MenuItem("Quit", "Alt+F4"))
            PostQuitMessage(0);

        ImGui::EndMenu();
    }

    // ── Edit ──────────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("Edit")) {
        bool hasSel = m_selTrack >= 0 && m_selBlock >= 0;

        if (ImGui::MenuItem("Delete Block", "Del", false, hasSel))
            DeleteSelectedBlock();

        if (ImGui::MenuItem("Duplicate Block", "Ctrl+D", false, hasSel))
            DuplicateSelectedBlock();

        ImGui::Separator();
        ImGui::MenuItem("Undo", "Ctrl+Z", false, false); // future
        ImGui::MenuItem("Redo", "Ctrl+Y", false, false); // future
        ImGui::EndMenu();
    }

    // ── View ──────────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("View")) {
        ContactTimes ct;
        { std::lock_guard lk(m_iqpMutex); ct = m_contacts; }
        if (!ct.valid) ct = m_beResult;

        if (ImGui::MenuItem("Full Eclipse", nullptr, false, ct.valid)) {
            m_tlViewStart = ct.c1Ms - 5LL*60000;
            m_tlViewEnd   = ct.c4Ms + 5LL*60000;
        }
        if (ImGui::MenuItem("Zoom to Totality", nullptr, false, ct.valid && ct.c2Ms>0)) {
            m_tlViewStart = ct.c2Ms - 30000;
            m_tlViewEnd   = ct.c3Ms + 30000;
        }
        if (ImGui::MenuItem("Zoom C2 \xc2\xb1 1 min", nullptr, false, ct.valid && ct.c2Ms>0)) {
            m_tlViewStart = ct.c2Ms - 60000;
            m_tlViewEnd   = ct.c2Ms + 60000;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Zoom", nullptr, false, ct.valid)) {
            m_tlViewStart = -1;
            m_tlViewEnd   = -1;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Style: Dark"))    ApplyStyleDark();
        if (ImGui::MenuItem("Style: Classic")) ImGui::StyleColorsClassic();
        if (ImGui::MenuItem("Style: Light"))   ImGui::StyleColorsLight();
        ImGui::EndMenu();
    }

    // ── Sequence ──────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("Sequence")) {
        if (ImGui::MenuItem("Calculate Contacts")) TriggerIqpFetch();
        ImGui::Separator();
        ImGui::MenuItem("Validate Timeline",   nullptr, false, false); // future
        ImGui::MenuItem("Auto-optimize",       nullptr, false, false); // future
        ImGui::EndMenu();
    }

    // ── About ─────────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("About")) {
        if (ImGui::MenuItem("About TotalControl"))
            m_showAbout = true;
        if (ImGui::MenuItem("Open GitHub\xe2\x86\x97")) {
            ShellExecuteW(nullptr, L"open",
                L"https://github.com/mrmgrpl/TotalControl",
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::Separator();
        ImGui::MenuItem("TSE 2026-08-12  Burgos/Lerma", nullptr, false, false);
        ImGui::EndMenu();
    }
}

// ─── About modal ──────────────────────────────────────────────────────────────

void App::RenderAboutModal() {
    if (!m_showAbout) return;
    ImGui::OpenPopup("About TotalControl");
    ImGui::SetNextWindowSize(ImVec2(480, 420), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("About TotalControl", &m_showAbout,
                               ImGuiWindowFlags_NoResize)) {
        static const ImVec4 kGold {0.95f, 0.80f, 0.20f, 1.0f};
        static const ImVec4 kGray {0.55f, 0.55f, 0.60f, 1.0f};
        static const ImVec4 kLink {0.45f, 0.75f, 1.00f, 1.0f};

        ImGui::PushFont(m_fontLarge);
        ImGui::TextColored(kGold, "TotalControl");
        ImGui::PopFont();
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(kGray, "Solar Eclipse Photography Controller");
        ImGui::TextColored(kGray, "Phase 3b  |  TSE 2026-08-12  Burgos/Lerma");
        ImGui::PopFont();

        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushFont(m_fontMono);

        ImGui::TextColored(kGold, "Technologies");
        ImGui::TextColored(kGray, "  Sony CrSDK          Camera Remote SDK v2.x");
        ImGui::TextColored(kGray, "  Dear ImGui v1.91.6  Omar Cornut (MIT)");
        ImGui::TextColored(kGray, "  SQLite 3.53.1       Public domain");
        ImGui::TextColored(kGray, "  WinHTTP             Windows SDK");
        ImGui::Spacing();

        ImGui::TextColored(kGold, "Eclipse Data & Algorithms");
        ImGui::TextColored(kGray, "  NASA Eclipse Data   Fred Espenak");
        ImGui::TextColored(kGray, "  IQP API             maps.besselianelements.com");
        ImGui::TextColored(kGray, "  Besselian Algo      Jean Meeus");
        ImGui::TextColored(kGray, "                      'Astronomical Algorithms'");
        ImGui::TextColored(kGray, "  Eclipse geometry    Greg Miller");
        ImGui::TextColored(kGray, "                      celestialprogramming.com");
        ImGui::Spacing();

        ImGui::TextColored(kGold, "Author");
        ImGui::TextColored(kGray, "  Andrzej Nowak  (mrmgrpl)");
        ImGui::Spacing();

        ImGui::TextColored(kGold, "Links");
        if (ImGui::MenuItem("  github.com/mrmgrpl/TotalControl")) {
            ShellExecuteW(nullptr, L"open",
                L"https://github.com/mrmgrpl/TotalControl",
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (ImGui::MenuItem("  maps.besselianelements.com")) {
            ShellExecuteW(nullptr, L"open",
                L"https://maps.besselianelements.com",
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (ImGui::MenuItem("  NASA Eclipse Page  (Espenak)")) {
            ShellExecuteW(nullptr, L"open",
                L"https://eclipse.gsfc.nasa.gov/",
                nullptr, nullptr, SW_SHOWNORMAL);
        }

        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btnW = 80.f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnW) * 0.5f);
        if (ImGui::Button("Close", ImVec2(btnW, 0)))
            m_showAbout = false;

        ImGui::EndPopup();
    }
}

// ─── Main frame ───────────────────────────────────────────────────────────────

void App::OnFrame() {
    ImGuiIO& io = ImGui::GetIO();

    // ── Main menu bar ─────────────────────────────────────────────────────
    float menuH = 0.f;
    if (ImGui::BeginMainMenuBar()) {
        menuH = ImGui::GetWindowHeight();
        RenderMenuBar();
        ImGui::EndMainMenuBar();
    }

    // ── Keyboard shortcuts ────────────────────────────────────────────────
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) NewTimeline();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) DuplicateSelectedBlock();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) DeleteSelectedBlock();

    // ── Host window (below menu bar) ──────────────────────────────────────
    ImGui::SetNextWindowPos(ImVec2(0.f, menuH));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - menuH));
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##host", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    // ── About + snapshot modals ───────────────────────────────────────────
    RenderAboutModal();
    if (m_configDb.IsOpen()) RenderSnapshotModal();

    const float colW      = 200.0f;   // Col1: Hardware
    const float colW2     = 400.0f;   // Col2: Eclipse
    const float kInspW    = 200.0f;   // Inspector + Palette
    const float kTimelineH = 380.0f;  // Bottom timeline height
    const float totalH    = io.DisplaySize.y - menuH;
    const float totalW    = io.DisplaySize.x;
    const float topH      = totalH - kTimelineH;
    const float statusW   = totalW - colW - colW2 - kInspW;

    bool connected = (m_pipe.GetState() == PipeClient::State::Connected);

    // ════════════════════════════════════════════════════════════════════════
    // COLUMN 1 — Hardware (TIME / CONNECTION / CAMERA STATUS)
    // ════════════════════════════════════════════════════════════════════════
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.07f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 14));
    ImGui::BeginChild("##col_hw", ImVec2(colW, topH), false, 0);
    ImGui::PopStyleVar();

    // ── app name ─────────────────────────────────────────────────────────
    ImGui::PushFont(m_fontMono);
    ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.92f, 1.0f), "TotalControl");
    ImGui::PopFont();

    // ════════════════════════════════
    // Section: TIME
    // ════════════════════════════════
    ImGui::SeparatorText("TIME");

    // ── UTC clock (always on, no options) ─────────────────────────────────
    {
        char buf[20];
        FormatUtcHms(UtcNowMs(), buf, sizeof(buf));
        ImGui::PushFont(m_fontMono);
        ImGui::TextColored(ImVec4(0.90f, 0.75f, 0.20f, 1.0f), "%s", buf);
        ImGui::TextColored(ImVec4(0.40f, 0.40f, 0.45f, 1.0f), "Universal Time Zone");
        ImGui::PopFont();
    }

    ImGui::Spacing();

    // ── Home clock ────────────────────────────────────────────────────────
    RenderExtraClock("Home Time Zone##home", "##popup_home", m_showHomeClock, m_homeTzIana);

    ImGui::Spacing();

    // ── Local / eclipse clock ─────────────────────────────────────────────
    RenderExtraClock("Local Time Zone##ecl", "##popup_ecl",  m_showEclClock,  m_eclTzIana);

    // ── Countdowns ────────────────────────────────────────────────────────────
    if (m_eclipseIdx >= 0 && m_eclipseIdx < static_cast<int>(m_eclipses.size())) {
        const auto& ec = m_eclipses[m_eclipseIdx];
        char tc = ec.type.empty() ? '?' : ec.type[0];
        ImVec4 cdCol = (tc == 'T') ? ImVec4{0.95f, 0.80f, 0.20f, 1.0f}
                     : (tc == 'A') ? ImVec4{0.85f, 0.50f, 0.15f, 1.0f}
                     : (tc == 'H') ? ImVec4{0.80f, 0.75f, 0.20f, 1.0f}
                                   : ImVec4{0.55f, 0.55f, 0.60f, 1.0f};
        static const ImVec4 kDim {0.35f, 0.35f, 0.40f, 1.0f};
        static const ImVec4 kGray{0.40f, 0.40f, 0.45f, 1.0f};

        ImGui::Spacing();
        ImGui::PushFont(m_fontMono);

        // Row 1 — GE (always visible, eclipse-axis reference)
        int64_t geMs = EclipseGeUtcMs(ec);
        if (geMs != INT64_MIN) {
            char cdBuf[32];
            FormatCountdown(geMs - UtcNowMs(), cdBuf, sizeof(cdBuf));
            ImGui::TextColored(kGray, "GE");
            ImGui::SameLine(25.0f);
            ImGui::TextColored(cdCol, "%s", cdBuf);
        }

        // Rows C1/C2/Max/C3/C4 from IQP or Besselian
        {
            int iqpSt = m_iqpState.load();
            if (iqpSt == 1) {
                ImGui::TextColored(kGray, "C1");
                ImGui::SameLine(25.0f);
                ImGui::TextColored(kDim, "loading...");
            } else if (iqpSt == 2 || iqpSt == 3) {
                ContactTimes ct;
                { std::lock_guard lk(m_iqpMutex); ct = m_contacts; }

                if (!ct.valid && ct.apiOk) {
                    ImGui::TextColored(kGray, "C1");
                    ImGui::SameLine(25.0f);
                    ImGui::TextColored(kDim, "no eclipse");
                } else if (ct.valid) {
                    // Source label shown once on C1 line
                    const char* srcLabel = (ct.source == ContactSource::IQP) ? "IQP" : "BE";

                    // Helper: format UTC ms → "HH:MM:SS"
                    auto fmtUtc = [](int64_t ms, char* buf, int len) {
                        if (ms < 0) { snprintf(buf, len, "--:--:--"); return; }
                        int64_t s  = ms / 1000;
                        int hh = static_cast<int>((s / 3600) % 24);
                        int mm = static_cast<int>((s / 60)   % 60);
                        int sc = static_cast<int>(s % 60);
                        snprintf(buf, len, "%02d:%02d:%02d", hh, mm, sc);
                    };

                    struct Row { const char* lbl; int64_t ms; };
                    Row rows[] = {
                        {"C1",  ct.c1Ms},
                        {"C2",  ct.c2Ms},
                        {"Max", ct.maxMs},
                        {"C3",  ct.c3Ms},
                        {"C4",  ct.c4Ms},
                    };
                    bool first = true;
                    for (auto& r : rows) {
                        if (r.ms < 0) continue;
                        char tbuf[12];
                        fmtUtc(r.ms, tbuf, sizeof(tbuf));
                        ImGui::TextColored(kGray, "%s", r.lbl);
                        ImGui::SameLine(30.0f);
                        ImGui::TextColored(cdCol, "%s", tbuf);
                        if (first) {
                            ImGui::SameLine();
                            ImGui::TextColored(kDim, " %s", srcLabel);
                            first = false;
                        }
                    }
                } else {
                    ImGui::TextColored(kGray, "C1");
                    ImGui::SameLine(25.0f);
                    ImGui::TextColored(kDim, "err");
                }
            }
        }

        ImGui::PopFont();
    }

    // ════════════════════════════════
    // Section: CONNECTION
    // ════════════════════════════════
    ImGui::SeparatorText("CONNECTION");

    ImGui::PushFont(m_fontMono);
    if (connected)
        ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.30f, 1.0f), "● connected");
    else
        ImGui::TextColored(ImVec4(0.75f, 0.22f, 0.18f, 1.0f), "○ no server");
    ImGui::PopFont();

    ImGui::Spacing();

    const ImVec2 btnSz(-1, 34);

    {
        bool srvRunning = false;
        HANDLE hm = OpenMutexW(SYNCHRONIZE, FALSE, L"TotalControl_DaemonRunning");
        if (hm) { srvRunning = true; CloseHandle(hm); }

        if (srvRunning) ImGui::BeginDisabled();
        if (ImGui::Button("Connect cameras", btnSz)) {
            LogLine("user: Start TotalControlSRV");
            if (TryLaunchDaemon()) {
                m_lastResult = "SRV launched...";
                m_reconnectCountdown = 0;
            } else {
                m_lastResult = "ERROR: cannot launch TotalControlSRV.exe";
                LogLine(m_lastResult);
            }
        }
        if (srvRunning) ImGui::EndDisabled();
    }

    ImGui::Spacing();

    if (!connected) ImGui::BeginDisabled();
    if (ImGui::Button("Test picture", btnSz)) {
        LogLine("user: take test picture");
        const char* req =
            "{\"cmd\":\"shoot\",\"drive\":\"single\","
            "\"ss\":\"1/8000\",\"iso\":100,\"f\":8.0}";
        if (auto res = m_pipe.SendRequest(req)) {
            bool ok  = JStr(*res, "ok") == "true";
            int  lat = JInt(*res, "latency_ms", -1);
            // Store shot latency in camera[0] (Test picture targets camera[0])
            if (ok && lat >= 0 && !m_cameras.empty())
                m_cameras[0].lastShotMs = lat;
            m_lastResult = ok
                ? std::format("Shot OK — {} ms", lat >= 0 ? lat : 0)
                : "ERROR: shoot failed";
            LogLine(m_lastResult);
        } else {
            m_lastResult = std::format("ERROR: {}", PipeErrorMessage(res.error()));
            LogLine(m_lastResult);
        }
    }
    if (!connected) ImGui::EndDisabled();

    ImGui::Spacing();

    if (!connected) ImGui::BeginDisabled();
    if (ImGui::Button("Disconnect cameras", btnSz)) {
        // Stop sequencer first if running
        if (m_seqRun.load()) { StopSeqThread(); m_guiSeqMode.store(GuiSeqMode::Idle); }
        LogLine("user: disconnect & quit SRV");
        (void)m_pipe.Send("{\"cmd\":\"quit\"}");  // fire-and-forget: SRV closes connection before replying
        m_pipe.Disconnect();
        { std::lock_guard lk(m_camerasMutex); m_cameras.clear(); }
        m_lastResult = "Server stopped.";
        LogLine("SRV quit sent");
    }
    if (!connected) ImGui::EndDisabled();

    // ── Sequencer buttons: TEST RUN / RUN ─────────────────────────────────
    RenderSequencerButtons();

    // ════════════════════════════════
    // Section: CAMERA STATUS
    // ════════════════════════════════
    ImGui::Spacing();
    ImGui::SeparatorText("CAMERA STATUS");
    ImGui::Spacing();
    RenderCameraSection();

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ════════════════════════════════════════════════════════════════════════
    // COLUMN 2 — Eclipse (selector / observer / calculate / contact times)
    // ════════════════════════════════════════════════════════════════════════
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.065f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 14));
    ImGui::BeginChild("##col_ecl", ImVec2(colW2, topH), false, 0);
    ImGui::PopStyleVar();

    ImGui::SeparatorText("ECLIPSE");
    ImGui::Spacing();
    RenderEclipseSection();

    ImGui::Spacing();
    {
        int iqpSt = m_iqpState.load();
        bool loading = (iqpSt == 1);
        if (loading) ImGui::BeginDisabled();
        if (ImGui::Button(loading ? "Calculating..." : "Calculate Contacts", ImVec2(-1, 34)))
            TriggerIqpFetch();
        if (loading) ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("CONTACTS");
    ImGui::Spacing();
    RenderContactTimesSection();

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ════════════════════════════════════════════════════════════════════════
    // COLUMN 3 — Status / Result
    // ════════════════════════════════════════════════════════════════════════
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.09f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::BeginChild("##col_status", ImVec2(statusW, topH), false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    RenderStatusColumn();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ════════════════════════════════════════════════════════════════════════
    // COLUMN 4 — Inspector + Palette
    // ════════════════════════════════════════════════════════════════════════
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.075f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
    ImGui::BeginChild("##col_insp", ImVec2(kInspW, topH), false, 0);
    ImGui::PopStyleVar();
    RenderInspectorColumn();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ════════════════════════════════════════════════════════════════════════
    // TIMELINE — full width at bottom
    // ════════════════════════════════════════════════════════════════════════
    ImGui::SetCursorPos({0.0f, topH});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.04f, 0.055f, 1.0f));
    ImGui::BeginChild("##col_tl", ImVec2(totalW, kTimelineH), false,
                      ImGuiWindowFlags_NoScrollbar);
    RenderTimelineBottom();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (m_showStyleEditor) {
        ImGui::Begin("Style Editor", &m_showStyleEditor);
        ImGui::ShowStyleEditor();
        ImGui::End();
    }
    if (m_showDemoWindow)
        ImGui::ShowDemoWindow(&m_showDemoWindow);

    ImGui::End();

    // ── persist timeline when changed ─────────────────────────────────────
    if (m_tlDirty && m_configDb.IsOpen()) {
        m_configDb.SaveTimeline(m_tracks);
        m_tlDirty = false;
    }

    // Camera status polling is handled by m_statusThread (background, ~2 s interval).

    // ── silent auto-reconnect ─────────────────────────────────────────────
    if (!connected) {
        if (m_reconnectCountdown <= 0) {
            m_pipe.Connect();
            m_reconnectCountdown = 60;
        } else {
            --m_reconnectCountdown;
        }
    }
}

bool App::OnCloseRequest() { return true; }

// ─── Daemon launch ────────────────────────────────────────────────────────────

bool App::TryLaunchDaemon() {
    HANDLE hm = OpenMutexW(SYNCHRONIZE, FALSE, L"TotalControl_DaemonRunning");
    if (hm) { CloseHandle(hm); return true; }

    std::wstring dir = ExeDir();
    std::wstring exe = dir + L"\\TotalControlSRV.exe";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr,
                             FALSE, CREATE_NEW_CONSOLE, nullptr,
                             dir.c_str(), &si, &pi);
    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return true; }
    return false;
}

} // namespace TotalControl
