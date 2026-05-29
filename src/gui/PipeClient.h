#pragma once
#include <windows.h>
#include <expected>
#include <string>
#include <string_view>
#include <atomic>
#include <mutex>

namespace TotalControl {

enum class PipeError {
    NotConnected,    // pipe handle is INVALID_HANDLE_VALUE
    WriteFailed,     // WriteFile() returned false
    ReadFailed,      // ReadFile() returned false or read == 0
    ResponseTooLarge // response exceeded safety limit (1 MiB)
};

std::string_view PipeErrorMessage(PipeError e) noexcept;

// Synchronous JSON-Lines client for \\.\pipe\TotalControl.
// SendRequest() is thread-safe (blocking, holds mutex for duration of call).
class PipeClient {
public:
    enum class State { Disconnected, Connected };

    PipeClient()  = default;
    ~PipeClient() { Disconnect(); }

    PipeClient(const PipeClient&)            = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // Open the pipe. Returns true on success. Non-blocking.
    bool Connect();

    // Close the pipe handle.
    void Disconnect();

    // Send one JSON request line, receive one JSON response line.
    // On error the pipe is closed and state set to Disconnected.
    std::expected<std::string, PipeError>
    SendRequest(std::string_view request);

    // Fire-and-forget: sends request, discards response.
    std::expected<void, PipeError>
    Send(std::string_view request);

    State GetState() const noexcept { return m_state.load(); }

private:
    HANDLE             m_pipe = INVALID_HANDLE_VALUE;
    std::atomic<State> m_state{State::Disconnected};
    mutable std::mutex m_pipeMutex;

    void BreakPipe() noexcept;
};

} // namespace TotalControl
