#include "SequencerEngine.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <sstream>

namespace TotalControl {

// ─── UTC helpers ──────────────────────────────────────────────────────────────

static const ULONGLONG kFileTimeEpochOffsetMs = 116444736000000ULL; // ms from 1601 to 1970

static int64_t UtcNowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ui;
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    // FILETIME is 100ns intervals; divide by 10000 for ms
    return (int64_t)(ui.QuadPart / 10000ULL - kFileTimeEpochOffsetMs);
}

// Parse "2026-08-12T20:29:51.000Z" → ms since Unix epoch. Returns -1 on error.
static int64_t ParseUtcMs(const std::wstring& s) {
    if (s.size() < 20) return -1;
    try {
        SYSTEMTIME st = {};
        st.wYear         = (WORD)std::stoi(s.substr(0, 4));
        st.wMonth        = (WORD)std::stoi(s.substr(5, 2));
        st.wDay          = (WORD)std::stoi(s.substr(8, 2));
        st.wHour         = (WORD)std::stoi(s.substr(11, 2));
        st.wMinute       = (WORD)std::stoi(s.substr(14, 2));
        st.wSecond       = (WORD)std::stoi(s.substr(17, 2));
        st.wMilliseconds = (s.size() >= 23 && s[19] == L'.')
                           ? (WORD)std::stoi(s.substr(20, 3)) : 0;
        FILETIME ft;
        if (!SystemTimeToFileTime(&st, &ft)) return -1;
        ULARGE_INTEGER ui;
        ui.LowPart  = ft.dwLowDateTime;
        ui.HighPart = ft.dwHighDateTime;
        return (int64_t)(ui.QuadPart / 10000ULL - kFileTimeEpochOffsetMs);
    } catch (...) { return -1; }
}

// ─── Local JSON helpers ───────────────────────────────────────────────────────

static std::wstring SJStr(const std::wstring& j, const wchar_t* key) {
    std::wstring k = std::wstring(L"\"") + key + L"\":";
    auto pos = j.find(k);
    if (pos == std::wstring::npos) return L"";
    pos += k.size();
    while (pos < j.size() && iswspace(j[pos])) ++pos;
    if (pos >= j.size() || j[pos] != L'"') return L"";
    ++pos;
    // Handle escaped quotes inside the value
    std::wstring result;
    bool escape = false;
    for (size_t i = pos; i < j.size(); ++i) {
        if (escape) { escape = false; result += j[i]; continue; }
        if (j[i] == L'\\') { escape = true; continue; }
        if (j[i] == L'"') break;
        result += j[i];
    }
    return result;
}

static int64_t SJInt64(const std::wstring& j, const wchar_t* key, int64_t def = 0) {
    std::wstring k = std::wstring(L"\"") + key + L"\":";
    auto pos = j.find(k);
    if (pos == std::wstring::npos) return def;
    pos += k.size();
    while (pos < j.size() && iswspace(j[pos])) ++pos;
    if (pos < j.size() && j[pos] == L'"') ++pos;
    try { return std::stoll(j.substr(pos)); } catch (...) { return def; }
}

static bool SJHas(const std::wstring& j, const wchar_t* key) {
    return j.find(std::wstring(L"\"") + key + L"\":") != std::wstring::npos;
}

// ─── Read whole file as wide string (UTF-8 input, BOM tolerated) ─────────────

static std::wstring ReadFileW(const std::wstring& path, std::wstring& err) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        err = L"Cannot open: " + path;
        return L"";
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return L""; }
    std::string raw(sz, '\0');
    (void)fread(raw.data(), 1, sz, f);
    fclose(f);
    // Skip UTF-8 BOM
    size_t off = 0;
    if (raw.size() >= 3 &&
        (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF)
        off = 3;
    int wn = MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                 (int)(raw.size() - off), nullptr, 0);
    if (wn <= 0) { err = L"UTF-8 decode error"; return L""; }
    std::wstring ws(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.data() + off, (int)(raw.size() - off),
                        ws.data(), wn);
    return ws;
}

// ─── Extract JSON array items matching "steps":[{...},{...},...] ─────────────
// Uses bracket-matching with escape/string tracking.

