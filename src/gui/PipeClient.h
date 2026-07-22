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
    ResponseTooLarge,// response exceeded safety limit (1 MiB)
    Timeout          // no response within kIoTimeoutMs — pipe is closed, caller may retry
};

std::string_view PipeErrorMessage(PipeError e) noexcept;

// Synchronous-style (blocking, but bounded) JSON-Lines client for
// \\.\pipe\TotalControl. SendRequest() is thread-safe (holds mutex for the
// duration of the call) and internally uses overlapped I/O with a timeout —
// a stuck/unresponsive server can never hang the calling thread forever
// (see Change log: a caller blocked in an untimed ReadFile could not be
// interrupted even by the caller's own shutdown request, freezing the GUI).
class PipeClient {
public:
    enum class State { Disconnected, Connected };

    // Bound on a single request/response round trip. Must comfortably exceed
    // the slowest legitimate SRV operation — CommandHandler's DriveMode
    // verify budget (kDriveModeVerifyMs) is 6000ms, so this leaves margin.
    static constexpr DWORD kIoTimeoutMs = 10000;

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
