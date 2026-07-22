#include "PipeClient.h"
#include <cassert>

namespace TotalControl {

static constexpr DWORD  kReadBufSize  = 4096;
static constexpr size_t kMaxResponse  = 1024 * 1024; // 1 MiB safety limit

std::string_view PipeErrorMessage(PipeError e) noexcept {
    switch (e) {
        case PipeError::NotConnected:    return "not connected";
        case PipeError::WriteFailed:     return "write failed — pipe broken";
        case PipeError::ReadFailed:      return "read failed — pipe broken";
        case PipeError::ResponseTooLarge:return "response too large";
        case PipeError::Timeout:         return "timed out — no response from SRV";
    }
    return "unknown error";
}

// ─── Internal helpers ─────────────────────────────────────────────────────────

void PipeClient::BreakPipe() noexcept {
    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    m_state.store(State::Disconnected);
}

// Bounded overlapped WriteFile/ReadFile. The pipe handle is always opened
// with FILE_FLAG_OVERLAPPED (see Connect()) specifically so these can be
// cancelled — an untimed synchronous ReadFile on an unresponsive SRV
// previously could not be interrupted by anything, including the calling
// thread's own shutdown request, which froze the whole GUI until the
// process was killed (see Change log). On timeout the pending I/O is
// cancelled and reaped before returning, so the OVERLAPPED/event on the
// stack are never touched again by the kernel after this function returns.
//
// outTimedOut distinguishes "no response in time" (caller should surface
// PipeError::Timeout) from a hard I/O failure (broken pipe etc.) — read via
// this out-param rather than GetLastError() after returning, since the
// cleanup calls below (CancelIoEx/GetOverlappedResult/CloseHandle) would
// otherwise overwrite it before the caller gets a chance to inspect it.
static bool OverlappedIo(HANDLE pipe, bool isWrite, void* buf, DWORD size,
                          DWORD& outBytes, DWORD timeoutMs, bool& outTimedOut) {
    outTimedOut = false;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return false;

    BOOL ok = isWrite ? WriteFile(pipe, buf, size, nullptr, &ov)
                       : ReadFile(pipe, buf, size, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(ov.hEvent);
        return false;
    }

    ok = GetOverlappedResultEx(pipe, &ov, &outBytes, timeoutMs, FALSE);
    if (!ok) {
        outTimedOut = (GetLastError() == WAIT_TIMEOUT);
        CancelIoEx(pipe, &ov);
        // Reap the cancelled operation so the kernel is done writing to `ov`
        // before it goes out of scope; result is ignored either way.
        DWORD discard = 0;
        GetOverlappedResult(pipe, &ov, &discard, TRUE);
        CloseHandle(ov.hEvent);
        return false;
    }
    CloseHandle(ov.hEvent);
    return true;
}

// ─── Public API ───────────────────────────────────────────────────────────────

bool PipeClient::Connect() {
    std::lock_guard lk(m_pipeMutex);
    if (m_pipe != INVALID_HANDLE_VALUE) return true;

    // SRV's accept loop (PipeServer::Run) creates one named-pipe instance at
    // a time, so a burst of near-simultaneous client connects (e.g. one per
    // camera track at sequencer start) can transiently hit ERROR_PIPE_BUSY.
    // WaitNamedPipeW only reserves the next available instance for a short
    // window — CreateFileW must be retried right after the wait to actually
    // claim it, otherwise the reservation is wasted and Connect() fails even
    // though an instance became available (see Change log).
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        h = CreateFileW(
            L"\\\\.\\pipe\\TotalControl",
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return false;
        WaitNamedPipeW(L"\\\\.\\pipe\\TotalControl", 200);
    }
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

    m_pipe = h;
    m_state.store(State::Connected);
    return true;
}

void PipeClient::Disconnect() {
    std::lock_guard lk(m_pipeMutex);
    BreakPipe();
}

std::expected<std::string, PipeError>
PipeClient::SendRequest(std::string_view request) {
    assert(!request.empty());         // caller must not send an empty JSON command
    assert(request.size() < 65536);   // sanity: no single command should approach 64 KiB
    std::lock_guard lk(m_pipeMutex);

    if (m_pipe == INVALID_HANDLE_VALUE)
        return std::unexpected(PipeError::NotConnected);

    // Append newline if missing
    std::string line{request};
    if (line.empty() || line.back() != '\n') line += '\n';

    DWORD written = 0;
    bool  timedOut = false;
    if (!OverlappedIo(m_pipe, true, line.data(),
                       static_cast<DWORD>(line.size()), written, kIoTimeoutMs, timedOut)) {
        BreakPipe();
        return std::unexpected(timedOut ? PipeError::Timeout : PipeError::WriteFailed);
    }

    // Read until newline
    std::string response;
    char buf[kReadBufSize];

    while (true) {
        DWORD read = 0;
        if (!OverlappedIo(m_pipe, false, buf, sizeof(buf), read, kIoTimeoutMs, timedOut)) {
            BreakPipe();
            return std::unexpected(timedOut ? PipeError::Timeout : PipeError::ReadFailed);
        }
        if (read == 0) {
            BreakPipe();
            return std::unexpected(PipeError::ReadFailed);
        }
        response.append(buf, read);
        if (response.find('\n') != std::string::npos) break;
        if (response.size() > kMaxResponse) {
            BreakPipe();
            return std::unexpected(PipeError::ResponseTooLarge);
        }
    }

    // Strip trailing whitespace
    while (!response.empty() &&
           (response.back() == '\n' || response.back() == '\r'))
        response.pop_back();

    return response;
}

std::expected<void, PipeError>
PipeClient::Send(std::string_view request) {
    return SendRequest(request)
        .transform([](const std::string&) {});  // drop response, keep error
}

} // namespace TotalControl
