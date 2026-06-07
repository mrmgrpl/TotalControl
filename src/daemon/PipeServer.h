#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace TotalControl {

// handler(request_json, response_json) → false = quit requested
using PipeHandlerFn = std::function<bool(const std::wstring&, std::wstring&)>;

// Named-pipe server supporting multiple concurrent clients.
// Each accepted connection is served by a detached worker thread.
// m_handlerMutex serialises handler calls — camera/sequencer state is not thread-safe.
class PipeServer {
public:
    PipeServer(const wchar_t* pipeName, PipeHandlerFn handler);
    ~PipeServer();

    void Run();   // blocking accept loop — returns after Stop() or fatal error
    void Stop();  // thread-safe, interrupts Run() and signals worker threads to exit

private:
    void ServeClient(HANDLE pipe);  // runs in per-client detached thread

    std::wstring      m_pipeName;
    PipeHandlerFn     m_handler;
    HANDLE            m_stopEvent  = INVALID_HANDLE_VALUE;
    std::atomic<bool> m_running    { true };
    std::mutex        m_handlerMtx;   // serialise concurrent handler invocations
};

} // namespace TotalControl
