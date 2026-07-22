#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace TotalControl {

// handler(request_json, response_json) → false = quit requested
using PipeHandlerFn = std::function<bool(const std::wstring&, std::wstring&)>;

// Named-pipe server supporting multiple concurrent clients.
//
// Maintains a fixed pool of acceptor threads (kAcceptorThreads), each
// independently cycling create-instance -> wait-for-client -> serve ->
// loop-back. This means several pipe instances are ALWAYS simultaneously
// listening, so a burst of near-simultaneous client connects (e.g. the GUI
// opening one dedicated pipe per camera track at sequencer start) can all be
// accepted immediately. The previous design accepted ONE client at a time on
// a single thread (create -> wait -> spawn a worker -> loop back) — under a
// real multi-client burst, later connects lost the race for the single
// listening instance and either failed outright or connected much later
// than expected (see Change log).
//
// Handler calls run concurrently, one per acceptor's currently-served
// client, with no locking here — the handler (CommandHandler::Handle) does
// its own internal synchronisation (per-camera locking), so a slow command
// on one camera never blocks commands to other cameras from other clients.
class PipeServer {
public:
    // Generous headroom: GUI's persistent status connection + up to
    // kMaxCamTracks (4) sequencer connections + CLI + slack for reconnects.
    static constexpr int kAcceptorThreads = 8;

    PipeServer(const wchar_t* pipeName, PipeHandlerFn handler);
    ~PipeServer();

    void Run();   // blocking — spawns the acceptor pool, returns after Stop()
    void Stop();  // thread-safe, interrupts Run() and signals acceptors to exit

private:
    void AcceptorLoop();            // one pool worker: accept, serve, repeat
    void ServeClient(HANDLE pipe);  // synchronous request/response loop for one client

    std::wstring      m_pipeName;
    PipeHandlerFn     m_handler;
    HANDLE            m_stopEvent  = INVALID_HANDLE_VALUE;
    std::atomic<bool> m_running    { true };
    std::thread       m_acceptors[kAcceptorThreads];
};

} // namespace TotalControl
