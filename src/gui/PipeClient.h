#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>

namespace TotalControl {

// Synchronous JSON-Lines client for \\.\pipe\TotalControl.
// Connect() / Disconnect() are called from the main thread.
// SendRequest() is thread-safe (blocking).
class PipeClient {
public:
    enum class State { Disconnected, Connecting, Connected };

    PipeClient();
    ~PipeClient();

    // Non-copyable
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // Try to open the pipe. Returns true on success. Non-blocking.
    bool Connect();

    // Close the pipe handle.
    void Disconnect();

    // Send one JSON request line, receive one JSON response line.
    // Returns false if pipe is broken (sets state to Disconnected).
    bool SendRequest(const std::string& request, std::string& response);

    State GetState() const { return m_state.load(); }

    // Convenience: send without caring about response (fire-and-forget with log).
    bool Send(const std::string& request);

private:
    HANDLE          m_pipe = INVALID_HANDLE_VALUE;
    std::atomic<State> m_state{State::Disconnected};
    mutable std::mutex m_pipeMutex;
};

} // namespace TotalControl