static std::vector<std::wstring> ExtractSteps(const std::wstring& json) {
    // Find "steps" key, then skip optional whitespace before '['.
    const std::wstring key = L"\"steps\"";
    auto start = json.find(key);
    if (start == std::wstring::npos) return {};
    start += key.size();
    while (start < json.size() && iswspace(json[start])) ++start;
    if (start >= json.size() || json[start] != L':') return {};
    ++start;
    while (start < json.size() && iswspace(json[start])) ++start;
    if (start >= json.size() || json[start] != L'[') return {};
    ++start;

    std::vector<std::wstring> items;
    size_t i = start;
    const size_t n = json.size();

    while (i < n) {
        while (i < n && iswspace(json[i])) ++i;
        if (i >= n || json[i] == L']') break;
        if (json[i] != L'{') { ++i; continue; }

        int   depth  = 0;
        bool  inStr  = false;
        bool  esc    = false;
        size_t objStart = i;
        bool  found  = false;

        for (size_t j = i; j < n; ++j) {
            wchar_t c = json[j];
            if (esc)    { esc = false; continue; }
            if (inStr)  { if (c == L'\\') esc = true; else if (c == L'"') inStr = false; continue; }
            if (c == L'"') { inStr = true; continue; }
            if (c == L'{') { ++depth; }
            else if (c == L'}') {
                --depth;
                if (depth == 0) {
                    items.push_back(json.substr(objStart, j - objStart + 1));
                    i = j + 1;
                    found = true;
                    break;
                }
            }
        }
        if (!found) break; // malformed JSON
    }
    return items;
}

// ─── SequencerEngine ──────────────────────────────────────────────────────────

SequencerEngine::SequencerEngine(SeqLogFn log)
    : m_log(std::move(log)) {}

SequencerEngine::~SequencerEngine() {
    Stop();
}

void SequencerEngine::SetDispatch(SeqDispatchFn fn) {
    m_dispatch = std::move(fn);
}

std::wstring SequencerEngine::Load(const std::wstring& path) {
    if (m_state.load() == SeqState::Running)
        return L"Cannot load while sequence is running";

    std::wstring err;
    std::wstring json = ReadFileW(path, err);
    if (!err.empty()) return err;

    // ── Tryb symulacji ────────────────────────────────────────────────────────
    // Jeśli JSON zawiera "c2_utc" i "sim_c2_utc", przesuń wszystkie timestampy
    // tak by prawdziwe C2 wypadło w czasie symulacji. Produkcja: usuń sim_c2_utc.
    int64_t simOffset = 0;
    {
        std::wstring realC2Str = SJStr(json, L"c2_utc");
        std::wstring simC2Str  = SJStr(json, L"sim_c2_utc");
        if (!realC2Str.empty() && !simC2Str.empty()) {
            int64_t realC2Ms = ParseUtcMs(realC2Str);
            int64_t simC2Ms  = ParseUtcMs(simC2Str);
            if (realC2Ms > 0 && simC2Ms > 0 && realC2Ms != simC2Ms) {
                simOffset = simC2Ms - realC2Ms;
                Logf(L"[SEQ] SYMULACJA aktywna — offset %+lld ms", (long long)simOffset);
                Logf(L"[SEQ]   prod C2: %s", realC2Str.c_str());
                Logf(L"[SEQ]   sim  C2: %s", simC2Str.c_str());
            }
        }
    }

    auto rawSteps = ExtractSteps(json);
    Logf(L"[SEQ] ExtractSteps: %d obiektów w \"steps\"", (int)rawSteps.size());
    if (rawSteps.empty()) return L"No steps found — check \"steps\" array in file";

    std::vector<SeqStep> steps;
    int skipped = 0;

    for (const auto& stepJson : rawSteps) {
        std::wstring atStr = SJStr(stepJson, L"at");
        if (atStr.empty()) {
            std::wstring lbl = SJStr(stepJson, L"label");
            Logf(L"[SEQ] SKIP (brak 'at')  label='%s'", lbl.c_str());
            ++skipped; continue;
        }
        int64_t atMs = ParseUtcMs(atStr) + simOffset;
        if (atMs < 0) {
            Logf(L"[SEQ] SKIP (błąd ParseUtcMs)  at='%s'", atStr.c_str());
            ++skipped; continue;
        }

        std::wstring label = SJStr(stepJson, L"label");

        // Repeating step: "interval_ms" + optional "until"
        int64_t intervalMs = SJHas(stepJson, L"interval_ms")
                             ? SJInt64(stepJson, L"interval_ms") : 0;
        int64_t untilMs = 0;
        if (intervalMs > 0 && SJHas(stepJson, L"until"))
            untilMs = ParseUtcMs(SJStr(stepJson, L"until")) + simOffset;

        if (intervalMs > 0 && untilMs > atMs) {
            int idx = 0;
            for (int64_t t = atMs; t <= untilMs; t += intervalMs) {
                std::wstring lbl = label.empty()
                                   ? std::to_wstring(idx)
                                   : label + L"[" + std::to_wstring(idx) + L"]";
                steps.push_back({ t, stepJson, lbl });
                ++idx;
            }
        } else {
            steps.push_back({ atMs, stepJson, label });
        }
    }

    if (steps.empty()) return L"All steps have invalid/missing timestamps";

    std::sort(steps.begin(), steps.end(),
              [](const SeqStep& a, const SeqStep& b) { return a.utcMs < b.utcMs; });

    {
        std::lock_guard<std::mutex> lk(m_statMutex);
        m_steps      = std::move(steps);
        m_loadedFile = path;
        m_totalSteps = (int)m_steps.size();
        m_doneSteps  = 0;
        m_skipSteps  = 0;
        m_failSteps  = 0;
        m_lastError.clear();
    }
    m_state.store(SeqState::Idle);

    Logf(L"[SEQ] Załadowano %d kroków (%d pominięto) z: %s",
         m_totalSteps, skipped, path.c_str());
    return L"";
}

