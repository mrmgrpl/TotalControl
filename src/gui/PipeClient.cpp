#include "PipeClient.h"

namespace TotalControl {

static constexpr DWORD  kReadBufSize  = 4096;
static constexpr size_t kMaxResponse  = 1024 * 1024; // 1 MiB safety limit

std::string_view PipeErrorMessage(PipeError e) noexcept {
    switch (e) {
        case PipeError::NotConnected:    return "not connected";
        case PipeError::WriteFailed:     return "write failed — pipe broken";
        case PipeError::ReadFailed:      return "read failed — pipe broken";
        case PipeError::ResponseTooLarge:return "response too large";
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

// ─── Public API ───────────────────────────────────────────────────────────────

bool PipeClient::Connect() {
    std::lock_guard lk(m_pipeMutex);
    if (m_pipe != INVALID_HANDLE_VALUE) return true;

    HANDLE h = CreateFileW(
        L"\\\\.\\pipe\\TotalControl",
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_PIPE_BUSY)
            WaitNamedPipeW(L"\\\\.\\pipe\\TotalControl", 200);
        return false;
    }

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
    std::lock_guard lk(m_pipeMutex);

    if (m_pipe == INVALID_HANDLE_VALUE)
        return std::unexpected(PipeError::NotConnected);

    // Append newline if missing
    std::string line{request};
    if (line.empty() || line.back() != '\n') line += '\n';

    DWORD written = 0;
    if (!WriteFile(m_pipe, line.data(), static_cast<DWORD>(line.size()),
                   &written, nullptr)) {
        BreakPipe();
        return std::unexpected(PipeError::WriteFailed);
    }

    // Read until newline
    std::string response;
    char buf[kReadBufSize];

    while (true) {
        DWORD read = 0;
        BOOL  ok   = ReadFile(m_pipe, buf, sizeof(buf), &read, nullptr);
        if (!ok || read == 0) {
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
