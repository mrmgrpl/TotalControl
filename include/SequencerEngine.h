#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace TotalControl {

// Returns false to signal "quit" (matches CommandHandler::Handle signature).
using SeqDispatchFn = std::function<bool(const std::wstring& req, std::wstring& resp)>;
using SeqLogFn      = std::function<void(const wchar_t*)>;

struct SeqStep {
    int64_t      utcMs;   // UTC ms since Unix epoch
    std::wstring json;    // full step JSON → forwarded to dispatch function
    std::wstring label;
};

enum class SeqState { Idle, Running, Done, Error };

class SequencerEngine {
public:
    explicit SequencerEngine(SeqLogFn log = nullptr);
    ~SequencerEngine();

    // Must be set before Start().
    void SetDispatch(SeqDispatchFn fn);

    // Load sequence JSON file. Returns empty string on success, error message on failure.
    // Steps with "interval_ms"+"until" are expanded into individual timed steps.
    std::wstring Load(const std::wstring& path);

    // Start execution thread. Requires prior Load() and SetDispatch().
    bool Start();

    // Request stop and block until thread exits.
    void Stop();

    SeqState     State()      const;
    std::wstring StatusJson() const; // JSON fragment suitable for pipe response
    int          TotalSteps() const;
    int          DoneSteps()  const;

private:
    void ThreadProc();
    void Log(const wchar_t* msg);
    void Logf(const wchar_t* fmt, ...);

    SeqDispatchFn         m_dispatch;
    SeqLogFn              m_log;
    std::vector<SeqStep>  m_steps;
    std::thread           m_thread;
    std::atomic<SeqState> m_state   { SeqState::Idle };
    std::atomic<bool>     m_stopReq { false };
    mutable std::mutex    m_statMutex;
    std::wstring          m_loadedFile;
    std::wstring          m_lastError;
    int                   m_totalSteps = 0;
    int                   m_doneSteps  = 0;
    int                   m_skipSteps  = 0;
    int                   m_failSteps  = 0;
};

} // namespace TotalControl