bool SequencerEngine::Start() {
    if (m_state.load() == SeqState::Running) return false;
    {
        std::lock_guard<std::mutex> lk(m_statMutex);
        if (m_steps.empty()) return false;
    }
    if (!m_dispatch) return false;

    m_stopReq.store(false);
    {
        std::lock_guard<std::mutex> lk(m_statMutex);
        m_doneSteps = 0;
        m_skipSteps = 0;
        m_failSteps = 0;
    }
    m_state.store(SeqState::Running);
    m_thread = std::thread([this] { ThreadProc(); });
    Log(L"[SEQ] Sekwencja uruchomiona.");
    return true;
}

void SequencerEngine::Stop() {
    m_stopReq.store(true);
    if (m_thread.joinable()) m_thread.join();
}

SeqState SequencerEngine::State() const {
    return m_state.load();
}

int SequencerEngine::TotalSteps() const {
    std::lock_guard<std::mutex> lk(m_statMutex);
    return m_totalSteps;
}

int SequencerEngine::DoneSteps() const {
    std::lock_guard<std::mutex> lk(m_statMutex);
    return m_doneSteps;
}

std::wstring SequencerEngine::StatusJson() const {
    const wchar_t* stateStr = L"idle";
    switch (m_state.load()) {
    case SeqState::Running: stateStr = L"running"; break;
    case SeqState::Done:    stateStr = L"done";    break;
    case SeqState::Error:   stateStr = L"error";   break;
    default: break;
    }

    std::lock_guard<std::mutex> lk(m_statMutex);
    std::wostringstream ss;
    ss << L"\"seq_state\":\"" << stateStr << L"\""
       << L",\"seq_total\":"  << m_totalSteps
       << L",\"seq_done\":"   << m_doneSteps
       << L",\"seq_skip\":"   << m_skipSteps
       << L",\"seq_fail\":"   << m_failSteps
       << L",\"seq_file\":\""  << m_loadedFile << L"\"";
    if (!m_lastError.empty())
        ss << L",\"seq_error\":\"" << m_lastError << L"\"";
    return ss.str();
}

// ─── Thread ───────────────────────────────────────────────────────────────────

void SequencerEngine::ThreadProc() {
    Log(L"[SEQ] Wątek sekwencera — start.");

    std::vector<SeqStep> steps;
    {
        std::lock_guard<std::mutex> lk(m_statMutex);
        steps = m_steps; // local copy — safe to iterate without holding lock
    }

    for (const auto& step : steps) {
        if (m_stopReq.load()) break;

        // Wait until scheduled time, waking every 50ms to check stop flag.
        while (!m_stopReq.load()) {
            int64_t waitMs = step.utcMs - UtcNowMs();
            if (waitMs <= 0) break;
            ::Sleep(waitMs > 50 ? 50 : (DWORD)waitMs);
        }
        if (m_stopReq.load()) break;

        int64_t drift = UtcNowMs() - step.utcMs;

        // Skip if more than 30s late — step is no longer meaningful.
        if (drift > 30000) {
            Logf(L"[SEQ] SKIP +%lldms  %s", (long long)drift, step.label.c_str());
            std::lock_guard<std::mutex> lk(m_statMutex);
            ++m_skipSteps;
            continue;
        }

        Logf(L"[SEQ] EXEC drift=%+lldms  %s", (long long)drift, step.label.c_str());
        Logf(L"[SEQ] → %s", step.json.c_str());

        std::wstring resp;
        bool contFlag = m_dispatch(step.json, resp);
        bool success  = resp.find(L"\"ok\":true") != std::wstring::npos;

        Logf(L"[SEQ] ← %s", resp.c_str());

        {
            std::lock_guard<std::mutex> lk(m_statMutex);
            if (success) ++m_doneSteps;
            else {
                ++m_failSteps;
                m_lastError = step.label + L": " + resp;
            }
        }

        // 'false' from dispatch means server is shutting down
        if (!contFlag) { m_stopReq.store(true); break; }
    }

    m_state.store(m_stopReq.load() ? SeqState::Idle : SeqState::Done);
    Log(m_stopReq.load() ? L"[SEQ] Zatrzymano." : L"[SEQ] Sekwencja zakończona.");
}

void SequencerEngine::Log(const wchar_t* msg) {
    if (m_log) m_log(msg);
}

void SequencerEngine::Logf(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);
    Log(buf);
}

} // namespace TotalControl
